#pragma once

// ============================================================================
//  PowerUtils.h —— 会话/电源相关的平台原语
//
//  为什么"取消"要单独放在平台层、而且不经过提权助手：
//      发起关机/重启走提权助手（D2 决策：特权操作集中在一处、可审计），
//      但**取消**必须是最后一道保险 —— 机器正在倒计时关闭的时候，
//      那道保险不能依赖"另一个进程还活着、管道还通、UAC 还没过期"。
//      取消不需要管理员权限（本地交互式登录用户持有 SeShutdownPrivilege，
//      只是默认禁用，启用一次即可），所以它就该是一个本地调用。
//
//  ⚠ 本文件不含任何状态，也不做重试/日志（与平台层其它模块一致）。
// ============================================================================

#include <QString>

namespace WinEase::Win32 {

/// 锁定当前会话（`LockWorkStation`，普通权限即可，不依赖提权助手）
bool lockWorkstation(QString *errorOut = nullptr);

/// 本机令牌是否**持有** SeShutdownPrivilege（持有 ≠ 已启用；仅用于界面说明）
bool hasShutdownPrivilege();

/// 取消挂起的系统关机/重启，并如实告知"取消之前到底有没有挂起"。
///
/// 调用本身成功即返回 true，"本来就没有挂起"也算成功（`hadPendingOut = false`）——
/// 因为对调用方而言"现在没有挂起的关机了"这个结论是成立的。
/// @param hadPendingOut 取消前是否真有挂起（可用于自检"确实取消掉了一次真实请求"）
bool abortPendingShutdown(bool *hadPendingOut = nullptr, QString *errorOut = nullptr);

/// 查询是否存在挂起的关机/重启。
///
/// ⚠ 这个查询**有副作用**：Windows 没有"只查不取消"的接口，实现是调一次
/// `AbortSystemShutdown`，所以"查到有"就等于"顺手取消了"。只允许用在
/// "我要确认没有挂起"的场景（例如自检收尾），不要用它做界面轮询。
bool hasPendingShutdown(bool *pendingOut, QString *errorOut = nullptr);

} // namespace WinEase::Win32
