#pragma once

// ============================================================================
//  Win32Error.h —— 系统错误码 → 中文描述
//
//  为什么需要它：
//      Win32 API 失败只给一个 DWORD，直接把它丢给用户毫无意义。
//      本模块统一把 DWORD / HRESULT 转成中文，供界面提示与日志使用。
//
//  策略：
//      1. 常见错误码走**内置中文表**——保证在英文系统上也是中文
//      2. 未收录的交给 FormatMessageW（跟随系统语言）
//      3. 都拿不到则回退为"未知错误（码 N）"
//
//  注意：本模块所有函数都不抛异常，也不修改 GetLastError()。
// ============================================================================

#include <QString>

#include <windows.h>

namespace WinEase::Win32 {

/// DWORD 错误码 → 中文描述
QString errorMessage(DWORD errorCode);

/// HRESULT → 中文描述（自动识别 HRESULT_FROM_WIN32 包装并还原为 Win32 码）
QString hresultMessage(HRESULT hr);

/// 组装统一文案："<动作> 失败：<原因>（错误码 N）"
QString describeFailure(const QString &action, DWORD errorCode);
QString describeHresultFailure(const QString &action, HRESULT hr);

/// 读取当前线程错误码（不修改它）
DWORD lastError();

/// 把错误码清零，便于在调用 API 前建立干净状态
void clearLastError();

/// 通用检查：result 为 FALSE 时输出失败描述
/// @return result != FALSE
bool checkBool(BOOL result, const QString &action, QString *errorOut = nullptr);

/// 通用检查：HRESULT 失败时输出失败描述
/// @return SUCCEEDED(hr)
bool checkHresult(HRESULT hr, const QString &action, QString *errorOut = nullptr);

/// 判断该错误码是否属于"可重试"类别（占用、忙）
bool isRetryableError(DWORD errorCode);

/// 该错误码是否代表权限不足（用于决定是否提示提权）
bool isAccessDenied(DWORD errorCode);

} // namespace WinEase::Win32
