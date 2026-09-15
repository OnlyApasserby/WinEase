#pragma once

// ============================================================================
//  ElevationClient.h —— 提权助手客户端（实现 SDK 契约 ElevationService）
//
//  职责：
//    * 探测/拉起 WinEaseHelper.exe（requireAdministrator 清单 → UAC 只弹一次，
//      helper 常驻，之后所有请求直接走管道）
//    * 同步执行一次 JSON 行协议请求-应答
//
//  失败语义（面向用户，中文、可行动）：
//    * 用户取消 UAC → "用户取消了管理员授权（UAC）"
//    * helper 未启动且拉起失败 → 带错误码的说明
//    * helper 拒绝 → 原样带回 helper 的中文原因（白名单/校验类拒绝）
// ============================================================================

#include "sdk/ElevationService.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace WinEase {

class ElevationClient : public QObject, public ElevationService
{
    Q_OBJECT

public:
    explicit ElevationClient(QObject *parent = nullptr);
    ~ElevationClient() override;

    // ---- ElevationService 契约 ----
    ElevationResult execute(const QString &operation, const QVariantMap &arguments) override;
    bool isHelperAvailable() override;
    QString statusText() override;

private:
    /// 确保 helper 在运行；不在时发起 UAC 拉起并等待管道出现
    bool ensureHelperRunning(QString *errorOut);

    /// 发送一次请求并等待应答（调用前必须已 ensureHelperRunning）
    ElevationResult sendRequest(const QString &operation,
                                const QVariantMap &arguments,
                                QString *errorOut);

    /// 上一次拉起失败的时刻（短时间内的重复调用快速失败，避免反复弹 UAC）
    qint64 m_lastLaunchFailMs = 0;
};

} // namespace WinEase
