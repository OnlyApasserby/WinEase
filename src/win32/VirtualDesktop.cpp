#include "win32/VirtualDesktop.h"

#include "win32/Win32Error.h"

#include <QUuid>

#include <algorithm>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <wrl/client.h> // Microsoft::WRL::ComPtr（接管后禁手工 Release，踩坑 #15/#39）

namespace {

// ⚠ 本层刻意**本地声明 vtable**，不包含 `shobjidl_core.h`：
//    这个接口是"公开但少用"的那种，预研阶段我们正是靠"自己照 vtable 布局声明"
//    确认它在今天的系统上仍然成立（`tests/p3_spike` 的 P3-01 段就是这么做的）。
//    保持同一策略的好处：探针与产品**共用同一种调用方式**，
//    不会出现"探针能跑、插件不能跑"这种只有换过写法才会暴露的差异。
//
// IID_IVirtualDesktopManager 是**只读**用途（本模块不再调用它的第三个方法，理由见头文件）。
const GUID kClsidVirtualDesktopManager = { 0xAA509086, 0x5CA9, 0x4C25,
                                           { 0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A } };
const GUID kIidVirtualDesktopManager = { 0xA5CD92FF, 0x29BE, 0x454C,
                                         { 0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B } };

/// 与系统头文件里的 `IVirtualDesktopManager` 布局一致（三个方法，顺序不可变）
struct IVirtualDesktopManagerLocal : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND topLevelWindow,
                                                                     BOOL *onCurrentDesktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND topLevelWindow,
                                                         GUID *desktopId) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND topLevelWindow,
                                                          REFGUID desktopId) = 0;
};

using Microsoft::WRL::ComPtr;

/// 取一次虚拟桌面管理器。返回空表示接口不可用（错误原因写进 errorOut）。
ComPtr<IVirtualDesktopManagerLocal> virtualDesktopManager(QString *errorOut)
{
    ComPtr<IVirtualDesktopManagerLocal> manager;
    const HRESULT hr = ::CoCreateInstance(kClsidVirtualDesktopManager,
                                         nullptr,
                                         CLSCTX_INPROC_SERVER,
                                         kIidVirtualDesktopManager,
                                         reinterpret_cast<void **>(manager.GetAddressOf()));
    if (FAILED(hr) || !manager) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法创建虚拟桌面管理器（IVirtualDesktopManager）：%1")
                            .arg(WinEase::Win32::virtualDesktopError(hr));
        }
        return {};
    }
    return manager;
}

QString guidText(const GUID &guid)
{
    return QUuid(guid).toString(QUuid::WithoutBraces).toUpper();
}

bool parseGuid(const QString &text, GUID *guidOut)
{
    const QUuid parsed = QUuid::fromString(text.trimmed());
    if (parsed.isNull()) {
        return false;
    }
    if (guidOut != nullptr) {
        *guidOut = parsed;
    }
    return true;
}

} // namespace

namespace WinEase::Win32 {

QString DesktopWindowInfo::label() const
{
    if (title.isEmpty()) {
        return processName.isEmpty() ? QStringLiteral("（无标题窗口）") : processName;
    }
    if (processName.isEmpty()) {
        return title;
    }
    return QStringLiteral("%1 · %2").arg(processName, title);
}

QString VirtualDesktopInfo::label() const
{
    return QStringLiteral("%1（%2 个窗口）")
        .arg(isCurrent ? QStringLiteral("当前桌面") : QStringLiteral("其它桌面"))
        .arg(windowCount);
}

QString normalizeDesktopId(const QString &text)
{
    GUID guid{};
    if (!parseGuid(text, &guid)) {
        return QString();
    }
    return guidText(guid);
}

bool isValidDesktopId(const QString &text)
{
    return parseGuid(text, nullptr);
}

QString virtualDesktopError(long hresult)
{
    switch (static_cast<unsigned long>(hresult)) {
    case static_cast<unsigned long>(E_INVALIDARG):
        return QStringLiteral("窗口句柄无效或目标虚拟桌面不存在");
    case static_cast<unsigned long>(E_HANDLE):
        return QStringLiteral("窗口句柄无效（窗口可能已经关掉了）");
    case static_cast<unsigned long>(REGDB_E_CLASSNOTREG):
        return QStringLiteral("本系统没有注册虚拟桌面管理器（不是 Windows 10/11？）");
    case static_cast<unsigned long>(TYPE_E_ELEMENTNOTFOUND):
        // ★ 预研踩过的坑：拿 shell 窗口/桌面窗口/工具窗口当目标时就是这个码。
        //   它不是"API 失效"，而是"这个窗口不属于任何虚拟桌面"
        return QStringLiteral("这个窗口不属于任何虚拟桌面（桌面 / 任务栏 / 外壳窗口就是这种）");
    default:
        break;
    }
    return describeHresultFailure(QStringLiteral("虚拟桌面操作"), static_cast<HRESULT>(hresult));
}

bool windowDesktopId(WindowHandle hwnd, QString *desktopIdOut, QString *errorOut)
{
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    if (desktopIdOut != nullptr) {
        desktopIdOut->clear();
    }
    if (!isValidWindow(hwnd)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("窗口句柄无效");
        }
        return false;
    }

