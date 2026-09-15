#include "HelperOps.h"

#include "win32/ProcessUtils.h"
#include "win32/RegistryUtils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <windows.h>
#include <powrprof.h>   // SetSuspendState（睡眠/休眠）

#include "HelperLog.h"

namespace WinEase::Helper {

namespace {

// ---------------------------------------------------------------------------
// 参数硬上限（防恶意客户端灌内存 / 注入换行）
// ---------------------------------------------------------------------------
constexpr int kMaxHostsLineChars = 4096;      // 单条 hosts 记录
constexpr qint64 kMaxHostsBytes = 1 << 20;    // hosts 整体替换上限 1 MiB
constexpr int kMaxEnvNameChars = 255;         // 环境变量名（Windows 硬限 32767，这里收紧）
constexpr qint64 kMaxEnvValueBytes = 32 * 1024;

/// hosts 文件路径。helper 是 64 位进程，System32 不会被重定向
QString hostsFilePath()
{
    const QString systemRoot =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    return QDir(systemRoot).filePath(QStringLiteral("System32/drivers/etc/hosts"));
}

/// 修改 hosts 前先备份原文件（调用方可通过返回值找到备份位置）
QString backupHostsCopy(QString *errorOut)
{
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/hosts-backup");
    if (!QDir().mkpath(baseDir)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法创建 hosts 备份目录: ") + baseDir;
        }
        return QString();
    }

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString target = baseDir + QStringLiteral("/hosts-") + stamp + QStringLiteral(".bak");
    if (!QFile::copy(hostsFilePath(), target)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("hosts 备份失败（写不到 ") + target + QStringLiteral("）");
        }
        return QString();
    }
    return target;
}

/// 结束进程操作的黑名单：这些进程被结束会直接蓝屏/失控，无论参数怎么写都拒绝
bool isCriticalSystemImage(const QString &image)
{
    static const QSet<QString> kCritical{
        QStringLiteral("system"),
        QStringLiteral("registry"),        // Win10 1709+ 的注册表进程
        QStringLiteral("smss.exe"),
        QStringLiteral("csrss.exe"),
        QStringLiteral("wininit.exe"),
        QStringLiteral("winlogon.exe"),
        QStringLiteral("services.exe"),
        QStringLiteral("lsass.exe"),
        QStringLiteral("memcompression"),  // 内存压缩
    };
    return kCritical.contains(image.toLower());
}

/// 启用当前进程令牌上的某个特权（电源操作需要 SeShutdownPrivilege）
bool enablePrivilege(LPCWSTR privilegeName, QString *errorOut)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("打开进程令牌失败（错误码 %1）").arg(::GetLastError());
        }
        return false;
    }

    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        ::CloseHandle(token);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("查询特权 %1 失败（错误码 %2）")
                            .arg(QString::fromWCharArray(privilegeName))
                            .arg(::GetLastError());
        }
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL ok = ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    const DWORD adjustError = ::GetLastError();
    ::CloseHandle(token);

    if (ok == FALSE || adjustError == ERROR_NOT_ALL_ASSIGNED) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("当前令牌不具备特权 %1")
                            .arg(QString::fromWCharArray(privilegeName));
        }
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// ping：健康检查 + 自证身份（自检用 elevated == true 证明 requireAdministrator 生效）
// ---------------------------------------------------------------------------
namespace {

OpResult opPing()
{
    // TokenElevation：helper 自己是否有管理员令牌
    bool elevated = false;
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        if (::GetTokenInformation(token, TokenElevation, &elevation,
                                  sizeof(elevation), &returned) == TRUE) {
            elevated = elevation.TokenIsElevated != 0;
        }
        ::CloseHandle(token);
    }

    QVariantMap data;
    data.insert(QStringLiteral("version"), QStringLiteral(WINEASE_APP_VERSION));
    data.insert(QStringLiteral("pid"), static_cast<quint32>(::GetCurrentProcessId()));
    data.insert(QStringLiteral("elevated"), elevated);
    return OpResult::success(data);
}

} // namespace

