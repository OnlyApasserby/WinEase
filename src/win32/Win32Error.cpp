#include "win32/Win32Error.h"

#include <QHash>

#include <iterator>

namespace WinEase::Win32 {

namespace {

/// 常见错误码的内置中文描述表
/// 说明：只收录高频且对用户有行动指导意义的错误，其余交给 FormatMessageW
struct ErrorEntry {
    DWORD code;
    const char *text;
};

constexpr ErrorEntry kErrorTable[] = {
    // ---- 文件 / 路径 ----
    { ERROR_FILE_NOT_FOUND, "找不到指定的文件" },
    { ERROR_PATH_NOT_FOUND, "找不到指定的路径" },
    { ERROR_ACCESS_DENIED, "访问被拒绝（权限不足）" },
    { ERROR_SHARING_VIOLATION, "文件正被其它程序占用" },
    { ERROR_LOCK_VIOLATION, "文件被锁定" },
    { ERROR_FILE_EXISTS, "文件已存在" },
    { ERROR_ALREADY_EXISTS, "对象已存在" },
    { ERROR_DIR_NOT_EMPTY, "目录不是空的" },
    { ERROR_DISK_FULL, "磁盘空间不足" },
    { ERROR_WRITE_PROTECT, "介质受写保护" },
    { ERROR_HANDLE_DISK_FULL, "磁盘已满" },
    { ERROR_TOO_MANY_OPEN_FILES, "打开的文件过多" },
    // ---- 参数 / 内存 ----
    { ERROR_INVALID_PARAMETER, "参数无效" },
    { ERROR_INVALID_HANDLE, "句柄无效" },
    { ERROR_NOT_ENOUGH_MEMORY, "内存不足" },
    { ERROR_OUTOFMEMORY, "系统内存不足" },
    { ERROR_INVALID_DATA, "数据无效" },
    { ERROR_BUFFER_OVERFLOW, "缓冲区太小" },
    { ERROR_INSUFFICIENT_BUFFER, "提供的缓冲区不足" },
    { ERROR_NOT_SUPPORTED, "该操作不被支持" },
    { ERROR_CALL_NOT_IMPLEMENTED, "该功能尚未实现" },
    { ERROR_INVALID_FUNCTION, "函数不正确（参数或调用方式有误）" },
    // ---- 进程 / 线程 ----
    { ERROR_PARTIAL_COPY, "只能读取进程的部分内存（进程可能已退出或权限不足）" },
    { ERROR_NO_MORE_FILES, "没有更多文件" },
    // ---- 显示 / 会话 ----
    { ERROR_INVALID_WINDOW_HANDLE, "窗口句柄无效或窗口已销毁" },
    { ERROR_HOTKEY_ALREADY_REGISTERED, "该快捷键已被其它程序占用" },
    { ERROR_CANCELLED, "操作被用户取消" },
    { ERROR_TIMEOUT, "操作超时" },
    { ERROR_BUSY, "资源忙，请稍后重试" },
    { ERROR_RETRY, "操作繁忙，请重试" },
    { ERROR_PRIVILEGE_NOT_HELD, "当前账户缺少执行该操作所需的权限" },
    { ERROR_ELEVATION_REQUIRED, "该操作需要管理员权限" },
    { ERROR_NO_SUCH_DEVICE, "找不到指定的设备" },
    { ERROR_DEVICE_NOT_CONNECTED, "设备未连接" },
    { ERROR_NOT_READY, "设备未就绪" },
    { ERROR_SEM_TIMEOUT, "等待信号量超时" },
};

/// HRESULT 内置中文表（COM 场景高频）
struct HresultEntry {
    HRESULT code;
    const char *text;
};

constexpr HresultEntry kHresultTable[] = {
    { S_OK, "成功" },
    { S_FALSE, "成功（无实际变更）" },
    { E_FAIL, "未指定的错误" },
    { E_INVALIDARG, "参数无效" },
    { E_OUTOFMEMORY, "内存不足" },
    { E_NOTIMPL, "该功能尚未实现" },
    { E_NOINTERFACE, "对象不支持请求的接口（可能版本不匹配）" },
    { E_POINTER, "指针参数为空" },
    { E_UNEXPECTED, "意外错误（对象状态不正确）" },
    { E_ACCESSDENIED, "访问被拒绝（权限不足）" },
    { E_ABORT, "操作被中止" },
    { E_HANDLE, "句柄无效" },
    { E_PENDING, "操作尚未完成" },
    { E_BOUNDS, "索引越界" },
    { E_CHANGED_STATE, "对象状态已改变" },
    { E_ILLEGAL_METHOD_CALL, "非法的状态转换调用" },
    { CLASS_E_CLASSNOTAVAILABLE, "找不到所需的组件（COM 类未注册）" },
    { CLASS_E_NOAGGREGATION, "该组件不支持聚合" },
    { REGDB_E_CLASSNOTREG, "COM 类未在注册表中登记（组件未安装）" },
    { RPC_E_CHANGED_MODE, "COM 套间模型冲突（当前线程已在另一种套间中）" },
    { CO_E_NOTINITIALIZED, "当前线程尚未初始化 COM" },
    { RPC_E_DISCONNECTED, "对象与服务器已断开连接" },
    { RPC_E_SERVERFAULT, "服务器端发生异常" },
    { CO_E_SERVER_EXEC_FAILURE, "启动 COM 服务器失败" },
};

/// 优先用内置表；未收录时用 FormatMessageW（跟随系统语言）
QString lookupBuiltin(DWORD code)
{
    for (const ErrorEntry &entry : kErrorTable) {
        if (entry.code == code) {
            return QString::fromUtf8(entry.text);
        }
    }
    return QString();
}

QString lookupBuiltin(HRESULT hr)
{
    for (const HresultEntry &entry : kHresultTable) {
        if (entry.code == hr) {
            return QString::fromUtf8(entry.text);
        }
    }
    return QString();
}

/// 调用 FormatMessageW 取系统文案，并去掉结尾的换行
QString formatSystemMessage(DWORD code, DWORD flags)
{
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        flags | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            ::LocalFree(buffer);
        }
        return QString();
    }

