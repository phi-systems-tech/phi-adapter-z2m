#pragma once

// A zigbee2mqtt device description, and the channels it turns into.
//
// This is most of what the adapter is. It runs before anything touches a
// broker, and it is where a silent mistake costs an operator a channel they
// cannot control - which is why it is pure and tested on its own.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/device.h"

#include "z2m_json.h"

namespace phicore::z2m::ipc {

/// How a contract channel maps onto a zigbee2mqtt property, both ways.
struct ChannelBinding {
    std::string channelId;
    std::string property;
    phicore::adapter::v1::ChannelKind kind = phicore::adapter::v1::ChannelKind::Unknown;
    phicore::adapter::v1::ChannelDataType dataType = phicore::adapter::v1::ChannelDataType::Unknown;
    phicore::adapter::v1::ChannelFlags flags = phicore::adapter::v1::ChannelFlag::None;
    std::string unit;
    double rawMin = 0.0;
    double rawMax = 0.0;
    double rawStep = 0.0;
    double valueScale = 1.0;
    std::string endpoint;
    std::string valueOn;
    std::string valueOff;
    /// "xy" or "hs": which colour model the device speaks.
    std::string colorMode;
    bool scalePercent = false;
    bool isAvailability = false;
    bool isUpdate = false;
    /// zigbee2mqtt will answer a `get` for this property (access bit 2).
    bool gettable = false;
    int actionButtonId = 0;
    bool actionIsDial = false;
    std::map<std::string, int> enumRawToValue;
    std::map<int, std::string> enumValueToRaw;
};

/// A device as this adapter knows it.
struct DeviceEntry {
    phicore::adapter::v1::Device device;
    /// The device meta as an object; `device.metaJson` is derived from it.
    Json meta = Json::object();
    /// zigbee2mqtt's friendly name, which is also its topic.
    std::string mqttId;
    std::string ieee;
    /// A digest of the definition's exposes. When it changes, the channels
    /// have to be rebuilt; when it does not, they are left alone.
    std::string definitionKey;
    bool coordinator = false;
    /// The last_seen that was last announced with the device, so a device
    /// that reports every few seconds is not re-announced every few seconds.
    std::int64_t lastSeenAnnouncedMs = 0;
    phicore::adapter::v1::ChannelList channels;
    std::map<std::string, ChannelBinding> bindings;

    [[nodiscard]] const ChannelBinding *binding(std::string_view channelId) const;
    [[nodiscard]] const ChannelBinding *availabilityBinding() const;
    [[nodiscard]] const ChannelBinding *updateBinding() const;
    /// The device with its meta serialised, ready to send.
    [[nodiscard]] phicore::adapter::v1::Device forWire() const;
};

/// Which properties are not turned into channels. From the static config.
struct ExposeFilter {
    std::vector<std::string> suppressedPrefixes;
    std::map<std::string, std::vector<std::string>> suppressedByModel;
    std::map<std::string, std::vector<std::string>> suppressedByModelId;
    std::map<std::string, std::vector<std::string>> allowedByModel;
    std::map<std::string, std::vector<std::string>> allowedByModelId;

    static ExposeFilter fromStaticConfig(const Json &config);
    [[nodiscard]] bool suppressed(std::string_view property, std::string_view model,
                                  std::string_view modelId) const;
};

/// The digest `DeviceEntry::definitionKey` holds, for a device object.
std::string definitionKeyOf(const Json &deviceObj);

/// One entry in `bridge/devices` into a device with channels.
DeviceEntry buildDeviceEntry(const Json &deviceObj, const ExposeFilter &filter);

/// Everything `exposes` describes, flattened: a composite's features become
/// entries of their own, except colour, which stays one channel.
std::vector<Json> collectExposes(const Json &exposes);

} // namespace phicore::z2m::ipc
