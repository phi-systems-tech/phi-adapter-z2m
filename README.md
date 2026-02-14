# phi-adapter-z2m

## Overview

Integrates Zigbee2MQTT via MQTT with phi-core.

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

### Runtime Requirements

- phi-core with plugin loading enabled
- MQTT broker (for example Mosquitto)
- Zigbee2MQTT instance

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `libmosquitto-dev`
- `phi-adapter-api` (local checkout or installed package)

### Configuration

- No dedicated config file in this repository
- MQTT host/credentials/topics are configured through phi-core

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/libphi_adapter_z2m.so`
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
