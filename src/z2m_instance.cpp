#include "z2m_instance.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "phi/adapter/sdk/button_presses.h"
#include "phi/adapter/v1/tlsconfig.h"
#include "phi/adapter/sdk/reachability.h"

#include "phi/runtime/loop.h"
#include "phi/runtime/str.h"

#include "z2m_actions.h"
#include "z2m_bridge.h"
#include "z2m_exposes.h"
#include "z2m_state.h"
#include "z2m_topics.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace std::chrono_literals;

namespace {

constexpr int kDefaultPort = 1883;
constexpr int kDefaultRetryIntervalMs = 10000;

/// A probe once a minute; two unanswered ones bring the bridge down.
constexpr auto kHealthProbeInterval = 60s;
/// Twenty seconds, from a measurement rather than a guess: a Zigbee2MQTT
/// that had just started and was still interviewing devices took 12.2 s to
/// answer. Ten would have counted that as a miss.
constexpr auto kHealthProbeReply = 20s;

constexpr auto kRenameTimeout = 10s;
constexpr auto kRemoveTimeout = 10s;
/// After a write, ask the device for its readable properties once things
/// have settled; several writes in a row ask once.
constexpr auto kPostSetRefreshDelay = 1s;
/// A dial's rotation goes back to zero once it stops turning.
constexpr auto kDialResetDelay = 700ms;
/// Core retracts its link-down blanket three seconds after link-up; the
/// coordinator is reported again after that.
constexpr auto kLinkSettle = 4s;
/// The same action string twice within this is one event.
constexpr std::int64_t kActionDuplicateWindowMs = 120;

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

v1::ScalarValue scalarOf(const Json &value)
{
    if (value.is_boolean())
        return value.get<bool>();
    if (value.is_number_integer())
        return static_cast<std::int64_t>(value.get<long long>());
    if (value.is_number_float())
        return value.get<double>();
    if (value.is_string())
        return value.get<std::string>();
    return std::monostate{};
}

std::string statusText(const Json &response)
{
    return str::toLower(jsonString(response, "status"));
}

std::string errorText(const Json &response, const char *fallback)
{
    const std::string error = jsonString(response, "error");
    return error.empty() ? fallback : error;
}

class Z2mInstance final : public sdk::AdapterInstance
{
protected:
    bool start() override
    {
        m_loop = phi::runtime::Loop::current();
        if (m_loop == nullptr) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Lifecycle,
                "started off a loop; no timers are possible");
            return false;
        }
        m_client.emplace(*m_loop);
        m_client->onConnected([this]() { onBrokerConnected(); });
        m_client->onDisconnected([this](const std::string &reason) { onBrokerDisconnected(reason); });
        m_client->onMessage([this](const MqttClient::Message &message) { onBrokerMessage(message); });
        m_lifecycle = Lifecycle::Running;
        setLinkUp(false, true);
        return true;
    }

    void stop() override
    {
        m_lifecycle = Lifecycle::Stopped;
        failPending("Instance stopped");
        disconnectBroker();
        setLinkUp(false);
        releaseLoopResources();
    }

    void onDisconnected() override
    {
        // phi-core went away, and comes back with the configuration. A pause,
        // not an end: the broker connection is dropped so nothing is reported
        // into the void, and rebuilt when the configuration returns.
        m_lifecycle = Lifecycle::Paused;
        failPending("Instance disconnected");
        disconnectBroker();
        forgetDevices();
        m_linkUp = false;
        // Nothing was said into the void either, so the next reason is new.
        m_linkDownSaid.clear();
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        if (m_lifecycle == Lifecycle::Stopped || !m_client)
            return;
        const MqttSettings previousSettings = m_settings;
        const std::string previousBaseTopic = m_baseTopic;
        m_info = request.adapter();
        m_meta = parseObject(request.adapter().metaJson());
        m_filter = ExposeFilter::fromStaticConfig(parseObject(request.staticConfigJson()));
        applyConfig();
        m_lifecycle = Lifecycle::Running;

        // Core sends config.changed for every change to the adapter's record,
        // including the meta patches this adapter itself sends - the bridge's
        // health report every ten minutes among them. Only a different broker
        // or a different base topic is a different connection; everything
        // else is applied to the one that is up. Reconnecting on each echo
        // dropped and re-announced every device every ten minutes.
        const bool sameBroker = m_settings.sameConnection(previousSettings)
            && m_baseTopic == previousBaseTopic;
        if (sameBroker && brokerConnected()) {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Config,
                "the same broker and base topic: the connection is kept");
            return;
        }

        log(sdk::LogLevel::Debug, sdk::LogCategory::Config,
            "broker %1:%2 (tls %3), base topic %4",
            {v1::Utf8String(m_settings.host), static_cast<std::int64_t>(m_settings.port),
             m_settings.tls.enabled(), v1::Utf8String(m_baseTopic)});

        // A new answer about where the broker is means a new connection,
        // whatever the old one was doing.
        disconnectBroker();
        forgetDevices();
        connectToBroker();
    }

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        const DeviceEntry *entry = m_devices.byExternalId(request.deviceExternalId());
        if (entry == nullptr) {
            answerCommand(request.cmdId(), v1::CmdStatus::NotSupported, "Unknown device");
            return;
        }
        const ChannelBinding *binding = entry->binding(request.channelExternalId());
        if (binding == nullptr) {
            answerCommand(request.cmdId(), v1::CmdStatus::NotSupported, "Unknown channel");
            return;
        }
        if (!v1::hasFlag(binding->flags, v1::ChannelFlag::Writable)) {
            answerCommand(request.cmdId(), v1::CmdStatus::NotSupported, "Channel is read-only");
            return;
        }
        if (!brokerConnected()) {
            answerCommand(request.cmdId(), v1::CmdStatus::TemporarilyOffline,
                          "MQTT broker not connected");
            return;
        }
        if (!m_health.online()) {
            answerCommand(request.cmdId(), v1::CmdStatus::TemporarilyOffline,
                          "Zigbee2MQTT bridge is offline");
            return;
        }

        CommandValue value;
        if (request.hasScalarValue())
            value.scalar = request.value();
        else
            value.json = parseJson(request.valueJson());
        if (!request.hasScalarValue() && value.json.is_primitive())
            value.scalar = scalarOf(value.json);

        Json payload = Json::object();
        std::string error;
        if (!commandPayload(*binding, value, payload, error)) {
            answerCommand(request.cmdId(), v1::CmdStatus::InvalidArgument, error);
            return;
        }
        const std::string topic = binding->endpoint.empty()
            ? deviceTopic(entry->mqttId, "set")
            : deviceTopic(entry->mqttId + "/" + binding->endpoint, "set");
        if (!m_client->publish(topic, dump(payload), 0, false, &error)) {
            answerCommand(request.cmdId(), v1::CmdStatus::Failure, error);
            return;
        }
        // The new state comes back from Zigbee2MQTT on the state topic, as
        // every state does. Nothing is reported from here as a shortcut,
        // because that report would race the real one.
        schedulePostSetRefresh(entry->mqttId, payload);
        answerCommand(request.cmdId(), v1::CmdStatus::Success, {});
    }

    void onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        const std::string actionId = str::trimmed(request.actionId());
        const Json params = parseObject(request.paramsJson());

        if (actionId == "settings") {
            if (params.empty())
                answerSettingsForm(request.cmdId());
            else
                applySettings(request.cmdId(), params);
            return;
        }
        if (actionId == "probe") {
            answerAction(request.cmdId(), v1::CmdStatus::NotImplemented, "probe is factory scoped");
            return;
        }
        if (actionId != "restartZ2M" && actionId != "device.delete") {
            answerAction(request.cmdId(), v1::CmdStatus::NotSupported, "Adapter action not supported");
            return;
        }
        if (!brokerConnected()) {
            answerAction(request.cmdId(), v1::CmdStatus::Failure, "MQTT client not connected.");
            return;
        }
        if (!m_health.online()) {
            answerAction(request.cmdId(), v1::CmdStatus::Failure, "Z2M bridge is offline.");
            return;
        }

        if (actionId == "device.delete") {
            handleDeleteDevice(request.cmdId(), params);
            return;
        }

        std::string error;
        if (!m_client->publish(bridgeTopic("request/restart"), dump(Json::object()), 0, false, &error)) {
            answerAction(request.cmdId(), v1::CmdStatus::Failure, error);
            return;
        }
        answerAction(request.cmdId(), v1::CmdStatus::Success, {});
    }

    // "Add": Zigbee2MQTT lets new devices join for the window; they arrive
    // through bridge/devices like any other.
    void onPairingStart(const sdk::PairingStartRequest &request) override { permitJoin(request.cmdId(), request.durationS()); }
    void onPairingStop(const sdk::PairingStopRequest &request) override { permitJoin(request.cmdId(), 0); }

    void permitJoin(v1::CmdId cmdId, int durationS)
    {
        if (!brokerConnected() || !m_health.online()) {
            answerCommand(cmdId, v1::CmdStatus::TemporarilyOffline, "Z2M bridge is offline.");
            return;
        }
        // Zigbee2MQTT 2.x reads `time` and nothing else, at most 254 s; 0 closes.
        const int seconds = std::clamp(durationS, 0, 254);
        std::string error;
        if (!m_client->publish(bridgeTopic("request/permit_join"), dump(Json{{"time", seconds}}), 0, false, &error)) {
            answerCommand(cmdId, v1::CmdStatus::Failure, error);
            return;
        }
        answerCommand(cmdId, v1::CmdStatus::Success, {});
        sendPairingState(seconds > 0, seconds);
    }

    void onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        const std::string name = str::trimmed(request.name());
        if (name.empty()) {
            answerCommand(request.cmdId(), v1::CmdStatus::InvalidArgument, "Name must not be empty");
            return;
        }
        const DeviceEntry *entry = m_devices.byExternalId(request.deviceExternalId());
        if (entry == nullptr) {
            answerCommand(request.cmdId(), v1::CmdStatus::NotSupported, "Unknown device");
            return;
        }
        if (m_renames.count(request.deviceExternalId())) {
            answerCommand(request.cmdId(), v1::CmdStatus::Busy, "Rename already pending");
            return;
        }
        if (!brokerConnected() || !m_health.online()) {
            answerCommand(request.cmdId(), v1::CmdStatus::TemporarilyOffline,
                          "MQTT broker not connected");
            return;
        }
        Json payload = Json::object();
        payload["from"] = entry->mqttId;
        payload["to"] = name;
        std::string error;
        if (!m_client->publish(bridgeTopic("request/device/rename"), dump(payload), 0, false,
                               &error)) {
            answerCommand(request.cmdId(), v1::CmdStatus::Failure, error);
            return;
        }
        PendingRename pending;
        pending.cmdId = request.cmdId();
        pending.from = entry->mqttId;
        pending.to = name;
        const std::string externalId = request.deviceExternalId();
        pending.timeout = m_loop->timerAfter(kRenameTimeout, [this, externalId]() {
            const auto it = m_renames.find(externalId);
            if (it == m_renames.end())
                return;
            const v1::CmdId cmdId = it->second.cmdId;
            m_renames.erase(it);
            answerCommand(cmdId, v1::CmdStatus::Timeout, "Zigbee2MQTT did not confirm the rename");
        });
        m_renames[externalId] = std::move(pending);
    }

    void onEntityNameUpdate(const sdk::EntityNameUpdateRequest &request) override
    {
        // Zigbee2MQTT has groups and no rooms, and its scenes are not entities
        // of their own - they hang off a group or an endpoint and are numbered
        // within it. So this adapter answers for groups and says plainly that
        // it cannot do the rest, rather than failing later and vaguely.
        if (request.kind() != v1::NamedEntityKind::Group) {
            answerCommand(request.cmdId(), v1::CmdStatus::NotSupported,
                          "Zigbee2MQTT only names groups");
            return;
        }

        const std::string name = str::trimmed(request.name());
        if (name.empty()) {
            answerCommand(request.cmdId(), v1::CmdStatus::InvalidArgument, "Name must not be empty");
            return;
        }
        const GroupEntry *entry = m_groups.byExternalId(request.entityExternalId());
        if (entry == nullptr) {
            answerCommand(request.cmdId(), v1::CmdStatus::NotSupported, "Unknown group");
            return;
        }
        if (m_groupRenames.count(request.entityExternalId())) {
            answerCommand(request.cmdId(), v1::CmdStatus::Busy, "Rename already pending");
            return;
        }
        if (!brokerConnected() || !m_health.online()) {
            answerCommand(request.cmdId(), v1::CmdStatus::TemporarilyOffline,
                          "MQTT broker not connected");
            return;
        }

        // `from` is the numeric id rather than the friendly name: both are
        // accepted, and the id is the one that cannot be ambiguous.
        Json payload = Json::object();
        payload["from"] = entry->id;
        payload["to"] = name;
        std::string error;
        if (!m_client->publish(bridgeTopic("request/group/rename"), dump(payload), 0, false,
                               &error)) {
            answerCommand(request.cmdId(), v1::CmdStatus::Failure, error);
            return;
        }

        PendingRename pending;
        pending.cmdId = request.cmdId();
        pending.from = entry->group.name();
        pending.to = name;
        const std::string externalId = request.entityExternalId();
        pending.timeout = m_loop->timerAfter(kRenameTimeout, [this, externalId]() {
            const auto it = m_groupRenames.find(externalId);
            if (it == m_groupRenames.end())
                return;
            const v1::CmdId cmdId = it->second.cmdId;
            m_groupRenames.erase(it);
            answerCommand(cmdId, v1::CmdStatus::Timeout, "Zigbee2MQTT did not confirm the rename");
        });
        m_groupRenames[externalId] = std::move(pending);
    }

    void onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        answerCommand(request.cmdId(), v1::CmdStatus::NotImplemented, "Device effect not supported");
    }

    void onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        answerCommand(request.cmdId(), v1::CmdStatus::NotImplemented,
                      "Scene invocation not supported");
    }

