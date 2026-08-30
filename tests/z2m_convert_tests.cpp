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
#include "z2m_schema.h"

#include "phi/adapter/qt/tlsconfig.h"

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
    using Z2mAdapter::handleBridgeInfoPayload;
    using Z2mAdapter::reportBridgeFacts;
    using Z2mAdapter::Z2mDeviceEntry;
    using Z2mAdapter::client;
    using Z2mAdapter::start;
    using Z2mAdapter::stop;
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

/// What zigbee2mqtt publishes on `bridge/info`, cut down to the parts that say
/// what the bridge is running on. The shapes are real: `config.serial.adapter`
/// is z2m's own word for the driver it opened the stick with, and the
/// coordinator's revision is where the firmware hides.
const char *kBridgeInfo = R"({
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
})";

} // namespace

/// Starting the adapter has to leave the account on the client.
///
/// It did not, and nothing said so. `stop()` deletes the client, so on every
/// start there is none - and applyConfig() gives up halfway when there is
/// nothing to configure, so the username and password were only ever applied
/// by a later config change. An adapter that was handed an account and never
/// applied it looks exactly like one that was handed none, right up until the
/// broker stops accepting anonymous clients.
void testStartingAppliesTheAccountToTheClient()
{
    TestableZ2mAdapter adapter;

    Adapter info;
    info.id = QStringLiteral("main");
    info.plugin = QStringLiteral("z2m");
    // No address: connectToBroker() gives up on an empty one, so start() does
    // everything except open a socket - which is the whole of what is under
    // test here.
    info.ip = QString();
    info.port = 1883;
    info.user = QStringLiteral("zigbee2mqtt_main");
    info.pw = QStringLiteral("geheimgeheim");
    adapter.assignAdapter(info);

    QString error;
    PHI_CHECK(adapter.start(error));
    PHI_CHECK(adapter.client() != nullptr);
    if (adapter.client()) {
        PHI_CHECK(adapter.client()->username() == QStringLiteral("zigbee2mqtt_main"));
        PHI_CHECK(adapter.client()->password() == QStringLiteral("geheimgeheim"));
    }
    adapter.stop();
}

/// Starting the adapter has to carry the operator's TLS answer to the client.
///
/// The same failure the account had: applyConfig gives up halfway when there is
/// no client, so an instance that was started and left alone would connect in
/// the clear while the interface said TLS. That one only surfaced when a broker
/// started asking; this one would surface as a password on the wire, which
/// nothing surfaces at all.
void testStartingCarriesTheTlsAnswerToTheClient()
{
    TestableZ2mAdapter adapter;

    Adapter info;
    info.id = QStringLiteral("main");
    info.plugin = QStringLiteral("z2m");
    info.ip = QString();
    info.port = 8883;
    info.meta = QJsonObject{{QStringLiteral("tls"), true},
                            {QStringLiteral("tlsCaFile"), QStringLiteral("/etc/ssl/broker.crt")},
                            {QStringLiteral("tlsVerifyHostname"), false}};
    adapter.assignAdapter(info);

    QString error;
    PHI_CHECK(adapter.start(error));
    PHI_CHECK(adapter.client() != nullptr);
    if (adapter.client()) {
        PHI_CHECK(adapter.client()->tls().enabled);
        PHI_CHECK(adapter.client()->tls().caFile == "/etc/ssl/broker.crt");
        PHI_CHECK(!adapter.client()->tls().verifyHostname);
    }
    adapter.stop();
}

void testAnInstanceThatSaidNothingConnectsInTheClearAndVerifies()
{
    // Every instance that exists today. Off, because an adapter pointed at a
    // plain broker has to keep working - and the check on, so that turning TLS
    // on later is enough.
    TestableZ2mAdapter adapter;

    Adapter info;
    info.id = QStringLiteral("main");
    info.plugin = QStringLiteral("z2m");
    info.ip = QString();
    info.port = 1883;
    adapter.assignAdapter(info);

    QString error;
    PHI_CHECK(adapter.start(error));
    if (adapter.client()) {
        PHI_CHECK(!adapter.client()->tls().enabled);
        PHI_CHECK(adapter.client()->tls().verifyHostname);
    }
    adapter.stop();
}

