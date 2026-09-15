#include "HelperSecurity.h"

#include "win32/ProcessUtils.h"

#include <QDir>
#include <QFileInfo>

#include <softpub.h>    // WinVerifyTrust 参数结构
#include <wintrust.h>

namespace WinEase::Helper {

namespace {

/// Authenticode 签名校验结论
enum class SignatureState {
    Unsigned,   ///< 文件没有数字签名（开发期产物属于这种）
    Valid,      ///< 有签名且验证通过
    Invalid     ///< 有签名但验证失败 —— 最危险的情况，必须拒绝
};

SignatureState verifySignature(const QString &path)
{
    // WinVerifyTrust 的路径参数是 UTF-16；toStdWString 只允许在
    // Release/RelWithDebInfo 配置下使用（见 ENV-SETUP §6 的 Debug 禁令）
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = native.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;              // 后台服务，绝不弹证书 UI
    data.fdwRevocationChecks = WTD_REVOKE_NONE; // 吊销检查离线不可靠，不做
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.dwStateAction = WTD_STATEACTION_IGNORE;
    data.pFile = &fileInfo;

    const LONG result = ::WinVerifyTrust(
        static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

    if (result == 0) {
        return SignatureState::Valid;
    }
    if (result == TRUST_E_NOSIGNATURE) {
        return SignatureState::Unsigned;
    }
    return SignatureState::Invalid;
}

CallerCheckResult reject(const QString &reason, QString *errorOut)
{
    if (errorOut != nullptr) {
        *errorOut = reason;
    }
    return CallerCheckResult::Rejected;
}

} // namespace

CallerCheckResult verifyCaller(HANDLE pipe,
                               quint32 *callerPidOut,
                               QString *errorOut)
{
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return reject(QStringLiteral("管道句柄无效"), errorOut);
    }

    // ---- 1. 管道对端真实 PID（内核元数据，客户端无法伪造）----
    ULONG clientPid = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &clientPid) || clientPid == 0) {
        return reject(QStringLiteral("无法取得管道对端进程（缺少命名管道元数据，"
                                     "可能是非本机/伪装连接），已拒绝"), errorOut);
    }

    // ---- 2. 不采信自报身份：到此为止我们从未读取客户端发来的任何数据 ----

    // ---- 3. 反查对端 exe 路径 ----
    const QString callerPath = Win32::processPath(static_cast<quint32>(clientPid));
    if (callerPath.isEmpty()) {
        return reject(QStringLiteral("无法读取调用方进程路径，已拒绝"), errorOut);
    }

    // ---- 4. 同目录校验：第三方进程天然不满足 ----
    // ⚠ 必须先归一化：QueryFullProcessImageNameW 给反斜杠，QFileInfo 给正斜杠，
    //   直接逐字符比较会永远不等（本次实测踩到）
    const auto normalizeDir = [](const QString &path) {
        return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
    };
    const QString callerDir = normalizeDir(QFileInfo(callerPath).absolutePath());
    const QString selfDir = normalizeDir(Win32::currentProcessDirectory());
    if (selfDir.isEmpty()
            || callerDir != selfDir) {
        return reject(QStringLiteral("调用方与提权助手不在同一程序目录，已拒绝：")
                          + callerPath, errorOut);
    }

    // ---- 5. 签名校验：有签名必须有效；无签名放行并记录 ----
    const SignatureState state = verifySignature(callerPath);
    if (state == SignatureState::Invalid) {
        return reject(QStringLiteral("调用方程序的数字签名验证失败（可能被篡改），已拒绝"), errorOut);
    }

    if (callerPidOut != nullptr) {
        *callerPidOut = static_cast<quint32>(clientPid);
    }
    return CallerCheckResult::Accepted;
}

} // namespace WinEase::Helper