private:
    enum class Lifecycle { Idle, Running, Paused, Stopped };

    struct PendingRename {
        v1::CmdId cmdId = 0;
        std::string from;
        std::string to;
        phi::runtime::Timer timeout;
    };

    struct PendingRemove {
        v1::CmdId cmdId = 0;
        std::string externalId;
        phi::runtime::Timer timeout;
    };

    // --- configuration ----------------------------------------------------

    void applyConfig()
    {
        const int retry = jsonInt(m_meta, "retryIntervalMs", kDefaultRetryIntervalMs);
        // Floored and capped like every other adapter's: a typo used to be
        // able to park the reconnect arbitrarily far out.
        m_retryIntervalMs = std::clamp(retry >= 1000 ? retry : kDefaultRetryIntervalMs, 1000, 600000);
        m_broker = sdk::Reachability(brokerPolicy());
        m_baseTopic = normalizedBaseTopic(jsonString(m_meta, "baseTopic"));
        m_settings = mqttSettingsFor(m_info, m_meta);
    }

    std::string bridgeTopic(const std::string &path) const
    {
        return m_baseTopic + "/bridge/" + path;
    }

    std::string deviceTopic(const std::string &mqttId, const char *tail) const
    {
        return m_baseTopic + "/" + mqttId + "/" + tail;
    }

    // --- the broker -------------------------------------------------------

    bool brokerConnected() const { return m_client && m_client->connected(); }

    void connectToBroker()
    {
        if (m_lifecycle != Lifecycle::Running || !m_client)
            return;
        m_reconnect.reset();
        if (m_client->state() != MqttClient::State::Disconnected)
            return;
        if (str::trimmed(m_settings.host).empty()) {
            reportBrokerProblem("MQTT host is not configured");
            return;
        }
        std::string error;
        if (!m_client->connect(m_settings, &error)) {
            reportBrokerProblem(error);
            scheduleReconnect();
        }
    }

    void disconnectBroker()
    {
        m_reconnect.reset();
        stopHealthProbe();
        m_healthTimer.reset();
        if (m_client)
            m_client->disconnect();
        m_health.reset();
        updateLink();
    }

    /// A broker that refuses one connection is not there; what grows is the
    /// wait, so one that is down for an hour is dialled a handful of times
    /// rather than three hundred and sixty.
    [[nodiscard]] sdk::Reachability::Policy brokerPolicy() const
    {
        const auto retry = static_cast<std::int64_t>(m_retryIntervalMs);
        sdk::Reachability::Policy policy;
        policy.intervalMs = retry;
        policy.strikes = 1;
        policy.retryDelaysMs = {retry, 2 * retry, 3 * retry, 6 * retry};
        return policy;
    }

    void scheduleReconnect()
    {
        if (m_lifecycle != Lifecycle::Running || !m_loop)
            return;
        m_reconnect = m_loop->timerAfter(std::chrono::milliseconds(m_nextDialMs > 0 ? m_nextDialMs
                                                                                    : m_retryIntervalMs),
                                         [this]() { connectToBroker(); });
    }

    void onBrokerConnected()
    {
        const sdk::Reachability::Verdict verdict = m_broker.answered(nowMs());
        m_nextDialMs = verdict.waitMs;
        log(sdk::LogLevel::Info, sdk::LogCategory::Network, "connected to the broker at %1:%2",
            {v1::Utf8String(m_settings.host), static_cast<std::int64_t>(m_settings.port)});
        std::string error;
        if (!m_client->subscribe(m_baseTopic + "/#", 0, &error)) {
            reportBrokerProblem(error);
            m_client->disconnect();
            scheduleReconnect();
            return;
        }
        m_client->publish(bridgeTopic("request/info"), "{}");
        // Right here, before the retained backlog arrives. Whatever
        // `bridge/state` the broker is about to replay, this is the question
        // that decides whether it means anything.
        probeBridgeHealth();
        m_healthTimer = m_loop->timerEvery(kHealthProbeInterval, [this]() { probeBridgeHealth(); });
        updateLink();
    }

    void onBrokerDisconnected(const std::string &reason)
    {
        reportBrokerProblem(reason);
        // A dropped socket takes the bridge with it as far as this adapter
        // knows. Keeping the old answer across a reconnect would carry a
        // judgement about the far end over a gap in which nothing was
        // observed - and the next probe costs one round trip on loopback.
        stopHealthProbe();
        m_healthTimer.reset();
        m_health.reset();
        updateLink();
        scheduleReconnect();
    }

    /// Said once per distinct reason, not once per retry: a broker that is
    /// down for an hour is one fact, not three hundred and sixty.
    void reportBrokerProblem(const std::string &reason)
    {
        if (reason.empty())
            return;
        const sdk::Reachability::Verdict verdict = m_broker.missed(reason, nowMs());
        m_nextDialMs = verdict.waitMs;
        if (!verdict.say)
            return;
        v1::Utf8String error;
        sendError(sdk::LogCategory::Network, "MQTT: " + reason, {}, "broker", {}, nowMs(), &error);
        log(sdk::LogLevel::Warn, sdk::LogCategory::Network, "the broker is not usable: %1",
            {v1::Utf8String(reason)});
    }

    void onBrokerMessage(const MqttClient::Message &message)
    {
        const TopicParts parts = splitTopic(m_baseTopic, message.topic);
        if (!parts.underBase)
            return;
        const std::int64_t tsMs = nowMs();
        if (parts.bridge) {
            handleBridgeMessage(parts.bridgePath, message.payload, tsMs);
            return;
        }
        const DeviceTopicRef ref = classifyDeviceTopic(parts.suffix, [this](std::string_view name) {
            return m_devices.knows(name);
        });
        switch (ref.kind) {
        case DeviceTopic::State: {
            const Json payload = parseJson(message.payload);
            if (payload.is_object())
                handleDeviceState(ref.device, payload, message.retained, tsMs);
            return;
        }
        case DeviceTopic::Availability:
            handleAvailability(ref.device, message.payload, tsMs);
            return;
        case DeviceTopic::Echo:
        case DeviceTopic::Other:
            return;
        }
    }

    // --- the bridge -------------------------------------------------------

    void handleBridgeMessage(const std::string &path, const std::string &payload, std::int64_t tsMs)
    {
        if (path == "state") {
            handleBridgeState(payload);
            return;
        }
        if (path == "response/health_check") {
            const Json response = parseObject(payload);
            if (statusText(response) != "ok")
                return;
            // Any answer counts, not only the one to our own question.
            // Responses are published without retain, so one exists on this
            // topic for exactly as long as it takes to deliver - seeing it at
            // all means a live Zigbee2MQTT put it there just now.
            m_healthReply.reset();
            if (m_health.answered())
                updateLink();
            else
                m_health.answered();
            return;
        }
        if (path == "response/options") {
            handleOptionsResponse(parseObject(payload));
            return;
        }
        if (path == "info") {
            const Json info = parseJson(payload);
            if (info.is_object())
                handleBridgeInfo(info, tsMs);
            return;
        }
        if (path == "devices" || path == "response/devices") {
            const Json doc = parseJson(payload);
            Json devices;
            if (doc.is_array()) {
                devices = doc;
            } else if (doc.is_object()) {
                const Json data = jsonValue(doc, "data");
                if (data.is_array())
                    devices = data;
                else if (statusText(doc) == "ok" && jsonValue(doc, "result").is_array())
                    devices = doc.at("result");
            }
            if (devices.is_array() && !devices.empty())
                handleDevicesList(devices, path == "devices", tsMs);
            return;
        }
        if (path == "response/device/rename") {
            handleRenameResponse(parseObject(payload), tsMs);
            return;
        }
        if (path == "response/device/remove") {
            handleRemoveResponse(parseObject(payload));
            return;
        }
        if (path == "response/options") {
            const Json response = parseObject(payload);
            if (jsonBool(response, "restart_required")) {
                log(sdk::LogLevel::Info, sdk::LogCategory::Device,
                    "Zigbee2MQTT says a restart is required for the options this adapter set",
                    {}, "bridge");
            }
            return;
        }
        if (path == "groups" || path == "response/groups") {
            const Json doc = parseJson(payload);
            Json groups;
            if (doc.is_array()) {
                groups = doc;
            } else if (doc.is_object()) {
                const Json data = jsonValue(doc, "data");
                if (data.is_array())
                    groups = data;
            }
            if (groups.is_array())
                handleGroupsList(groups);
            return;
        }
        if (path == "response/group/rename") {
            handleGroupRenameResponse(parseObject(payload));
            return;
        }
        // bridge/logging, bridge/extensions, other responses: not read.
    }

    void handleBridgeState(const std::string &payload)
    {
        const std::string text = str::toLower(str::trimmed(payload));
        const bool offline = text == "offline" || text == "{\"state\":\"offline\"}";
        const bool online = text == "online" || text == "{\"state\":\"online\"}";
        if (!offline && !online) {
            const Json obj = parseObject(payload);
            const std::string state = str::toLower(jsonString(obj, "state"));
            if (state == "offline")
                return handleBridgeState("offline");
            if (state == "online")
                return handleBridgeState("online");
            return;
        }
        if (offline) {
            // Believed without asking, because nobody publishes their own
            // absence by mistake: either Zigbee2MQTT said it on the way out or
            // the broker delivered its will. Both mean gone.
            stopHealthProbe();
            m_health.saidOffline();
            updateLink();
            return;
        }
        // Not believed. This topic is retained, so an "online" carries no
        // date and may have outlived the process that wrote it by days. Ask
        // instead - and let the answer decide.
        probeBridgeHealth();
        requestLastSeenOption();
    }

    /**
     * The one thing this adapter changes in the operator's Zigbee2MQTT
     * configuration, and it says so when it does: `advanced.last_seen` is set
     * to `epoch`, so every state message carries when the device last spoke.
     * Without it a retained state replayed by the broker looks like a device
     * that spoke just now, and a device that died a month ago stays reachable
     * until something else says otherwise. Zigbee2MQTT writes the option to
     * its configuration.yaml; that is where it will be found.
     */
    void requestLastSeenOption()
    {
        if (m_lastSeenRequested || !brokerConnected())
            return;
        Json payload = Json::object();
        payload["options"] = Json{{"advanced", Json{{"last_seen", "epoch"}}}};
        if (m_client->publish(bridgeTopic("request/options"), dump(payload))) {
            m_lastSeenRequested = true;
            log(sdk::LogLevel::Info, sdk::LogCategory::Device,
                "asked Zigbee2MQTT to report last_seen as epoch (advanced.last_seen in its"
                " configuration.yaml), so device reachability can be judged from state messages",
                {}, "bridge");
        }
    }

    // --- the settings form ---------------------------------------------

    /// The channel the form offers: the one Zigbee2MQTT is configured for,
    /// which is the active one unless a change waits for a restart.
    int formChannel() const
    {
        const int configured = configuredChannelOf(m_bridgeInfo);
        return configured > 0 ? configured : activeChannelOf(m_bridgeInfo);
    }

    void answerSettingsForm(v1::CmdId cmdId)
    {
        v1::ActionResponse response = v1::ActionResponse::success(cmdId);
        response.setTsMs(nowMs());
        v1::AdapterFormValues values = bridgeFactsPatch(m_bridgeInfo);
        for (v1::AdapterFormValue &value : values) {
            if (value.key() == "zigbeeChannel")
                value.setKey("activeChannel");
        }
        response.setFormValues(std::move(values));
        if (formChannel() > 0)
            response.addFormValue("zigbeeChannel", v1::ScalarValue(std::int64_t{formChannel()}));
        sendResult(response, nullptr);
    }

    /// The one setting in the dialog is the channel; everything else there is
    /// what Zigbee2MQTT reports and is not written back.
    void applySettings(v1::CmdId cmdId, const Json &params)
    {
        const int requested = jsonInt(params, "zigbeeChannel", 0);
        if (requested == 0 || requested == formChannel()) {
            answerAction(cmdId, v1::CmdStatus::Success, {});
            return;
        }
        if (requested < 11 || requested > 26) {
            answerAction(cmdId, v1::CmdStatus::InvalidArgument, "A Zigbee channel is between 11 and 26.");
            return;
        }
        if (!brokerConnected() || !m_health.online()) {
            answerAction(cmdId, v1::CmdStatus::TemporarilyOffline, "Z2M bridge is offline.");
            return;
        }
        const std::string adapter = serialAdapterOf(m_bridgeInfo);
        if (!movesDevicesToAnotherChannel(adapter)) {
            answerAction(cmdId, v1::CmdStatus::NotSupported,
                         "Zigbee2MQTT cannot move the devices of a " + (adapter.empty() ? std::string("this") : adapter)
                             + " coordinator to another channel; every device would have to be paired again.");
            return;
        }
        Json payload = Json::object();
        payload["options"] = Json{{"advanced", Json{{"channel", requested}}}};
        std::string error;
        if (!m_client->publish(bridgeTopic("request/options"), dump(payload), 0, false, &error)) {
            answerAction(cmdId, v1::CmdStatus::Failure, error);
            return;
        }
        m_requestedChannel = requested;
        log(sdk::LogLevel::Info, sdk::LogCategory::Device, "asked Zigbee2MQTT to move the network to channel %1",
            {std::int64_t{requested}}, "bridge");
        v1::ActionResponse response = v1::ActionResponse::success(cmdId);
        response.setTsMs(nowMs());
        response.setResult(v1::Utf8String("Zigbee2MQTT restarts and moves the network to channel "
                                          + std::to_string(requested)
                                          + ". Devices follow within a minute; battery devices when they next wake."),
                           v1::ActionResultType::String);
        sendResult(response, nullptr);
    }

    /// A channel takes effect when Zigbee2MQTT starts: it says so with
    /// `restart_required`, and the restart is what moves the network.
    void handleOptionsResponse(const Json &response)
    {
        if (m_requestedChannel == 0)
            return;
        const int channel = m_requestedChannel;
        m_requestedChannel = 0;
        if (statusText(response) != "ok") {
            sendError(sdk::LogCategory::Device, "Zigbee2MQTT refused channel %1: %2",
                      {std::int64_t{channel}, jsonString(response, "error")}, "bridge", {}, nowMs());
            return;
        }
        if (!jsonBool(jsonValue(response, "data"), "restart_required", false))
            return;
        if (!m_client->publish(bridgeTopic("request/restart"), "{}"))
            sendError(sdk::LogCategory::Device, "could not ask Zigbee2MQTT to restart for channel %1",
                      {std::int64_t{channel}}, "bridge", {}, nowMs());
    }

    void probeBridgeHealth()
    {
        if (!brokerConnected())
            return;
        // One question at a time. A second one asked while the first is
        // unanswered would either be answered by the reply to the first, or
        // turn one silence into two - and both readings are wrong.
        const std::string transaction = "phi-" + externalId() + "-" + str::number(nowMs());
        if (!m_health.beginProbe(transaction))
            return;
        Json payload = Json::object();
        payload["transaction"] = transaction;
        m_client->publish(bridgeTopic("request/health_check"), dump(payload));
        m_healthReply = m_loop->timerAfter(kHealthProbeReply, [this]() {
            if (m_health.probeTimedOut()) {
                log(sdk::LogLevel::Warn, sdk::LogCategory::Device,
                    "Zigbee2MQTT has not answered two health checks; reporting it offline", {},
                    "bridge");
                updateLink();
            }
        });
    }

    void stopHealthProbe()
    {
        m_healthReply.reset();
    }

    void handleBridgeInfo(const Json &info, std::int64_t tsMs)
    {
        // What the bridge says about itself first, and unconditionally: which
        // serial adapter Zigbee2MQTT is driving the coordinator with is a fact
        // about the bridge and does not depend on any device existing.
        m_bridgeInfo = info;
        sendMetaPatch(bridgeFactsPatch(info));

        DeviceEntry *coordinator = m_devices.coordinatorExternalId().empty()
            ? nullptr
            : m_devices.byExternalId(m_devices.coordinatorExternalId());
        if (coordinator == nullptr) {
            m_pendingBridgeInfo = info;
            return;
        }
        m_pendingBridgeInfo = Json();
        applyBridgeInfoToCoordinator(info, *coordinator);
        announce(*coordinator);
        reportCoordinatorReachable(tsMs);
        if (const ChannelBinding *update = coordinator->updateBinding()) {
            if (const auto reading = updateReading(jsonValue(info, "update"), true))
                reportReading(coordinator->device.externalId(), update->channelId, *reading, tsMs);
        }
    }

    void handleDevicesList(const Json &devices, bool fullList, std::int64_t tsMs)
    {
        const SnapshotResult result = m_devices.applySnapshot(devices, fullList, m_filter, tsMs);
        for (const std::string &externalId : result.removed) {
            v1::Utf8String error;
            sendDeviceRemoved(externalId, &error);
        }
        for (const std::string &mqttId : result.announce) {
            if (const DeviceEntry *entry = m_devices.byMqttId(mqttId))
                announce(*entry);
        }
        if (!result.announce.empty())
            reportCoordinatorReachable(tsMs);
        for (const auto &[ieee, newName] : result.renamed)
            completeRename(ieee, newName, tsMs);
        for (const ConnectivityReport &report : result.connectivity)
            reportConnectivity(report.externalId, report.channelId, report.status, tsMs);
        for (const auto &[mqttId, payload] : result.releasedState)
            handleDeviceState(mqttId, payload, true, tsMs);

        if (!m_pendingGroups.is_null() && !m_devices.all().empty()) {
            const Json groups = m_pendingGroups;
            m_pendingGroups = Json();
            handleGroupsList(groups);
        }
        if (!m_pendingBridgeInfo.is_null() && !m_devices.coordinatorExternalId().empty()) {
            const Json info = m_pendingBridgeInfo;
            handleBridgeInfo(info, tsMs);
        }
        if (!result.announce.empty() || !result.removed.empty()) {
            log(sdk::LogLevel::Info, sdk::LogCategory::Device,
                "devices: %1 known, %2 announced, %3 removed",
                {static_cast<std::int64_t>(m_devices.all().size()),
                 static_cast<std::int64_t>(result.announce.size()),
                 static_cast<std::int64_t>(result.removed.size())});
        }
    }

    void handleRenameResponse(const Json &response, std::int64_t tsMs)
    {
        const Json data = jsonValue(response, "data");
        const std::string from = jsonString(data, "from");
        const std::string to = jsonString(data, "to");
        const std::string status = statusText(response);
        for (auto it = m_renames.begin(); it != m_renames.end(); ++it) {
            const PendingRename &pending = it->second;
            const bool matches = (!to.empty() && pending.to == to)
                || (!from.empty() && pending.from == from);
            if (!matches)
                continue;
            const v1::CmdId cmdId = pending.cmdId;
            const std::string externalId = it->first;
            m_renames.erase(it);
            if (status == "ok") {
                // The device list that follows moves the entry; the answer
                // to the person does not have to wait for it.
                answerCommand(cmdId, v1::CmdStatus::Success, {});
            } else {
                answerCommand(cmdId, v1::CmdStatus::Failure,
                              errorText(response, "Zigbee2MQTT refused the rename"));
            }
            (void)externalId;
            (void)tsMs;
            return;
        }
    }

    void handleGroupRenameResponse(const Json &response)
    {
        const Json data = jsonValue(response, "data");
        const std::string from = jsonString(data, "from");
        const std::string to = jsonString(data, "to");
        const std::string status = statusText(response);
        for (auto it = m_groupRenames.begin(); it != m_groupRenames.end(); ++it) {
            const PendingRename &pending = it->second;
            const bool matches = (!to.empty() && pending.to == to)
                || (!from.empty() && pending.from == from);
            if (!matches)
                continue;
            const v1::CmdId cmdId = pending.cmdId;
            m_groupRenames.erase(it);
            if (status == "ok") {
                // The groups list that follows carries the new name; the
                // answer to the person does not have to wait for it.
                answerCommand(cmdId, v1::CmdStatus::Success, {});
            } else {
                answerCommand(cmdId, v1::CmdStatus::Failure,
                              errorText(response, "Zigbee2MQTT refused the rename"));
            }
            return;
        }
    }

    void completeGroupRename(const std::string &externalId, const std::string &newName)
    {
        const auto it = m_groupRenames.find(externalId);
        if (it == m_groupRenames.end() || it->second.to != newName)
            return;
        const v1::CmdId cmdId = it->second.cmdId;
        m_groupRenames.erase(it);
        answerCommand(cmdId, v1::CmdStatus::Success, {});
    }

    void completeRename(const std::string &ieee, const std::string &newName, std::int64_t tsMs)
    {
        (void)tsMs;
        const auto it = m_renames.find(ieee);
        if (it == m_renames.end() || it->second.to != newName)
            return;
        const v1::CmdId cmdId = it->second.cmdId;
        m_renames.erase(it);
        answerCommand(cmdId, v1::CmdStatus::Success, {});
    }

    void handleDeleteDevice(v1::CmdId cmdId, const Json &params)
    {
        const std::string externalId = jsonString(params, "externalId");
        if (externalId.empty()) {
            answerAction(cmdId, v1::CmdStatus::InvalidArgument, "Missing device externalId.");
            return;
        }
        const DeviceEntry *entry = m_devices.byExternalId(externalId);
        if (entry == nullptr) {
            answerAction(cmdId, v1::CmdStatus::InvalidArgument, "Device not found.");
            return;
        }
        if (m_removes.count(entry->mqttId)) {
            answerAction(cmdId, v1::CmdStatus::Busy, "Removal already pending.");
            return;
        }
        Json payload = Json::object();
        payload["id"] = entry->mqttId;
        std::string error;
        if (!m_client->publish(bridgeTopic("request/device/remove"), dump(payload), 0, false,
                               &error)) {
            answerAction(cmdId, v1::CmdStatus::Failure, error);
            return;
        }
        // The device stays until Zigbee2MQTT says it is gone. Forgetting it
        // here and being corrected by the next device list is how a refused
        // removal used to flap.
        PendingRemove pending;
        pending.cmdId = cmdId;
        pending.externalId = externalId;
        const std::string mqttId = entry->mqttId;
        pending.timeout = m_loop->timerAfter(kRemoveTimeout, [this, mqttId]() {
            const auto it = m_removes.find(mqttId);
            if (it == m_removes.end())
                return;
            const v1::CmdId pendingCmd = it->second.cmdId;
            m_removes.erase(it);
            answerAction(pendingCmd, v1::CmdStatus::Timeout,
                         "Zigbee2MQTT did not confirm the removal");
        });
        m_removes[mqttId] = std::move(pending);
    }

    void handleRemoveResponse(const Json &response)
    {
        const std::string id = jsonString(jsonValue(response, "data"), "id");
        auto it = id.empty() ? m_removes.end() : m_removes.find(id);
        if (it == m_removes.end() && m_removes.size() == 1)
            it = m_removes.begin();
        if (it == m_removes.end())
            return;
        const v1::CmdId cmdId = it->second.cmdId;
        const std::string externalId = it->second.externalId;
        const std::string mqttId = it->first;
        m_removes.erase(it);
        if (statusText(response) != "ok") {
            answerAction(cmdId, v1::CmdStatus::Failure,
                         errorText(response, "Zigbee2MQTT refused the removal"));
            return;
        }
        m_devices.remove(mqttId);
        v1::Utf8String error;
        sendDeviceRemoved(externalId, &error);
        answerAction(cmdId, v1::CmdStatus::Success, {});
    }

    // --- devices ----------------------------------------------------------

    void handleGroupsList(const Json &groups)
    {
        // A group names its members by ieee address, and core can only make a
        // membership out of that once it knows the devices. Both lists are
        // retained, so on a fresh connection they arrive in whatever order the
        // broker sends them - if the groups come first they wait here for the
        // devices, the way a device state that arrives before its description
        // waits in the table below.
        if (m_devices.all().empty()) {
            m_pendingGroups = groups;
            return;
        }
        m_pendingGroups = Json();

        const GroupSnapshotResult result = m_groups.applySnapshot(groups);
        for (const std::string &externalId : result.removed) {
            v1::Utf8String error;
            sendGroupRemoved(externalId, &error);
        }
        for (const std::string &externalId : result.announce) {
            if (const GroupEntry *entry = m_groups.byExternalId(externalId))
                announceGroup(*entry);
        }
        for (const auto &[externalId, newName] : result.renamed)
            completeGroupRename(externalId, newName);

        if (!result.announce.empty() || !result.removed.empty()) {
            log(sdk::LogLevel::Info, sdk::LogCategory::Device,
                "groups: %1 known, %2 announced, %3 removed",
                {static_cast<std::int64_t>(m_groups.all().size()),
                 static_cast<std::int64_t>(result.announce.size()),
                 static_cast<std::int64_t>(result.removed.size())},
                "bridge");
        }
    }

    void announceGroup(const GroupEntry &entry)
    {
        v1::Utf8String error;
        if (!sendGroupUpdated(entry.forWire(), &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send groupUpdated(%1): %2",
                {v1::Utf8String(entry.group.externalId()), error});
    }

    void announce(const DeviceEntry &entry)
    {
        v1::Utf8String error;
        if (!sendDeviceUpdated(entry.forWire(), entry.channels, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send deviceUpdated(%1): %2",
                {v1::Utf8String(entry.mqttId), error});
    }

    void handleDeviceState(const std::string &mqttId, const Json &payload, bool retained,
                           std::int64_t tsMs)
    {
        DeviceEntry *entry = m_devices.byMqttId(mqttId);
        if (entry == nullptr) {
            m_devices.holdState(mqttId, payload);
            return;
        }
        const std::string externalId = entry->device.externalId();

        // Facts about the device that ride along with its state.
        bool metaChanged = false;
        const Json update = jsonValue(payload, "update");
        if (update.is_object() && jsonValue(entry->meta, "update") != update) {
            entry->meta["update"] = update;
            metaChanged = true;
        }
        std::int64_t seenMs = 0;
        if (payload.contains("last_seen")) {
            seenMs = lastSeenMs(payload.at("last_seen"));
            // Recorded every time, announced rarely: a device that reports
            // every few seconds would otherwise reconcile itself in phi-core
            // every few seconds to say nothing had changed.
            entry->meta["last_seen"] = payload.at("last_seen");
            if (seenMs > 0 && seenMs - entry->lastSeenAnnouncedMs > 60000) {
                entry->lastSeenAnnouncedMs = seenMs;
                metaChanged = true;
            }
        }
        if (metaChanged)
            announce(*entry);

        // Reachability: the availability field when it is there, else how
        // long ago the device last spoke, else the fact that it spoke - but
        // only for a message published now. A retained one is the broker
        // repeating something old, and says nothing about now.
        v1::ConnectivityStatus connectivity = v1::ConnectivityStatus::Unknown;
        if (payload.contains("availability"))
            connectivity = availabilityStatus(payload.at("availability"));
        if (connectivity == v1::ConnectivityStatus::Unknown && seenMs > 0) {
            connectivity = (tsMs - seenMs) > kStaleAfterMs ? v1::ConnectivityStatus::Disconnected
                                                           : v1::ConnectivityStatus::Connected;
        }
        if (connectivity == v1::ConnectivityStatus::Unknown && !retained && !payload.empty())
            connectivity = v1::ConnectivityStatus::Connected;
        if (connectivity != v1::ConnectivityStatus::Unknown) {
            if (const ChannelBinding *availability = entry->availabilityBinding())
                reportConnectivity(externalId, availability->channelId, connectivity, tsMs);
        }

        for (const auto &[channelId, binding] : entry->bindings) {
            if (binding.isAvailability)
                continue;
            if (binding.isUpdate) {
                if (const auto reading = updateReading(update, entry->coordinator))
                    reportReading(externalId, channelId, *reading, tsMs);
                continue;
            }
            if (!payload.contains(binding.property))
                continue;
            const Json &value = payload.at(binding.property);
            if (binding.property == "action") {
                if (value.is_string())
                    handleAction(*entry, binding, value.get<std::string>(), payload, tsMs);
                continue;
            }
            if (const auto reading = readingFor(binding, value))
                reportReading(externalId, channelId, *reading, tsMs);
        }
    }

    void handleAction(const DeviceEntry &entry, const ChannelBinding &binding,
                      const std::string &actionRaw, const Json &payload, std::int64_t tsMs)
    {
        const std::string action = str::toLower(str::trimmed(actionRaw));
        if (action.empty())
            return; // Zigbee2MQTT clears the action after publishing it
        const std::string &externalId = entry.device.externalId();
        const std::string key = externalId + ":" + binding.channelId;

        if (binding.actionIsDial) {
            if (!isDialAction(action))
                return;
            const int magnitude = dialMagnitude(action, payload);
            if (magnitude <= 0)
                return;
            int sign = dialDirection(action, payload);
            if (sign == 0)
                sign = 1;
            report(externalId, binding.channelId, static_cast<std::int64_t>(sign * magnitude), tsMs);
            const std::string channelId = binding.channelId;
            m_dialResets[key] = m_loop->timerAfter(kDialResetDelay, [this, key, externalId, channelId]() {
                m_dialResets.erase(key);
                report(externalId, channelId, static_cast<std::int64_t>(0), nowMs());
            });
            return;
        }

        // The same action twice within a blink is one press.
        const std::string dedupKey = key + ":" + action;
        const auto recent = m_recentActions.find(dedupKey);
        if (recent != m_recentActions.end() && (tsMs - recent->second) <= kActionDuplicateWindowMs)
            return;
        m_recentActions[dedupKey] = tsMs;
        if (m_recentActions.size() > 512) {
            const std::int64_t cutoff = tsMs - 10000;
            for (auto it = m_recentActions.begin(); it != m_recentActions.end();) {
                if (it->second < cutoff)
                    it = m_recentActions.erase(it);
                else
                    ++it;
            }
        }

        // A multi-button remote: this channel only listens to its number.
        const int buttonId = buttonIdFromAction(action);
        if (binding.actionButtonId > 0 && buttonId != binding.actionButtonId)
            return;

        v1::ButtonEventCode code = buttonEventFor(action);
        const bool sceneButton = action.rfind("scene_", 0) == 0;
        if (!sceneButton && code == v1::ButtonEventCode::None) {
            const Json actionType = jsonValue(payload, "action_type");
            if (actionType.is_string())
                code = buttonEventFor(actionType.get<std::string>());
        }
        if (!sceneButton && code == v1::ButtonEventCode::None)
            return;

        const std::string channelId = binding.channelId;
        const auto apply = [&](const sdk::ButtonPresses::Outcome &outcome) {
            for (const sdk::ButtonPresses::Report &entry : outcome.report)
                report(externalId, channelId, static_cast<std::int64_t>(entry.code), entry.tsMs);
            if (outcome.cancelWindow)
                m_pressWindows.erase(key);
            if (outcome.windowUntilMs) {
                const auto delay = std::chrono::milliseconds(
                    std::max<std::int64_t>(1, *outcome.windowUntilMs - nowMs()));
                m_pressWindows[key] = m_loop->timerAfter(delay, [this, key, externalId, channelId]() {
                    m_pressWindows.erase(key);
                    for (const sdk::ButtonPresses::Report &entry : m_presses.onWindowClosed(key))
                        report(externalId, channelId, static_cast<std::int64_t>(entry.code),
                               entry.tsMs);
                });
            }
        };
        if (sceneButton) {
            // A scene button is a press and a release in one word.
            apply(m_presses.onEvent(key, v1::ButtonEventCode::InitialPress, tsMs, tsMs));
            apply(m_presses.onEvent(key, v1::ButtonEventCode::ShortPressRelease, tsMs, tsMs));
        } else {
            apply(m_presses.onEvent(key, code, tsMs, tsMs));
        }
    }

    void handleAvailability(const std::string &mqttId, const std::string &payload,
                            std::int64_t tsMs)
    {
        const DeviceEntry *entry = m_devices.byMqttId(mqttId);
        if (entry == nullptr)
            return;
        const ChannelBinding *availability = entry->availabilityBinding();
        if (availability == nullptr)
            return;
        Json value = parseJson(payload);
        if (!value.is_object())
            value = str::trimmed(payload);
        const v1::ConnectivityStatus status = availabilityStatus(value);
        reportConnectivity(entry->device.externalId(), availability->channelId, status, tsMs);
    }

    /// Say the coordinator is reachable, whenever there is reason to think
    /// core may have forgotten. Zigbee2MQTT publishes nothing about the
    /// coordinator after announcing it - no availability topic, no state
    /// topic - so unless this adapter says it is reachable, nothing ever will.
    /// Core blanks every device behind an adapter when the link drops, so the
    /// edge that brings the link back is the edge that has to say it again.
    void reportCoordinatorReachable(std::int64_t tsMs)
    {
        if (!m_linkUp)
            return;
        const std::string &coordinatorId = m_devices.coordinatorExternalId();
        if (coordinatorId.empty())
            return;
        const DeviceEntry *entry = m_devices.byExternalId(coordinatorId);
        if (entry == nullptr)
            return;
        if (const ChannelBinding *availability = entry->availabilityBinding()) {
            reportConnectivity(coordinatorId, availability->channelId,
                               v1::ConnectivityStatus::Connected, tsMs);
            log(sdk::LogLevel::Debug, sdk::LogCategory::Device, "coordinator %1 reported reachable",
                {v1::Utf8String(coordinatorId)});
        }
    }

    void schedulePostSetRefresh(const std::string &mqttId, const Json &written)
    {
        // Every write before the timer fires counts: several writes in a row
        // ask once, and none of what they wrote is asked for.
        std::set<std::string> &properties = m_postSetWritten[mqttId];
        for (auto it = written.begin(); it != written.end(); ++it)
            properties.insert(it.key());
        m_postSetRefresh[mqttId] = m_loop->timerAfter(kPostSetRefreshDelay, [this, mqttId]() {
            m_postSetRefresh.erase(mqttId);
            const std::set<std::string> properties = m_postSetWritten[mqttId];
            m_postSetWritten.erase(mqttId);
            const DeviceEntry *entry = m_devices.byMqttId(mqttId);
            if (entry == nullptr || !brokerConnected())
                return;
            const Json ask = postSetRefreshRequest(*entry, properties);
            if (ask.empty())
                return;
            m_client->publish(deviceTopic(mqttId, "get"), dump(ask));
        });
    }

    void forgetDevices()
    {
        m_devices.clear();
        m_groups.clear();
        m_pendingGroups = Json();
        m_presses.clear();
        m_recentActions.clear();
        m_dialResets.clear();
        m_pressWindows.clear();
        m_postSetRefresh.clear();
        m_postSetWritten.clear();
        m_pendingBridgeInfo = Json();
        m_lastSeenRequested = false;
    }

    // --- reporting --------------------------------------------------------

    void report(const std::string &deviceId, const std::string &channelId,
                const v1::ScalarValue &value, std::int64_t tsMs)
    {
        v1::Utf8String error;
        if (!sendChannelStateUpdated(deviceId, channelId, value, tsMs, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                "failed to send channelStateUpdated(%1): %2", {v1::Utf8String(channelId), error});
    }

    void reportReading(const std::string &deviceId, const std::string &channelId,
                       const Reading &reading, std::int64_t tsMs)
    {
        v1::Utf8String error;
        switch (reading.shape) {
        case Reading::Shape::Scalar:
            report(deviceId, channelId, reading.scalar, tsMs);
            return;
        case Reading::Shape::Color:
            if (!sendChannelColorStateUpdated(deviceId, channelId, reading.color.r, reading.color.g,
                                              reading.color.b, tsMs, &error))
                log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                    "failed to send channelColorStateUpdated: %1", {error});
            return;
        case Reading::Shape::Object:
            if (!sendChannelObjectStateUpdated(deviceId, channelId, reading.fields, tsMs, &error))
                log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                    "failed to send channelObjectStateUpdated: %1", {error});
            return;
        }
    }

    void reportConnectivity(const std::string &deviceId, const std::string &channelId,
                            v1::ConnectivityStatus status, std::int64_t tsMs)
    {
        report(deviceId, channelId, static_cast<std::int64_t>(status), tsMs);
    }

    void sendMetaPatch(const v1::AdapterFormValues &patch)
    {
        v1::Utf8String error;
        if (!sendAdapterMetaUpdated(patch, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                "failed to send adapterMetaUpdated: %1", {error});
    }

    // --- the link to core -------------------------------------------------

    void updateLink()
    {
        setLinkUp(brokerConnected() && m_health.online());
        if (!m_linkUp)
            sayWhyTheLinkIsDown();
    }

    /// Which half is missing, for the line that says the link is down.
    ///
    /// The two are far apart in practice: no broker is a broker that is not
    /// running or not reachable, while a broker with no bridge behind it is
    /// the usual shape of a Zigbee2MQTT that cannot start - a coordinator
    /// that is unplugged, most often. Saying which one it is saves the reader
    /// the walk from "not connected" to a backend's journal.
    std::string linkDownReason() const
    {
        if (!brokerConnected())
            return "no connection to the MQTT broker";
        if (!m_health.online())
            return "the broker is up, Zigbee2MQTT is not answering";
        return "nothing is missing";
    }

    /// Says why the link is down, once per answer.
    ///
    /// Said again when the answer changes, because the two halves come back
    /// separately: the broker turns up seconds after this adapter starts, and
    /// a line that named it as the missing one would stand there naming the
    /// half that is now fine while the reader goes looking in the wrong place.
    /// A reason that has not changed is the same sentence about the same
    /// unchanged fact, and is not repeated.
    void sayWhyTheLinkIsDown()
    {
        std::string reason = linkDownReason();
        if (reason == m_linkDownSaid)
            return;
        m_linkDownSaid = reason;
        log(sdk::LogLevel::Info, sdk::LogCategory::Lifecycle,
            "the link to Zigbee2MQTT is down: %1", {v1::Utf8String(std::move(reason))});
    }

    void setLinkUp(bool up, bool force = false)
    {
        if (m_linkUp == up && !force)
            return;
        m_linkUp = up;
        m_linkDownSaid.clear();
        if (up)
            log(sdk::LogLevel::Info, sdk::LogCategory::Lifecycle, "the link to Zigbee2MQTT is up");
        else
            sayWhyTheLinkIsDown();
        v1::Utf8String error;
        if (!sendConnectionStateChanged(up, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                "failed to send connectionStateChanged: %1", {error});
        m_linkSettle.reset();
        if (!up)
            return;
        // Immediately after saying the link is back, on this edge rather than
        // on any particular MQTT message - and once more after core has had
        // its say. Core answers a link edge with its own writes to every
        // device's connectivity (a blanket Disconnected on the way down, its
        // retraction three seconds after the way up), and those have been
        // measured landing a few milliseconds *after* this first report, on
        // top of it. The coordinator is the one device nothing else will
        // ever report again, so it is said again once the dust has settled.
        reportCoordinatorReachable(nowMs());
        m_linkSettle = m_loop->timerAfter(kLinkSettle, [this]() {
            reportCoordinatorReachable(nowMs());
        });
    }

    // --- answering --------------------------------------------------------

    void answerCommand(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::CmdResponse response = v1::CmdResponse().setId(cmdId).setStatus(status).setError(error);
        response.setTsMs(nowMs());
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the command result: %1",
                {sendError});
    }

    void answerAction(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::ActionResponse response = v1::ActionResponse().setId(cmdId).setStatus(status).setError(error);
        response.setTsMs(nowMs());
        response.setResultType(v1::ActionResultType::None);
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the action result: %1",
                {sendError});
    }

    void failPending(const std::string &reason)
    {
        for (auto &[externalId, pending] : m_renames)
            answerCommand(pending.cmdId, v1::CmdStatus::Failure, reason);
        m_renames.clear();
        for (auto &[groupExternalId, pending] : m_groupRenames)
            answerCommand(pending.cmdId, v1::CmdStatus::Failure, reason);
        m_groupRenames.clear();
        for (auto &[mqttId, pending] : m_removes)
            answerAction(pending.cmdId, v1::CmdStatus::Failure, reason);
        m_removes.clear();
    }

    void releaseLoopResources()
    {
        m_reconnect.reset();
        m_healthTimer.reset();
        m_healthReply.reset();
        m_linkSettle.reset();
        m_dialResets.clear();
        m_pressWindows.clear();
        m_postSetRefresh.clear();
        m_postSetWritten.clear();
        m_client.reset();
        m_loop = nullptr;
    }

    // --- state ------------------------------------------------------------

    phi::runtime::Loop *m_loop = nullptr;
    Lifecycle m_lifecycle = Lifecycle::Idle;
    std::optional<MqttClient> m_client;

    v1::Adapter m_info;
    Json m_meta = Json::object();
    ExposeFilter m_filter;
    MqttSettings m_settings;
    std::string m_baseTopic = "zigbee2mqtt";
    int m_retryIntervalMs = kDefaultRetryIntervalMs;
    sdk::Reachability m_broker;
    /// What the gate said to wait before dialling the broker again.
    std::int64_t m_nextDialMs = 0;

    phi::runtime::Timer m_reconnect;
    phi::runtime::Timer m_healthTimer;
    phi::runtime::Timer m_healthReply;
    phi::runtime::Timer m_linkSettle;
    BridgeHealth m_health;
    bool m_linkUp = false;
    /// The reason last said out loud while the link was down, so that a reason
    /// nobody has heard is said and one everybody has heard is not.
    std::string m_linkDownSaid;
    bool m_lastSeenRequested = false;
    Json m_pendingBridgeInfo;
    /// The last `bridge/info`, for the settings form.
    Json m_bridgeInfo;
    /// The channel asked for in `bridge/request/options`, until Zigbee2MQTT answered.
    int m_requestedChannel = 0;

    DeviceTable m_devices;
    GroupTable m_groups;
    /// A `bridge/groups` payload that arrived before any device did.
    Json m_pendingGroups;
    sdk::ButtonPresses m_presses;
    std::map<std::string, std::int64_t> m_recentActions;
    std::map<std::string, phi::runtime::Timer> m_dialResets;
    std::map<std::string, phi::runtime::Timer> m_pressWindows;
    std::map<std::string, phi::runtime::Timer> m_postSetRefresh;
    /// What was written since the refresh was last scheduled, per device.
    std::map<std::string, std::set<std::string>> m_postSetWritten;
    std::map<std::string, PendingRename> m_renames;
    /// Group renames in flight, keyed by the group's external id.
    std::map<std::string, PendingRename> m_groupRenames;
    std::map<std::string, PendingRemove> m_removes;
};

} // namespace

MqttSettings mqttSettingsFor(const v1::Adapter &adapter, const Json &meta)
{
    MqttSettings settings;
    settings.clientId = "phi-core-z2m-" + adapter.externalId();
    settings.host = str::trimmed(adapter.ip());
    if (settings.host.empty())
        settings.host = str::trimmed(adapter.host());
    settings.port = adapter.port() > 0 ? adapter.port() : kDefaultPort;
    settings.username = str::trimmed(adapter.user());
    settings.password = adapter.password();
    // Read through the contract rather than out of the meta directly, so that
    // the string "false" and the boolean false mean the same thing here as
    // they do in every other adapter - which is the whole reason the fields
    // are shared.
    settings.tls = v1::tlsSettingsFrom(scalarOf(jsonValue(meta, v1::kTlsFieldKey)),
                                       scalarOf(jsonValue(meta, v1::kTlsCaFileFieldKey)),
                                       scalarOf(jsonValue(meta, v1::kTlsVerifyHostnameFieldKey)));
    return settings;
}

std::unique_ptr<sdk::AdapterInstance> makeInstance()
{
    return std::make_unique<Z2mInstance>();
}

} // namespace phicore::z2m::ipc
