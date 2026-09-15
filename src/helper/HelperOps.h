#pragma once

// ============================================================================
//  HelperOps.h —— 提权操作白名单（P0-4）
//
//  helper 只执行这里列出的操作，白名单外的请求一律拒绝。
//  每个操作都必须做参数校验 + 大小上限，防恶意客户端灌内存 / 注入。
// ============================================================================

#include <QString>
#include <QVariantMap>

namespace WinEase::Helper {

struct OpResult {
    bool ok = false;
    QString error;      ///< 失败时的中文原因
    QVariantMap data;   ///< 成功时的附加数据

    static OpResult success(const QVariantMap &data = QVariantMap())
    {
        OpResult r;
        r.ok = true;
        r.data = data;
        return r;
    }

    static OpResult failure(const QString &reason)
    {
        OpResult r;
        r.ok = false;
        r.error = reason;
        return r;
    }
};

/// 白名单操作分发器（无状态，可安全地被多个连接复用）
class OpDispatcher
{
public:
    /// @param callerPid 已通过安全校验的调用方 PID（供"禁止自杀"等规则使用）
    OpResult dispatch(const QString &op, const QVariantMap &args, quint32 callerPid);
};

} // namespace WinEase::Helper
