// A zigbee2mqtt device description, and the channels it turns into.
//
// What is worth testing in an adapter is this conversion: it is most of what
// the adapter is, it runs before anything touches a broker or a device, and it
// is where a silent mistake costs an operator a channel they cannot control.

#include <phi/adapter/testing/check.h>

#include "z2m_exposes.h"
#include "z2m_instance.h"
#include "z2m_json.h"
#include "z2m_schema.h"
#include "z2m_state.h"
#include "z2m_bridge.h"

#include "phi/adapter/v1/tlsconfig.h"

#include <set>
#include <string>

using namespace phicore::z2m::ipc;
namespace v1 = phicore::adapter::v1;

namespace {

const v1::Channel *channelById(const v1::ChannelList &channels, const std::string &id)
{
    for (const v1::Channel &channel : channels) {
        if (channel.externalId == id)
            return &channel;
    }
    return nullptr;
}

template <typename Enum>
bool has(Enum flags, Enum flag)
{
    return v1::hasFlag(flags, flag);
}

const char *kLamp = R"json({
  "friendly_name": "Kitchen lamp",
  "ieee_address": "0x00158d0001a2b3c4",
  "type": "Router",
  "power_source": "Mains (single phase)",
  "model_id": "LCT015",
  "definition": {
    "model": "9290012573A",
    "vendor": "Philips",
    "description": "Hue white and color ambiance E26/E27/E14",
    "exposes": [
      { "type": "light", "features": [
        { "type": "binary", "name": "state", "property": "state", "access": 7,
          "value_on": "ON", "value_off": "OFF", "value_toggle": "TOGGLE" },
        { "type": "numeric", "name": "brightness", "property": "brightness", "access": 7,
          "value_min": 0, "value_max": 254 },
        { "type": "numeric", "name": "color_temp", "property": "color_temp", "access": 7,
          "value_min": 153, "value_max": 500, "unit": "mired" },
        { "type": "composite", "name": "color_xy", "property": "color", "access": 7,
          "features": [
            { "type": "numeric", "name": "x", "property": "x", "access": 7 },
            { "type": "numeric", "name": "y", "property": "y", "access": 7 } ] }
      ] },
      { "type": "numeric", "name": "linkquality", "property": "linkquality", "access": 1,
        "value_min": 0, "value_max": 255, "unit": "lqi" }
    ]
  }
})json";

/// What zigbee2mqtt publishes on `bridge/info`, cut down to the parts that say
/// what the bridge is running on.
const char *kBridgeInfo = R"json({
  "version": "2.13.0",
  "commit": "abc1234",
  "permit_join": false,
  "log_level": "info",
  "coordinator": {
    "type": "EmberZNet",
    "ieee_address": "0x00124b0029ab1234",
    "meta": { "revision": "7.4.4.0", "maintrel": 0 }
  },
  "network": { "channel": 15, "pan_id": 6754, "extended_pan_id": "0xdddddddddddddddd" },
  "config": {
    "serial": { "port": "/dev/serial/by-id/usb-Itead_Sonoff-if00", "adapter": "ember" }
  }
})json";

