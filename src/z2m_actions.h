#pragma once

// Zigbee2MQTT's `action` vocabulary, and what a button press becomes.
//
// A remote publishes strings - "on_press_release", "button_2_hold",
// "brightness_step_up", "rotate_left" - and this adapter turns them into the
// contract's button events and rotation steps. Everything here is pure; the
// press machine itself is the SDK's ButtonPresses, shared with every adapter, so a
// test can play a double click in no time at all.

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "phi/adapter/v1/types.h"

#include "z2m_json.h"

namespace phicore::z2m::ipc {

/// Lower-cased alphanumeric runs of `text`; everything else separates.
std::vector<std::string> actionTokens(std::string_view text);

/// The button number an action names ("button_2_single", "2_double"), 1..16;
/// 0 when it names none.
int buttonIdFromAction(std::string_view action);

/// Every button number the listed actions name between them.
std::set<int> buttonIdsFromActions(const std::vector<std::string> &actions);

/// Whether the action is a dial or step rather than a press.
bool isDialAction(std::string_view action);

/// How far a dial action turned: from a step field in the payload when there
/// is one, else from a number in the action, else 1. Zero when the payload
/// carries a step field that says nothing.
int dialMagnitude(std::string_view action, const Json &payload);

/// -1 for left/counter-clockwise/down, +1 for right/clockwise/up, 0 unknown.
/// Zigbee2MQTT's `action_direction` field (1 = down, 2 = up) counts too.
int dialDirection(std::string_view action, const Json &payload);

/// The contract's word for an action or an `action_type` string.
phicore::adapter::v1::ButtonEventCode buttonEventFor(std::string_view text);

} // namespace phicore::z2m::ipc