/// The transport is offered wherever the password is.
///
/// That is the invariant rather than "in the instance section": the connection
/// settings all live in one place - host, port, user, password - and how the
/// connection is protected belongs with the credential it protects. A form that
/// asks for a password and not for TLS is a form that can only send it in the
/// clear.
///
/// Checked against the SDK's own list rather than against three names written
/// out here: a copy of the keys in this test would pass while the adapter and
/// the rest of phi drifted apart, which is the exact failure the shared fields
/// exist to prevent.
void testTheTransportIsOfferedWhereverThePasswordIs()
{
    const QJsonObject schema =
        QJsonDocument::fromJson(QByteArray::fromStdString(
                                    phicore::z2m::ipc::configSchemaJson()))
            .object();

    int sectionsAsking = 0;
    for (const QString &sectionName : {QStringLiteral("factory"), QStringLiteral("instance")}) {
        QStringList keys;
        for (const QJsonValue &value :
             schema.value(sectionName).toObject().value(QStringLiteral("fields")).toArray()) {
            keys.append(value.toObject().value(QStringLiteral("key")).toString());
        }
        if (!keys.contains(QStringLiteral("password")))
            continue;
        ++sectionsAsking;
        for (const QJsonValue &expected : phicore::adapter::tlsConfigFields()) {
            const QString key = expected.toObject().value(QStringLiteral("key")).toString();
            PHI_CHECK_MSG(keys.contains(key), "%s fehlt neben dem Passwort in %s",
                          qPrintable(key), qPrintable(sectionName));
        }
    }
    // And a schema that asked for a password nowhere would pass the loop above
    // without checking anything at all.
    PHI_CHECK(sectionsAsking > 0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testStartingAppliesTheAccountToTheClient();
    testTheTransportIsOfferedWhereverThePasswordIs();
    testStartingCarriesTheTlsAnswerToTheClient();
    testAnInstanceThatSaidNothingConnectsInTheClearAndVerifies();
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

    // What the bridge says about itself, and when it says it.
    //
    // This used to be reported at the end of the function that also updates the
    // coordinator *device*, behind two early returns waiting for that device to
    // exist. `bridge/info` normally arrives before the device list, so a fresh
    // installation showed nothing at all about its own radio until devices
    // turned up. Which serial adapter z2m is driving is a fact about the
    // bridge; it does not depend on any device.
    QJsonObject facts;
    int reported = 0;
    QObject::connect(&adapter, &Z2mAdapter::adapterMetaUpdated,
                     [&](const QJsonObject &patch) {
                         facts = patch;
                         ++reported;
                     });
    adapter.reportBridgeFacts(QJsonDocument::fromJson(QByteArray(kBridgeInfo)).object());

    // No devices were ever built into this adapter, which is the whole point.
    PHI_CHECK(reported == 1);
    PHI_CHECK(facts.value(QStringLiteral("serialAdapter")).toString() == QStringLiteral("ember"));
    PHI_CHECK(facts.value(QStringLiteral("coordinatorType")).toString()
              == QStringLiteral("EmberZNet"));
    PHI_CHECK(facts.value(QStringLiteral("coordinatorFirmware")).toString()
              == QStringLiteral("7.4.4.0"));
    PHI_CHECK(facts.value(QStringLiteral("serialPort")).toString()
              == QStringLiteral("/dev/serial/by-id/usb-Itead_Sonoff-if00"));
    PHI_CHECK(facts.value(QStringLiteral("z2mVersion")).toString() == QStringLiteral("2.13.0"));
    PHI_CHECK(facts.value(QStringLiteral("z2mCommit")).toString() == QStringLiteral("abc1234"));
    PHI_CHECK(facts.value(QStringLiteral("zigbeeChannel")).toInt() == 15);
    PHI_CHECK(facts.value(QStringLiteral("panId")).toString() == QStringLiteral("6754"));
    PHI_CHECK(facts.value(QStringLiteral("extPanId")).toString()
              == QStringLiteral("0xdddddddddddddddd"));
    PHI_CHECK(facts.value(QStringLiteral("permitJoin")).toBool() == false);
    PHI_CHECK(facts.value(QStringLiteral("logLevel")).toString() == QStringLiteral("info"));

    // And through the door the broker actually knocks on. Calling the whole
    // handler with no devices known is the case that was broken: it stashes the
    // payload to replay once the coordinator turns up, and that is fine - but
    // the facts have to come out now regardless.
    QJsonObject viaHandler;
    int viaHandlerCount = 0;
    QObject::connect(&adapter, &Z2mAdapter::adapterMetaUpdated,
                     [&](const QJsonObject &patch) {
                         viaHandler = patch;
                         ++viaHandlerCount;
                     });
    adapter.handleBridgeInfoPayload(QJsonDocument::fromJson(QByteArray(kBridgeInfo)).object(), 0);
    PHI_CHECK(viaHandlerCount == 1);
    PHI_CHECK(viaHandler.value(QStringLiteral("serialAdapter")).toString()
              == QStringLiteral("ember"));

    // A bridge that says less is reported for what it did say, rather than
    // reported with empty strings: absent means "z2m told us nothing", which is
    // a different claim from "there is nothing there".
    QJsonObject sparse;
    QObject::connect(&adapter, &Z2mAdapter::adapterMetaUpdated,
                     [&](const QJsonObject &patch) { sparse = patch; });
    adapter.reportBridgeFacts(QJsonDocument::fromJson(QByteArray(R"({"version":"2.13.0"})"))
                                  .object());
    PHI_CHECK(sparse.value(QStringLiteral("z2mVersion")).toString() == QStringLiteral("2.13.0"));
    PHI_CHECK(!sparse.contains(QStringLiteral("serialAdapter")));
    PHI_CHECK(!sparse.contains(QStringLiteral("coordinatorType")));
    PHI_CHECK(!sparse.contains(QStringLiteral("panId")));

    return phi::testing::report("z2m_convert_tests");
}