void testALampBecomesALight()
{
    const DeviceEntry lamp = buildDeviceEntry(parseObject(kLamp), ExposeFilter{});

    PHI_CHECK(lamp.device.externalId == "0x00158d0001a2b3c4");
    PHI_CHECK(lamp.device.name == "Kitchen lamp");
    PHI_CHECK(lamp.mqttId == "Kitchen lamp");
    PHI_CHECK(lamp.device.deviceClass == v1::DeviceClass::Light);
    PHI_CHECK(lamp.device.manufacturer == "Philips");
    PHI_CHECK(lamp.device.model == "9290012573A");
    PHI_CHECK(has(lamp.device.flags, v1::DeviceFlag::Wireless));
    PHI_CHECK(!has(lamp.device.flags, v1::DeviceFlag::Battery));
    PHI_CHECK(jsonString(lamp.meta, "model_id") == "LCT015");
    PHI_CHECK(!lamp.definitionKey.empty());

    const v1::Channel *state = channelById(lamp.channels, "state");
    PHI_CHECK_MSG(state != nullptr, "the lamp has no state channel");
    if (state) {
        PHI_CHECK(state->kind == v1::ChannelKind::PowerOnOff);
        PHI_CHECK(state->dataType == v1::ChannelDataType::Bool);
        PHI_CHECK(has(state->flags, v1::ChannelFlag::Writable));
        PHI_CHECK(has(state->flags, v1::ChannelFlag::Readable));
    }
    const ChannelBinding *stateBinding = lamp.binding("state");
    PHI_CHECK(stateBinding != nullptr && stateBinding->valueOn == "ON"
              && stateBinding->valueOff == "OFF");
    // access 7 = state | set | get: the device answers a get for this.
    PHI_CHECK(stateBinding != nullptr && stateBinding->gettable);

    const v1::Channel *brightness = channelById(lamp.channels, "brightness");
    PHI_CHECK_MSG(brightness != nullptr, "the lamp has no brightness channel");
    if (brightness) {
        // Percent to the outside, 0..254 to the device.
        PHI_CHECK(brightness->kind == v1::ChannelKind::Brightness);
        PHI_CHECK(brightness->dataType == v1::ChannelDataType::Float);
        PHI_CHECK(brightness->minValue == 0.0);
        PHI_CHECK(brightness->maxValue == 100.0);
        PHI_CHECK(brightness->unit == "%");
        PHI_CHECK(has(brightness->flags, v1::ChannelFlag::Writable));
    }

    const v1::Channel *color = channelById(lamp.channels, "color");
    PHI_CHECK_MSG(color != nullptr, "the lamp has no colour channel");
    const ChannelBinding *colorBinding = lamp.binding("color");
    PHI_CHECK(colorBinding != nullptr && colorBinding->colorMode == "xy");
    // The composite's x and y are not channels of their own.
    PHI_CHECK(channelById(lamp.channels, "x") == nullptr);

    const v1::Channel *link = channelById(lamp.channels, "linkquality");
    PHI_CHECK_MSG(link != nullptr, "the lamp has no link quality channel");
    if (link) {
        PHI_CHECK(link->kind == v1::ChannelKind::LinkQuality);
        PHI_CHECK(link->maxValue == 100.0);
        PHI_CHECK(!has(link->flags, v1::ChannelFlag::Writable));
    }

    // Every device gets these two, whatever it exposes.
    PHI_CHECK(channelById(lamp.channels, "connectivity") != nullptr);
    PHI_CHECK(channelById(lamp.channels, "device_software_update") != nullptr);
    PHI_CHECK(lamp.availabilityBinding() != nullptr && lamp.availabilityBinding()->isAvailability);
    PHI_CHECK(lamp.updateBinding() != nullptr && lamp.updateBinding()->isUpdate);

    // A device with nothing to say still has those two.
    const DeviceEntry bare = buildDeviceEntry(
        parseObject(R"({"friendly_name":"Nothing","ieee_address":"0x1","definition":{"exposes":[]}})"),
        ExposeFilter{});
    PHI_CHECK(bare.channels.size() == 2);

    // The coordinator is a gateway, not "unknown".
    const DeviceEntry coordinator = buildDeviceEntry(
        parseObject(R"({"friendly_name":"Coordinator","ieee_address":"0x00124b0029ab1234","type":"Coordinator"})"),
        ExposeFilter{});
    PHI_CHECK(coordinator.coordinator);
    PHI_CHECK(coordinator.device.deviceClass == v1::DeviceClass::Gateway);
}

/// Values in and out for the lamp: the scaling has to agree with itself.
void testReadingsAndCommandsAgree()
{
    const DeviceEntry lamp = buildDeviceEntry(parseObject(kLamp), ExposeFilter{});
    const ChannelBinding &brightness = *lamp.binding("brightness");
    const ChannelBinding &state = *lamp.binding("state");
    const ChannelBinding &color = *lamp.binding("color");
    const ChannelBinding &link = *lamp.binding("linkquality");

    const auto reading = readingFor(brightness, Json(127));
    PHI_CHECK(reading.has_value() && reading->shape == Reading::Shape::Scalar);
    if (reading) {
        const double *percent = std::get_if<double>(&reading->scalar);
        PHI_CHECK(percent != nullptr && *percent > 49.9 && *percent < 50.1);
    }
    const auto on = readingFor(state, Json("ON"));
    PHI_CHECK(on.has_value() && std::get_if<bool>(&on->scalar) && std::get<bool>(on->scalar));
    const auto off = readingFor(state, Json("OFF"));
    PHI_CHECK(off.has_value() && !std::get<bool>(off->scalar));
    const auto lqi = readingFor(link, Json(255));
    PHI_CHECK(lqi.has_value() && std::get<double>(lqi->scalar) == 100.0);
    const auto xy = readingFor(color, parseObject(R"({"x":0.3127,"y":0.3290})"));
    PHI_CHECK(xy.has_value() && xy->shape == Reading::Shape::Color);

    Json payload = Json::object();
    std::string error;
    CommandValue half;
    half.scalar = 50.0;
    PHI_CHECK(commandPayload(brightness, half, payload, error));
    PHI_CHECK(payload.contains("brightness") && payload["brightness"].get<double>() == 127.0);

    payload = Json::object();
    CommandValue onValue;
    onValue.scalar = true;
    PHI_CHECK(commandPayload(state, onValue, payload, error));
    PHI_CHECK(payload["state"] == "ON");

    payload = Json::object();
    CommandValue red;
    red.json = parseObject(R"({"r":255,"g":0,"b":0})");
    PHI_CHECK(commandPayload(color, red, payload, error));
    PHI_CHECK(payload.contains("color") && payload["color"].contains("x"));

    payload = Json::object();
    CommandValue notAColor;
    notAColor.scalar = std::string("red");
    PHI_CHECK(!commandPayload(color, notAColor, payload, error));
    PHI_CHECK(!error.empty());

    // A read-only channel is refused before any of this - by the instance -
    // but the update object has to come through as fields.
    const auto update = updateReading(parseObject(R"({"state":"available","installed_version":1,"latest_version":2})"), false);
    PHI_CHECK(update.has_value() && update->shape == Reading::Shape::Object);
    if (update) {
        PHI_CHECK(update->fields.size() == 3);
        PHI_CHECK(update->fields[0].first == "status"
                  && std::get<std::string>(update->fields[0].second) == "available");
        PHI_CHECK(std::get<std::string>(update->fields[2].second) == "2");
    }
}

