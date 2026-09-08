# phi-adapter-z2m

## Overview

Integrates Zigbee2MQTT via MQTT with phi-core via IPC sidecar.

## Supported Devices / Systems

- Zigbee2MQTT deployments reachable through MQTT
- Devices supported by your Zigbee2MQTT environment

## Cloud Functionality

- Cloud required: `no`
- Local MQTT integration

## Known Issues

- Runtime behavior depends on Zigbee2MQTT topic conventions and broker setup.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provides MQTT bridge integration between Zigbee2MQTT and phi-core.

### Features

- MQTT connection management, on the adapter's own loop
- Bridge/device topic synchronization
- Logging category `phi-core.adapters.z2m`

### Layout

One concern per translation unit; everything above the broker is pure and
tested without one.

| File | What it decides |
|---|---|
| `z2m_mqtt_client` | libmosquitto driven by the phi-runtime loop: one thread, a connect deadline, a reason for every disconnect |
| `z2m_topics` | where a topic under the base points, including friendly names with slashes |
| `z2m_exposes` | a `bridge/devices` entry into a device with channels and bindings |
| `z2m_state` | a state payload into channel values, a channel command into a `set` payload |
| `z2m_actions` | the `action` vocabulary; the press machine itself is the SDK's `ButtonPresses` |
| `z2m_bridge` | `bridge/info` facts, the health rule, and the device table the list is diffed against |
| `z2m_instance` | the AdapterInstance: everything that talks to phi-core |
| `z2m_probe` | the factory's "Test connection" |
| `z2m_schema` | name, icon, capabilities, configuration form |

### How a button press is reported

Through the SDK's `ButtonPresses` (`phi/adapter/sdk/button_presses.h`), the
same machine every adapter uses: either a single click or a double click,
never both, decided 500 ms after the release. A hold ends the window at once.
Devices that count for themselves ("double", "triple" in the action) are
believed as they are, without a window.

### What this adapter changes in Zigbee2MQTT

Once per connection, after the first `bridge/state online`, the adapter asks
Zigbee2MQTT to set `advanced.last_seen` to `epoch` (through
`bridge/request/options`), and logs that it did. Zigbee2MQTT writes the option
to its `configuration.yaml`. With it every state message carries when the
device last spoke, which is what device reachability is judged from; without
it a retained state replayed by the broker looks like a device that spoke just
now.

### How reachability is decided

An instance counts as reachable only while a live Zigbee2MQTT has answered
`bridge/request/health_check` on `bridge/response/health_check`. Responses are
published without retain, so an answer can only have come from a process that
was running at that moment.

`bridge/state` is not enough on its own. It is retained and carries no date, so
the broker replays whatever an instance last announced to every subscriber that
turns up afterwards - including an `online` from a run days earlier. A will
corrects that only for a client that was connected when it died; an instance
that has not connected in this broker's lifetime leaves its last word standing,
and mosquitto restores it from persistence across its own restarts. An
`offline` still counts on sight: nobody publishes their own absence by mistake.

The probe runs on every MQTT connect, on every `online` the broker replays, and
once a minute after that. Two unanswered probes, not one, bring the link down.

This needs Zigbee2MQTT 2.x, where `health_check` exists as a bridge request.

### Adapter-Dev Guideline: Enum Mapping

Use this rule for all adapter implementations:

- Numeric enum values:
  - Keep the real numeric values from the device/protocol (do not normalize to `1..N`).
  - Example: if a device exposes `["10","30","60"]`, the channel values must be `10`, `30`, `60`.
- Textual enum values:
  - Map to stable internal values only when needed for cross-device normalization.
  - Preserve raw text mapping metadata so write-back can send the original raw value.

Reason:
- Keeps UI preselection and automations aligned with real device semantics.
- Avoids ambiguity when different devices expose different numeric ranges.

### Runtime Requirements

- phi-core with IPC adapter runtime enabled
- MQTT broker (for example Mosquitto)
- Zigbee2MQTT instance

### Build Requirements

- `cmake`
- `libmosquitto-dev`
- `nlohmann-json3-dev`
- `phi-adapter-sdk` >= 0.12.0 (local checkout or installed package), which
  brings `phi-runtime`

No Qt.

### Configuration

- Static adapter config: `z2m-config.json`
- MQTT host/credentials/topics are configured through phi-core

### Build

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel
ctest --test-dir build
```

The MQTT client test runs against a broker of its own (a few hundred lines
that speak enough MQTT 3.1.1 to accept, refuse, echo and go quiet), so no
broker has to be installed to run the suite.

### Installation

- Build output: `build/plugins/adapters/phi_adapter_z2m_ipc`
- Installed by the Debian package to `/usr/lib/<multiarch>/phi/plugins/adapters/`

### Troubleshooting

- Error: `libmosquitto not found` during configure
- Cause: missing development package
- Fix: install `libmosquitto-dev`

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-z2m/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-z2m/releases
- https://github.com/phi-systems-tech/phi-adapter-z2m/tags
