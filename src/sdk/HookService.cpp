#include "sdk/HookService.h"

#include <QMetaObject>

// ⚠ 这是 SDK 中唯一包含 windows.h 的文件。
//   原因：注入 API（SendInput）与修饰键查询本身就是平台能力，而它们被声明为
//   HookService 的静态成员，其定义必须存在于**主程序与插件都会链接**的地方——
//   SDK 正是这样的地方（静态库被双方链接）。
//   其余 SDK 头文件保持与平台无关。
#include <windows.h>

namespace WinEase {

// ============================================================================
//  HookListener
// ============================================================================

HookListener::HookListener(QObject *parent)
    : QObject(parent)
{
}

HookListener::~HookListener()
{
    // 自动退订：插件忘记退订时不会留下悬空监听者
    if (m_service != nullptr) {
        m_service->unsubscribe(this);
        m_service = nullptr;
    }
}

void HookListener::setOwningService(HookService *service)
{
    m_service = service;
}

bool HookListener::dispatchHookEvent(const HookEvent &event)
{
    return onHookEvent(event);
}

void HookListener::notifyServiceStopped(const QString &reason)
{
    onServiceStopped(reason);
}

void HookListener::notifyDegraded(const QString &reason)
{
    onDegraded(reason);
}

void HookListener::reportQueuedProcessed()
{
    if (m_service != nullptr) {
        m_service->notifyQueuedEventProcessed(this);
    }
}

void HookListener::onServiceStopped(const QString &reason)
{
    Q_UNUSED(reason)
}

void HookListener::onDegraded(const QString &reason)
{
    Q_UNUSED(reason)
}

// ============================================================================
//  注入（拦截并改写）
// ============================================================================

bool HookService::sendKey(quint32 virtualKey, bool pressed, bool extended)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(virtualKey);
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = 0;
    if (!pressed) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    if (extended) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    return ::SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool HookService::sendMouseButton(HookMouseButton button, bool pressed)
{
    INPUT input{};
    input.type = INPUT_MOUSE;

    switch (button) {
    case HookMouseButton::Left:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case HookMouseButton::Right:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case HookMouseButton::Middle:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case HookMouseButton::X1:
    case HookMouseButton::X2:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = (button == HookMouseButton::X1) ? XBUTTON1 : XBUTTON2;
        break;
    }

    return ::SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool HookService::sendMouseWheel(int delta, bool horizontal)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = static_cast<DWORD>(delta);
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    return ::SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool HookService::sendMouseMove(const QPoint &screenPos)
{
    // SendInput 的绝对坐标是相对**虚拟桌面**的归一化值（0~65535）
    const int virtualX = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualY = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 1 || virtualHeight <= 1) {
        return false;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = ((screenPos.x() - virtualX) * 65535) / (virtualWidth - 1);
    input.mi.dy = ((screenPos.y() - virtualY) * 65535) / (virtualHeight - 1);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return ::SendInput(1, &input, sizeof(INPUT)) == 1;
}

// ============================================================================
//  修饰键
// ============================================================================

HookModifiers HookService::currentModifiers()
{
    HookModifiers modifiers = HookModNone;

    // GetAsyncKeyState 的最高位表示"当前是否按下"
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= HookModCtrl;
    }
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= HookModShift;
    }
    if ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= HookModAlt;
    }
    if (((::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0)
        || ((::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)) {
        modifiers |= HookModWin;
    }
    return modifiers;
}

bool HookService::modifiersMatch(HookModifiers actual, HookModifiers required)
{
    // 必须"恰好"匹配：要求之外的修饰键处于按下状态时视为不匹配。
    // 这样 Ctrl+Shift+滚轮 不会被误判为 Ctrl+滚轮。
    return (actual & HookModAll) == (required & HookModAll);
}

} // namespace WinEase
