#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QJsonObject>

#include "z2madapter.h"
#include "phi/adapter/sdk/sidecar.h"

namespace phicore::z2m::ipc {

class Z2mSidecar final : public phicore::adapter::sdk::AdapterSidecar
{
public:
    Z2mSidecar();

protected:
    void onConnected() override;
    void onDisconnected() override;
    void onBootstrap(const phicore::adapter::sdk::BootstrapRequest &request) override;
    void onConfigChanged(const phicore::adapter::sdk::ConfigChangedRequest &request) override;

    phicore::adapter::v1::CmdResponse onChannelInvoke(
        const phicore::adapter::sdk::ChannelInvokeRequest &request) override;
    phicore::adapter::v1::ActionResponse onAdapterActionInvoke(
        const phicore::adapter::sdk::AdapterActionInvokeRequest &request) override;
    phicore::adapter::v1::CmdResponse onDeviceNameUpdate(
        const phicore::adapter::sdk::DeviceNameUpdateRequest &request) override;
    phicore::adapter::v1::CmdResponse onDeviceEffectInvoke(
        const phicore::adapter::sdk::DeviceEffectInvokeRequest &request) override;
    phicore::adapter::v1::CmdResponse onSceneInvoke(
        const phicore::adapter::sdk::SceneInvokeRequest &request) override;

    phicore::adapter::v1::Utf8String displayName() const override;
    phicore::adapter::v1::Utf8String description() const override;
    phicore::adapter::v1::Utf8String iconSvg() const override;
    phicore::adapter::v1::Utf8String apiVersion() const override;
    int timeoutMs() const override;
    int maxInstances() const override;
    phicore::adapter::v1::AdapterCapabilities capabilities() const override;
    phicore::adapter::v1::JsonText configSchemaJson() const override;

private:
    using CmdResponse = phicore::adapter::v1::CmdResponse;
    using ActionResponse = phicore::adapter::v1::ActionResponse;
    using CmdStatus = phicore::adapter::v1::CmdStatus;

    void wireRuntimeSignals();
    void applyRuntimeConfig(const phicore::adapter::sdk::ConfigChangedRequest &request);

    CmdResponse waitCmdResponse(std::uint64_t cmdId,
                                const std::function<void()> &invoke,
                                int timeoutMs = 15000);
    ActionResponse waitActionResponse(std::uint64_t cmdId,
                                      const std::function<void()> &invoke,
                                      int timeoutMs = 15000);

    ActionResponse invokeProbe(std::uint64_t cmdId, const QJsonObject &params);

    CmdResponse makeFailure(std::uint64_t cmdId, CmdStatus status, const QString &message) const;
    CmdResponse makeSuccess(std::uint64_t cmdId) const;
    ActionResponse makeActionFailure(std::uint64_t cmdId, CmdStatus status, const QString &message) const;
    ActionResponse makeActionSuccess(std::uint64_t cmdId,
                                     const phicore::adapter::v1::ScalarValue &result = std::monostate{}) const;

    static std::int64_t nowMs();

    phicore::adapter::Z2mAdapter m_runtime;
    phicore::adapter::v1::Adapter m_runtimeAdapter;
    QJsonObject m_runtimeMeta;
    QJsonObject m_staticConfig;
    bool m_connected = false;
    bool m_started = false;
};

} // namespace phicore::z2m::ipc
