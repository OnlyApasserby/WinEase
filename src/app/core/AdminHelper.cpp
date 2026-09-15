#include "core/AdminHelper.h"

#include "core/Logging.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QStringList>

#include <iterator>
#include <string>

#include <windows.h>
#include <shellapi.h>

namespace WinEase::Admin {

namespace {

/// 查询令牌中的某个信息
template <typename T>
bool queryTokenInformation(TOKEN_INFORMATION_CLASS infoClass, T *buffer)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(token, infoClass, buffer, sizeof(T), &returned);
    ::CloseHandle(token);
    return ok == TRUE;
}

} // namespace

bool isProcessElevated()
{
    static const bool cached = [] {
        TOKEN_ELEVATION elevation{};
        if (!queryTokenInformation(TokenElevation, &elevation)) {
            return false;
        }
        return elevation.TokenIsElevated != 0;
    }();
    return cached;
}

bool isUserInAdminGroup()
{
    static const bool cached = [] {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_READ, &token)) {
            return false;
        }

        DWORD size = 0;
        ::GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
        if (size == 0) {
            ::CloseHandle(token);
            return false;
        }

        QByteArray buffer(static_cast<int>(size), '\0');
        auto *groups = reinterpret_cast<PTOKEN_GROUPS>(buffer.data());
        const BOOL ok = ::GetTokenInformation(token, TokenGroups, groups, size, &size);
        ::CloseHandle(token);
        if (!ok) {
            return false;
        }

        // 管理员组 SID：S-1-5-32-544
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID adminSid = nullptr;
        if (!::AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid)) {
            return false;
        }

        bool found = false;
        for (DWORD i = 0; i < groups->GroupCount; ++i) {
            if (::EqualSid(groups->Groups[i].Sid, adminSid)) {
                found = true;
                break;
            }
        }
        ::FreeSid(adminSid);
        return found;
    }();
    return cached;
}

QString elevationDescription()
{
    if (isProcessElevated()) {
        return QStringLiteral("已获得管理员权限");
    }
    if (isUserInAdminGroup()) {
        return QStringLiteral("普通权限（部分功能需以管理员身份重启）");
    }
    return QStringLiteral("普通用户权限");
}

bool restartAsElevated(const QString &arguments)
{
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (executable.isEmpty()) {
        return false;
    }

    std::wstring verb = L"runas";
    std::wstring file = executable.toStdWString();
    std::wstring params = arguments.toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = verb.c_str();
    info.lpFile = file.c_str();
    info.lpParameters = params.empty() ? nullptr : params.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_CANCELLED) {
            qCInfo(lcApp) << "用户取消了 UAC 提权请求";
        } else {
            qCWarning(lcApp) << "提权重启失败，错误码:" << error;
        }
        return false;
    }

    if (info.hProcess) {
        ::CloseHandle(info.hProcess);
    }
    return true;
}

QString currentUserName()
{
    wchar_t buffer[256] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!::GetUserNameW(buffer, &size)) {
        return QString();
    }
    return QString::fromWCharArray(buffer, static_cast<int>(size > 0 ? size - 1 : 0));
}

} // namespace WinEase::Admin
