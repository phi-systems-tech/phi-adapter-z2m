#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <QCoreApplication>
#include <QEventLoop>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTcpSocket>

#include "z2m_schema.h"
#include "z2m_sidecar.h"
#include "phi/adapter/sdk/sidecar.h"
#include "phi/adapter/sdk/qt/instance_execution_backend_qt.h"

namespace {

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;

std::atomic_bool g_running{true};
using ActionResponse = v1::ActionResponse;
using CmdStatus = v1::CmdStatus;

std::int64_t nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

ActionResponse factoryProbe(std::uint64_t cmdId, const QJsonObject &params)
{
    QJsonObject source = params;
    const QJsonObject factoryAdapter = params.value("factoryAdapter").toObject();
    if (!factoryAdapter.isEmpty())
        source = factoryAdapter;

    const QString host = source.value(QStringLiteral("host")).toString().trimmed().isEmpty()
        ? source.value(QStringLiteral("ip")).toString().trimmed()
        : source.value(QStringLiteral("host")).toString().trimmed();
    const int port = source.value(QStringLiteral("port")).toInt(1883);

    ActionResponse response;
    response.id = cmdId;
    response.tsMs = nowMs();
    response.resultType = v1::ActionResultType::Boolean;
    response.resultValue = false;

    if (host.isEmpty()) {
        response.status = CmdStatus::InvalidArgument;
        response.error = "Host must not be empty.";
        return response;
    }

    QTcpSocket socket;
    socket.connectToHost(host, static_cast<quint16>(port > 0 ? port : 1883));
    if (!socket.waitForConnected(2000)) {
        const QString error = socket.errorString().trimmed().isEmpty()
            ? QStringLiteral("Connection failed")
            : socket.errorString().trimmed();
        socket.abort();
        response.status = CmdStatus::Failure;
        response.error = error.toStdString();
        return response;
    }

    socket.disconnectFromHost();
    response.status = CmdStatus::Success;
    response.resultValue = true;
    return response;
}

void handleSignal(int)
{
    g_running.store(false);
}

class Z2mFactory final : public sdk::AdapterFactory
{
public:
    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const v1::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::qt::createInstanceExecutionBackend();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(
        const v1::ExternalId &externalId) override
    {
        (void)externalId;
        return std::make_unique<phicore::z2m::ipc::Z2mSidecar>();
    }

    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        v1::ActionResponse response;

        if (request.actionId != "probe") {
            response.id = request.cmdId;
            response.status = v1::CmdStatus::NotImplemented;
            response.error = "Factory action not implemented";
            response.tsMs = nowMs();
        } else {
            response = factoryProbe(
                request.cmdId,
                QJsonDocument::fromJson(QByteArray::fromStdString(request.paramsJson)).object());
        }

        v1::Utf8String err;
        if (!sendResult(response, &err))
            std::cerr << "failed to send factory action result: " << err << '\n';
    }

    v1::Utf8String pluginType() const override
    {
        return phicore::z2m::ipc::kPluginType;
    }

    v1::Utf8String displayName() const override
    {
        return phicore::z2m::ipc::displayName();
    }

    v1::Utf8String description() const override
    {
        return phicore::z2m::ipc::description();
    }

    v1::Utf8String iconSvg() const override
    {
        return phicore::z2m::ipc::iconSvg();
    }

    v1::Utf8String apiVersion() const override
    {
        return "1.0.0";
    }

    int timeoutMs() const override
    {
        return 15000;
    }

    int maxInstances() const override
    {
        return 0;
    }

    phicore::adapter::v1::AdapterCapabilities capabilities() const override
    {
        return phicore::z2m::ipc::capabilities();
    }

    phicore::adapter::v1::JsonText configSchemaJson() const override
    {
        return phicore::z2m::ipc::configSchemaJson();
    }
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-z2m-ipc.sock"));

    std::cerr << "starting phi_adapter_z2m_ipc for pluginType=" << phicore::z2m::ipc::kPluginType
              << " socket=" << socketPath << '\n';

    Z2mFactory factory;
    sdk::SidecarHost host(socketPath, factory);

    v1::Utf8String error;
    if (!host.start(&error)) {
        std::cerr << "failed to start sidecar host: " << error << '\n';
        return 1;
    }

    while (g_running.load()) {
        if (!host.pollOnce(std::chrono::milliseconds(250), &error)) {
            std::cerr << "poll failed: " << error << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    host.stop();
    std::cerr << "stopping phi_adapter_z2m_ipc" << '\n';
    return 0;
}
