#include "core/PluginGuard.h"

#include <windows.h>

namespace WinEase::Guard {

namespace {

/// C++ 异常的 SEH 代码（"msc" 的 ASCII 拼写：0xE06D7363）
constexpr unsigned long kCppExceptionCode = 0xE06D7363ul;
/// 栈溢出：处理器自身都可能没有栈可用，安全做法是放行给系统默认处理
constexpr unsigned long kStackOverflowCode = 0xC00000FDul;

} // namespace

// ⚠ 本函数内只允许出现 POD 局部变量：一旦有需要栈展开的 C++ 对象，
//   MSVC 会直接报 C2712（cannot use __try in functions that require object unwinding）
bool callSehProtected(void (*fn)(void *), void *context, unsigned long *codeOut)
{
    if (fn == nullptr) {
        return true;
    }

    bool ok = false;
    __try {
        fn(context);
        ok = true;
    } __except (GetExceptionCode() == kCppExceptionCode
                        || GetExceptionCode() == kStackOverflowCode
                    ? EXCEPTION_CONTINUE_SEARCH   // 交还给 C++ 展开链 / 系统默认处理
                    : EXCEPTION_EXECUTE_HANDLER) {
        if (codeOut != nullptr) {
            *codeOut = GetExceptionCode();
        }
        ok = false;
    }
    return ok;
}

QString describeSehCode(unsigned long code)
{
    switch (code) {
    case 0xC0000005ul:
        return QStringLiteral("访问违例（非法内存读写，0xC0000005）");
    case 0xC0000094ul:
        return QStringLiteral("整数除零（0xC0000094）");
    case 0xC0000096ul:
        return QStringLiteral("执行了特权指令（0xC0000096）");
    case 0xC000001Dul:
        return QStringLiteral("非法指令（0xC000001D）");
    case 0xC00000FDul:
        return QStringLiteral("栈溢出（0xC00000FD）");
    case 0xC0000374ul:
        return QStringLiteral("堆损坏，堆管理器检测到错误（0xC0000374）");
    case 0xC0000409ul:
        return QStringLiteral("栈缓冲区越界，/GS 检测到（0xC0000409）");
    case 0xC000008Cul:
        return QStringLiteral("数组越界（0xC000008C）");
    case 0xC000013Aul:
        return QStringLiteral("被 Ctrl+C / 关闭信号中断（0xC000013A）");
    case 0x80000003ul:
        return QStringLiteral("命中断点（0x80000003）");
    case 0xE06D7363ul:
        return QStringLiteral("C++ 异常（0xE06D7363）");
    default:
        return QStringLiteral("未知结构化异常（0x%1）").arg(code, 8, 16, QLatin1Char('0'));
    }
}

} // namespace WinEase::Guard
