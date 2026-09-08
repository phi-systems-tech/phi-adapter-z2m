#include "z2m_bridge.h"

#include <set>
#include <utility>

#include "phi/runtime/str.h"

#include "z2m_state.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

namespace {

std::string coordinatorFirmware(const Json &coordinatorMeta)
{
    for (const char *key : {"revision", "firmware", "version"}) {
        const std::string text = jsonScalarText(jsonValue(coordinatorMeta, key));
        if (!str::trimmed(text).empty())
            return str::trimmed(text);
    }
    return {};
}

} // namespace

Json bridgeFactsPatch(const Json &info)
{
    const Json coordinator = jsonValue(info, "coordinator");
    const Json coordinatorMeta = jsonValue(coordinator, "meta");
    const Json serial = jsonValue(jsonValue(info, "config"), "serial");
    const Json network = jsonValue(info, "network");

    Json patch = Json::object();
    patch["bridge_info"] = info;

    const std::string version = jsonString(info, "version", false);
    if (!version.empty())
        patch["z2mVersion"] = version;
    const std::string commit = jsonString(info, "commit", false);
    if (!commit.empty())
        patch["z2mCommit"] = commit;
    if (info.is_object() && info.contains("permit_join"))
        patch["permitJoin"] = info.at("permit_join");
    if (info.is_object() && info.contains("log_level"))
        patch["logLevel"] = info.at("log_level");
    if (network.is_object() && network.contains("channel"))
        patch["zigbeeChannel"] = network.at("channel");

    const std::string panId = str::trimmed(jsonScalarText(jsonValue(network, "pan_id")));
    if (!panId.empty())
        patch["panId"] = panId;
    const std::string extPanId = str::trimmed(jsonScalarText(jsonValue(network, "extended_pan_id")));
    if (!extPanId.empty())
        patch["extPanId"] = extPanId;

    const std::string serialPort = jsonString(serial, "port");
    if (!serialPort.empty())
        patch["serialPort"] = serialPort;
    // Zigbee2MQTT's own word for the driver it opened the stick with: `ember`,
    // `zstack`, `deconz`. The host layer names the same thing a radio family,
    // and the two agreeing is worth being able to see rather than assume.
    const std::string serialAdapter = jsonString(serial, "adapter");
    if (!serialAdapter.empty())
        patch["serialAdapter"] = serialAdapter;

    const std::string coordinatorType = jsonString(coordinator, "type");
    if (!coordinatorType.empty())
        patch["coordinatorType"] = coordinatorType;
    const std::string firmware = coordinatorFirmware(coordinatorMeta);
    if (!firmware.empty())
        patch["coordinatorFirmware"] = firmware;
    return patch;
}

void applyBridgeInfoToCoordinator(const Json &info, DeviceEntry &entry)
{
    const Json coordinator = jsonValue(info, "coordinator");
    const Json coordinatorMeta = jsonValue(coordinator, "meta");
    const std::string manufacturer = jsonString(coordinatorMeta, "manufacturer", false);
    if (!manufacturer.empty())
        entry.device.manufacturer = manufacturer;
    const std::string model = jsonString(coordinatorMeta, "model", false);
    if (!model.empty())
        entry.device.model = model;
    const std::string firmware = coordinatorFirmware(coordinatorMeta);
    if (!firmware.empty())
        entry.device.firmware = firmware;
    entry.device.deviceClass = v1::DeviceClass::Gateway;
    entry.meta["coordinator"] = coordinator.is_null() ? Json(true) : coordinator;
    const Json serial = jsonValue(jsonValue(info, "config"), "serial");
    const std::string serialPort = jsonString(serial, "port");
    if (!serialPort.empty())
        entry.meta["serial_port"] = serialPort;
    const std::string serialAdapter = jsonString(serial, "adapter");
    if (!serialAdapter.empty())
        entry.meta["serial_adapter"] = serialAdapter;
}

bool BridgeHealth::beginProbe(std::string transaction)
{
    if (!m_transaction.empty())
        return false;
    m_transaction = std::move(transaction);
    return true;
}

bool BridgeHealth::answered()
{
    m_transaction.clear();
    m_misses = 0;
    if (m_online)
        return false;
    m_online = true;
    return true;
}

bool BridgeHealth::probeTimedOut()
{
    m_transaction.clear();
    if (!m_online)
        return false;
    if (++m_misses < kMissesBeforeOffline)
        return false;
    m_online = false;
    m_misses = 0;
    return true;
}

void BridgeHealth::saidOffline()
{
    m_online = false;
    m_misses = 0;
}

void BridgeHealth::reset()
{
    m_online = false;
    m_misses = 0;
    m_transaction.clear();
}

const DeviceEntry *DeviceTable::byMqttId(std::string_view mqttId) const
{
    const auto it = m_byMqttId.find(std::string(mqttId));
    return it == m_byMqttId.end() ? nullptr : &it->second;
}

DeviceEntry *DeviceTable::byMqttId(std::string_view mqttId)
{
    const auto it = m_byMqttId.find(std::string(mqttId));
    return it == m_byMqttId.end() ? nullptr : &it->second;
}

std::string DeviceTable::mqttIdFor(std::string_view externalId) const
{
    const auto it = m_mqttByExternal.find(std::string(externalId));
    if (it != m_mqttByExternal.end())
        return it->second;
    // An external id that is itself a friendly name (a device without an
    // ieee address) is its own mqtt id.
    return knows(externalId) ? std::string(externalId) : std::string();
}

const DeviceEntry *DeviceTable::byExternalId(std::string_view externalId) const
{
    const std::string mqttId = mqttIdFor(externalId);
    return mqttId.empty() ? nullptr : byMqttId(mqttId);
}

