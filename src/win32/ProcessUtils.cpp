#include "win32/ProcessUtils.h"

#include "win32/Win32Error.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <vector>

#include <tlhelp32.h>
#include <windows.h>

namespace WinEase::Win32 {

namespace {

/// 以最小必要权限打开进程句柄
/// PROCESS_QUERY_LIMITED_INFORMATION 对普通权限进程也通常可用，
/// 比 PROCESS_QUERY_INFORMATION 的通过率高得多
HANDLE openProcessForQuery(quint32 pid)
{
    return ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
}

/// RAII 句柄守卫（内部使用）
class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) {}
    ~ScopedHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_handle);
        }
    }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    HANDLE get() const { return m_handle; }
    explicit operator bool() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle = nullptr;
};

} // namespace

QString ProcessInfo::baseName() const
{
    const QString stem = QFileInfo(name).completeBaseName();
    return stem.isEmpty() ? name : stem;
}

// ---------------------------------------------------------------------------
//  单进程查询
// ---------------------------------------------------------------------------

QString processPath(quint32 pid)
{
    if (pid == 0U) {
        return QString();
    }

    ScopedHandle process(openProcessForQuery(pid));
    if (!process) {
        return QString();
    }

    std::vector<wchar_t> buffer(MAX_PATH * 2, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (::QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size) == FALSE) {
        return QString();
    }
    return QString::fromWCharArray(buffer.data(), static_cast<int>(size));
}

QString processName(quint32 pid)
{
    const QString path = processPath(pid);
    if (!path.isEmpty()) {
        return QFileInfo(path).fileName();
    }

    // 路径读不到时退回快照里的映像名（不需要打开进程，权限要求更低）
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return QString();
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);
    QString result;
    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (static_cast<quint32>(entry.th32ProcessID) == pid) {
                result = QString::fromWCharArray(entry.szExeFile);
                break;
            }
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return result;
}

quint32 parentProcessId(quint32 pid)
{
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0U;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);
    quint32 result = 0U;
    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (static_cast<quint32>(entry.th32ProcessID) == pid) {
                result = static_cast<quint32>(entry.th32ParentProcessID);
                break;
            }
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return result;
}

bool isProcessRunning(quint32 pid)
{
    if (pid == 0U) {
        return false;
    }

    ScopedHandle process(::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
    if (!process) {
        // 打不开句柄不一定代表不存在（可能是权限问题），用快照兜底确认
        return !processName(pid).isEmpty();
    }
    return ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
}

bool isProcessElevated(quint32 pid)
{
    ScopedHandle process(openProcessForQuery(pid));
    if (!process) {
        return false;
    }

    HANDLE token = nullptr;
    if (::OpenProcessToken(process.get(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    ::CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0U;
}

// ---------------------------------------------------------------------------
//  枚举
// ---------------------------------------------------------------------------

QList<ProcessInfo> processes()
{
    QList<ProcessInfo> result;

    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);
    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            ProcessInfo info;
            info.pid = static_cast<quint32>(entry.th32ProcessID);
            info.parentPid = static_cast<quint32>(entry.th32ParentProcessID);
            info.name = QString::fromWCharArray(entry.szExeFile);
            result.append(info);
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);

    std::sort(result.begin(), result.end(), [](const ProcessInfo &lhs, const ProcessInfo &rhs) {
        return lhs.pid < rhs.pid;
    });
    return result;
}

QList<ProcessInfo> findProcessesByName(const QString &nameFragment)
{
    QList<ProcessInfo> result;

    QString needle = nameFragment.trimmed().toLower();
    if (needle.isEmpty()) {
        return result;
    }
    // 允许调用方省略 .exe 后缀
    if (!needle.endsWith(QStringLiteral(".exe"))) {
        needle += QStringLiteral(".exe");
    }

    // 去掉后缀后的词干用于包含匹配；过短的词干（如 "a"）会命中一切，因此限制最小长度
    const QString stem = needle.chopped(4);

    const QList<ProcessInfo> all = processes();
    for (const ProcessInfo &info : all) {
        const QString name = info.name.toLower();
        if (name == needle) {
            result.append(info);
            continue;
        }
        if (stem.size() >= 3 && name.contains(stem)) {
            result.append(info);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
//  操作
// ---------------------------------------------------------------------------

bool terminateProcess(quint32 pid, quint32 exitCode, QString *errorOut)
{
    if (pid == 0U) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("进程 ID 无效");
        }
        return false;
    }

    if (pid == currentProcessId()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("拒绝结束当前进程本身");
        }
        return false;
    }

    ScopedHandle process(::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid)));
    if (!process) {
        const DWORD error = lastError();
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("打开进程以结束它"), error);
            if (isAccessDenied(error)) {
                *errorOut += QStringLiteral("；该进程可能需要管理员权限，请通过提权助手重试");
            }
        }
        return false;
    }

    if (::TerminateProcess(process.get(), static_cast<UINT>(exitCode)) == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("结束进程"), lastError());
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  当前进程
// ---------------------------------------------------------------------------

quint32 currentProcessId()
{
    return static_cast<quint32>(::GetCurrentProcessId());
}

QString currentProcessPath()
{
    return processPath(currentProcessId());
}

QString currentProcessDirectory()
{
    const QString path = currentProcessPath();
    if (path.isEmpty()) {
        return QString();
    }
    return QDir::toNativeSeparators(QFileInfo(path).absolutePath());
}

} // namespace WinEase::Win32