// ---------------------------------------------------------------------------
// writeHosts：写 hosts（append 单条 / replace 整体，写前自动备份）
// ---------------------------------------------------------------------------
namespace {

OpResult opWriteHosts(const QVariantMap &args)
{
    const QString mode = args.value(QStringLiteral("mode")).toString();

    QFile hostsFile(hostsFilePath());
    if (!hostsFile.open(QIODevice::ReadOnly)) {
        return OpResult::failure(QStringLiteral("无法读取 hosts 文件（错误码 %1）")
                                     .arg(hostsFile.error()));
    }
    const QByteArray original = hostsFile.readAll();
    hostsFile.close();

    if (original.size() > kMaxHostsBytes) {
        return OpResult::failure(QStringLiteral("hosts 文件异常偏大（%1 字节），拒绝写入")
                                     .arg(original.size()));
    }

    QByteArray next;
    if (mode == QStringLiteral("append")) {
        QString line = args.value(QStringLiteral("line")).toString();
        if (line.isEmpty()) {
            return OpResult::failure(QStringLiteral("参数 line 不能为空"));
        }
        if (line.size() > kMaxHostsLineChars) {
            return OpResult::failure(QStringLiteral("参数 line 超过 %1 字符上限")
                                         .arg(kMaxHostsLineChars));
        }
        if (line.contains(QLatin1Char('\n')) || line.contains(QLatin1Char('\r'))) {
            return OpResult::failure(QStringLiteral("参数 line 不允许包含换行符"));
        }
        next = original;
        if (!next.isEmpty() && !next.endsWith('\n')) {
            next += "\r\n";
        }
        next += line.toUtf8();
        next += "\r\n";
    } else if (mode == QStringLiteral("replace")) {
        // 首选 base64：字节级精确还原（JSON 文本通道会剥掉 UTF-8 BOM 等字节，
        // 本次实测原文件恢复后差 3 字节就是这个原因）
        if (args.contains(QStringLiteral("contentBase64"))) {
            next = QByteArray::fromBase64(
                args.value(QStringLiteral("contentBase64")).toString().toLatin1());
        } else {
            const QString content = args.value(QStringLiteral("content")).toString();
            next = content.toUtf8();
        }
        if (next.size() > kMaxHostsBytes) {
            return OpResult::failure(QStringLiteral("替换内容超过 %1 字节上限")
                                         .arg(kMaxHostsBytes));
        }
    } else {
        return OpResult::failure(QStringLiteral("参数 mode 必须是 append 或 replace"));
    }

    QString backupError;
    const QString backupPath = backupHostsCopy(&backupError);
    if (backupPath.isEmpty()) {
        return OpResult::failure(backupError);
    }

    // 修改 hosts 常会触发安全软件的短暂锁定（错误码 5），重试拿回写权限
    bool opened = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (hostsFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            opened = true;
            break;
        }
        if (hostsFile.error() != 5 /* ERROR_ACCESS_DENIED */) {
            break;   // 其它错误重试无意义
        }
        QThread::msleep(250);
    }
    if (!opened) {
        return OpResult::failure(QStringLiteral("无法写入 hosts 文件（错误码 %1，"
                                                "可能被安全软件锁定）").arg(hostsFile.error()));
    }
    const qint64 written = hostsFile.write(next);
    hostsFile.close();

    if (written != next.size()) {
        // 写一半失败：把原内容写回去，绝不能留半个 hosts
        if (hostsFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            hostsFile.write(original);
            hostsFile.close();
        }
        return OpResult::failure(QStringLiteral("hosts 写入不完整，已回滚原内容"));
    }

    qCInfo(lcHelper) << "hosts 已更新, 模式:" << mode
                     << "备份:" << QFileInfo(backupPath).fileName();

    QVariantMap data;
    data.insert(QStringLiteral("backupPath"), backupPath);
    data.insert(QStringLiteral("bytes"), static_cast<qint64>(next.size()));
    return OpResult::success(data);
}

} // namespace

// ---------------------------------------------------------------------------
// hostsBackups：列出 / 读取 hosts 备份（**只读**）
//
//  为什么要在助手侧开这个口子：备份目录在 helper 自己的
//  `%LOCALAPPDATA%\<helper>\hosts-backup` 下，普通权限的插件既猜不到这个路径，
//  也不该为了"看历史版本"去遍历用户的 AppData。**只读**、且文件名被严格校验
//  （只接受本目录下的一个文件名，防目录穿越）。
// ---------------------------------------------------------------------------
namespace {

QString hostsBackupDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/hosts-backup");
}

