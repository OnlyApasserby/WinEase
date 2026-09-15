#pragma once

// ============================================================================
//  ProcessUtils.h —— 进程查询与操作
//
//  用途（对应路线图中多个功能）：
//      * 端口占用查看（PID → 进程名/路径）
//      * 文件锁定解除（找出占用文件的进程）
//      * 命令面板（"关闭所有记事本"）
//      * 窗口管理（识别窗口归属进程，跳过自身）
//
//  权限说明：
//      读取进程路径需要 PROCESS_QUERY_LIMITED_INFORMATION（普通权限通常够用）；
//      结束**系统进程**或**其它用户会话**的进程需要管理员权限，
//      这类调用应经由 WinEaseHelper 提权助手执行。
// ============================================================================

#include <QList>
#include <QString>

namespace WinEase::Win32 {

/// 进程基础信息
struct ProcessInfo {
    quint32 pid = 0;
    quint32 parentPid = 0;
    QString name;      ///< 映像名，如 "notepad.exe"
    QString path;      ///< 完整路径；无权限读取时为空
    /// 名称去扩展名后的部分（便于模糊匹配）
    QString baseName() const;
};

/// 进程完整路径（失败返回空串）
QString processPath(quint32 pid);

/// 进程映像名，如 "notepad.exe"（失败返回空串）
QString processName(quint32 pid);

/// 进程的父进程 PID（失败返回 0）
quint32 parentProcessId(quint32 pid);

/// 枚举全部进程（按 PID 升序）
QList<ProcessInfo> processes();

/// 判断进程是否仍在运行
bool isProcessRunning(quint32 pid);

/// 判断目标进程是否以管理员权限运行（读取失败时返回 false）
bool isProcessElevated(quint32 pid);

/// 按名称模糊查找（忽略大小写，可省略 .exe 后缀）
QList<ProcessInfo> findProcessesByName(const QString &nameFragment);

/// 结束进程
/// @param exitCode 退出码，默认 1
/// @param errorOut 失败原因（中文）
bool terminateProcess(quint32 pid, quint32 exitCode = 1, QString *errorOut = nullptr);

/// 当前进程信息
quint32 currentProcessId();
QString currentProcessPath();

/// 当前进程所在目录（结尾不带分隔符）
QString currentProcessDirectory();

} // namespace WinEase::Win32