    const ComPtr<IVirtualDesktopManagerLocal> manager = virtualDesktopManager(errorOut);
    if (!manager) {
        return false;
    }

    GUID guid{};
    const HRESULT hr = manager->GetWindowDesktopId(hwnd, &guid);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = virtualDesktopError(hr);
        }
        return false;
    }
    // ⚠ 全零 GUID = "不属于任何可用桌面"（实测有 UWP 宿主/工具窗口会"成功"返回它）：
    //   这种答案必须当成"没有答案"，否则界面里会冒出点不动的幽灵桌面（踩坑 #64）
    const QString id = guidText(guid);
    if (id.isEmpty() || !isValidDesktopId(id)) {
        if (errorOut != nullptr) {
            *errorOut = virtualDesktopError(TYPE_E_ELEMENTNOTFOUND);
        }
        return false;
    }
    if (desktopIdOut != nullptr) {
        *desktopIdOut = id;
    }
    return true;
}

bool isWindowOnCurrentDesktop(WindowHandle hwnd, bool *onCurrentOut, QString *errorOut)
{
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    if (!isValidWindow(hwnd)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("窗口句柄无效");
        }
        return false;
    }

    const ComPtr<IVirtualDesktopManagerLocal> manager = virtualDesktopManager(errorOut);
    if (!manager) {
        return false;
    }

    BOOL onCurrent = FALSE;
    const HRESULT hr = manager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = virtualDesktopError(hr);
        }
        return false;
    }
    if (onCurrentOut != nullptr) {
        *onCurrentOut = (onCurrent != FALSE);
    }
    return true;
}

