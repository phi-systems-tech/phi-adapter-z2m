// What the adapter decides without a broker: where a topic points, what a
// button did, which devices changed, whether the bridge is there.

#include <phi/adapter/testing/check.h>

#include "z2m_actions.h"
#include "z2m_bridge.h"
#include "z2m_exposes.h"
#include "z2m_json.h"
#include "z2m_state.h"
#include "z2m_topics.h"

#include <set>
#include <string>

using namespace phicore::z2m::ipc;
namespace v1 = phicore::adapter::v1;
using Code = v1::ButtonEventCode;

namespace {

void testWhereATopicPoints()
{
    PHI_CHECK(normalizedBaseTopic("") == "zigbee2mqtt");
    PHI_CHECK(normalizedBaseTopic(" home/z2m/ ") == "home/z2m");

    TopicParts parts = splitTopic("zigbee2mqtt", "zigbee2mqtt/bridge/response/health_check");
    PHI_CHECK(parts.underBase && parts.bridge && parts.bridgePath == "response/health_check");
    parts = splitTopic("zigbee2mqtt", "zigbee2mqtt/Kitchen lamp");
    PHI_CHECK(parts.underBase && !parts.bridge && parts.suffix == "Kitchen lamp");
    PHI_CHECK(!splitTopic("zigbee2mqtt", "zigbee2mqtt2/x").underBase);
    PHI_CHECK(!splitTopic("zigbee2mqtt", "homeassistant/light/x").underBase);

    const std::set<std::string> known = {"Kitchen lamp", "living room/lamp"};
    const auto isKnown = [&known](std::string_view name) { return known.count(std::string(name)) > 0; };

    DeviceTopicRef ref = classifyDeviceTopic("Kitchen lamp", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::State && ref.device == "Kitchen lamp");
    ref = classifyDeviceTopic("Kitchen lamp/availability", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::Availability && ref.device == "Kitchen lamp");
    ref = classifyDeviceTopic("Kitchen lamp/set", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::Echo);
    // A name with a slash in it is still one device.
    ref = classifyDeviceTopic("living room/lamp", isKnown);
    PHI_CHECK_MSG(ref.kind == DeviceTopic::State && ref.device == "living room/lamp",
                  "a friendly name with a slash was read as a sub-topic");
    ref = classifyDeviceTopic("living room/lamp/availability", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::Availability && ref.device == "living room/lamp");
    // A device nobody knows yet: its state is held, its sub-topics are not.
    ref = classifyDeviceTopic("New device", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::State && ref.device == "New device");
    ref = classifyDeviceTopic("New device/l1", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::Other);
    ref = classifyDeviceTopic("New device/availability", isKnown);
    PHI_CHECK(ref.kind == DeviceTopic::Availability && ref.device == "New device");
}

void testTheActionVocabulary()
{
    PHI_CHECK(buttonIdFromAction("button_2_single") == 2);
    PHI_CHECK(buttonIdFromAction("3_double") == 3);
    PHI_CHECK(buttonIdFromAction("on_press_release") == 0);
    PHI_CHECK(buttonIdFromAction("brightness_step_up") == 0);
    const std::set<int> ids = buttonIdsFromActions({"button_1_press", "button_4_hold", "toggle"});
    PHI_CHECK(ids.size() == 2 && ids.count(1) && ids.count(4));

    PHI_CHECK(isDialAction("dial_rotate_left_step"));
    PHI_CHECK(isDialAction("brightness_step_up"));
    PHI_CHECK(!isDialAction("on_press"));

    PHI_CHECK(dialMagnitude("rotate_left", parseObject("{}")) == 1);
    PHI_CHECK(dialMagnitude("dial_rotate_right_step", parseObject(R"({"action_step_size":8})")) == 8);
    PHI_CHECK(dialMagnitude("brightness_step_up_3", parseObject("{}")) == 3);
    // A step field that says nothing means nothing turned.
    PHI_CHECK(dialMagnitude("dial_rotate_right_step", parseObject(R"({"action_step_size":0})")) == 0);

    PHI_CHECK(dialDirection("dial_rotate_left_step", parseObject("{}")) == -1);
    PHI_CHECK(dialDirection("brightness_step_up", parseObject("{}")) == 1);
    PHI_CHECK(dialDirection("brightness_step", parseObject(R"({"action_direction":1})")) == -1);
    PHI_CHECK(dialDirection("brightness_step", parseObject(R"({"action_direction":2})")) == 1);
    PHI_CHECK(dialDirection("brightness_step", parseObject("{}")) == 0);

    PHI_CHECK(buttonEventFor("on_press") == Code::InitialPress);
    PHI_CHECK(buttonEventFor("on_press_release") == Code::ShortPressRelease);
    PHI_CHECK(buttonEventFor("on_hold") == Code::LongPress);
    PHI_CHECK(buttonEventFor("on_hold_release") == Code::LongPressRelease);
    PHI_CHECK(buttonEventFor("double") == Code::DoublePress);
    PHI_CHECK(buttonEventFor("single") == Code::InitialPress);
    PHI_CHECK(buttonEventFor("") == Code::None);
    PHI_CHECK(buttonEventFor("toggle") == Code::None);
}

Json deviceList(const char *text)
{
    return parseJson(text);
}

/// A device list is diffed against what is known, not applied whole.
void testTheDeviceListIsDiffed()
{
    const ExposeFilter filter;
    DeviceTable table;
    const std::int64_t now = 1'800'000'000'000;

    const Json first = deviceList(R"([
      {"friendly_name":"Coordinator","ieee_address":"0xc0","type":"Coordinator"},
      {"friendly_name":"Lamp","ieee_address":"0x1","definition":{"exposes":[
        {"type":"binary","property":"state","access":7}]},"availability":"online"},
      {"friendly_name":"Sensor","ieee_address":"0x2","definition":{"exposes":[
        {"type":"numeric","property":"temperature","access":1}]},"last_seen":1799999000000}
    ])");
    SnapshotResult result = table.applySnapshot(first, true, filter, now);
    PHI_CHECK(result.announce.size() == 3);
    PHI_CHECK(result.removed.empty());
    PHI_CHECK(table.coordinatorExternalId() == "0xc0");
    PHI_CHECK(table.byExternalId("0x1") != nullptr && table.byExternalId("0x1")->mqttId == "Lamp");
    PHI_CHECK(table.mqttIdFor("0x2") == "Sensor");
    // Reachability from the list: the lamp says so, the sensor spoke 17
    // minutes ago and is stale.
    PHI_CHECK(result.connectivity.size() == 2);
    for (const ConnectivityReport &report : result.connectivity) {
        if (report.externalId == "0x1")
            PHI_CHECK(report.status == v1::ConnectivityStatus::Connected);
        if (report.externalId == "0x2")
            PHI_CHECK(report.status == v1::ConnectivityStatus::Disconnected);
    }