/// 备份文件名白名单校验：必须**只是一个文件名**（不含路径分隔符 / 盘符 / 交替数据流）
bool isValidBackupName(const QString &name)
{
    if (name.isEmpty() || name.size() > 128) {
        return false;
    }
    if (QFileInfo(name).fileName() != name) {
        return false;
    }
    if (name.contains(QLatin1Char(':')) || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\'))) {
        return false;
    }
    return name.startsWith(QStringLiteral("hosts-")) && name.endsWith(QStringLiteral(".bak"));
}

OpResult opHostsBackups(const QVariantMap &args)
{
    const QString action = args.value(QStringLiteral("action"), QStringLiteral("list")).toString();
    const QString directory = hostsBackupDirectory();

    if (action == QStringLiteral("list")) {
        const QFileInfoList files = QDir(directory).entryInfoList({ QStringLiteral("hosts-*.bak") },
                                                                  QDir::Files,
                                                                  QDir::Time);
        QStringList names;
        QStringList stamps;
        names.reserve(files.size());
        for (const QFileInfo &info : files) {
            names << info.fileName();
            stamps << info.lastModified().toString(Qt::ISODate);
        }

        QVariantMap data;
        data.insert(QStringLiteral("directory"), directory);
        data.insert(QStringLiteral("backups"), names);
        data.insert(QStringLiteral("stamps"), stamps);
        return OpResult::success(data);
    }

    if (action == QStringLiteral("read")) {
        const QString name = args.value(QStringLiteral("name")).toString();
        if (!isValidBackupName(name)) {
            return OpResult::failure(QStringLiteral("备份文件名非法（只接受本目录下的 "
                                                    "hosts-*.bak）"));
        }

        QFile file(QDir(directory).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            return OpResult::failure(QStringLiteral("读取备份失败: %1").arg(file.errorString()));
        }
        const QByteArray bytes = file.readAll();
        if (bytes.size() > kMaxHostsBytes) {
            return OpResult::failure(QStringLiteral("备份文件异常偏大（%1 字节）")
                                         .arg(bytes.size()));
        }

        QVariantMap data;
        data.insert(QStringLiteral("name"), name);
        // base64：字节级精确（hosts 可能带 UTF-8 BOM，文本通道会把它剥掉）
        data.insert(QStringLiteral("contentBase64"), QString::fromLatin1(bytes.toBase64()));
        return OpResult::success(data);
    }

    return OpResult::failure(QStringLiteral("参数 action 必须是 list 或 read"));
}

} // namespace

// ---------------------------------------------------------------------------
// setEnvVar：写/删环境变量（machine = HKLM，user = HKCU）
// ---------------------------------------------------------------------------
namespace {

OpResult opSetEnvVar(const QVariantMap &args)
{
    const QString scope = args.value(QStringLiteral("scope")).toString();
    if (scope != QStringLiteral("machine") && scope != QStringLiteral("user")) {
        return OpResult::failure(QStringLiteral("参数 scope 必须是 machine 或 user"));
    }

    const QString name = args.value(QStringLiteral("name")).toString();
    if (name.isEmpty() || name.size() > kMaxEnvNameChars
            || name.contains(QLatin1Char('='))) {
        return OpResult::failure(QStringLiteral("参数 name 缺失、超长或包含非法字符"));
    }

    const bool isMachine = (scope == QStringLiteral("machine"));
    const Win32::RegistryRoot root = isMachine
        ? Win32::RegistryRoot::LocalMachine
        : Win32::RegistryRoot::CurrentUser;
    const QString subKey = isMachine
        ? QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment")
        : QStringLiteral("Environment");

    // 没带 value 或显式 null → 删除该变量
    const bool remove = !args.contains(QStringLiteral("value"))
                        || args.value(QStringLiteral("value")).isNull();

    QString error;
    if (remove) {
        if (!Win32::deleteValue(root, subKey, name, Win32::RegistryView::Default, &error)) {
            return OpResult::failure(QStringLiteral("删除环境变量失败: ") + error);
        }
    } else {
        const QString value = args.value(QStringLiteral("value")).toString();
        if (value.size() > kMaxEnvValueBytes) {
            return OpResult::failure(QStringLiteral("参数 value 超过 %1 字节上限")
                                         .arg(kMaxEnvValueBytes));
        }
        // 含 %VAR% 引用的值必须写 REG_EXPAND_SZ，否则 PATH 之类会失去展开能力
        const Win32::RegistryValueType type = value.contains(QLatin1Char('%'))
            ? Win32::RegistryValueType::ExpandString
            : Win32::RegistryValueType::String;
        if (!Win32::writeValue(root, subKey, name, value, type,
                               Win32::RegistryView::Default, &error)) {
            return OpResult::failure(QStringLiteral("写入环境变量失败: ") + error);
        }
    }

    // 环境变量改完必须广播，否则已运行的程序要重启才能看到
    Win32::broadcastSettingChange(QStringLiteral("Environment"));

    qCInfo(lcHelper) << "环境变量已" << (remove ? "删除" : "写入")
                     << "范围:" << scope << "名称:" << name;
    return OpResult::success();
}

} // namespace

