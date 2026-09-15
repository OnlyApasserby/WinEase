#pragma once

// ============================================================================
//  WinRtSupport.h —— C++/WinRT 使用支撑
//
//  用途：PDF 渲染（P2-02 快速预览）、OCR、媒体控制（P3-11）、录屏（P3-08）
//        这些能力在 Windows 上只有 WinRT 提供，而 WinRT 又**不属于第三方库**
//        （C++/WinRT 头文件与 windowsapp.lib 均随 Windows SDK 提供）。
//
//  为什么要有这一层：
//      1. **异常边界**：WinRT 用 C++ 异常（winrt::hresult_error）报错，
//         绝不允许异常穿透到 Qt 事件循环或插件边界，否则会直接终止进程
//      2. **前置条件集中检查**：WinRT 要求线程已初始化 COM
//      3. **编译期隔离**：本头文件**不包含任何 winrt/*.h**，
//         因此插件引用它不会把 WinRT 类型扩散到整个工程；
//         需要 WinRT 类型的实现文件自行包含 winrt 头即可
//
//  ⚠ 关于套间模型：
//      本层**不调用** winrt::init_apartment()。原因是 Qt 的 Windows 平台插件
//      已经（或我们的 ComApartment 已经）为线程建立了套间，
//      再次 init_apartment 会因套间模型冲突抛 RPC_E_CHANGED_MODE。
//      C++/WinRT 本身只要求"线程已初始化 COM"，这一点由 ComApartment 保证。
// ============================================================================

#include <QString>

#include <functional>
#include <string>

namespace WinEase::Win32 {

/// 确认当前线程已具备调用 WinRT 的前置条件（COM 套间已初始化）
bool ensureWinRtReady(QString *errorOut = nullptr);

/// 探测一个 WinRT 运行时类在当前系统上是否可用（能取到激活工厂即视为可用）。
/// 用于按系统版本优雅降级，例如：
///     if (probeWinRtClass(L"Windows.Data.Pdf.PdfDocument")) { 启用 PDF 预览 }
///
/// ⚠ 为什么不使用 Windows.Foundation.Metadata.ApiInformation::IsTypePresent：
///     该 API 面向有"包标识"(package identity) 的应用。在**未打包的桌面程序**里调用它，
///     会命中 C++/WinRT 内部断言（实测 winrt/base.h:2948 `assert(value)`）并导致进程
///     崩溃，而不是优雅地返回错误 —— 这种失败方式不可接受。
///     RoGetActivationFactory 才是桌面程序判断"某个运行时类是否可用"的标准做法。
bool probeWinRtClass(const wchar_t *runtimeClassName, QString *errorOut = nullptr);

/// 为一段 WinRT 调用建立异常边界。
/// 用法：
///     QString error;
///     const bool ok = runWinRt(QStringLiteral("渲染 PDF 页面"), &error, [&] {
///         auto document = winrt::Windows::Data::Pdf::PdfDocument::LoadFromFileAsync(file).get();
///         // ... 这里可以随意抛 winrt::hresult_error
///     });
///     if (!ok) { 把 error 显示给用户 }
bool runWinRt(const QString &action, QString *errorOut, const std::function<void()> &body);

/// WinRT 失败 → 中文描述（含 HRESULT 与系统文案）
QString describeWinRtFailure(const QString &action, long hresult);

// ---------------------------------------------------------------------------
//  关于 WinRT 字符串转换（待办，见 docs/ROADMAP.md「实现踩坑记录」）
//
//  这里**暂时不提供** QString ↔ winrt::hstring 的辅助函数。
//
//  原因：自检过程中发现本机环境下存在一个无法解释的互操作异常——
//  即使只用 Qt 自身的 API（不涉及 WinRT）：
//        const QString s = QStringLiteral("WinEase 中文测试");   // 12 个 UTF-16 单元
//        const std::wstring w = s.toStdWString();               // size() 竟为 15
//        QString::fromWCharArray(w.c_str(), 15)                 // 结果为空
//  隔离测试确认问题不在本层代码。在把它查清之前，把行为不可解释的辅助函数
//  放进公共 API 只会把问题扩散到所有插件。
//
//  实际需要用到时（P2-02 PDF 预览）再解决，届时直接写
//      QString::fromWCharArray(text.c_str(), static_cast<int>(text.size()))
//  并针对 Windows.Data.Pdf 的真实数据校验。
// ---------------------------------------------------------------------------

} // namespace WinEase::Win32
