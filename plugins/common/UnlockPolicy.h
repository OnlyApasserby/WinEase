#pragma once

// ============================================================================
//  UnlockPolicy.h —— "这个占用者该不该被结束"的判定（全纯函数）
//
//  给谁用：P3-02 文件锁定解除（`file.unlock`）插件 + `tests/feature_smoke`。
//  插件与自检**共用同一份源码**：这类"能不能杀"的判断一旦两边各写一份，
//  自检验的就不是用户跑的那套规则了。
//
//  ---------------------------------------------------------------------------
//  四条纪律（本文件存在的理由）
//
//   1. **有些进程绝对不许结束**，而且理由要具体 —— 判断依据是
//      「**它是不是操作系统的组成部分**」，不是「系统 API 怎么说」：
//      * WinEase 自己与提权助手（结束自己会让程序当场消失 / 让需要管理员的功能失效）；
//      * 一小份**system-owned 的进程名单**（`System`/`smss.exe`/`csrss.exe`/`lsass.exe`…）。
//        ⚠ 这些进程即便放行，`TerminateProcess` 也会被系统拒绝（它们是受保护进程）；
//        名单的意义是**把"拒绝访问"翻译成人话**，而不是另立一套权限。
//
//      ⚠⚠ **不要拿 Restart Manager 的 `RmCritical`(=1000) 当"关键进程"判据**：
//      它的官方含义是"这句结论是『装不下去、得重启』"，成因有三种 ——
//      ① 它确实是关键进程；② 当前进程没权限关它；③ **它就是发起查询的这个程序自己**。
//      本机实测：同一份 `feature_smoke.exe`，作为**查询方**被标 `1000`，
//      作为**普通子进程**被标 `5`。拿它当"禁止结束"会让普通程序变成杀不掉
//      （P3-02 自检一开始就是这么红的）。它只配一句**如实提示**，见
//      `systemSaysCannotShutDown()`。
//
//   2. **有些进程必须走提权助手**：以管理员身份运行的、以及其它登录会话里的进程。
//      普通权限 `TerminateProcess` 对它们只会拿到"拒绝访问"，
//      而"拒绝访问"对用户来说是个死胡同（他没法知道该怎么做）。
//
//   3. **有些进程结束掉会让用户看到明显变化**：资源管理器（任务栏与桌面会短暂消失）。
//      这不禁止，但必须在界面上**提前说出来**。
//
//   4. **风险要提前说，而不是事后解释**：二次确认的文案由 `terminationWarning()`
//      生成，它必须说到点子上（会丢什么、会看到什么变化）——
//      只说"确定要结束吗"的确认框等于没确认。
//
//  ⚠ 本头文件不含 Q_OBJECT（与 TextTools.h / WindowFeatureState.h 同理）。
// ============================================================================

#include "win32/RestartManager.h"

#include <QString>

namespace WinEase::Common {

/// 占用者的身份标签（界面列表里那个短后缀）
/// @param selfPid 当前进程 PID（用来识别"这就是 WinEase 自己"）
QString lockerRoleText(const WinEase::Win32::FileLocker &locker, quint32 selfPid);

/// **绝对不允许结束**时返回中文原因；允许结束时返回空串。
/// 调用方（插件与自检）都必须先问这一句，再谈"能不能杀得掉"。
QString protectedReason(const WinEase::Win32::FileLocker &locker, quint32 selfPid);

/// 它是不是"操作系统自己的进程"（`System`/`smss.exe`/`csrss.exe`/`lsass.exe` 这一小份名单）
bool isSystemProcessName(const QString &name);

/// 结束它是否**必须**经提权助手（以管理员身份运行 / 其它用户会话）
bool needsElevation(const WinEase::Win32::FileLocker &locker);

/// 结束它会不会让用户看到明显的系统变化（资源管理器）——不禁止，但要提前说
bool isDisruptive(const WinEase::Win32::FileLocker &locker);

/// 系统认为它"关不掉、要重启才能释放"（RM 的 `RmCritical`）。
/// ⚠ 这**不是**"不许结束"的依据，只是一句要如实告诉用户的提示
/// （原因可能只是"没权限"或"它就是发起查询的程序"）
bool systemSaysCannotShutDown(const WinEase::Win32::FileLocker &locker);

/// 结束前的风险提示语（对"要杀的那一批"整体说一句话）
QString terminationWarning(const QList<WinEase::Win32::FileLocker> &lockers);

} // namespace WinEase::Common