    QString text = QString::fromWCharArray(buffer, static_cast<int>(length));
    ::LocalFree(buffer);

    // 去掉结尾的空白与换行
    while (!text.isEmpty() && (text.endsWith(u'\n') || text.endsWith(u'\r') || text.endsWith(u' '))) {
        text.chop(1);
    }
    // 去掉结尾的句号，便于与自定义文案拼接
    while (text.endsWith(u'。') || text.endsWith(u'.')) {
        text.chop(1);
    }
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
//  DWORD
// ---------------------------------------------------------------------------

QString errorMessage(DWORD errorCode)
{
    if (errorCode == ERROR_SUCCESS) {
        return QStringLiteral("成功");
    }

    const QString builtin = lookupBuiltin(errorCode);
    if (!builtin.isEmpty()) {
        return builtin;
    }

    const QString system = formatSystemMessage(errorCode, FORMAT_MESSAGE_FROM_SYSTEM);
    if (!system.isEmpty()) {
        return system;
    }

    return QStringLiteral("未知错误");
}

QString describeFailure(const QString &action, DWORD errorCode)
{
    return QStringLiteral("%1 失败：%2（错误码 %3）").arg(action, errorMessage(errorCode)).arg(errorCode);
}

// ---------------------------------------------------------------------------
//  HRESULT
// ---------------------------------------------------------------------------

QString hresultMessage(HRESULT hr)
{
    if (SUCCEEDED(hr)) {
        return QStringLiteral("成功");
    }

    const QString builtin = lookupBuiltin(hr);
    if (!builtin.isEmpty()) {
        return builtin;
    }

    // HRESULT_FROM_WIN32(x) 包装：还原成 Win32 错误码，复用上面的中文表
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        const DWORD win32Code = HRESULT_CODE(hr);
        const QString win32Text = errorMessage(win32Code);
        if (!win32Text.isEmpty()) {
            return win32Text;
        }
    }

    const QString system = formatSystemMessage(static_cast<DWORD>(hr), FORMAT_MESSAGE_FROM_SYSTEM);
    if (!system.isEmpty()) {
        return system;
    }

    return QStringLiteral("未知 COM 错误");
}

QString describeHresultFailure(const QString &action, HRESULT hr)
{
    return QStringLiteral("%1 失败：%2（HRESULT 0x%3）")
        .arg(action, hresultMessage(hr))
        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))
        .toUpper();
}

// ---------------------------------------------------------------------------
//  辅助
// ---------------------------------------------------------------------------

DWORD lastError()
{
    return ::GetLastError();
}

void clearLastError()
{
    ::SetLastError(ERROR_SUCCESS);
}

bool checkBool(BOOL result, const QString &action, QString *errorOut)
{
    if (result != FALSE) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = describeFailure(action, ::GetLastError());
    }
    return false;
}

bool checkHresult(HRESULT hr, const QString &action, QString *errorOut)
{
    if (SUCCEEDED(hr)) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = describeHresultFailure(action, hr);
    }
    return false;
}

bool isRetryableError(DWORD errorCode)
{
    switch (errorCode) {
    case ERROR_BUSY:
    case ERROR_RETRY:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_TIMEOUT:
        return true;
    default:
        return false;
    }
}

bool isAccessDenied(DWORD errorCode)
{
    return errorCode == ERROR_ACCESS_DENIED || errorCode == ERROR_PRIVILEGE_NOT_HELD
           || errorCode == ERROR_ELEVATION_REQUIRED
           || errorCode == static_cast<DWORD>(E_ACCESSDENIED);
}

} // namespace WinEase::Win32
