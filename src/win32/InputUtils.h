#pragma once

// ============================================================================
//  InputUtils.h —— 输入注入（合成按键）
//
//  用途（对应路线图 P1-09 / P1-10 / P1-11 等"取选中文本"类功能）：
//      * 取选中文本：向前台窗口发 Ctrl+C，然后把剪贴板读出来
//      * 就地把转换结果替换回去：写回剪贴板后发 Ctrl+V
//
//  ⚠ 这是"注入到真实桌面"的操作，调用方必须自己判断场合：
//      1. 它作用于**当前前台窗口**，前台是谁就发给谁 —— 调用前最好先确认前台
//      2. 前台窗口若以管理员权限运行，普通权限进程的 SendInput 会被 UIPI 静默丢弃
//         （本函数会返回 false，调用方应据此回退到"只用剪贴板内容"）
//      3. 自检/自动化场景默认应避开它（见 ROADMAP 踩坑 #17）
//
//  无状态：纯函数，不保存任何按键状态（见 WinEaseWin32 设计原则 2）。
// ============================================================================

namespace WinEase::Win32 {

/// 是否具备向当前前台窗口注入输入的能力（UIPI 检查，探测用）
/// 注意：只能给出"当前这一刻"的判断，真正的失败以发送返回值号为准。
bool canSendInputToForeground();

/// 发送一个按键组合（按下 + 抬起，修饰键自动包裹）
/// @param virtualKey VK_* 虚拟键码；也可以用 'A'..'Z' / '0'..'9' 的 ASCII 值
/// @return 输入是否成功进入系统输入队列（false 表示被 UIPI 拦截或被系统拒绝）
bool sendKeyChord(unsigned int virtualKey,
                  bool ctrl = false,
                  bool shift = false,
                  bool alt = false,
                  bool win = false);

/// 发送 Ctrl+C（复制当前选中内容）
bool sendCopyShortcut();

/// 发送 Ctrl+V（粘贴）
bool sendPasteShortcut();

} // namespace WinEase::Win32
