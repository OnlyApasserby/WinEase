#pragma once

// ============================================================================
//  VirtualDesktop.h —— 虚拟桌面：**只读**回答"有哪些桌面、每个桌面上有哪些窗口"
//
//  给谁用：P3-01（`window.virtual_desktop`，收窄后是"多桌面窗口一览"）。
//
//  ---------------------------------------------------------------------------
//  ⚠⚠ 本模块**只读**。为什么不做搬移/切换（2026-09-14 定案，细节见 docs/traps.md #66）：
//
//   * `IVirtualDesktopManager::MoveWindowToDesktop` 官方文档只写"成功返回 S_OK"，
//     **没写权限限制**；本工程用零副作用探针实测出：它只允许搬**调用进程自己**的窗口
//     （自己的窗口 `S_OK` / 探针子进程的窗口 `E_ACCESSDENIED` / 第三方程序的窗口 `E_ACCESSDENIED`）
//     —— 也就是"把别人的窗口挪走"这件事用公开 API **做不到**。
//   * 唯一的替代路子是系统快捷键 `Win+Ctrl+Shift+←/→`（把**当前活动窗口**移到相邻桌面），
//     但它要**抢用户的焦点**、依赖用户没改过键位、没有相邻桌面时还什么都不发生
//     —— 代价与收益不匹配，**已按用户要求砍掉**。
//
//  于是这个功能的定位变成：**告诉你"那个窗口跑到哪个桌面去了"**（只显示，不动任何东西）。
//
//  ---------------------------------------------------------------------------
//  三条**能力边界**（界面上也写着，别让用户误会）：
//   1. **没有"枚举虚拟桌面"的公开接口** → 本模块用"枚举顶层窗口反查 GUID"来发现桌面，
//      所以**只能看到"当前有窗口的"桌面**（刚建好、还没放过窗口的桌面不会出现），
//      桌面被删掉后下一轮刷新会自动少一项（不留幽灵项）。
//   2. **拿不到桌面的名字与编号**（系统里那个"桌面 1 / 2"是外壳自己排的）→ 用
//      "是不是当前桌面 + 有几个窗口 + 窗口标题"帮用户认。
//   3. **`IsWindowOnCurrentVirtualDesktop` 单独用会答错**：被 DWM 隐去（cloaked）的窗口会回答"是"
//      （本机实测出现过"两个桌面同时被判为当前"）→ 本模块**以"前台窗口所在桌面"为准**，
//      拿不到时才退化成"未被隐去 + 回答为真"的投票，且**只认一个**当前桌面。
//
//  ⚠ 平台层约定：只依赖 Qt6::Core（QString/QList），只放无状态原语。
// ============================================================================

#include "win32/WindowUtils.h" // WindowHandle

#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 某个虚拟桌面上的一个顶层窗口（"这个窗口跑哪去了"的最小信息）
struct DesktopWindowInfo {
    WindowHandle handle = nullptr; ///< 顶层窗口句柄（只读展示用，本模块不操作它）
    QString title;                 ///< 窗口标题（可能为空）
    QString processName;           ///< 进程名（标题为空时靠它认人）
    quint32 processId = 0;

    /// 界面用的一行文本：有标题就用"进程名 · 标题"，没标题就退回进程名
    QString label() const;
};

/// 探测到的一个虚拟桌面（只能探测到"有窗口的"桌面，见头注释边界 1）
struct VirtualDesktopInfo {
    /// 桌面 GUID 文本（大写、无花括号；`normalizeDesktopId()` 归一化后的形式）
    QString id;
    int windowCount = 0;          ///< 该桌面上的顶层窗口数（可见、非外壳窗口）
    bool isCurrent = false;       ///< 是不是当前显示出来的那个桌面（**有且仅有一个**）
    QStringList sampleTitles;     ///< 窗口标题样例（最多 3 个）—— 列表项里用，便于一眼认出
    /// 该桌面的**全部**窗口，按 z 序（最上面的在前）—— "窗口一览"就是它
    QList<DesktopWindowInfo> windows;

    /// 界面/日志用的一句话（"当前桌面（3 个窗口）"）
    QString label() const;
};

/// 探测当前存在的虚拟桌面（按"当前桌面优先、窗口多的优先、GUID"排序 —— 结果可复现）
QList<VirtualDesktopInfo> virtualDesktops(QString *errorOut = nullptr);

/// 窗口所在虚拟桌面的 GUID（`desktopIdOut` 为归一化后的文本）
bool windowDesktopId(WindowHandle hwnd, QString *desktopIdOut, QString *errorOut = nullptr);

/// 窗口是否在**当前**虚拟桌面上
bool isWindowOnCurrentDesktop(WindowHandle hwnd,
                              bool *onCurrentOut,
                              QString *errorOut = nullptr);

/// 把任意形态的桌面 GUID 文本归一化（去花括号、统一大写）；非法输入返回空串
QString normalizeDesktopId(const QString &text);

/// GUID 文本是否合法（会拒绝全零 GUID —— 它不是桌面）
bool isValidDesktopId(const QString &text);

/// HRESULT → 中文（虚拟桌面语境下人话）
QString virtualDesktopError(long hresult);

} // namespace WinEase::Win32
