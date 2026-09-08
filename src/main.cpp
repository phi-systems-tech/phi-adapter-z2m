// Process entry point for the Zigbee2MQTT sidecar: the adapter factory and
// the SDK's own main loop. The instance lives in z2m_instance; the broker in
// z2m_mqtt_client; the conversion in z2m_exposes and z2m_state.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"

#include "z2m_instance.h"
#include "z2m_json.h"
#include "z2m_probe.h"
#include "z2m_schema.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace phicore::z2m::ipc;
using namespace std::chrono_literals;

namespace {

constexpr auto kProbeTimeout = 2000ms;

class Z2mFactory final : public sdk::AdapterFactory
{
protected:
    // The probe waits for a TCP connect for up to two seconds; on the host
    // poll thread that stalled IPC for every instance of this sidecar.
    std::unique_ptr<sdk::InstanceExecutionBackend> createFactoryExecutionBackend() override
    {
        return sdk::createLoopExecutionBackend("z2m-factory");
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::createLoopExecutionBackend("z2m-instance");
    }

    v1::Utf8String pluginType() const override { return kPluginType; }
    v1::Utf8String displayName() const override { return phicore::z2m::ipc::displayName(); }
    v1::Utf8String description() const override { return phicore::z2m::ipc::description(); }
    v1::Utf8String apiVersion() const override { return "1.0.0"; }
    v1::Utf8String iconSvg() const override { return phicore::z2m::ipc::iconSvg(); }
    int timeoutMs() const override { return 15000; }
    int maxInstances() const override { return 0; }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::z2m::ipc::capabilities();
    }

    v1::JsonText configSchemaJson() const override
    {
        return phicore::z2m::ipc::configSchemaJson();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return makeInstance();
    }

    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        v1::ActionResponse response;
        response.id = request.cmdId;
        response.tsMs = 0;
        response.resultType = v1::ActionResultType::Boolean;
        response.resultValue = false;

        if (request.actionId != "probe") {
            response.status = v1::CmdStatus::NotImplemented;
            response.error = "Factory action not implemented";
            response.resultType = v1::ActionResultType::None;
            send(response);
            return;
        }

        const ProbeTarget target = probeTargetFromParams(parseObject(request.paramsJson));
        if (target.host.empty()) {
            response.status = v1::CmdStatus::InvalidArgument;
            response.error = "Host must not be empty.";
            send(response);
            return;
        }

        std::string error;
        if (probeEndpoint(target, kProbeTimeout, [this]() { return stopRequested(); }, &error)) {
            response.status = v1::CmdStatus::Success;
            response.resultValue = true;
        } else {
            response.status = v1::CmdStatus::Failure;
            response.error = error.empty() ? "Connection failed" : error;
        }
        send(response);
    }

private:
    void send(const v1::ActionResponse &response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            std::cerr << "failed to send factory.action.invoke result: " << error << '\n';
    }
};

} // namespace

int main(int argc, char **argv)
{
    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-z2m-ipc.sock"));

    std::cerr << "starting phi_adapter_z2m_ipc for pluginType=" << kPluginType
              << " socket=" << socketPath << '\n';

    Z2mFactory factory;
    sdk::SidecarHost host(socketPath, factory);
    return sdk::runSidecarMain(host);
}
