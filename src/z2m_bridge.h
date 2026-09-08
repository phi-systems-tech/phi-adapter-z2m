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

#include "phi/adapter/v1/types.h"

#include "z2m_exposes.h"
#include "z2m_json.h"

namespace phicore::z2m::ipc {

/// The meta patch `bridge/info` turns into: version, commit, channel, PAN ids,
/// serial port and adapter, coordinator type and firmware, permit-join state.
Json bridgeFactsPatch(const Json &info);

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