// ---------------------------------------------------------------------------
// killProcess：结束进程（关键系统进程黑名单 + 禁止自杀/杀调用方）
// ---------------------------------------------------------------------------
namespace {

OpResult opKillProcess(const QVariantMap &args, quint32 callerPid)
{
    bool numericOk = false;
    const quint32 pid = args.value(QStringLiteral("pid")).toUInt(&numericOk);
    if (!numericOk || pid == 0) {
        return OpResult::failure(QStringLiteral("参数 pid 缺失或非法"));
    }
    if (pid == ::GetCurrentProcessId()) {
        return OpResult::failure(QStringLiteral("拒绝结束提权助手自身"));
    }
    if (pid == callerPid) {
        return OpResult::failure(QStringLiteral("拒绝结束调用方进程（退出 WinEase 请使用其界面）"));
    }

    const QString path = Win32::processPath(pid);
    if (path.isEmpty()) {
        // 读不到映像路径就不能放行终止操作（fail-closed）：
        // 关键系统进程（如 System, pid=4）恰恰就是读不出路径的那类
        return OpResult::failure(QStringLiteral("无法读取目标进程的映像路径，"
                                                "为安全起见拒绝结束（pid=%1）").arg(pid));
    }
    const QString image = QFileInfo(path).fileName();
    if (isCriticalSystemImage(image)) {
        return OpResult::failure(QStringLiteral("拒绝结束关键系统进程: ") + image);
    }

    QString error;
    if (!Win32::terminateProcess(pid, 1, &error)) {
        return OpResult::failure(QStringLiteral("结束进程失败: ") + error);
    }

    qCInfo(lcHelper) << "已结束进程, PID:" << pid << "映像:" << image;
    QVariantMap data;
    data.insert(QStringLiteral("pid"), pid);
    data.insert(QStringLiteral("image"), image);
    return OpResult::success(data);
}

} // namespace

// ---------------------------------------------------------------------------
// powerAction：电源操作（lock/logoff/sleep/hibernate/reboot/shutdown）
// ---------------------------------------------------------------------------
namespace {

OpResult opPowerAction(const QVariantMap &args)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    if (action != QStringLiteral("lock") && action != QStringLiteral("logoff")
            && action != QStringLiteral("sleep") && action != QStringLiteral("hibernate")
            && action != QStringLiteral("reboot") && action != QStringLiteral("shutdown")
            && action != QStringLiteral("abort")) {
        return OpResult::failure(QStringLiteral("参数 action 非法：必须是 lock/logoff/"
                                               "sleep/hibernate/reboot/shutdown/abort 之一"));
    }

    if (action == QStringLiteral("lock")) {
        if (::LockWorkStation() == FALSE) {
            return OpResult::failure(QStringLiteral("锁定会话失败（错误码 %1）").arg(::GetLastError()));
        }
        return OpResult::success();
    }

    QString error;
    if (!enablePrivilege(SE_SHUTDOWN_NAME, &error)) {
        return OpResult::failure(error);
    }

