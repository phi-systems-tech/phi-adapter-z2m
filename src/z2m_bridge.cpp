#include "z2m_bridge.h"

#include <algorithm>
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

v1::AdapterFormValues bridgeFactsPatch(const Json &info)
{
    const Json coordinator = jsonValue(info, "coordinator");
    const Json coordinatorMeta = jsonValue(coordinator, "meta");
    const Json serial = jsonValue(jsonValue(info, "config"), "serial");
    const Json network = jsonValue(info, "network");

    v1::AdapterFormValues facts;
    const auto text = [&facts](const char *key, const std::string &value) {
        if (!value.empty())
            facts.push_back({key, v1::ScalarValue(value)});
    };

    text("z2mVersion", jsonString(info, "version", false));
    text("z2mCommit", jsonString(info, "commit", false));
    if (const Json permitJoin = jsonValue(info, "permit_join"); permitJoin.is_boolean())
        facts.push_back({"permitJoin", v1::ScalarValue(permitJoin.get<bool>())});
    if (const Json channel = jsonValue(network, "channel"); channel.is_number_integer())
        facts.push_back({"zigbeeChannel", v1::ScalarValue(channel.get<std::int64_t>())});
    text("panId", str::trimmed(jsonScalarText(jsonValue(network, "pan_id"))));
    text("extPanId", str::trimmed(jsonScalarText(jsonValue(network, "extended_pan_id"))));
    text("serialPort", jsonString(serial, "port"));
    // Zigbee2MQTT's own word for the driver it opened the stick with: `ember`,
    // `zstack`, `deconz`. The host layer names the same thing a radio family,
    // and the two agreeing is worth being able to see rather than assume.
    text("serialAdapter", serialAdapterOf(info));
    text("coordinatorType", jsonString(coordinator, "type"));
    text("coordinatorFirmware", coordinatorFirmware(coordinatorMeta));
    return facts;
}

int activeChannelOf(const Json &info)
{
    return jsonInt(jsonValue(info, "network"), "channel", 0);
}

int configuredChannelOf(const Json &info)
{
    return jsonInt(jsonValue(jsonValue(info, "config"), "advanced"), "channel", 0);
}

std::string serialAdapterOf(const Json &info)
{
    return jsonString(jsonValue(jsonValue(info, "config"), "serial"), "adapter");
}

bool movesDevicesToAnotherChannel(std::string_view serialAdapter)
{
    return serialAdapter == "ember" || serialAdapter == "zstack";
}

void applyBridgeInfoToCoordinator(const Json &info, DeviceEntry &entry)
{
    const Json coordinator = jsonValue(info, "coordinator");
    const Json coordinatorMeta = jsonValue(coordinator, "meta");
    const std::string manufacturer = jsonString(coordinatorMeta, "manufacturer", false);
    if (!manufacturer.empty())
        entry.device.setManufacturer(manufacturer);
    const std::string model = jsonString(coordinatorMeta, "model", false);
    if (!model.empty())
        entry.device.setModel(model);
    const std::string firmware = coordinatorFirmware(coordinatorMeta);
    if (!firmware.empty())
        entry.device.setFirmware(firmware);
    entry.device.setDeviceClass(v1::DeviceClass::Gateway);
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
    if (!entry.device.externalId().empty())
        m_mqttByExternal[entry.device.externalId()] = entry.mqttId;
    if (entry.coordinator)
        m_coordinatorExternalId = entry.device.externalId();
}

