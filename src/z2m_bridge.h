#pragma once

// What Zigbee2MQTT says about itself and its devices, kept and diffed.
//
// The bridge topics: `bridge/info` (facts about the radio), `bridge/devices`
// (every device, republished whole on any change), `bridge/state` and the
// health check that decides whether anyone is home. Pure - the instance
// feeds these from MQTT and sends what they return.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "phi/adapter/v1/group.h"
#include "phi/adapter/v1/types.h"

#include "z2m_exposes.h"
#include "z2m_json.h"

namespace phicore::z2m::ipc {

/// The meta patch `bridge/info` turns into: version, commit, active channel,
/// PAN ids, serial port and adapter, coordinator type and firmware, permit-join
/// state. Facts a client shows, scalars only.
phicore::adapter::v1::AdapterFormValues bridgeFactsPatch(const Json &info);

/// The channel the network runs on (`network.channel`), 0 when not reported.
int activeChannelOf(const Json &info);
/// The channel Zigbee2MQTT is configured for (`config.advanced.channel`): the
/// one it moves the network to at its next start.
int configuredChannelOf(const Json &info);
/// `config.serial.adapter`: ember, zstack, deconz, zigate, zboss.
std::string serialAdapterOf(const Json &info);
/// Whether Zigbee2MQTT moves paired devices along when the channel changes.
/// zigbee-herdsman does it for ember and zstack; on the other drivers every
/// device would have to be paired again.
bool movesDevicesToAnotherChannel(std::string_view serialAdapter);

/// What `bridge/info` adds to the coordinator's device entry: manufacturer,
/// model, firmware, and the serial port and adapter in its meta.
void applyBridgeInfoToCoordinator(const Json &info, DeviceEntry &coordinator);

/**
 * @brief Whether a live Zigbee2MQTT is there.
 *
 * `bridge/state` is retained, so the broker replays whatever Zigbee2MQTT last
 * announced to every subscriber that turns up afterwards - including a months
 * old "online" from an instance whose backend has not run since. A will only
 * corrects that for a client that was connected when it died; one that was
 * never connected in this broker's lifetime leaves its last word standing
 * forever. `bridge/request/health_check` is the other half: a request whose
 * answer is published without retain, so receiving one is proof that a live
 * process was there to send it. That is what this adapter believes.
 *
 * Two silences, not one, bring the bridge down: Zigbee2MQTT answers on its
 * event loop, and that loop has real work on it.
 */
class BridgeHealth
{
public:
    static constexpr int kMissesBeforeOffline = 2;

    [[nodiscard]] bool online() const { return m_online; }
    [[nodiscard]] bool probeInFlight() const { return !m_transaction.empty(); }
    [[nodiscard]] const std::string &transaction() const { return m_transaction; }

    /// False when a probe is already unanswered: a second question would be
    /// answered by the reply to the first, or turn one silence into two.
    bool beginProbe(std::string transaction);
    /// Any answer counts, not only the one to our own question. True when
    /// this brought the bridge online.
    bool answered();
    /// True when this silence took the bridge offline.
    bool probeTimedOut();
    /// Believed without asking: nobody publishes their own absence by mistake.
    void saidOffline();
    /// The MQTT link dropped; nothing observed since.
    void reset();

private:
    bool m_online = false;
    int m_misses = 0;
    std::string m_transaction;
};

struct ConnectivityReport {
    std::string externalId;
    std::string channelId;
    phicore::adapter::v1::ConnectivityStatus status;
};

/// What applying a `bridge/devices` list changed.
struct SnapshotResult {
    /// Devices to (re)announce, by mqtt id: new, rebuilt, or renamed.
    std::vector<std::string> announce;
    /// External ids of devices that are gone.
    std::vector<std::string> removed;
    /// Renames seen: ieee address and the new friendly name.
    std::vector<std::pair<std::string, std::string>> renamed;
    /// What the list says about each device's reachability.
    std::vector<ConnectivityReport> connectivity;
    /// Devices whose state arrived before their description, now deliverable.
    std::vector<std::pair<std::string, Json>> releasedState;
};

/// A Zigbee2MQTT group as this adapter knows it.
///
/// A group is a Zigbee group address: a set of endpoints that answer to one
/// broadcast, which is why switching twelve bulbs through one is one radio
/// frame rather than twelve. Zigbee2MQTT gives it a numeric `id` that never
/// changes and a `friendly_name` that a person can rename at will, so the id
/// is what we carry upward as the external id and the friendly name is the
/// name.
///
/// It is a group and never a room: Zigbee2MQTT has no notion of a place, and
/// calling one a room would invent a fact the vendor never stated. Hue draws
/// the same line - its rooms are rooms, its zones are groups.
struct GroupEntry {
    phicore::adapter::v1::Group group;
    /// The group meta as an object; `group.metaJson` is derived from it.
    Json meta = Json::object();
    /// Zigbee2MQTT's numeric id, which is also the external id as text.
    int id = 0;
    /// What the members look like to Zigbee2MQTT, kept because our own
    /// membership list cannot hold it - see applyGroupSnapshot.
    std::vector<std::pair<std::string, int>> members;

