#pragma once

// Values both ways: what a zigbee2mqtt state payload says about a channel,
// and what a channel command becomes on the wire. Pure.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "phi/adapter/v1/color.h"
#include "phi/adapter/v1/types.h"
#include "phi/adapter/v1/value.h"

#include "z2m_exposes.h"
#include "z2m_json.h"

namespace phicore::z2m::ipc {

/// One channel's new value, in whichever shape the contract carries it.
struct Reading {
    enum class Shape { Scalar, Color, Object };
    Shape shape = Shape::Scalar;
    phicore::adapter::v1::ScalarValue scalar;
    phicore::adapter::v1::Color color;
    phicore::adapter::v1::ChannelValueFields fields;
};

/// The reading for a non-action binding from the property's JSON value, or
/// nothing when the value does not translate.
std::optional<Reading> readingFor(const ChannelBinding &binding, const Json &value);

/// The firmware-update object as fields: status, currentVersion, targetVersion.
/// Zigbee2MQTT's `update` object on a device, or the `update` block of
/// `bridge/info` for the coordinator (which carries only a version).
std::optional<Reading> updateReading(const Json &update, bool coordinator);

/// What a command arrives as from core: a scalar, or JSON text for anything
/// richer (a colour is an object with r, g, b).
struct CommandValue {
    phicore::adapter::v1::ScalarValue scalar;
    Json json;

    [[nodiscard]] bool toBool() const;
    [[nodiscard]] double toDouble() const;
    [[nodiscard]] int toInt() const;
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] bool isNumber() const;
    /// r, g, b in 0..1 or 0..255, as an object or a Color.
    [[nodiscard]] std::optional<phicore::adapter::v1::Color> toColor() const;
};

/// The `set` payload for a command. False with `error` when the value cannot
/// be expressed for this binding.
bool commandPayload(const ChannelBinding &binding, const CommandValue &value, Json &payload,
                    std::string &error);

double scaleToPercent(double raw, double rawMin, double rawMax);
double scaleFromPercent(double percent, double rawMin, double rawMax);

/// Milliseconds since the epoch from a `last_seen` value: seconds, milliseconds
/// or ISO 8601 text. Zero when it says nothing.
std::int64_t lastSeenMs(const Json &value);

/// Connected/Disconnected for "online"/"offline" (or an object with `state`);
/// Unknown otherwise.
phicore::adapter::v1::ConnectivityStatus availabilityStatus(const Json &value);

/// The stale threshold applied to `last_seen`.
constexpr std::int64_t kStaleAfterMs = 5 * 60 * 1000;

} // namespace phicore::z2m::ipc