    // The same list again: nothing to announce.
    result = table.applySnapshot(first, true, filter, now);
    PHI_CHECK_MSG(result.announce.empty(), "an unchanged device was re-announced");
    PHI_CHECK(result.removed.empty());

    // A rename moves the entry and says so; the channels are kept.
    const Json renamed = deviceList(R"([
      {"friendly_name":"Coordinator","ieee_address":"0xc0","type":"Coordinator"},
      {"friendly_name":"Kitchen lamp","ieee_address":"0x1","definition":{"exposes":[
        {"type":"binary","property":"state","access":7}]}},
      {"friendly_name":"Sensor","ieee_address":"0x2","definition":{"exposes":[
        {"type":"numeric","property":"temperature","access":1}]}}
    ])");
    result = table.applySnapshot(renamed, true, filter, now);
    PHI_CHECK(result.announce.size() == 1 && result.announce[0] == "Kitchen lamp");
    PHI_CHECK(result.renamed.size() == 1 && result.renamed[0].first == "0x1"
              && result.renamed[0].second == "Kitchen lamp");
    PHI_CHECK(!table.knows("Lamp") && table.knows("Kitchen lamp"));
    PHI_CHECK(table.mqttIdFor("0x1") == "Kitchen lamp");
    PHI_CHECK(table.byMqttId("Kitchen lamp")->binding("state") != nullptr);

    // A changed definition rebuilds the channels.
    const Json redefined = deviceList(R"([
      {"friendly_name":"Coordinator","ieee_address":"0xc0","type":"Coordinator"},
      {"friendly_name":"Kitchen lamp","ieee_address":"0x1","definition":{"exposes":[
        {"type":"binary","property":"state","access":7},
        {"type":"numeric","property":"brightness","access":7}]}},
      {"friendly_name":"Sensor","ieee_address":"0x2","definition":{"exposes":[
        {"type":"numeric","property":"temperature","access":1}]}}
    ])");
    result = table.applySnapshot(redefined, true, filter, now);
    PHI_CHECK_MSG(result.announce.size() == 1 && result.announce[0] == "Kitchen lamp",
                  "a device whose definition changed was not re-announced");
    PHI_CHECK(table.byMqttId("Kitchen lamp")->binding("brightness") != nullptr);

    // A partial list (a response) adds without dropping; a full one drops.
    const Json partial = deviceList(R"([
      {"friendly_name":"Plug","ieee_address":"0x3","definition":{"exposes":[
        {"type":"binary","property":"state","access":7}]}}
    ])");
    result = table.applySnapshot(partial, false, filter, now);
    PHI_CHECK(result.announce.size() == 1 && result.removed.empty());
    PHI_CHECK(table.all().size() == 4);
    result = table.applySnapshot(redefined, true, filter, now);
    PHI_CHECK(result.removed.size() == 1 && result.removed[0] == "0x3");
    PHI_CHECK(table.all().size() == 3);

    // Mid-interview: not a device yet, and gone if it was one.
    const Json interviewing = deviceList(R"([
      {"friendly_name":"Coordinator","ieee_address":"0xc0","type":"Coordinator"},
      {"friendly_name":"Kitchen lamp","ieee_address":"0x1","interview_completed":false},
      {"friendly_name":"Sensor","ieee_address":"0x2","definition":{"exposes":[
        {"type":"numeric","property":"temperature","access":1}]}}
    ])");
    result = table.applySnapshot(interviewing, true, filter, now);
    PHI_CHECK(result.removed.size() == 1 && result.removed[0] == "0x1");
    PHI_CHECK(!table.knows("Kitchen lamp"));

    // State that arrived before the description is handed back with it.
    table.holdState("Plug", parseObject(R"({"state":"ON"})"));
    result = table.applySnapshot(partial, false, filter, now);
    PHI_CHECK(result.releasedState.size() == 1 && result.releasedState[0].first == "Plug"
              && result.releasedState[0].second["state"] == "ON");
    PHI_CHECK(table.remove("Plug") == "0x3");
    PHI_CHECK(table.byExternalId("0x3") == nullptr);
}

