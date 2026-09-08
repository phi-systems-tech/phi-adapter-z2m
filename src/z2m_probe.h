#pragma once

// "Test connection": does anything accept TCP at the broker address.
//
// Not an MQTT exchange, because the dialog it belongs to is shown before the
// credentials are necessarily right, and "the broker is there but said no to
// this password" is what connecting will report in words anyway.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "z2m_json.h"

namespace phicore::z2m::ipc {

struct ProbeTarget {
    std::string host;
    std::uint16_t port = 1883;
};

/// The form's own values win over the discovered candidate under
/// `factoryAdapter`, because the form is what the person just typed.
ProbeTarget probeTargetFromParams(const Json &params);

/**
 * @brief One connect attempt, bounded, cancellable.
 *
 * Runs on the factory thread and waits there in short slices, asking
 * `cancelled` between them so a shutdown never sits out the whole budget.
 */
bool probeEndpoint(const ProbeTarget &target,
                   std::chrono::milliseconds timeout,
                   const std::function<bool()> &cancelled,
                   std::string *errorMessage);

} // namespace phicore::z2m::ipc
