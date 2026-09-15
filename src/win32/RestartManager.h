#pragma once

// ============================================================================
//  RestartManager.h —— 查询"谁占用了这个文件"
//
//  给谁用：P3-02 文件锁定解除（file.unlock）。
//
//  ---------------------------------------------------------------------------
//  ⚠ 为什么用 Restart Manager 而不是自己扫句柄表
//
//  "删不掉文件，想知道是谁在用"是 Windows 上的老问题。常见做法有三条：
//    ① 自己 `NtQuerySystemInformation` 扫全系统句柄 + 反查名字 —— 免提权做不全，
//       而且句柄表随时在变，扫描结果天然是"拍脑袋的瞬间快照"；
//    ② 调 `handle.exe` / `Process Explorer` 之类的第三方工具 —— 引入外部依赖；
//    ③ **Restart Manager**（`rstrtmgr.dll`，Vista 起内置）—— 这正是 Windows
//       安装程序用来判断"要重启才能替换哪些文件"的那套机制，
//       它由**系统自己**维护"谁打开了哪个文件"，**免提权、只读、零依赖**。
//  预研已在本机完整自证：自己独占一个文件 → `RmGetList` 报出的占用者就是自己
//  （应用名 + PID 都对得上），全程普通权限。
//
//  ---------------------------------------------------------------------------
//  ⚠ Restart Manager **看不到**的东西（必须如实告诉用户，别让人以为"没人占用"）
//
//  RM 只会报告"打开了该文件的**进程**"。以下情况它报不出来：
//    * 纯内核态占用（文件系统过滤驱动、杀软实时扫描、Windows 索引服务）；
//    * 内存映射/预览缓存等不走普通文件句柄的路径；
//    * 已被删除但仍被打开的文件（路径都还在，但句柄挂在"已删除"的文件上）。
//  所以"列表为空"的正确说法是"**没有进程**被查出来占用它"，而不是"没人用它"。
//
//  ⚠ 本模块**只查、不动**：不结束任何进程、不删任何文件，全部调用都是只读的。
//
//  ⚠ 平台层约定：只依赖 Qt6::Core（QString/QList），只放无状态原语。
// ============================================================================

#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 一个"正在占用某个文件"的进程（数据来自 Restart Manager）
struct FileLocker {
    quint32 pid = 0;
    /// 应用名（RM 报的，通常是映像名如 "notepad.exe"；服务占用时为服务显示名）
    QString name;
    /// 完整映像路径（另查 `ProcessUtils`；系统进程/其它会话读不到时为空）
    QString path;
    /// 服务短名（`ApplicationType == RmService` 时非空）
    QString serviceName;

    /// `RM_APP_TYPE` 原值（`RmMainWindow=1` / `RmOtherWindow=2` / `RmService=3` /
    /// `RmExplorer=4` / `RmConsole=5` / `RmCritical=1000` / `RmUnknownApp=0`）
    int applicationType = 0;
    /// ★ 系统认为"这个进程关不掉、要重启系统才能释放它占用"（`RmCritical` = 1000）。
    ///
    /// ⚠⚠ **它不是"系统关键进程"的判据**，别拿它当"不许结束"的依据。官方定义里
    /// `RmCritical` 表达的是一句结论——"装不下去了，得重启"，而它的成因有三种：
    ///   ① 它确实是关键进程；② **当前进程没权限关它**；③ **它就是发起查询的这个程序自己**
    ///      （Restart Manager 不可能建议"把你自己关掉"）。
    /// 实测证据（`tests/feature_smoke` P3-02 段，本机）：同一份 `feature_smoke.exe`，
    /// 作为**查询方**时被标成 `type=1000`，作为**普通子进程**时被标成 `type=5`（控制台程序）。
    /// 所以本字段只用来**如实告诉用户"系统认为它关不掉"**，保护规则另有依据
    /// （见 `plugins/common/UnlockPolicy`）。
    bool cannotShutdown = false;
    /// 是个 Windows 服务（杀服务的后果比杀普通程序重得多）
    bool isService = false;
    /// 资源管理器（结束它任务栏与桌面会短暂消失，系统通常会自动重启它）
    bool isExplorer = false;
    /// 目标进程以管理员身份运行（普通权限杀不掉，要走提权助手）
    bool isElevated = false;
    /// 与当前进程处于**同一登录会话**（其它会话的进程要走提权助手）
    bool sameSession = true;
    /// 系统认为它可以在重启/恢复后自动回到原状态（纯展示用）
    bool restartable = false;

    /// 界面/日志用的一句话描述
    QString describe() const;
};

/// 查询占用指定文件的进程（**免提权、只读**）。
/// @param path 必须是带盘符的绝对路径（Restart Manager 的硬要求）
/// @return 占用者列表（按 PID 去重）；查询本身失败时返回空列表并在 errorOut 给出中文原因。
///         ⚠ "列表为空 + errorOut 为空" = 没有**进程**在占用（但可能是驱动级占用）。
QList<FileLocker> findFileLockers(const QString &path, QString *errorOut = nullptr);

/// 批量查询（例如"这个目录里所有文件"）；结果按 PID 去重并保持稳定顺序
QList<FileLocker> findFileLockers(const QStringList &paths, QString *errorOut = nullptr);

/// Restart Manager / 文件查询的错误码 → 中文（面向用户，可直接展示）
QString restartManagerError(unsigned long errorCode);

/// `RM_APP_TYPE` 原值 → 中文（"图形程序" / "控制台程序" / "服务" / …）
QString appTypeText(int applicationType);

} // namespace WinEase::Win32