/// Numeric enum values stay what the device says; textual ones get stable
/// numbers, and the two contract enums their contract numbers and names.
void testEnumsKeepTheirNumbers()
{
    const DeviceEntry sensor = buildDeviceEntry(parseObject(R"({
      "friendly_name": "Motion",
      "ieee_address": "0x2",
      "definition": { "exposes": [
        { "type": "binary", "property": "occupancy", "access": 1, "value_on": true, "value_off": false },
        { "type": "enum", "property": "motion_sensitivity", "access": 7, "values": ["low", "medium", "high"] },
        { "type": "enum", "property": "keep_time", "access": 7, "values": ["10", "30", "60"] },
        { "type": "enum", "property": "mode", "access": 7, "values": ["b", "a"] },
        { "type": "numeric", "property": "voltage", "access": 1, "unit": "mV", "value_min": 2000, "value_max": 3300 }
      ] }
    })"), ExposeFilter{});
    PHI_CHECK(sensor.device.deviceClass == v1::DeviceClass::Sensor);

    const ChannelBinding *sensitivity = sensor.binding("motion_sensitivity");
    PHI_CHECK(sensitivity != nullptr);
    if (sensitivity) {
        PHI_CHECK(sensitivity->enumRawToValue.at("low") == static_cast<int>(v1::SensitivityLevel::Low));
        PHI_CHECK(sensitivity->enumRawToValue.at("high") == static_cast<int>(v1::SensitivityLevel::High));
        const v1::Channel *channel = channelById(sensor.channels, "motion_sensitivity");
        PHI_CHECK(channel != nullptr && channel->choices.size() == 3);
        PHI_CHECK(channel != nullptr && channel->choices[0].label == "Low");
        PHI_CHECK(channel != nullptr && parseObject(channel->metaJson)["enumName"] == "SensitivityLevel");
        // A sensor's settings stay writable; its measurements do not.
        PHI_CHECK(channel != nullptr && has(channel->flags, v1::ChannelFlag::Writable));
    }
    const ChannelBinding *keepTime = sensor.binding("keep_time");
    PHI_CHECK(keepTime != nullptr && keepTime->enumRawToValue.at("30") == 30);
    const ChannelBinding *mode = sensor.binding("mode");
    PHI_CHECK(mode != nullptr && mode->enumRawToValue.at("a") == 1 && mode->enumRawToValue.at("b") == 2);

    // An enum write goes back as the device's own word.
    Json payload = Json::object();
    std::string error;
    CommandValue high;
    high.scalar = static_cast<std::int64_t>(v1::SensitivityLevel::High);
    PHI_CHECK(sensitivity != nullptr && commandPayload(*sensitivity, high, payload, error));
    PHI_CHECK(payload["motion_sensitivity"] == "high");

    const v1::Channel *voltage = channelById(sensor.channels, "voltage");
    PHI_CHECK(voltage != nullptr && voltage->unit == "V" && voltage->maxValue == 3.3);
    const auto volts = readingFor(*sensor.binding("voltage"), Json(3000));
    PHI_CHECK(volts.has_value() && std::get<double>(volts->scalar) == 3.0);
    const v1::Channel *occupancy = channelById(sensor.channels, "occupancy");
    PHI_CHECK(occupancy != nullptr && !has(occupancy->flags, v1::ChannelFlag::Writable));
}

