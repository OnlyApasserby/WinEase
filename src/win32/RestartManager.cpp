#include "win32/RestartManager.h"

#include "win32/ProcessUtils.h"
#include "win32/Win32Error.h"

#include <QDir>
#include <QUuid>

#include <set>
#include <vector>

#include <windows.h>

// ⚠ 这里要的是 **Windows SDK 的 restartmanager.h**（不是本工程这个同名文件）。
//    本工程的头在 include 路径里的名字是 "win32/RestartManager.h"，
//    而 SDK 的 `restartmanager.h` 只存在于系统包含目录，尖括号写法不会撞车。
#include <restartmanager.h>

namespace WinEase::Win32 {

namespace {

/// RM 会话的 RAII 守卫：`RmStartSession` 拿到的句柄**必须** `RmEndSession`。
/// 注意：进程内同时存在的 RM 会话有上限（64），漏掉一次就少一次可用额度。
class RmSession
{
public:
    RmSession()
    {
        // 会话键用于跨进程识别同一个会话；这里用随机 GUID，避免与其它程序撞车
        const QByteArray key = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        wchar_t sessionKey[CCH_RM_SESSION_KEY + 1] = {};
        for (int index = 0; index < key.size() && index < CCH_RM_SESSION_KEY; ++index) {
            sessionKey[index] = static_cast<wchar_t>(key.at(index));
        }

        m_result = ::RmStartSession(&m_handle, 0, sessionKey);
    }

    ~RmSession()
    {
        if (m_result == ERROR_SUCCESS) {
            ::RmEndSession(m_handle);
        }
    }

    RmSession(const RmSession &) = delete;
    RmSession &operator=(const RmSession &) = delete;

    bool isValid() const { return m_result == ERROR_SUCCESS; }
    unsigned long result() const { return m_result; }
    DWORD handle() const { return m_handle; }

private:
    DWORD m_handle = 0;
    unsigned long m_result = ERROR_SUCCESS;
};

DWORD currentSessionId()
{
    DWORD sessionId = 0;
    if (::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId) == FALSE) {
        return static_cast<DWORD>(-1);
    }
    return sessionId;
}

} // namespace

QString FileLocker::describe() const
{
    // 只回答"是谁"（应用名 + PID）。身份/风险由 `common/UnlockPolicy` 判定，
    // 免得同一个判断在两处各写一份、迟早分叉。
    return QStringLiteral("%1 · PID %2")
        .arg(name.isEmpty() ? QStringLiteral("未知进程") : name)
        .arg(pid);
}

QList<FileLocker> findFileLockers(const QString &path, QString *errorOut)
{
    return findFileLockers(QStringList{ path }, errorOut);
}

