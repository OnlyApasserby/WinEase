#pragma once

// ============================================================================
//  WindowTarget.h —— "目标窗口"解析原语
//
//  为什么需要它：
//      窗口管理类功能（置顶 / 分屏 / 透明度 / 置底 …）都要先回答同一个问题：
//      **"这次操作作用在哪个窗口上？"**
//      这个问题的答案与具体功能无关，但如果四个插件各写一份，行为一定对不上
//      （有的跟随鼠标、有的用前台窗口；有的会误伤自己或桌面）。因此把它做成
//      无状态纯函数放进平台层，由主程序与各插件共同链接。
//
//  两种解析模式：
//      FollowCursor —— 光标下的窗口（不需要先激活窗口，最符合直觉）
//      LockedWindow —— 事先锁定的窗口（用于反复调节同一个窗口，例如逐步降透明度）
//
//  ⚠ 坐标一律物理像素：本进程为 per-monitor DPI aware，GetCursorPos 返回的
//      就是物理像素，可直接交给 topLevelWindowAt()。
// ============================================================================

#include <QPoint>
#include <QString>

#include "win32/WindowUtils.h"

namespace WinEase::Win32 {

/// 目标窗口的解析模式
enum class WindowTargetMode {
    FollowCursor = 0, ///< 跟随鼠标下的窗口
    LockedWindow      ///< 使用锁定窗口
};

/// 光标位置（物理像素、屏幕坐标）
QPoint cursorPosition();

/// 该窗口是否适合作为"窗口管理"类功能的操作目标。
/// 在 isManageableWindow() 的基础上再排除最小化窗口（最小化时几何信息没有意义）。
bool isAcceptableTarget(WindowHandle hwnd);

/// 解析目标窗口；找不到合适目标时返回 nullptr。
///
/// 细节（有意为之，四个插件共享同一语义）：
///   * LockedWindow 模式下只认锁定窗口，锁定窗口失效（已关闭）时返回 nullptr
///   * FollowCursor 模式下取光标下窗口；若它不可用（例如光标正压在 WinEase 自己的
///     界面上、或压在桌面/任务栏上），回退到前台窗口 —— 否则用户会觉得"按了没反应"
WindowHandle resolveWindowTarget(WindowTargetMode mode, WindowHandle lockedWindow);

/// 配置持久化用的稳定键名（"follow_cursor" / "locked_window"）
QString targetModeKey(WindowTargetMode mode);
WindowTargetMode targetModeFromKey(const QString &key, WindowTargetMode fallback);

} // namespace WinEase::Win32