    if (action == QStringLiteral("abort")) {
        // 取消挂起的关机/重启。白名单里**必须**有这一条：它是"机器即将关闭"的
        // 撤销通道，少一条就等于让用户没有退路（插件侧另有一条不依赖助手的本地取消路径）
        if (::AbortSystemShutdownW(nullptr) != FALSE) {
            QVariantMap data;
            data.insert(QStringLiteral("hadPending"), true);
            return OpResult::success(data);
        }
        const DWORD abortError = ::GetLastError();
        if (abortError == ERROR_NO_SHUTDOWN_IN_PROGRESS) {
            QVariantMap data;
            data.insert(QStringLiteral("hadPending"), false);
            return OpResult::success(data); // "本来就没有挂起"是成立的成功结论
        }
        return OpResult::failure(QStringLiteral("取消挂起的关机失败（错误码 %1）").arg(abortError));
    }

    if (action == QStringLiteral("logoff")) {
        if (::ExitWindowsEx(EWX_LOGOFF,
                            SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE)
                == FALSE) {
            return OpResult::failure(QStringLiteral("注销失败（错误码 %1）").arg(::GetLastError()));
        }
        return OpResult::success();
    }

    if (action == QStringLiteral("sleep") || action == QStringLiteral("hibernate")) {
        const bool hibernate = (action == QStringLiteral("hibernate"));
        if (::SetSuspendState(hibernate ? TRUE : FALSE, FALSE, FALSE) == FALSE) {
            return OpResult::failure(QStringLiteral("%1失败（错误码 %2）")
                                         .arg(hibernate ? QStringLiteral("休眠")
                                                        : QStringLiteral("睡眠"))
                                         .arg(::GetLastError()));
        }
        return OpResult::success();
    }

    // reboot / shutdown：默认 5 秒倒计时，给用户反悔窗口
    bool numericOk = false;
    int timeout = args.value(QStringLiteral("timeout"), 5).toInt(&numericOk);
    if (!numericOk || timeout < 0 || timeout > 600) {
        timeout = 5;
    }
    const UINT flags = (action == QStringLiteral("reboot"))
        ? (EWX_REBOOT | EWX_FORCEIFHUNG)
        : (EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCEIFHUNG);

    const QString messageText = args.contains(QStringLiteral("message"))
        ? args.value(QStringLiteral("message")).toString()
        : QStringLiteral("WinEase 请求执行电源操作");
    std::wstring message = messageText.toStdWString();

    if (::InitiateSystemShutdownExW(nullptr, message.empty() ? nullptr : message.data(),
                                    static_cast<DWORD>(timeout), FALSE, flags,
                                    SHTDN_REASON_MAJOR_APPLICATION
                                        | SHTDN_REASON_MINOR_MAINTENANCE)
            == FALSE) {
        return OpResult::failure(QStringLiteral("电源操作请求失败（错误码 %1）").arg(::GetLastError()));
    }
    return OpResult::success();
}

} // namespace

// ---------------------------------------------------------------------------
// dispatch：白名单分发（白名单外一律拒绝）
// ---------------------------------------------------------------------------
OpResult OpDispatcher::dispatch(const QString &op, const QVariantMap &args, quint32 callerPid)
{
    if (op == QStringLiteral("ping")) {
        return opPing();
    }
    if (op == QStringLiteral("writeHosts")) {
        return opWriteHosts(args);
    }
    if (op == QStringLiteral("setEnvVar")) {
        return opSetEnvVar(args);
    }
    if (op == QStringLiteral("killProcess")) {
        return opKillProcess(args, callerPid);
    }
    if (op == QStringLiteral("hostsBackups")) {
        return opHostsBackups(args);
    }
    if (op == QStringLiteral("powerAction")) {
        return opPowerAction(args);
    }
    if (op == QStringLiteral("quit")) {
        // 先应答再退出（应答写回由服务层完成，这里只排队退出）
        QMetaObject::invokeMethod(QCoreApplication::instance(),
                                  [] { QCoreApplication::quit(); },
                                  Qt::QueuedConnection);
        return OpResult::success();
    }
    return OpResult::failure(QStringLiteral("操作 '%1' 不在提权助手白名单中，已拒绝").arg(op));
}

} // namespace WinEase::Helper