QList<FileLocker> findFileLockers(const QStringList &paths, QString *errorOut)
{
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    QStringList usable;
    for (const QString &path : paths) {
        if (!path.isEmpty()) {
            usable.append(path);
        }
    }
    if (usable.isEmpty()) {
        return {};
    }

    // Restart Manager 要求绝对路径（带盘符）。相对路径直接判为参数错误，
    // 而不是交给 API 去猜当前目录 —— 猜错的话用户会看到"没人占用"。
    std::vector<LPCWSTR> resources;
    resources.reserve(static_cast<size_t>(usable.size()));
    for (const QString &path : usable) {
        if (!QDir::isAbsolutePath(path)) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("路径必须是绝对路径：%1").arg(path);
            }
            return {};
        }
        resources.push_back(reinterpret_cast<LPCWSTR>(path.utf16()));
    }

    RmSession session;
    if (!session.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = restartManagerError(session.result());
        }
        return {};
    }

    const unsigned long registered =
        ::RmRegisterResources(session.handle(),
                              static_cast<UINT>(resources.size()),
                              resources.data(),
                              0,
                              nullptr,
                              0,
                              nullptr);
    if (registered != ERROR_SUCCESS) {
        if (errorOut != nullptr) {
            *errorOut = restartManagerError(registered);
        }
        return {};
    }

    // 两段式调用：先问"需要几条"，再按需要的量取。
    // ⚠ 中间这段时间占用者可能变多（新进程打开了文件）→ 第二段可能再回 ERROR_MORE_DATA，
    //    此时**已经填好的那部分仍然有效**，如实返回即可（下次查询会刷新）。
    UINT needed = 0;
    UINT capacity = 0;
    DWORD reasons = 0;
    unsigned long result = ::RmGetList(session.handle(), &needed, &capacity, nullptr, &reasons);

    std::vector<RM_PROCESS_INFO> infos;
    UINT filled = 0;
    if (needed > 0) {
        infos.resize(needed);
        capacity = needed;
        result = ::RmGetList(session.handle(), &needed, &capacity, infos.data(), &reasons);
        filled = capacity;
    } else if (result != ERROR_SUCCESS) {
        if (errorOut != nullptr) {
            *errorOut = restartManagerError(result);
        }
        return {};
    }

    if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
        if (errorOut != nullptr) {
            *errorOut = restartManagerError(result);
        }
        return {};
    }

    const DWORD ownSession = currentSessionId();

    QList<FileLocker> lockers;
    std::set<quint32> seen;
    for (UINT index = 0; index < filled; ++index) {
        const RM_PROCESS_INFO &info = infos.at(index);
        const quint32 pid = static_cast<quint32>(info.Process.dwProcessId);
        if (pid == 0 || seen.count(pid) > 0) {
            continue; // 同一个进程可能因多个资源被报多次，按 PID 去重
        }
        seen.insert(pid);

        FileLocker locker;
        locker.pid = pid;
        locker.name = QString::fromWCharArray(info.strAppName);
        locker.serviceName = QString::fromWCharArray(info.strServiceShortName);
        locker.restartable = (info.bRestartable != FALSE);
        locker.applicationType = static_cast<int>(info.ApplicationType);
        locker.sameSession = (ownSession == static_cast<DWORD>(-1))
                             || (info.TSSessionId == ownSession);

        switch (info.ApplicationType) {
        case RmCritical:
            // ⚠ "关不掉"不等于"关键进程"：也可能是"我们没权限"或"它就是查询方自己"。
            //    所以这里只记事实，不在这里下保护结论（保护规则见 common/UnlockPolicy）
            locker.cannotShutdown = true;
            break;
        case RmService:
            locker.isService = true;
            break;
        case RmExplorer:
            locker.isExplorer = true;
            break;
        default:
            break;
        }

        // 路径与提权状态这里另查一次：RM 只给 PID，别的都要自己补
        locker.path = processPath(pid);
        locker.isElevated = isProcessElevated(pid);
        if (locker.name.isEmpty()) {
            locker.name = processName(pid);
        }

        lockers.append(locker);
    }

    return lockers;
}

QString appTypeText(int applicationType)
{
    switch (applicationType) {
    case RmMainWindow:
        return QStringLiteral("图形程序（有主窗口）");
    case RmOtherWindow:
        return QStringLiteral("图形程序（无独立窗口）");
    case RmService:
        return QStringLiteral("系统服务");
    case RmExplorer:
        return QStringLiteral("资源管理器");
    case RmConsole:
        return QStringLiteral("控制台程序");
    case RmCritical:
        return QStringLiteral("系统认为「关不掉」（要重启才能释放）");
    case RmUnknownApp:
    default:
        break;
    }
    return QStringLiteral("未分类程序");
}

QString restartManagerError(unsigned long errorCode)
{
    switch (errorCode) {
    case ERROR_SUCCESS:
        return QStringLiteral("成功");
    case ERROR_MORE_DATA:
        // 这不是失败：查询期间占用者列表变长了，已取到的部分有效
        return QStringLiteral("占用者列表在读取过程中还在增长（本次结果可能不是最新，重新查询即可）");
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("权限不足（其它用户会话的占用情况需要管理员身份才能读取）");
    case ERROR_SEM_TIMEOUT:
        return QStringLiteral("Restart Manager 会话超时（有程序长时间占用该文件，稍后重试）");
    case ERROR_BAD_ARGUMENTS:
        return QStringLiteral("参数非法（路径必须是带盘符的绝对路径）");
    case ERROR_FILE_NOT_FOUND:
        return QStringLiteral("文件不存在");
    case ERROR_PATH_NOT_FOUND:
        return QStringLiteral("路径不存在");
    case ERROR_WRITE_FAULT:
        return QStringLiteral("Restart Manager 内部写入失败（稍后重试）");
    case ERROR_OUTOFMEMORY:
        return QStringLiteral("内存不足");
#ifdef ERROR_MAX_SESSIONS_REACHED
    case ERROR_MAX_SESSIONS_REACHED:
        return QStringLiteral("同时打开的 Restart Manager 会话过多（有别的程序用满了配额，稍后重试）");
#endif
    default:
        break;
    }
    return describeFailure(QStringLiteral("查询文件占用"), static_cast<DWORD>(errorCode));
}

} // namespace WinEase::Win32
