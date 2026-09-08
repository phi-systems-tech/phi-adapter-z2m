#pragma once

// Zigbee2MQTT's `action` vocabulary, and what a button press becomes.
//
// A remote publishes strings - "on_press_release", "button_2_hold",
// "brightness_step_up", "rotate_left" - and this adapter turns them into the
// contract's button events and rotation steps. Everything here is pure; the
// one thing with memory, ButtonPresses, takes its clock from the caller so a
// test can play a double click in no time at all.

#include <cstdint>
#include <map>
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

/**
 * @brief What a stream of press events becomes for one button.
 *
 * A single click is reported the moment it happens. The old version held it
 * back for 1.3 seconds in case a second click followed, which made the most
 * common thing a button does the slowest. Now a second release within
 * kMultiPressWindowMs is reported *in addition* as a double press, a third
 * as a triple, and so on - the click itself is never delayed. A long press
 * that repeats within kLongPressRepeatWindowMs of the previous one is
 * reported as a repeat.
 */
class ButtonPresses
{
public:
    static constexpr std::int64_t kMultiPressWindowMs = 500;
    static constexpr std::int64_t kLongPressRepeatWindowMs = 800;

    /// The codes to report now, in order, for `code` arriving at `tsMs`.
    std::vector<phicore::adapter::v1::ButtonEventCode> onEvent(
        const std::string &key, phicore::adapter::v1::ButtonEventCode code, std::int64_t tsMs);

    void forget(const std::string &key) { m_keys.erase(key); }
    void clear() { m_keys.clear(); }

private:
    struct Memory {
        int lastCode = 0;
        std::int64_t lastTs = 0;
        int releases = 0;
        std::int64_t lastReleaseTs = 0;
    };
    std::map<std::string, Memory> m_keys;
};

} // namespace phicore::z2m::ipc
