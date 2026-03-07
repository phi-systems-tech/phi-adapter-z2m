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

- MQTT connection management
- Bridge/device topic synchronization
- Logging category `phi-core.adapters.z2m`

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
- Qt6 modules: `Core`, `Network`
- `libmosquitto-dev`
- `phi-adapter-sdk` (local checkout or installed package)
- `phi-adapter-sdk-qt` (local checkout or installed package)

### Configuration

- Static adapter config: `z2m-config.json`
- MQTT host/credentials/topics are configured through phi-core

### Build

```bash
cmake -S . -B ../build/phi-adapter-z2m/release-ninja -G Ninja
cmake --build ../build/phi-adapter-z2m/release-ninja --parallel
```

### Installation

- Build output: `../build/phi-adapter-z2m/release-ninja/plugins/adapters/phi_adapter_z2m_ipc`
- Deploy to: `/opt/phi/plugins/adapters/`

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