/// A remote with numbered buttons and a dial.
void testARemoteBecomesButtonsAndADial()
{
    const DeviceEntry remote = buildDeviceEntry(parseObject(R"({
      "friendly_name": "Dial",
      "ieee_address": "0x3",
      "power_source": "Battery",
      "definition": { "exposes": [
        { "type": "enum", "property": "action", "access": 1, "values": [
          "button_1_press", "button_1_press_release", "button_2_hold", "dial_rotate_left_step" ] }
      ] }
    })"), ExposeFilter{});
    PHI_CHECK(remote.device.deviceClass == v1::DeviceClass::Button);
    PHI_CHECK(has(remote.device.flags, v1::DeviceFlag::Battery));
    PHI_CHECK(channelById(remote.channels, "button1") != nullptr);
    PHI_CHECK(channelById(remote.channels, "button2") != nullptr);
    PHI_CHECK(channelById(remote.channels, "action") == nullptr);
    const v1::Channel *dial = channelById(remote.channels, "dial");
    PHI_CHECK(dial != nullptr && dial->kind == v1::ChannelKind::RelativeRotation);
    PHI_CHECK(remote.binding("button2") != nullptr && remote.binding("button2")->actionButtonId == 2);
    PHI_CHECK(remote.binding("dial") != nullptr && remote.binding("dial")->actionIsDial);

    // A remote whose actions name no button gets one channel for all of them.
    const DeviceEntry plain = buildDeviceEntry(parseObject(R"({
      "friendly_name": "Plain", "ieee_address": "0x4",
      "definition": { "exposes": [ { "type": "enum", "property": "action", "access": 1,
        "values": ["on_press", "on_press_release", "off_hold"] } ] } })"), ExposeFilter{});
    PHI_CHECK(channelById(plain.channels, "action") != nullptr);
}

/// The static config's suppression lists apply by model id.
void testSuppressedPropertiesAreNotChannels()
{
    const ExposeFilter filter = ExposeFilter::fromStaticConfig(parseObject(
        R"({"suppressedPropertyPrefixesByModelId":{"RDM002":["brightness"]}})"));
    const DeviceEntry dial = buildDeviceEntry(parseObject(R"({
      "friendly_name": "Hue dial", "ieee_address": "0x5", "model_id": "RDM002",
      "definition": { "exposes": [
        { "type": "numeric", "property": "brightness", "access": 1 },
        { "type": "numeric", "property": "battery", "access": 1 } ] } })"), filter);
    PHI_CHECK(channelById(dial.channels, "brightness") == nullptr);
    PHI_CHECK(channelById(dial.channels, "battery") != nullptr);
    PHI_CHECK(filter.suppressed("brightness", "", "RDM002"));
    PHI_CHECK(!filter.suppressed("brightness", "", "LCT015"));
}

/// bridge/info is reported as facts about the bridge, before and independent
/// of any device.
void testBridgeFacts()
{
    const Json facts = bridgeFactsPatch(parseObject(kBridgeInfo));
    PHI_CHECK(facts["serialAdapter"] == "ember");
    PHI_CHECK(facts["coordinatorType"] == "EmberZNet");
    PHI_CHECK(facts["coordinatorFirmware"] == "7.4.4.0");
    PHI_CHECK(facts["serialPort"] == "/dev/serial/by-id/usb-Itead_Sonoff-if00");
    PHI_CHECK(facts["z2mVersion"] == "2.13.0");
    PHI_CHECK(facts["z2mCommit"] == "abc1234");
    PHI_CHECK(facts["zigbeeChannel"] == 15);
    PHI_CHECK(facts["panId"] == "6754");
    PHI_CHECK(facts["extPanId"] == "0xdddddddddddddddd");
    PHI_CHECK(facts["permitJoin"] == false);
    PHI_CHECK(facts["logLevel"] == "info");

    const Json sparse = bridgeFactsPatch(parseObject(R"({"version":"2.13.0"})"));
    PHI_CHECK(sparse["z2mVersion"] == "2.13.0");
    PHI_CHECK(!sparse.contains("serialAdapter"));
    PHI_CHECK(!sparse.contains("coordinatorType"));

    DeviceEntry coordinator = buildDeviceEntry(
        parseObject(R"({"friendly_name":"Coordinator","ieee_address":"0x00124b0029ab1234","type":"Coordinator"})"),
        ExposeFilter{});
    applyBridgeInfoToCoordinator(parseObject(kBridgeInfo), coordinator);
    PHI_CHECK(coordinator.device.firmware == "7.4.4.0");
    PHI_CHECK(coordinator.device.deviceClass == v1::DeviceClass::Gateway);
    PHI_CHECK(coordinator.meta["serial_adapter"] == "ember");
    PHI_CHECK(parseObject(coordinator.forWire().metaJson)["serial_port"]
              == "/dev/serial/by-id/usb-Itead_Sonoff-if00");
}

