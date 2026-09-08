#pragma once

// One Zigbee2MQTT instance: a broker connection, the bridge behind it, and
// the devices behind that. Everything that talks to phi-core is in here;
// everything that can be decided without a broker is in the modules it uses.

#include <memory>
#include <string>

#include "phi/adapter/sdk/sidecar.h"

#include "z2m_json.h"
#include "z2m_mqtt_client.h"

namespace phicore::z2m::ipc {

/// The broker connection an adapter configuration asks for. Pure, so a test
/// can check that an account or a TLS answer given to the adapter is what
/// reaches the socket - a half-applied configuration is silent, and an
/// adapter with no credentials looks exactly like one whose broker does not
/// ask for any, right up until the broker starts asking.
MqttSettings mqttSettingsFor(const phicore::adapter::v1::Adapter &adapter, const Json &meta);

std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance();

} // namespace phicore::z2m::ipc
