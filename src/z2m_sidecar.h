#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <QJsonObject>

#include "z2madapter.h"
#include "phi/adapter/sdk/sidecar.h"

namespace phicore::z2m::ipc {

class Z2mSidecar final : public phicore::adapter::sdk::AdapterInstance
{
public:
    Z2mSidecar();

protected:
    bool start() override;
    void stop() override;

    void onConnected() override;
    void onDisconnected() override;
    void onConfigChanged(const phicore::adapter::sdk::ConfigChangedRequest &request) override;

    void onChannelInvoke(
        const phicore::adapter::sdk::ChannelInvokeRequest &request) override;
    void onAdapterActionInvoke(
        const phicore::adapter::sdk::AdapterActionInvokeRequest &request) override;
    void onDeviceNameUpdate(
        const phicore::adapter::sdk::DeviceNameUpdateRequest &request) override;
    void onDeviceEffectInvoke(
        const phicore::adapter::sdk::DeviceEffectInvokeRequest &request) override;
    void onSceneInvoke(
        const phicore::adapter::sdk::SceneInvokeRequest &request) override;

private:
    static constexpr int kDefaultTimeoutMs = 15000;

    using CmdResponse = phicore::adapter::v1::CmdResponse;
    using ActionResponse = phicore::adapter::v1::ActionResponse;
    using CmdStatus = phicore::adapter::v1::CmdStatus;

    void submitCmdResult(CmdResponse response, const char *context);
    void submitActionResult(ActionResponse response, const char *context);

    void wireRuntimeSignals();
    void applyRuntimeConfig(const phicore::adapter::sdk::ConfigChangedRequest &request);
    bool ensureRuntime();

    CmdResponse waitCmdResponse(std::uint64_t cmdId,
                                const std::function<void()> &invoke,
                                int timeoutMs = 15000);
    ActionResponse waitActionResponse(std::uint64_t cmdId,
                                      const std::function<void()> &invoke,
                                      int timeoutMs = 15000);

    CmdResponse makeFailure(std::uint64_t cmdId, CmdStatus status, const QString &message) const;
    ActionResponse makeActionFailure(std::uint64_t cmdId, CmdStatus status, const QString &message) const;

    static std::int64_t nowMs();

    std::unique_ptr<phicore::adapter::Z2mAdapter> m_runtime;
    phicore::adapter::v1::Adapter m_runtimeAdapter;
    QJsonObject m_runtimeMeta;
    QJsonObject m_staticConfig;
    bool m_started = false;
};

} // namespace phicore::z2m::ipc