DeviceEntry *DeviceTable::byExternalId(std::string_view externalId)
{
    const std::string mqttId = mqttIdFor(externalId);
    return mqttId.empty() ? nullptr : byMqttId(mqttId);
}

void DeviceTable::index(const DeviceEntry &entry)
{
    if (!entry.device.externalId.empty())
        m_mqttByExternal[entry.device.externalId] = entry.mqttId;
    if (entry.coordinator)
        m_coordinatorExternalId = entry.device.externalId;
}

void DeviceTable::unindex(const DeviceEntry &entry)
{
    if (!entry.device.externalId.empty()) {
        const auto it = m_mqttByExternal.find(entry.device.externalId);
        if (it != m_mqttByExternal.end() && it->second == entry.mqttId)
            m_mqttByExternal.erase(it);
    }
    if (entry.coordinator && m_coordinatorExternalId == entry.device.externalId)
        m_coordinatorExternalId.clear();
}

SnapshotResult DeviceTable::applySnapshot(const Json &devices, bool fullList,
                                          const ExposeFilter &filter, std::int64_t nowMs)
{
    SnapshotResult result;
    if (!devices.is_array())
        return result;

    std::set<std::string> seen;
    for (const Json &obj : devices) {
        if (!obj.is_object())
            continue;
        const std::string mqttId = jsonString(obj, "friendly_name");
        if (mqttId.empty())
            continue;
        const std::string ieee = jsonString(obj, "ieee_address");
        const bool interviewCompleted = jsonBool(obj, "interview_completed", true);
        const bool supported = jsonBool(obj, "supported", true);

        // Where this device lived until now, if anywhere.
        std::string previousMqttId;
        if (!ieee.empty()) {
            const auto it = m_mqttByExternal.find(ieee);
            if (it != m_mqttByExternal.end())
                previousMqttId = it->second;
        }
        if (previousMqttId.empty() && knows(mqttId))
            previousMqttId = mqttId;

        if (!interviewCompleted || !supported) {
            // Mid-interview or unsupported: not a device yet, or not one this
            // adapter can drive. Whatever was known under that name goes.
            if (!previousMqttId.empty()) {
                const std::string externalId = remove(previousMqttId);
                if (!externalId.empty())
                    result.removed.push_back(externalId);
            }
            m_heldState.erase(mqttId);
            continue;
        }

        seen.insert(mqttId);
        const bool renamed = !previousMqttId.empty() && previousMqttId != mqttId;
        const std::string key = definitionKeyOf(obj);
        DeviceEntry *existing = previousMqttId.empty() ? nullptr : byMqttId(previousMqttId);
        bool announce = false;

        DeviceEntry entry;
        if (existing != nullptr && existing->definitionKey == key) {
            // Same device, same description. Keep what was built; only the
            // name may have moved.
            entry = *existing;
            if (renamed) {
                entry.mqttId = mqttId;
                entry.device.name = mqttId;
                entry.meta["friendly_name"] = mqttId;
                announce = true;
            }
        } else {
            entry = buildDeviceEntry(obj, filter);
            announce = true;
        }

        if (existing != nullptr) {
            unindex(*existing);
            m_byMqttId.erase(previousMqttId);
            if (renamed) {
                const auto held = m_heldState.find(previousMqttId);
                if (held != m_heldState.end()) {
                    m_heldState[mqttId] = held->second;
                    m_heldState.erase(held);
                }
            }
        }
        if (renamed && !ieee.empty())
            result.renamed.emplace_back(ieee, mqttId);

        m_byMqttId[mqttId] = entry;
        index(entry);
        if (announce)
            result.announce.push_back(mqttId);

        const auto held = m_heldState.find(mqttId);
        if (held != m_heldState.end()) {
            result.releasedState.emplace_back(mqttId, held->second);
            m_heldState.erase(held);
        }

        // What the list says about reachability: the availability field when
        // present, else last_seen against the stale threshold.
        if (const ChannelBinding *availability = entry.availabilityBinding()) {
            v1::ConnectivityStatus status = availabilityStatus(jsonValue(obj, "availability"));
            if (status == v1::ConnectivityStatus::Unknown)
                status = availabilityStatus(jsonValue(obj, "availability_state"));
            if (status == v1::ConnectivityStatus::Unknown) {
                const std::int64_t seenMs = lastSeenMs(jsonValue(obj, "last_seen"));
                if (seenMs > 0) {
                    status = (nowMs - seenMs) > kStaleAfterMs ? v1::ConnectivityStatus::Disconnected
                                                              : v1::ConnectivityStatus::Connected;
                }
            }
            if (status != v1::ConnectivityStatus::Unknown) {
                result.connectivity.push_back({entry.device.externalId, availability->channelId,
                                               status});
            }
        }
    }

    if (fullList) {
        for (auto it = m_byMqttId.begin(); it != m_byMqttId.end();) {
            if (seen.count(it->first)) {
                ++it;
                continue;
            }
            unindex(it->second);
            if (!it->second.device.externalId.empty())
                result.removed.push_back(it->second.device.externalId);
            it = m_byMqttId.erase(it);
        }
    }
    return result;
}

void DeviceTable::holdState(const std::string &mqttId, const Json &payload)
{
    m_heldState[mqttId] = payload;
}

std::string DeviceTable::remove(std::string_view mqttId)
{
    const auto it = m_byMqttId.find(std::string(mqttId));
    if (it == m_byMqttId.end())
        return {};
    const std::string externalId = it->second.device.externalId;
    unindex(it->second);
    m_byMqttId.erase(it);
    m_heldState.erase(std::string(mqttId));
    return externalId;
}

void DeviceTable::clear()
{
    m_byMqttId.clear();
    m_mqttByExternal.clear();
    m_heldState.clear();
    m_coordinatorExternalId.clear();
}

} // namespace phicore::z2m::ipc
