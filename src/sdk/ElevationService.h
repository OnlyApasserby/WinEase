#pragma once

// ============================================================================
//  ElevationService.h —— 提权操作通道契约（P0-4）
//
//  背景：主程序保持普通权限运行（D2 决策），需要管理员权限的操作
//  （写 hosts、结束系统进程、改 HKLM 环境变量、电源操作等）
//  统一交给常驻的 WinEaseHelper.exe（管理员权限，UAC 只在首次弹一次）执行。
//
//  安全边界（必须与 helper 端一致，缺一不可）：
//    * helper 只接受**白名单内**的操作，白名单外的请求直接拒绝
//    * helper 通过命名管道对端的真实 PID（内核元数据，客户端无法谎报）
//      反查调用方 exe，要求与 helper 同目录，且签名有效
//    * 本接口只是"通道"，不做权限校验——校验全部在 helper 端完成
//      （即使主程序被劫持，多一道白名单也拦得住）
//
//  插件不得自己 ShellExecute 提权进程，一律经由本接口。
// ============================================================================

#include <QVariantMap>
#include <QString>

namespace WinEase {

/// 一次提权请求的结果
struct ElevationResult {
    bool ok = false;
    QString error;      ///< ok == false 时的中文原因（面向用户，可直接展示）
    QVariantMap data;   ///< 成功时的附加数据（各操作自定义，见 ROADMAP P0-4）

    bool isSuccess() const { return ok; }
};

class ElevationService
{
public:
    ElevationService() = default;
    virtual ~ElevationService() = default;

    ElevationService(const ElevationService &) = delete;
    ElevationService &operator=(const ElevationService &) = delete;

    /// 同步执行一次提权操作（白名单见 HelperOps.cpp 中的 dispatch()）。
    /// 首次调用时若 helper 未运行会发起 UAC 授权请求（用户取消则返回失败并说明原因）。
    /// ⚠ 同步接口：等待期间主线程会处理事件（否则 UAC 等待会冻结界面）。
    virtual ElevationResult execute(const QString &operation, const QVariantMap &arguments) = 0;

    /// 提权助手是否可用（只探测管道，**不会**触发 UAC）
    virtual bool isHelperAvailable() = 0;

    /// 中文状态描述（设置界面/状态栏显示用）
    virtual QString statusText() = 0;
};

} // namespace WinEase
