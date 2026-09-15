#pragma once

// ============================================================================
//  AdminHelper.h —— 管理员权限检测与提权
//
//  被标记 requiresAdmin() 的插件在主程序未提权时会被拒绝启用，
//  并在界面上给出明确提示（而不是静默失败）。
// ============================================================================

#include <QString>

namespace WinEase::Admin {

/// 当前进程是否已提权（TokenElevation）
bool isProcessElevated();

/// 当前用户是否属于 Administrators 组（未提权但具备提权可能）
bool isUserInAdminGroup();

/// 生成权限状态的中文描述，用于状态栏展示
QString elevationDescription();

/// 以管理员身份重新启动当前程序
/// @param arguments 附加命令行参数
/// @return 用户确认并成功启动返回 true；用户取消 UAC 返回 false
bool restartAsElevated(const QString &arguments = QString());

/// 当前登录用户名（"DOMAIN\\User"）
QString currentUserName();

} // namespace WinEase::Admin