/// Starting the adapter has to leave the account, and the operator's TLS
/// answer, on the client.
///
/// It did not once, and nothing said so: the username and password were
/// only ever applied by a later config change. An adapter that was handed
/// an account and never applied it looks exactly like one that was handed
/// none, right up until the broker stops accepting anonymous clients. The
/// TLS version of the same failure would surface as a password on the wire,
/// which nothing surfaces at all.
void testTheConfigurationReachesTheClient()
{
    v1::Adapter info;
    info.externalId = "main";
    info.pluginType = "z2m";
    info.ip = "127.0.0.1";
    info.port = 1883;
    info.user = " zigbee2mqtt_main ";
    info.password = "geheimgeheim";

    const MqttSettings plain = mqttSettingsFor(info, parseObject("{}"));
    PHI_CHECK(plain.clientId == "phi-core-z2m-main");
    PHI_CHECK(plain.host == "127.0.0.1");
    PHI_CHECK(plain.port == 1883);
    PHI_CHECK(plain.username == "zigbee2mqtt_main");
    PHI_CHECK(plain.password == "geheimgeheim");
    // An instance that said nothing connects in the clear and verifies.
    PHI_CHECK(!plain.tls.enabled);
    PHI_CHECK(plain.tls.verifyHostname);

    const MqttSettings tls = mqttSettingsFor(
        info, parseObject(R"({"tls":"true","tlsCaFile":" /etc/ssl/broker.crt ","tlsVerifyHostname":false})"));
    PHI_CHECK(tls.tls.enabled);
    PHI_CHECK(tls.tls.caFile == "/etc/ssl/broker.crt");
    PHI_CHECK(!tls.tls.verifyHostname);

    info.ip.clear();
    info.host = "broker.local";
    info.port = 0;
    const MqttSettings byName = mqttSettingsFor(info, parseObject("{}"));
    PHI_CHECK(byName.host == "broker.local");
    PHI_CHECK(byName.port == 1883);
}

/// The transport is offered wherever the password is: a section that asks
/// for credentials asks the TLS questions in the SDK's spelling.
void testTheTransportIsOfferedWhereverThePasswordIs()
{
    const Json schema = parseObject(configSchemaJson());
    PHI_CHECK(schema.contains("factory") && schema.contains("instance"));
    int sectionsAsking = 0;
    for (const char *name : {"factory", "instance"}) {
        const Json fields = jsonValue(schema[name], "fields");
        std::set<std::string> keys;
        bool asksForPassword = false;
        for (const Json &field : fields) {
            keys.insert(jsonString(field, "key"));
            if (jsonString(field, "type") == "Password")
                asksForPassword = true;
        }
        if (!asksForPassword)
            continue;
        ++sectionsAsking;
        for (const char *key : {v1::kTlsFieldKey, v1::kTlsCaFileFieldKey, v1::kTlsVerifyHostnameFieldKey})
            PHI_CHECK_MSG(keys.count(key), "%s is missing next to the password in %s", key, name);
    }
    PHI_CHECK(sectionsAsking > 0);

    // The CA field only shows once TLS is on, in the shape core parses.
    for (const Json &field : jsonValue(schema["factory"], "fields")) {
        if (jsonString(field, "key") != v1::kTlsCaFileFieldKey)
            continue;
        const Json visibility = jsonValue(field, "visibility");
        PHI_CHECK(visibility["fieldKey"] == v1::kTlsFieldKey);
        PHI_CHECK(visibility["value"] == true);
        PHI_CHECK(visibility["op"] == "equals");
        PHI_CHECK(jsonString(field, "type") == "String");
    }
    const v1::AdapterCapabilities caps = capabilities();
    PHI_CHECK(caps.factoryActions.size() == 1 && caps.factoryActions[0].id == "probe");
    PHI_CHECK(caps.instanceActions.size() == 4);
}

} // namespace

int main()
{
    testALampBecomesALight();
    testReadingsAndCommandsAgree();
    testEnumsKeepTheirNumbers();
    testARemoteBecomesButtonsAndADial();
    testSuppressedPropertiesAreNotChannels();
    testBridgeFacts();
    testTheConfigurationReachesTheClient();
    testTheTransportIsOfferedWhereverThePasswordIs();
    return phi::testing::report("z2m_convert_tests");
}
