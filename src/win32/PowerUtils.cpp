#include "win32/PowerUtils.h"

#include "win32/Win32Error.h"

#include <windows.h>

namespace WinEase::Win32 {

namespace {

/// 启用当前进程令牌里的某个特权（启用过也当成功；不持有才算失败）
bool enablePrivilege(const wchar_t *privilegeName, QString *errorOut)
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)
        == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("打开进程令牌"), lastError());
        }
        return false;
    }

    LUID luid{};
    if (::LookupPrivilegeValueW(nullptr, privilegeName, &luid) == FALSE) {
        const DWORD code = lastError();
        ::CloseHandle(token);
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("查询特权 %1")
                                            .arg(QString::fromWCharArray(privilegeName)),
                                        code);
        }
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL ok = ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    const DWORD adjustError = lastError();
    ::CloseHandle(token);

    if (ok == FALSE || adjustError == ERROR_NOT_ALL_ASSIGNED) {
        if (errorOut != nullptr) {
            // ERROR_NOT_ALL_ASSIGNED 表示"令牌里根本没有这个特权"（不是权限不够这么简单）
            *errorOut = adjustError == ERROR_NOT_ALL_ASSIGNED
                            ? QStringLiteral("当前令牌不具备特权 %1")
                                  .arg(QString::fromWCharArray(privilegeName))
                            : describeFailure(QStringLiteral("启用特权 %1")
                                                  .arg(QString::fromWCharArray(privilegeName)),
                                              adjustError);
        }
        return false;
    }
    return true;
}

} // namespace

bool lockWorkstation(QString *errorOut)
{
    if (::LockWorkStation() == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("锁定会话"), lastError());
        }
        return false;
    }
    return true;
}

bool hasShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }

    DWORD size = 0;
    ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
    if (size == 0) {
        ::CloseHandle(token);
        return false;
    }

    QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
    const bool ok = ::GetTokenInformation(token,
                                          TokenPrivileges,
                                          buffer.data(),
                                          size,
                                          &size)
                    != FALSE;
    ::CloseHandle(token);
    if (!ok) {
        return false;
    }

    LUID target{};
    if (::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &target) == FALSE) {
        return false;
    }

    const auto *privileges = reinterpret_cast<const TOKEN_PRIVILEGES *>(buffer.constData());
    for (DWORD index = 0; index < privileges->PrivilegeCount; ++index) {
        const LUID &luid = privileges->Privileges[index].Luid;
        if (luid.LowPart == target.LowPart && luid.HighPart == target.HighPart) {
            return true;
        }
    }
    return false;
}

bool abortPendingShutdown(bool *hadPendingOut, QString *errorOut)
{
    if (hadPendingOut != nullptr) {
        *hadPendingOut = false;
    }

    QString privilegeError;
    if (!enablePrivilege(SE_SHUTDOWN_NAME, &privilegeError)) {
        if (errorOut != nullptr) {
            *errorOut = privilegeError;
        }
        return false;
    }

    if (::AbortSystemShutdownW(nullptr) != FALSE) {
        if (hadPendingOut != nullptr) {
            *hadPendingOut = true;
        }
        return true;
    }

    const DWORD code = lastError();
    if (code == ERROR_NO_SHUTDOWN_IN_PROGRESS) {
        // 本来就没有挂起 —— 对调用方来说这是**成功**结论
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = describeFailure(QStringLiteral("取消挂起的关机"), code);
    }
    return false;
}

bool hasPendingShutdown(bool *pendingOut, QString *errorOut)
{
    bool hadPending = false;
    if (!abortPendingShutdown(&hadPending, errorOut)) {
        if (pendingOut != nullptr) {
            *pendingOut = false;
        }
        return false;
    }
    if (pendingOut != nullptr) {
        *pendingOut = hadPending;
    }
    return true;
}

} // namespace WinEase::Win32
