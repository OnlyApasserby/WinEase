#pragma once

// ============================================================================
//  PluginGuard.h —— 插件调用的 SEH + C++ 异常边界（P0-5）
//
//  插件是独立编译的第三方代码，**必须假设它会崩**：解引用空指针、越界写、
//  抛未捕获异常……任何一次跨 DLL 的调用都不能把主程序带走。
//  本模块提供两级边界，一次调用同时被两者保护：
//
//      外层   try / catch(const std::exception &) + catch(...)   —— C++ 异常
//      内层   __try / __except                                  —— 结构化异常（SEH）
//
//  用法：
//      Guard::CrashReport report;
//      bool ok = Guard::invoke([&] { plugin->initialize(); }, &report);
//      if (!ok) {
//          // report.description 是可直接展示给用户的中文原因
//          // 例如 "访问违例（0xC0000005）" / "C++ 异常：boom"
//      }
//
//  ---------------------------------------------------------------------------
//  三条必须知道的约束（都是实测踩出来的）：
//
//   1. `__try` 只能出现在**不需要栈展开**的函数里（MSVC 报 C2712：
//      "cannot use __try in functions that require object unwinding"）。
//      因此本模块把 `__try` 只关在 PluginGuard.cpp 的 callSehProtected() 里
//      （该函数只使用 POD 局部变量），其余代码一律通过 Guard::invoke() 间接使用。
//   2. `__except` 的过滤器必须**放行 C++ 异常**（SEH 码 0xE06D7363）与栈溢出
//      （0xC00000FD）：前者若被当成 SEH 处理，就会被误判成"访问违例"，
//      并且会截断 /EHsc 的 C++ 展开链；后者在栈已耗尽时无法安全恢复。
//   3. SEH 发生时，出错帧内**已构造的 C++ 对象不会被析构**（语义类似 longjmp）。
//      这是隔离崩溃必须付出的代价：可能泄漏少量资源，但主程序活着。
//
//  与"隔离"的分工：本模块只负责"捕获并说明原因"，
//  崩溃后不再调用该插件的策略由 PluginManager 的隔离机制负责（见 PluginManager.h）。
// ============================================================================

#include <QString>

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace WinEase::Guard {

/// 崩溃类型
enum class CrashKind {
    None = 0,            ///< 没有崩溃
    StructuredException, ///< Windows 结构化异常（访问违例、除零…）
    CppException         ///< C++ 异常（std::exception 或 ...）
};

/// 一次调用的崩溃报告；crashed() 为 false 时其余字段无意义
struct CrashReport {
    CrashKind kind = CrashKind::None;
    unsigned long code = 0; ///< SEH 异常码，例如 0xC0000005
    QString description;    ///< 中文原因（可直接展示）
    QString detail;         ///< 附加细节（C++ 异常的 what()）

    bool crashed() const { return kind != CrashKind::None; }
};

/// 把 SEH 异常码翻译成中文描述（未知码返回"未知结构化异常（0x…）"）
QString describeSehCode(unsigned long code);

/// SEH 保护调用：fn(context) 内发生结构化异常时返回 false 并写入异常码。
///
/// ⚠ 本函数的实现所在编译单元**不得**出现需要栈展开的 C++ 对象（MSVC C2712），
///   请勿在本函数内添加 QString / std::string 等局部变量，也别把它挪进别的文件。
bool callSehProtected(void (*fn)(void *), void *context, unsigned long *codeOut);

namespace detail {

/// 把 void* 还原成可调用对象并调用（SEH 保护函数的入口，必须是普通函数）
template <typename Fn>
void trampoline(void *context)
{
    (*static_cast<Fn *>(context))();
}

} // namespace detail

/// 组合边界：内层 SEH + 外层 C++ 异常。正常返回 true；崩溃时返回 false 并填充 report。
/// @param report 可为 nullptr（只关心成败时）
template <typename Fn>
bool invoke(Fn &&fn, CrashReport *report)
{
    if (report != nullptr) {
        *report = CrashReport();
    }

    using Callable = std::remove_reference_t<Fn>;
    unsigned long code = 0;

    try {
        if (!callSehProtected(&detail::trampoline<Callable>,
                              const_cast<void *>(static_cast<const void *>(std::addressof(fn))),
                              &code)) {
            if (report != nullptr) {
                report->kind = CrashKind::StructuredException;
                report->code = code;
                report->description = describeSehCode(code);
            }
            return false;
        }
    } catch (const std::exception &error) {
        if (report != nullptr) {
            report->kind = CrashKind::CppException;
            report->detail = QString::fromUtf8(error.what());
            report->description = QStringLiteral("C++ 异常：%1").arg(report->detail);
        }
        return false;
    } catch (...) {
        if (report != nullptr) {
            report->kind = CrashKind::CppException;
            report->description = QStringLiteral("C++ 异常（非 std::exception 类型，无法取得描述）");
        }
        return false;
    }
    return true;
}

} // namespace WinEase::Guard
