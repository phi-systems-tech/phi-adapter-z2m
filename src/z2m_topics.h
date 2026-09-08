#pragma once

// Where an MQTT topic under the Zigbee2MQTT base topic points.
//
// Pure: no broker, no state beyond what the caller passes in. A friendly name
// may contain a slash ("living room/lamp" is a legal name and a legal topic),
// so a device's topics are recognised by matching against the names the
// adapter already knows before any guess about the last path segment.

#include <functional>
#include <string>
#include <string_view>

namespace phicore::z2m::ipc {

/// Trimmed, no trailing slash, "zigbee2mqtt" when empty.
std::string normalizedBaseTopic(std::string_view raw);

struct TopicParts {
    /// False when the topic is not under the base at all.
    bool underBase = false;
    /// Everything after "<base>/".
    std::string suffix;
    /// True for "<base>/bridge/...", with `bridgePath` the part after it.
    bool bridge = false;
    std::string bridgePath;
};

TopicParts splitTopic(std::string_view baseTopic, std::string_view topic);

enum class DeviceTopic {
    /// "<base>/<device>": the device's state, or one of its actions.
    State,
    /// "<base>/<device>/availability".
    Availability,
    /// "<base>/<device>/set" or ".../get": our own or somebody's command,
    /// echoed by the broker because we subscribed to everything.
    Echo,
    /// A sub-topic this adapter does not read.
    Other,
};

struct DeviceTopicRef {
    DeviceTopic kind = DeviceTopic::Other;
    std::string device;
};

/**
 * @brief Classifies a non-bridge suffix.
 *
 * `isKnownDevice` is asked first, longest candidate first, so a device named
 * "a/b" claims "a/b/availability" before the fallback would read it as a
 * sub-topic of "a". A suffix nobody knows is read as a state topic when it
 * has no slash - a device whose description has not arrived yet - and as
 * nothing otherwise.
 */
DeviceTopicRef classifyDeviceTopic(std::string_view suffix,
                                   const std::function<bool(std::string_view)> &isKnownDevice);

} // namespace phicore::z2m::ipc
