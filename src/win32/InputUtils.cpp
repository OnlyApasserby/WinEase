#include "win32/InputUtils.h"

#include "win32/ProcessUtils.h" // isProcessElevated
#include "win32/WindowUtils.h"  // windows.h（SendInput / INPUT / VK_*）

#include <array>

namespace WinEase::Win32 {

namespace {

/// 修饰键按下/抬起的顺序固定为 Ctrl、Shift、Alt、Win
constexpr unsigned int kModifierVirtualKeys[4] = { VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN };

/// 左右 Win 键必须带 KEYEVENTF_EXTENDEDKEY，否则系统会当成别的键处理
constexpr WORD extendedFlag(unsigned int virtualKey)
{
    return (virtualKey == VK_LWIN || virtualKey == VK_RWIN) ? KEYEVENTF_EXTENDEDKEY : 0;
}

} // namespace

bool canSendInputToForeground()
{
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(foreground, &pid);
    if (pid == 0) {
        return false;
    }
    // 前台进程完整性级别更高时，SendInput 会被 UIPI 静默丢弃
    return !isProcessElevated(pid) || isProcessElevated(::GetCurrentProcessId());
}

bool sendKeyChord(unsigned int virtualKey, bool ctrl, bool shift, bool alt, bool win)
{
    if (virtualKey == 0) {
        return false;
    }

    // 最多个数 = 4 个修饰键按下 + 目标键按下/抬起 + 4 个修饰键抬起
    std::array<INPUT, 10> inputs{};
    int count = 0;

    const auto addKey = [&inputs, &count](unsigned int vk, bool keyUp) {
        if (count >= static_cast<int>(inputs.size())) {
            return;
        }
        INPUT &input = inputs[static_cast<size_t>(count++)];
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(vk);
        input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = extendedFlag(vk) | (keyUp ? KEYEVENTF_KEYUP : 0);
        input.ki.time = 0;
        input.ki.dwExtraInfo = 0;
    };

    const bool modifiers[4] = { ctrl, shift, alt, win };
    for (int i = 0; i < 4; ++i) {
        if (modifiers[i]) {
            addKey(kModifierVirtualKeys[i], false);
        }
    }
    addKey(virtualKey, false);
    addKey(virtualKey, true);
    // 抬起顺序与按下相反，避免应用看到"修饰键一直按着"
    for (int i = 3; i >= 0; --i) {
        if (modifiers[i]) {
            addKey(kModifierVirtualKeys[i], true);
        }
    }

    // 一次性投递整串事件：系统按顺序处理，应用不会看到半按状态
    const UINT sent = ::SendInput(static_cast<UINT>(count), inputs.data(), sizeof(INPUT));
    return sent == static_cast<UINT>(count);
}

bool sendCopyShortcut()
{
    return sendKeyChord('C', true, false, false, false);
}

bool sendPasteShortcut()
{
    return sendKeyChord('V', true, false, false, false);
}

} // namespace WinEase::Win32