/// Reachable only while a live Zigbee2MQTT has answered; two silences bring
/// it down; its own "offline" is believed on sight.
void testWhoIsHome()
{
    BridgeHealth health;
    PHI_CHECK(!health.online());
    PHI_CHECK(health.beginProbe("t1"));
    PHI_CHECK_MSG(!health.beginProbe("t2"), "a second probe was allowed while the first was unanswered");
    PHI_CHECK(health.answered());
    PHI_CHECK(health.online());
    PHI_CHECK(!health.answered()); // already online: no edge
    PHI_CHECK(health.beginProbe("t3"));
    PHI_CHECK(!health.probeTimedOut()); // one miss
    PHI_CHECK(health.online());
    PHI_CHECK(health.beginProbe("t4"));
    PHI_CHECK(health.probeTimedOut()); // two: down
    PHI_CHECK(!health.online());
    PHI_CHECK(health.beginProbe("t5"));
    PHI_CHECK(!health.probeTimedOut()); // already down: no edge
    PHI_CHECK(health.beginProbe("t6"));
    PHI_CHECK(health.answered());
    health.saidOffline();
    PHI_CHECK(!health.online());
    health.reset();
    PHI_CHECK(!health.probeInFlight());
}

void testWhenADeviceLastSpoke()
{
    PHI_CHECK(lastSeenMs(Json(1799999000000)) == 1799999000000);
    PHI_CHECK(lastSeenMs(Json(1799999000)) == 1799999000000);
    PHI_CHECK(lastSeenMs(Json("2026-09-08T12:00:00Z")) == 1788868800000LL);
    PHI_CHECK(lastSeenMs(Json("2026-09-08T14:00:00.250+02:00")) == 1788868800250LL);
    PHI_CHECK(lastSeenMs(Json("never")) == 0);
    PHI_CHECK(lastSeenMs(Json()) == 0);

    PHI_CHECK(availabilityStatus(Json("online")) == v1::ConnectivityStatus::Connected);
    PHI_CHECK(availabilityStatus(Json(" Offline ")) == v1::ConnectivityStatus::Disconnected);
    PHI_CHECK(availabilityStatus(parseObject(R"({"state":"online"})")) == v1::ConnectivityStatus::Connected);
    PHI_CHECK(availabilityStatus(Json("unknown")) == v1::ConnectivityStatus::Unknown);
}

} // namespace

int main()
{
    testWhereATopicPoints();
    testTheActionVocabulary();
    testTheDeviceListIsDiffed();
    testWhoIsHome();
    testWhenADeviceLastSpoke();
    return phi::testing::report("z2m_behaviour_tests");
}
