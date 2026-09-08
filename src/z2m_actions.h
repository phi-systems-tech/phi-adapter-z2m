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
#include <optional>
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
 * Either a single click or a double click, never both. In the moment a
 * button is released it cannot be known whether a second release is on its
 * way, so the release is held for kMultiPressWindowMs: if nothing follows it
 * is reported as a single click when the window closes, if a second one
 * follows they are reported as a double when the window closes, a third as
 * a triple. Half a second, because that is the upper bound operating
 * systems use for a double click; the 1.3 seconds this used to wait made
 * the most common thing a button does the slowest thing the adapter did.
 *
 * The machine has no clock of its own. It says when a window is due and the
 * caller arms a timer for it and calls onWindowClosed() when it fires - which
 * is what makes a double click playable in a test in no time at all.
 *
 * A long press that repeats within kLongPressRepeatWindowMs of the previous
 * one is reported as a repeat. A device that counts for itself ("double" in
 * the action) is believed as it is.
 */
class ButtonPresses
{
public:
    static constexpr std::int64_t kMultiPressWindowMs = 500;
    static constexpr std::int64_t kLongPressRepeatWindowMs = 800;

    struct Report {
        phicore::adapter::v1::ButtonEventCode code;
        std::int64_t tsMs;
    };

    struct Outcome {
        /// What to report now, in order.
        std::vector<Report> report;
        /// When the caller has to come back with onWindowClosed(), if at all.
        std::optional<std::int64_t> windowUntilMs;
        /// The pending window, if any, is void.
        bool cancelWindow = false;
    };

    Outcome onEvent(const std::string &key, phicore::adapter::v1::ButtonEventCode code,
                    std::int64_t tsMs);

    /// The window for `key` has passed: what the held releases amount to.
    std::vector<Report> onWindowClosed(const std::string &key);

    void forget(const std::string &key) { m_keys.erase(key); }
    void clear() { m_keys.clear(); }

private:
    struct Memory {
        int lastCode = 0;
        std::int64_t lastTs = 0;
        int releases = 0;
        std::int64_t lastReleaseTs = 0;
    };
    std::vector<Report> flush(Memory &memory);

    std::map<std::string, Memory> m_keys;
};

} // namespace phicore::z2m::ipc
