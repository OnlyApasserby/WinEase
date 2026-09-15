#pragma once

// ============================================================================
//  Magnifier.h —— 原生放大镜窗口（Windows Magnification API）
//
//  为什么不用"自抓屏 + 缩放"（GetDC / BitBlt / StretchBlt）：
//      自抓屏方案里**每一次刷新**都要：抓一块屏幕 → 缩放 → 自己画到窗口。
//      Magnification API 的价值在于：取像与缩放由系统的放大镜控件负责，
//      我们自己的代码里**一次 BitBlt 都没有** —— 因此没有抓屏停顿、没有撕裂，
//      并且硬件叠加层 / UWP 窗口 / 多 DPI / 多显示器这些"自抓屏必错"的场景由系统保证正确。
//
//  ⚠ 但它**不是免费的**（这一点实测过，别再口头假设）：取像与缩放最终仍跑在
//      **本进程**（放大镜控件是本进程的子窗口，重绘就在我们的线程里）。
//      自检 `feature_smoke` 里有一份"自抓屏参考实现"（同一个源区域、同一帧率、同样呈现到
//      真实窗口）当场对照，实测（240 px / 33 fps / 40 帧，分母 = 实测「单核跑满」）：
//          静止（只保持画面新鲜）≈ 3703 万周期（单核 1.0%）  vs  自抓屏参考 3016 万  → **同量级**
//          跟随（每帧移动窗口 + 改源区）≈ 1.32 亿周期（单核 3.6%）
//      即：**绝对 CPU 并不比自抓屏低**，"把放大交给系统所以更省"是错的。
//      我们能省、也已经省掉的只有"无意义的重复劳动"：源区与窗口位置都没变时
//      不重复调用 `MagSetWindowSource`（见 `applySource()`）。
//
//  ⚠ 两条使用契约：
//      1. `MagInitialize` / `MagUninitialize` 必须成对。本类**每实例成对调用**
//         （不搞进程级全局引用计数：平台层的约定是"无全局状态"，而 API 内部
//         自己的计数天然支持成对调用）。
//      2. 调用线程必须已初始化 COM（见 ComApartment）。
//
//  结构上为什么是"宿主窗口 + 子窗口"两层：
//      `WC_MAGNIFIER` 是**子窗口类**（官方示例就是把它放进一个宿主窗口），
//      遮罩（圆角/圆形）用 `SetWindowRgn` 设在宿主上，子窗口会被父窗口的区域裁掉，
//      于是形状对整块放大区域生效；同时宿主还负责"点击穿透 / 不抢焦点 / 置顶"。
// ============================================================================

#include "win32/WindowUtils.h"

#include <QPoint>
#include <QRect>
#include <QString>

#include <optional>

namespace WinEase::Win32 {

/// 放大窗遮罩形状
enum class MagnifierShape {
    Rectangle, ///< 矩形
    Rounded,   ///< 圆角（圆角半径 = 短边 / 4）
    Circle     ///< 圆形（正方形窗口的内切圆）
};

/// 宿主窗口类名。类名是对外可见的约定：自检/调试要按它找窗口，
/// 所以从这里取而不是各处硬编码字符串。
QString magnifierHostClassName();

/// 形状的稳定键名（配置文件里存 "rect" / "rounded" / "circle"）
QString magnifierShapeKey(MagnifierShape shape);
/// 键名 → 形状（无法识别时回退到 Rounded）
MagnifierShape magnifierShapeFromKey(const QString &key);

/// 放大镜窗口（置顶、点击穿透、不抢焦点）
class Magnifier
{
public:
    /// 探测 Magnification API 在本进程是否可用（`canEnable()` 用）。
    /// 真正的探测就是初始化一次 —— API 不可用时它自己会失败（典型原因：
    /// 进程不是 DPI 感知的、桌面合成被禁用、远程会话的某些配置）。
    static bool apiAvailable(QString *errorOut = nullptr);

    Magnifier();
    ~Magnifier();

    Magnifier(const Magnifier &) = delete;
    Magnifier &operator=(const Magnifier &) = delete;

    /// 创建并显示放大镜窗口（重复调用前会先销毁旧的）
    bool create(int sizePx, double factor, MagnifierShape shape, QString *errorOut = nullptr);
    /// 销毁窗口（可重复调用；析构也会调用）
    void destroy();

    bool isValid() const;   ///< 窗口是否已创建且仍然有效（窗口被系统销毁时会自动失效）
    WindowHandle hostWindow() const;      ///< 宿主窗口（自检用来定位）
    WindowHandle magnifierWindow() const; ///< WC_MAGNIFIER 子窗口

    /// 跟随光标：把窗口移到"光标旁边且不出屏"的位置，并把放大源矩形中心对准光标。
    /// 这是每个刷新周期调用的主函数。
    bool followCursor(QString *errorOut = nullptr);

    /// 让某个屏幕点成为放大源中心（不移动窗口；自检与调试用）
    bool setSourceCenter(const QPoint &screenPoint, QString *errorOut = nullptr);

    bool setFactor(double factor, QString *errorOut = nullptr);
    bool setSizePx(int sizePx, QString *errorOut = nullptr);
    bool setShape(MagnifierShape shape, QString *errorOut = nullptr);

    double factor() const { return m_factor; }
    int sizePx() const { return m_sizePx; }
    MagnifierShape shape() const { return m_shape; }

    /// 当前放大源矩形（物理像素，屏幕坐标）：由 `MagGetWindowSource` 读回。
    /// 这是"倍率真的生效了"的硬证据：源矩形边长 == 窗口边长 / 倍率。
    QRect sourceRect() const;
    /// 放大窗当前位置（物理像素，屏幕坐标）
    QRect windowRect() const;

private:
    /// 按当前倍率重算源矩形并交给 API
    bool applySource(const QPoint &center, QString *errorOut);
    /// 按当前形状设置宿主窗口区域（遮罩）
    void applyRegion();
    /// 窗口位置：放在光标旁边，靠边自动翻到另一侧，并夹在显示器可用区内
    static QPoint preferredTopLeft(const QPoint &cursor, const QSize &size, const QRect &workArea);

    WindowHandle m_host = nullptr;
    WindowHandle m_magnifier = nullptr;
    double m_factor = 2.5;
    int m_sizePx = 240;
    MagnifierShape m_shape = MagnifierShape::Rounded;
    QPoint m_sourceCenter{ 0, 0 };
    bool m_hasSource = false;
    /// 上一次交给 API 的源矩形：`applySource()` 靠它跳过"没变化也照设一次"的重设
    std::optional<QRect> m_lastSource;
    /// 本实例是否已经 MagInitialize 过（决定 destroy 时要不要 MagUninitialize）
    bool m_apiInitialized = false;
};

} // namespace WinEase::Win32
