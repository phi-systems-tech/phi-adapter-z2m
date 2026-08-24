// A zigbee2mqtt device description, and the channels it turns into.
//
// The first test in any of the adapter repositories. What is worth testing in
// an adapter is this conversion: it is most of what the adapter is, it runs
// before anything touches a broker or a device, and it is where a silent
// mistake costs an operator a channel they cannot control. The harness is the
// one phi-core uses, now shared through the SDK, so the next adapter copies
// this file rather than inventing its own.

#include <phi/adapter/testing/check.h>

#include "z2madapter.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace phicore::adapter;

namespace {

/// The conversion is protected, because it belongs to the adapter and not to
/// its callers; a test is the one caller that is allowed to look.
class TestableZ2mAdapter : public Z2mAdapter
{
public:
    using Z2mAdapter::buildDeviceEntry;
    using Z2mAdapter::Z2mDeviceEntry;
};

QJsonObject deviceJson(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

const Channel *channelById(const ChannelList &channels, const QString &id)
{
    for (const Channel &channel : channels) {
        if (channel.id == id)
            return &channel;
    }
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestableZ2mAdapter adapter;

    // A dimmable lamp, in the shape zigbee2mqtt publishes on bridge/devices:
    // the exposes tree, with a light feature holding state and brightness, and
    // a link quality reading next to it.
    const auto lamp = adapter.buildDeviceEntry(deviceJson(R"({
        "ieee_address": "0x00158d0001a2b3c4",
        "friendly_name": "Kitchen lamp",
        "type": "Router",
        "definition": {
            "model": "LED1836G9",
            "vendor": "IKEA",
            "exposes": [
                {
                    "type": "light",
                    "features": [
                        {"type":"binary","name":"state","property":"state","access":7,
                         "value_on":"ON","value_off":"OFF"},
                        {"type":"numeric","name":"brightness","property":"brightness","access":7,
                         "value_min":0,"value_max":254}
                    ]
                },
                {"type":"numeric","name":"linkquality","property":"linkquality","access":1,
                 "value_min":0,"value_max":255,"unit":"lqi"}
            ]
        }
    })"));

    // The device is named by what its owner called it, and identified by what
    // cannot change.
    PHI_CHECK(lamp.device.id == QStringLiteral("0x00158d0001a2b3c4"));
    PHI_CHECK(lamp.device.name == QStringLiteral("Kitchen lamp"));
    PHI_CHECK(lamp.mqttId == QStringLiteral("Kitchen lamp"));
    PHI_CHECK(lamp.device.deviceClass == DeviceClass::Light);

    const Channel *state = channelById(lamp.channels, QStringLiteral("state"));
    PHI_CHECK_MSG(state != nullptr, "the lamp has no state channel");
    if (state) {
        PHI_CHECK(state->kind == ChannelKind::PowerOnOff);
        PHI_CHECK(state->dataType == ChannelDataType::Bool);
        // access 7 is read, write and report.
        PHI_CHECK(state->flags.testFlag(ChannelFlag::ChannelFlagWritable));
        PHI_CHECK(state->flags.testFlag(ChannelFlag::ChannelFlagReadable));
    }

    const Channel *brightness = channelById(lamp.channels, QStringLiteral("brightness"));
    PHI_CHECK_MSG(brightness != nullptr, "the lamp has no brightness channel");
    if (brightness) {
        PHI_CHECK(brightness->kind == ChannelKind::Brightness);
        // Zigbee counts brightness 0..254 and the contract counts it in percent
        // ("[0..100] percent", `v1/types.h`), so the adapter converts rather
        // than passing its vendor's scale on. Everything above this is then
        // spared knowing what a Zigbee level is.
        PHI_CHECK(brightness->dataType == ChannelDataType::Float);
        PHI_CHECK(brightness->minValue == 0.0);
        PHI_CHECK(brightness->maxValue == 100.0);
        PHI_CHECK(brightness->unit == QStringLiteral("%"));
        PHI_CHECK(brightness->flags.testFlag(ChannelFlag::ChannelFlagWritable));
    }

    const Channel *link = channelById(lamp.channels, QStringLiteral("linkquality"));
    PHI_CHECK_MSG(link != nullptr, "the lamp has no link quality channel");
    if (link) {
        PHI_CHECK(link->kind == ChannelKind::LinkQuality);
        // Zigbee reports 0..255, the contract is percent again.
        PHI_CHECK(link->maxValue == 100.0);
        // access 1 is read only: nothing may try to write it.
        PHI_CHECK(!link->flags.testFlag(ChannelFlag::ChannelFlagWritable));
    }

    // Every device gets the two channels that are about the device rather than
    // about what it does: whether it is reachable, and whether its firmware is
    // current. A coordinator, which exposes nothing at all, gets those and
    // nothing else.
    PHI_CHECK(channelById(lamp.channels, QStringLiteral("connectivity")) != nullptr);
    PHI_CHECK(channelById(lamp.channels, QStringLiteral("device_software_update")) != nullptr);

    const auto bare = adapter.buildDeviceEntry(deviceJson(R"({
        "ieee_address": "0x00158d0009999999",
        "friendly_name": "Coordinator",
        "type": "Coordinator",
        "definition": null
    })"));
    PHI_CHECK(bare.channels.size() == 2);
    PHI_CHECK(channelById(bare.channels, QStringLiteral("connectivity")) != nullptr);
    PHI_CHECK(channelById(bare.channels, QStringLiteral("device_software_update")) != nullptr);

    return phi::testing::report("z2m_convert_tests");
}