void DeviceTable::unindex(const DeviceEntry &entry)
{
    if (!entry.device.externalId().empty()) {
        const auto it = m_mqttByExternal.find(entry.device.externalId());
        if (it != m_mqttByExternal.end() && it->second == entry.mqttId)
            m_mqttByExternal.erase(it);
    }
    if (entry.coordinator && m_coordinatorExternalId == entry.device.externalId())
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
                entry.device.setName(mqttId);
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
                result.connectivity.push_back({entry.device.externalId(), availability->channelId,
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
            if (!it->second.device.externalId().empty())
                result.removed.push_back(it->second.device.externalId());
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
    const std::string externalId = it->second.device.externalId();
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

// ---------------------------------------------------------------- groups

phicore::adapter::v1::Group GroupEntry::forWire() const
{
    phicore::adapter::v1::Group out = group;
    out.setMetaJson(meta.empty() ? std::string() : dump(meta));
    return out;
}

const GroupEntry *GroupTable::byExternalId(std::string_view externalId) const
{
    const auto it = m_byExternalId.find(std::string(externalId));
    return it == m_byExternalId.end() ? nullptr : &it->second;
}

GroupSnapshotResult GroupTable::applySnapshot(const Json &groups)
{
    GroupSnapshotResult result;
    if (!groups.is_array())
        return result;

    std::map<std::string, GroupEntry> next;
    for (const Json &raw : groups) {
        if (!raw.is_object())
            continue;
        const Json idValue = jsonValue(raw, "id");
        if (!idValue.is_number_integer())
            continue;

        GroupEntry entry;
        entry.id = idValue.get<int>();
        // The id, not the name: a rename changes the friendly name and leaves
        // the id alone, so this is the half that can identify a group across
        // one. The same reason a device is keyed by its ieee address.
        entry.group.setExternalId(std::to_string(entry.id));
        entry.group.setName(jsonString(raw, "friendly_name"));
        if (entry.group.name().empty())
            entry.group.setName(entry.group.externalId());

        // Zigbee2MQTT addresses a member as an endpoint of a device; our own
        // membership is per device, because that is what a household points
        // at. A device with two endpoints in one group is therefore listed
        // once, and what was lost - which endpoints - is kept in the meta, so
        // a reader can still see it and nothing has to be fetched again.
        Json memberMeta = Json::array();
        for (const Json &member : jsonValue(raw, "members")) {
            if (!member.is_object())
                continue;
            const std::string ieee = jsonString(member, "ieee_address");
            if (ieee.empty())
                continue;
            const Json endpointValue = jsonValue(member, "endpoint");
            const int endpoint = endpointValue.is_number_integer() ? endpointValue.get<int>() : 0;
            entry.members.emplace_back(ieee, endpoint);

            Json one = Json::object();
            one["ieee_address"] = ieee;
            one["endpoint"] = endpoint;
            memberMeta.push_back(one);

            const auto &ids = entry.group.deviceExternalIds();
            if (std::find(ids.begin(), ids.end(), ieee) == ids.end())
                entry.group.addDeviceExternalId(ieee);
        }

        entry.meta["id"] = entry.id;
        entry.meta["friendly_name"] = entry.group.name();
        entry.meta["members"] = memberMeta;
        next.insert_or_assign(entry.group.externalId(), std::move(entry));
    }

    for (const auto &[externalId, entry] : m_byExternalId) {
        if (!next.count(externalId))
            result.removed.push_back(externalId);
    }

    for (const auto &[externalId, entry] : next) {
        const auto previous = m_byExternalId.find(externalId);
        if (previous == m_byExternalId.end()) {
            result.announce.push_back(externalId);
            continue;
        }
        if (previous->second.group.name() != entry.group.name())
            result.renamed.emplace_back(externalId, entry.group.name());
        // Announced only where something a client can see actually changed.
        // The retained array is republished whole on any change to any group,
        // and an unchanged group going out again is what made the room events
        // in core noise before anyone looked.
        if (previous->second.group.name() != entry.group.name()
            || previous->second.group.deviceExternalIds() != entry.group.deviceExternalIds()
            || previous->second.members != entry.members) {
            result.announce.push_back(externalId);
        }
    }

    m_byExternalId = std::move(next);
    return result;
}

void GroupTable::clear()
{
    m_byExternalId.clear();
}

} // namespace phicore::z2m::ipc