QList<VirtualDesktopInfo> virtualDesktops(QString *errorOut)
{
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    const ComPtr<IVirtualDesktopManagerLocal> manager = virtualDesktopManager(errorOut);
    if (!manager) {
        return {};
    }

    // ① 枚举全部顶层窗口（**包括在别的桌面上的窗口** —— 它们只是被 DWM 隐去，
    //    句柄仍然存在，这正是我们能反查出桌面列表的原因）
    std::vector<HWND> windows;
    {
        struct CollectContext {
            std::vector<HWND> *windows = nullptr;
        };
        CollectContext context{ &windows };
        ::EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                auto *ctx = reinterpret_cast<CollectContext *>(param);
                ctx->windows->push_back(hwnd);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
    }

    struct Bucket {
        QString id;
        int windowCount = 0;
        /// 有"没被 DWM 隐去"的窗口回答过"我在当前桌面上"（退化判据，见下）
        bool uncloakedSaysCurrent = false;
        QStringList titles;
        QList<DesktopWindowInfo> windows;
    };
    std::vector<Bucket> buckets;
    const auto bucketFor = [&buckets](const QString &id) -> Bucket & {
        for (Bucket &bucket : buckets) {
            if (bucket.id == id) {
                return bucket;
            }
        }
        buckets.push_back(Bucket{ id, 0, false, {}, {} });
        return buckets.back();
    };

    // ★★ 当前桌面怎么判：**以"前台窗口"为准**。
    //
    //   ⚠ 不能只信 `IsWindowOnCurrentVirtualDesktop` 的返回值：**被 DWM 隐去
    //     （cloaked）的窗口会报出"是"**。本机实测出现过"两个桌面同时被判为当前"
    //     （其中一个是只有 UWP 窗口的桌面）——列表里两个条目都写着"当前桌面"，
    //     功能当场变成笑话。前台窗口按定义就在用户正在看的那个桌面上，
    //     它是这里唯一不会说谎的信号（拿不到它时再退化成"未被隐去 + 回答为真"的投票）。
    QString currentId;
    {
        const HWND foreground = ::GetForegroundWindow();
        GUID guid{};
        if (isValidWindow(foreground) && SUCCEEDED(manager->GetWindowDesktopId(foreground, &guid))) {
            const QString id = guidText(guid);
            if (!id.isEmpty() && isValidDesktopId(id)) {
                currentId = id;
            }
        }
    }

    for (HWND hwnd : windows) {
        GUID guid{};
        if (FAILED(manager->GetWindowDesktopId(hwnd, &guid))) {
            continue; // 桌面窗口 / 任务栏这类不属于任何虚拟桌面 —— 跳过即可，不是错误
        }
        // ★★ **全零 GUID 不是"一个桌面"**：本机实测有窗口（UWP/后台/外壳托管的那类）
        //    会成功返回 GUID_NULL。初版把它当成了一个"桌面"，
        //    于是列表里多出一条 `其它桌面（4 个窗口）{00000000}`，而用户根本切不过去。
        //    这种项必须**根本不出现**（踩坑 #64）。
        if (IsEqualGUID(guid, GUID_NULL)) {
            continue;
        }
        const QString id = guidText(guid);
        if (id.isEmpty()) {
            continue;
        }

        Bucket &bucket = bucketFor(id);

        // 数"用户心里算数的窗口"：可见的、不是外壳窗口的（在别的桌面上的窗口
        // `IsWindowVisible` 仍然是真 —— 被隐去只是 DWM 的效果，而我们要的正是
        // "那个桌面上有哪些窗口"）
        const bool countable = isWindowVisible(hwnd) && !isShellWindow(hwnd);
        if (countable) {
            ++bucket.windowCount;
            const QString title = windowTitle(hwnd);
            if (!title.isEmpty() && bucket.titles.size() < 3 && !bucket.titles.contains(title)) {
                bucket.titles << title;
            }
            DesktopWindowInfo info;
            info.handle = hwnd;
            info.title = title;
            info.processName = windowProcessName(hwnd);
            info.processId = windowProcessId(hwnd);
            bucket.windows.append(info);
        }

        if (!bucket.uncloakedSaysCurrent) {
            BOOL onCurrent = FALSE;
            if (SUCCEEDED(manager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent))
                && onCurrent != FALSE && !isWindowCloaked(hwnd)) {
                bucket.uncloakedSaysCurrent = true;
            }
        }
    }

    // 前台窗口的桌面优先；拿不到退化成投票结果（但只认**一个**"当前桌面"，
    // 免得界面上出现两个"当前"）
    if (currentId.isEmpty()) {
        for (const Bucket &bucket : buckets) {
            if (bucket.uncloakedSaysCurrent) {
                currentId = bucket.id;
                break;
            }
        }
    }

    QList<VirtualDesktopInfo> result;
    for (const Bucket &bucket : buckets) {
        VirtualDesktopInfo info;
        info.id = bucket.id;
        info.windowCount = bucket.windowCount;
        info.isCurrent = (!currentId.isEmpty() && bucket.id == currentId);
        info.sampleTitles = bucket.titles;
        info.windows = bucket.windows;
        result.append(info);
    }

    // 稳定排序：当前桌面最前，然后窗口多的在前，最后按 GUID —— 结果可复现（自检要断言）
    std::sort(result.begin(), result.end(),
              [](const VirtualDesktopInfo &left, const VirtualDesktopInfo &right) {
                  if (left.isCurrent != right.isCurrent) {
                      return left.isCurrent;
                  }
                  if (left.windowCount != right.windowCount) {
                      return left.windowCount > right.windowCount;
                  }
                  return left.id < right.id;
              });
    return result;
}

} // namespace WinEase::Win32