    /// The group with its meta serialised, ready to send.
    [[nodiscard]] phicore::adapter::v1::Group forWire() const;
};

/// What applying a `bridge/groups` list changed. Zigbee2MQTT republishes the
/// whole retained array whenever anything about any group changes, so this is
/// diffed for the same reason the device list is: an unchanged group is not
/// news, and announcing all of them on every membership change would be.
struct GroupSnapshotResult {
    /// External ids of groups to (re)announce: new, renamed, or re-membered.
    std::vector<std::string> announce;
    /// External ids of groups that are gone.
    std::vector<std::string> removed;
    /// Renames seen: external id and the new friendly name.
    std::vector<std::pair<std::string, std::string>> renamed;
};

/**
 * @brief The groups this adapter knows, keyed by their external id.
 *
 * Keyed by the numeric id rather than the name, because that is the half a
 * rename leaves alone.
 */
class GroupTable
{
public:
    [[nodiscard]] const GroupEntry *byExternalId(std::string_view externalId) const;
    [[nodiscard]] const std::map<std::string, GroupEntry> &all() const { return m_byExternalId; }

    /// Diffs a `bridge/groups` payload against what is known.
    GroupSnapshotResult applySnapshot(const Json &groups);

    void clear();

private:
    std::map<std::string, GroupEntry> m_byExternalId;
};

/**
 * @brief The devices this adapter knows, keyed by friendly name.
 *
 * A snapshot is diffed against it rather than replacing it: a device whose
 * definition has not changed keeps its entry and is not re-announced, one
 * whose definition changed is rebuilt, one that was renamed moves under its
 * new name, and one that is missing from a full list is dropped.
 */
class DeviceTable
{
public:
    [[nodiscard]] const DeviceEntry *byMqttId(std::string_view mqttId) const;
    [[nodiscard]] DeviceEntry *byMqttId(std::string_view mqttId);
    [[nodiscard]] const DeviceEntry *byExternalId(std::string_view externalId) const;
    [[nodiscard]] DeviceEntry *byExternalId(std::string_view externalId);
    [[nodiscard]] std::string mqttIdFor(std::string_view externalId) const;
    [[nodiscard]] bool knows(std::string_view mqttId) const { return m_byMqttId.count(std::string(mqttId)) > 0; }
    [[nodiscard]] const std::map<std::string, DeviceEntry> &all() const { return m_byMqttId; }
    [[nodiscard]] const std::string &coordinatorExternalId() const { return m_coordinatorExternalId; }

    /// `nowMs` decides whether a `last_seen` is stale.
    SnapshotResult applySnapshot(const Json &devices, bool fullList, const ExposeFilter &filter,
                                 std::int64_t nowMs);

    /// A state payload for a device not yet described; delivered with the
    /// description when it arrives.
    void holdState(const std::string &mqttId, const Json &payload);

    /// Removes by mqtt id; the external id that went with it, or empty.
    std::string remove(std::string_view mqttId);
    void clear();

private:
    void index(const DeviceEntry &entry);
    void unindex(const DeviceEntry &entry);

    std::map<std::string, DeviceEntry> m_byMqttId;
    std::map<std::string, std::string> m_mqttByExternal;
    std::map<std::string, Json> m_heldState;
    std::string m_coordinatorExternalId;
};

} // namespace phicore::z2m::ipc
