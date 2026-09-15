#include "UnlockPolicy.h"

#include <QStringList>

namespace WinEase::Common {

namespace {

/// 提权助手的映像名。结束它会让"需要管理员的功能"整段时间不可用，
/// 而且它本身就是 WinEase 的一部分 —— 用户不该从这里杀掉自己程序的两个进程。
const QString kHelperName = QStringLiteral("wineasehelper.exe");
/// 主程序映像名（正常情况下主程序不会出现在占用者列表里，但万一出现也不许杀）
const QString kMainExeName = QStringLiteral("winease.exe");

/// system-owned 的进程名单（**这一小份名单是"不该结束"的真正依据**）。
///
/// 为什么用工整的名单而不是某个 API 的返回值：
///   * `RmCritical` 不可靠（见头文件第 1 条，它把查询方自己也标进去）；
///   * 但这几个名字几十年没变过，而且它们同时也是**受保护进程** ——
///     放行也杀不掉，只会得到一句"拒绝访问"。
///     名单的价值就是把那句"拒绝访问"翻译成"这是操作系统的组成部分，不能结束"。
const QStringList &systemProcessNames()
{
    static const QStringList names = {
        QStringLiteral("system"),
        QStringLiteral("idle"),
        QStringLiteral("secure system"),
        QStringLiteral("registry"),
        QStringLiteral("memory compression"),
        QStringLiteral("smss.exe"),
        QStringLiteral("csrss.exe"),
        QStringLiteral("wininit.exe"),
        QStringLiteral("services.exe"),
        QStringLiteral("lsass.exe"),
        QStringLiteral("winlogon.exe"),
        QStringLiteral("fontdrvhost.exe"),
    };
    return names;
}

bool sameName(const QString &name, const QString &expected)
{
    return !name.isEmpty() && name.compare(expected, Qt::CaseInsensitive) == 0;
}

} // namespace

bool isSystemProcessName(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (const QString &candidate : systemProcessNames()) {
        if (name.compare(candidate, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString lockerRoleText(const WinEase::Win32::FileLocker &locker, quint32 selfPid)
{
    if (locker.pid == selfPid) {
        return QStringLiteral("WinEase 自身");
    }
    if (sameName(locker.name, kHelperName) || sameName(locker.name, kMainExeName)) {
        return QStringLiteral("WinEase");
    }
    if (isSystemProcessName(locker.name)) {
        return QStringLiteral("系统进程");
    }
    if (locker.isService) {
        return locker.serviceName.isEmpty()
                   ? QStringLiteral("系统服务")
                   : QStringLiteral("系统服务（%1）").arg(locker.serviceName);
    }
    if (locker.isExplorer) {
        return QStringLiteral("资源管理器");
    }
    if (locker.isElevated) {
        return QStringLiteral("管理员权限程序");
    }
    if (!locker.sameSession) {
        return QStringLiteral("其它用户会话");
    }
    return QStringLiteral("普通程序");
}

QString protectedReason(const WinEase::Win32::FileLocker &locker, quint32 selfPid)
{
    if (locker.pid == selfPid) {
        return QStringLiteral("它是 WinEase 自己 —— 结束自己会让程序当场消失，还会丢掉还没落盘的设置。"
                              "要解除占用请先关掉对应的窗口或功能。");
    }
    if (sameName(locker.name, kHelperName) || sameName(locker.name, kMainExeName)) {
        return QStringLiteral("它是 WinEase 的一部分（%1）—— 结束后需要管理员的功能会暂时不可用。")
            .arg(locker.name);
    }
    if (isSystemProcessName(locker.name)) {
        return QStringLiteral("它是操作系统的组成部分（%1）—— 这是受保护进程，"
                              "系统不允许结束它，强行结束也可能让系统不稳定。")
            .arg(locker.name);
    }
    return QString();
}

bool needsElevation(const WinEase::Win32::FileLocker &locker)
{
    return locker.isElevated || !locker.sameSession;
}

bool isDisruptive(const WinEase::Win32::FileLocker &locker)
{
    return locker.isExplorer;
}

bool systemSaysCannotShutDown(const WinEase::Win32::FileLocker &locker)
{
    return locker.cannotShutdown;
}

QString terminationWarning(const QList<WinEase::Win32::FileLocker> &lockers)
{
    if (lockers.isEmpty()) {
        return QString();
    }

    QStringList names;
    bool hasService = false;
    bool hasDisruptive = false;
    bool hasCannotShutdown = false;
    bool hasUnsavedRisk = false;
    int count = 0;
    for (const WinEase::Win32::FileLocker &locker : lockers) {
        const QString who = locker.name.isEmpty() ? QStringLiteral("PID %1").arg(locker.pid)
                                                  : locker.name;
        if (!names.contains(who)) {
            names << who;
        }
        ++count;
        hasService = hasService || locker.isService;
        hasDisruptive = hasDisruptive || isDisruptive(locker);
        hasCannotShutdown = hasCannotShutdown || locker.cannotShutdown;
        // 有窗口的程序才谈得上"未保存的内容"；服务与系统进程谈的是系统稳定性
        hasUnsavedRisk = hasUnsavedRisk
                         || (!locker.isService && !isSystemProcessName(locker.name));
    }

    QStringList notes;
    if (hasUnsavedRisk) {
        notes << QStringLiteral("被结束的程序里如果有未保存的内容，**会丢**");
    }
    if (hasService) {
        notes << QStringLiteral("其中有系统服务，结束它可能影响正在运行的系统功能");
    }
    if (hasDisruptive) {
        notes << QStringLiteral("其中有资源管理器，任务栏与桌面会短暂消失（系统通常会自动重启它）");
    }
    if (hasCannotShutdown) {
        notes << QStringLiteral("其中有的被系统标为「需要重启才能释放」（可能只是权限不足，"
                                "也可能它不接受被关闭）");
    }
    notes << QStringLiteral("这些程序正在使用该文件，结束它们可能造成数据损坏");

    return QStringLiteral("将结束 %1 个进程（%2）：%3。")
        .arg(count)
        .arg(names.join(QStringLiteral("、")))
        .arg(notes.join(QStringLiteral("；")));
}

} // namespace WinEase::Common
