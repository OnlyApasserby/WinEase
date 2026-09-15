// ============================================================================
//  overlay_smoke —— OverlayKit 验收测试（对应 ROADMAP P0-3 的验收标准）
//
//  验收标准（逐条对应）：
//    1. "全屏透明覆盖层不影响下层窗口点击"
//       → 用 WindowFromPoint 做**行为级**验证，并带反向对照：
//         穿透开启时命中下层窗口；**临时关闭穿透后必须命中悬浮层本身**。
//         反向对照很关键——它证明这个探针确实能检测出遮挡，
//         而不是一个永远通过的假测试。
//    2. "跨 125%/150% DPI 显示器不模糊"
//       → 逐显示器验证三件事：
//           a) 窗口物理矩形 == 显示器矩形（不错位）
//           b) 绘制表面 devicePixelRatio == 该显示器缩放系数
//           c) 窗口物理尺寸 == 逻辑尺寸 × DPR（= 按原生分辨率分配绘制表面，
//              这正是"不模糊"的机制：不是事后放大，而是本来就按物理像素绘制）
//    3. 不抢焦点；销毁后本进程不再残留任何悬浮层窗口（不留残影/不泄漏）
//
//  ⚠ 测试期间悬浮层**绘制内容为空**（完全透明），不会干扰用户视觉。
//     唯一可能被感知的时刻是"临时关闭点击穿透"的那几毫秒（此时仍不抢焦点）。
// ============================================================================

#include "sdk/OverlayHost.h"
#include "sdk/OverlayWindow.h"
#include "win32/WindowUtils.h"

#include <QApplication>
#include <QColor>
#include <QEventLoop>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#include <windows.h>

namespace {

using WinEase::OverlayHost;
using WinEase::OverlayWindow;
using WinEase::Win32::MonitorInfo;

// ---------------------------------------------------------------------------
//  输出
// ---------------------------------------------------------------------------

constexpr const char *kReset = "\x1b[0m";
constexpr const char *kGreen = "\x1b[32m";
constexpr const char *kRed = "\x1b[31m";
constexpr const char *kGray = "\x1b[90m";
constexpr const char *kCyan = "\x1b[36m";

void printLine(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
    std::fputc('\n', stdout);
}

class Reporter
{
public:
    void section(const QString &title)
    {
        printLine(QStringLiteral("\n%1== %2 ==%3")
                      .arg(QString::fromUtf8(kCyan), title, QString::fromUtf8(kReset)));
    }

    void check(bool ok, const QString &name, const QString &detail = QString())
    {
        if (ok) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3")
                          .arg(QString::fromUtf8(kGreen), QString::fromUtf8(kReset), name));
        } else {
            ++m_failed;
            printLine(QStringLiteral("  %1[失败]%2 %3")
                          .arg(QString::fromUtf8(kRed), QString::fromUtf8(kReset), name));
        }
        if (!detail.isEmpty()) {
            printLine(QStringLiteral("         %1%2%3")
                          .arg(QString::fromUtf8(kGray), detail, QString::fromUtf8(kReset)));
        }
    }

    void note(const QString &text)
    {
        printLine(QStringLiteral("  %1· %2%3")
                      .arg(QString::fromUtf8(kGray), text, QString::fromUtf8(kReset)));
    }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    int m_passed = 0;
    int m_failed = 0;
};

void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QString rectText(const QRect &rect)
{
    return QStringLiteral("(%1,%2 %3x%4)")
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height());
}

QString sizeText(const QSize &size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QString hwndText(HWND hwnd)
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(hwnd), 0, 16);
}

QString percentText(qreal scaleFactor)
{
    return QStringLiteral("%1%").arg(scaleFactor * 100.0, 0, 'f', 0);
}

// ---------------------------------------------------------------------------
//  原生探针
// ---------------------------------------------------------------------------

HWND nativeHandleOf(const QWidget *widget)
{
    return reinterpret_cast<HWND>(static_cast<quintptr>(const_cast<QWidget *>(widget)->winId()));
}

/// 该点处的命中测试结果。点击穿透的实现效果就是"它不会是我们自己"
HWND windowAtPoint(const QPoint &physicalPoint)
{
    const POINT native{physicalPoint.x(), physicalPoint.y()};
    return ::WindowFromPoint(native);
}

LONG_PTR exStyleOf(HWND hwnd)
{
    return ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
}

struct WindowCountContext {
    DWORD processId = 0;
    int count = 0;
};

BOOL CALLBACK countMarkerWindowsProc(HWND hwnd, LPARAM parameter)
{
    auto *context = reinterpret_cast<WindowCountContext *>(parameter);
    DWORD owner = 0;
    ::GetWindowThreadProcessId(hwnd, &owner);
    if (owner != context->processId) {
        return TRUE;
    }
    if (::GetPropW(hwnd, OverlayWindow::overlayMarkerProperty()) == nullptr) {
        return TRUE;
    }
    ++context->count;
    return TRUE;
}

/// 本进程当前残留的悬浮层窗口数量（"不留残影/不泄漏"的判据）
int residualOverlayWindowCount()
{
    WindowCountContext context;
    context.processId = ::GetCurrentProcessId();
    ::EnumWindows(countMarkerWindowsProc, reinterpret_cast<LPARAM>(&context));
    return context.count;
}

// ---------------------------------------------------------------------------
//  测试用悬浮层
// ---------------------------------------------------------------------------

class ProbeOverlay : public OverlayWindow
{
public:
    explicit ProbeOverlay(QWidget *parent = nullptr)
        : OverlayWindow(parent)
    {
    }

    int paintCount = 0;
    QRect lastLogicalRect;
    QRect lastDirtyRect;
    qreal lastDevicePixelRatio = 0.0;
    bool sawNullDevice = false;

    /// 是否在正中绘制一块**完全不透明**的色块。
    ///
    /// 这是让"点击穿透"测试真正有效的前提：如果悬浮层内容全透明，
    /// Windows 对分层窗口的命中测试本来就按"该像素是否透明"判定，
    /// 于是无论有没有 WS_EX_TRANSPARENT 都不会被命中——测试会假通过。
    /// 只有内容不透明时，"没有被命中"才真能证明穿透生效。
    /// 色块仅在同点位检查期间存在（约 1 秒），之后立即关闭。
    bool showOpaqueMarker = false;

    void paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect) override
    {
        ++paintCount;
        lastDirtyRect = dirtyLogicalRect;
        lastLogicalRect = overlayLogicalRect();

        if (painter.device() == nullptr) {
            sawNullDevice = true;
        } else {
            lastDevicePixelRatio = painter.device()->devicePixelRatioF();
        }

        if (showOpaqueMarker) {
            const QRect area = overlayLogicalRect();
            const QRect marker(QPoint(area.center().x() - kMarkerHalfSize,
                                      area.center().y() - kMarkerHalfSize),
                               QSize(kMarkerHalfSize * 2, kMarkerHalfSize * 2));
            painter.fillRect(marker, QColor(20, 20, 20, 255));
        }
    }

    static constexpr int kMarkerHalfSize = 120;
};

} // namespace

int main(int argc, char *argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("overlay_smoke"));

    printLine(QStringLiteral("WinEase OverlayKit 验收测试（P0-3）"));

    Reporter reporter;

    // ========================================================================
    reporter.section(QStringLiteral("环境：显示器与 DPI"));
    // ========================================================================

    const QList<MonitorInfo> monitors = WinEase::Win32::monitors();
    reporter.check(!monitors.isEmpty(), QStringLiteral("枚举到显示器"),
                   QStringLiteral("数量 %1").arg(monitors.size()));
    if (monitors.isEmpty()) {
        return 1;
    }

    for (int index = 0; index < monitors.size(); ++index) {
        const MonitorInfo &monitor = monitors.at(index);
        reporter.note(QStringLiteral("[%1] %2  矩形 %3  工作区 %4  DPI %5  缩放 %6  %7")
                          .arg(index)
                          .arg(monitor.deviceName)
                          .arg(rectText(monitor.geometry))
                          .arg(rectText(monitor.workArea))
                          .arg(monitor.dpi)
                          .arg(percentText(monitor.scaleFactor))
                          .arg(monitor.primary ? QStringLiteral("[主显示器]") : QString()));
    }
    reporter.note(QStringLiteral("进程 DPI 感知级别：%1").arg(WinEase::Win32::dpiAwarenessText()));

    bool hasMultipleScales = false;
    for (const MonitorInfo &monitor : monitors) {
        if (!qFuzzyCompare(monitor.scaleFactor, monitors.first().scaleFactor)) {
            hasMultipleScales = true;
            break;
        }
    }
    if (hasMultipleScales) {
        reporter.note(QStringLiteral("检测到缩放不一致的显示器，可完整覆盖跨 DPI 场景"));
    } else {
        reporter.note(QStringLiteral("本机显示器缩放一致 → 跨 DPI 的模糊问题无法自动覆盖；"
                                     "每显示器的换算逻辑已逐台验证，跨屏需双屏异构 DPI 手工复核"));
    }

    OverlayHost host;

    // ========================================================================
    reporter.section(QStringLiteral("点击穿透：不影响下层窗口（验收标准 1）"));
    // ========================================================================

    const MonitorInfo primary = WinEase::Win32::monitorForPoint(QPoint(1, 1));
    const MonitorInfo target = primary.valid ? primary : monitors.first();
    const QPoint probe = target.geometry.center();

    const HWND underBefore = windowAtPoint(probe);
    reporter.note(QStringLiteral("探针点 (%1,%2) 处窗口（创建悬浮层之前）：%3")
                      .arg(probe.x())
                      .arg(probe.y())
                      .arg(hwndText(underBefore)));

    auto *overlay = new ProbeOverlay();
    overlay->coverMonitor(0);
    // 先在正中画一块不透明色块：只有"内容不透明却没被命中"才能证明穿透真的生效
    overlay->showOpaqueMarker = true;
    overlay->show();
    pump(300);

    const HWND overlayHandle = nativeHandleOf(overlay);
    reporter.check(overlayHandle != nullptr && overlay->isOverlayVisible(),
                   QStringLiteral("全屏悬浮层已显示"),
                   QStringLiteral("%1  物理矩形 %2  透明度内容=空")
                       .arg(hwndText(overlayHandle))
                       .arg(rectText(overlay->overlayGeometry())));
    reporter.check(WinEase::Win32::isTopMost(overlayHandle), QStringLiteral("悬浮层为置顶窗口"));

    const LONG_PTR exStyle = exStyleOf(overlayHandle);
    reporter.check((exStyle & WS_EX_TRANSPARENT) != 0,
                   QStringLiteral("扩展样式含 WS_EX_TRANSPARENT"));
    reporter.check((exStyle & WS_EX_LAYERED) != 0,
                   QStringLiteral("扩展样式含 WS_EX_LAYERED（半透明所必需）"));
    reporter.check((exStyle & WS_EX_NOACTIVATE) != 0,
                   QStringLiteral("扩展样式含 WS_EX_NOACTIVATE（不抢焦点）"));
    reporter.check(WinEase::Win32::isClickThrough(overlayHandle),
                   QStringLiteral("平台层读取确认点击穿透已开启"));

    const HWND underWithOverlay = windowAtPoint(probe);
    reporter.check(underWithOverlay != overlayHandle,
                   QStringLiteral("【核心】全屏悬浮层未截获该点的命中测试"),
                   QStringLiteral("命中 %1（悬浮层为 %2）")
                       .arg(hwndText(underWithOverlay))
                       .arg(hwndText(overlayHandle)));
    reporter.check(underWithOverlay == underBefore,
                   QStringLiteral("【核心】穿透状态下命中窗口与创建前完全一致"),
                   QStringLiteral("创建前 %1 → 创建后 %2")
                       .arg(hwndText(underBefore))
                       .arg(hwndText(underWithOverlay)));

    // ---------------- 反向对照：证明探针真的能检测出遮挡 ----------------
    overlay->setClickThrough(false);
    pump(150);
    const LONG_PTR exStyleOff = exStyleOf(overlayHandle);
    const HWND underNoClickThrough = windowAtPoint(probe);
    reporter.check((exStyleOff & WS_EX_TRANSPARENT) == 0,
                   QStringLiteral("运行时关闭穿透后 WS_EX_TRANSPARENT 已被清除"),
                   QStringLiteral("扩展样式 0x%1").arg(exStyleOff, 0, 16));
    reporter.check(underNoClickThrough == overlayHandle,
                   QStringLiteral("【反向对照】关闭穿透后该点确实被悬浮层遮挡"
                                  "（证明上面的未命中是真的穿透，不是假通过）"),
                   QStringLiteral("命中 %1").arg(hwndText(underNoClickThrough)));

    overlay->setClickThrough(true);
    pump(150);
    const HWND underRestored = windowAtPoint(probe);
    reporter.check(underRestored == underBefore,
                   QStringLiteral("【核心】重新开启穿透后恢复为不遮挡"),
                   QStringLiteral("命中 %1，与创建前一致：%2")
                       .arg(hwndText(underRestored))
                       .arg(underRestored == underBefore ? QStringLiteral("是")
                                                         : QStringLiteral("否")));

    // 后续测试不再需要色块，恢复为完全透明（不影响用户视觉）
    overlay->showOpaqueMarker = false;
    overlay->requestOverlayUpdate();
    overlay->repaint();
    pump(120);

    // ========================================================================
    reporter.section(QStringLiteral("不抢焦点"));
    // ========================================================================

    const HWND foregroundBefore = ::GetForegroundWindow();
    overlay->hide();
    pump(100);
    overlay->show();
    pump(250);
    const HWND foregroundAfter = ::GetForegroundWindow();
    reporter.check(foregroundAfter == foregroundBefore,
                   QStringLiteral("显示悬浮层不改变前台窗口"),
                   QStringLiteral("%1 → %2")
                       .arg(hwndText(foregroundBefore))
                       .arg(hwndText(foregroundAfter)));
    reporter.check(foregroundAfter != overlayHandle,
                   QStringLiteral("悬浮层自身不会成为前台窗口"));

    // ========================================================================
    reporter.section(QStringLiteral("DPI：按原生分辨率绘制（验收标准 2）"));
    // ========================================================================

    bool geometryAllMatch = true;
    bool scaleAllMatch = true;
    bool nativeResolutionAllMatch = true;
    QStringList details;

    for (int index = 0; index < monitors.size(); ++index) {
        const MonitorInfo &monitor = monitors.at(index);
        if (!monitor.valid) {
            continue;
        }

        overlay->coverMonitor(index);
        overlay->show();
        pump(250);
        overlay->repaint();
        pump(80);

        const QRect physical = overlay->overlayGeometry();
        const qreal scale = overlay->overlayScaleFactor();
        const QSize logicalSize = overlay->size();
        const QSize expectedPhysical(qMax(1, qRound(logicalSize.width() * scale)),
                                     qMax(1, qRound(logicalSize.height() * scale)));

        // a) 不错位。允许 ≤1 像素的取整损耗（非整数缩放下无法精确命中），
        //    但必须是"宁大勿小"——绝不能比目标小，否则覆盖层边缘会漏缝
        const bool sizeWithinTolerance = (qAbs(physical.width() - monitor.geometry.width()) <= 1)
                                         && (qAbs(physical.height() - monitor.geometry.height())
                                             <= 1);
        const bool noGap = physical.contains(monitor.geometry);
        const bool geometryOk = (physical.topLeft() == monitor.geometry.topLeft())
                                && sizeWithinTolerance && noGap;
        // b) 绘制缩放跟随该显示器
        const bool scaleOk = qFuzzyCompare(scale, monitor.scaleFactor);
        // c) 绘制表面按原生分辨率分配（尺寸容差 2px 来自物理↔逻辑取整）
        const bool nativeOk = (qAbs(physical.width() - expectedPhysical.width()) <= 2)
                              && (qAbs(physical.height() - expectedPhysical.height()) <= 2)
                              && (qAbs(overlay->devicePixelRatioF() - monitor.scaleFactor) < 0.01);

        geometryAllMatch = geometryAllMatch && geometryOk;
        scaleAllMatch = scaleAllMatch && scaleOk;
        nativeResolutionAllMatch = nativeResolutionAllMatch && nativeOk;

        details.append(QStringLiteral("[%1] 目标 %2 → 实际 %3 · 缩放 %4 · 逻辑 %5 · "
                                      "期望物理 %6 · 绘制DPR %7 → %8")
                           .arg(index)
                           .arg(rectText(monitor.geometry))
                           .arg(rectText(physical))
                           .arg(percentText(monitor.scaleFactor))
                           .arg(sizeText(logicalSize))
                           .arg(sizeText(expectedPhysical))
                           .arg(overlay->devicePixelRatioF(), 0, 'f', 2)
                           .arg((geometryOk && scaleOk && nativeOk) ? QStringLiteral("OK")
                                                                    : QStringLiteral("异常")));
    }

    reporter.check(geometryAllMatch,
                   QStringLiteral("【核心】悬浮层物理矩形与目标显示器吻合"
                                  "（容差 ≤1 像素且绝不小于目标，即无缝隙）"),
                   details.join(QStringLiteral("\n         ")));
    reporter.check(scaleAllMatch,
                   QStringLiteral("【核心】绘制缩放系数跟随所在显示器的 DPI"));
    reporter.check(nativeResolutionAllMatch,
                   QStringLiteral("【核心】绘制表面按原生分辨率分配"
                                  "（物理尺寸 = 逻辑尺寸 × DPR，即高 DPI 下不模糊的机制）"));

    // ========================================================================
    reporter.section(QStringLiteral("绘制契约与后备位图"));
    // ========================================================================

    overlay->coverMonitor(0);
    overlay->show();
    pump(200);
    overlay->requestOverlayUpdate();
    overlay->repaint();
    pump(100);

    reporter.check(overlay->paintCount > 0, QStringLiteral("paintOverlay 被调用"),
                   QStringLiteral("调用 %1 次").arg(overlay->paintCount));
    reporter.check(!overlay->sawNullDevice, QStringLiteral("painter 绘制设备有效"));
    reporter.check(qAbs(overlay->lastDevicePixelRatio - overlay->overlayScaleFactor()) < 0.01,
                   QStringLiteral("painter 设备 DPR == 显示器缩放"),
                   QStringLiteral("设备 %1 / 显示器 %2")
                       .arg(overlay->lastDevicePixelRatio, 0, 'f', 2)
                       .arg(overlay->overlayScaleFactor(), 0, 'f', 2));
    reporter.check(overlay->lastLogicalRect == QRect(QPoint(0, 0), overlay->size()),
                   QStringLiteral("逻辑坐标系原点在左上角、尺寸等于逻辑尺寸"),
                   rectText(overlay->lastLogicalRect));

    const qreal scale = overlay->overlayScaleFactor();
    const QSize expectedLayerSize(qMax(1, qRound(overlay->size().width() * scale)),
                                  qMax(1, qRound(overlay->size().height() * scale)));
    QPixmap *layer = overlay->backingLayer();

    reporter.check(layer != nullptr && !layer->isNull(), QStringLiteral("后备位图可获取"));
    reporter.check(layer->size() == expectedLayerSize,
                   QStringLiteral("后备位图尺寸 = 逻辑尺寸 × DPR"),
                   QStringLiteral("%1（期望 %2）")
                       .arg(sizeText(layer->size()))
                       .arg(sizeText(expectedLayerSize)));
    reporter.check(qAbs(layer->devicePixelRatio() - scale) < 0.01,
                   QStringLiteral("后备位图带正确 devicePixelRatio（可直接用逻辑坐标绘制）"),
                   QStringLiteral("DPR %1").arg(layer->devicePixelRatio(), 0, 'f', 2));

    // ========================================================================
    reporter.section(QStringLiteral("宿主归属管理"));
    // ========================================================================

    overlay->closeOverlay();
    pump(300);
    reporter.check(residualOverlayWindowCount() == 0,
                   QStringLiteral("closeOverlay() 后自建悬浮层的原生窗口已销毁"),
                   QStringLiteral("残留 %1 个").arg(residualOverlayWindowCount()));

    host.createOverlay(QStringLiteral("owner.a"));
    host.createOverlay(QStringLiteral("owner.a"));
    host.createOverlay(QStringLiteral("owner.b"));
    reporter.check(host.overlayCount() == 3, QStringLiteral("按归属者创建悬浮层"),
                   QStringLiteral("总数 %1").arg(host.overlayCount()));
    reporter.check(host.overlaysOfOwner(QStringLiteral("owner.a")).size() == 2,
                   QStringLiteral("按归属者查询"));

    const int closedA = host.closeOverlaysOfOwner(QStringLiteral("owner.a"));
    pump(200);
    reporter.check(closedA == 2 && host.overlayCount() == 1,
                   QStringLiteral("按归属者批量回收（插件停用时的兜底路径）"),
                   QStringLiteral("回收 %1 个，剩余 %2 个").arg(closedA).arg(host.overlayCount()));

    const QList<OverlayWindow *> perMonitor =
        host.createOverlaysForAllMonitors(QStringLiteral("owner.c"));
    reporter.check(perMonitor.size() == monitors.size(),
                   QStringLiteral("每显示器各建一个悬浮层（跨 DPI 的正确做法）"),
                   QStringLiteral("创建 %1 个 / 显示器 %2 个")
                       .arg(perMonitor.size())
                       .arg(monitors.size()));

    for (OverlayWindow *item : perMonitor) {
        item->show();
    }
    pump(300);
    reporter.note(QStringLiteral("铺满全部显示器后，本进程悬浮层窗口数：%1")
                      .arg(residualOverlayWindowCount()));

    // ========================================================================
    reporter.section(QStringLiteral("销毁：不留残影"));
    // ========================================================================

    const int beforeClose = residualOverlayWindowCount();
    reporter.check(beforeClose >= monitors.size(),
                   QStringLiteral("销毁前确实存在悬浮层窗口（否则下面的检查无意义）"),
                   QStringLiteral("%1 个").arg(beforeClose));

    const int closedAll = host.closeAll();
    pump(600); // 等 deleteLater 真正执行完

    reporter.check(host.overlayCount() == 0, QStringLiteral("宿主登记表已清空"),
                   QStringLiteral("本次关闭 %1 个").arg(closedAll));
    reporter.check(residualOverlayWindowCount() == 0,
                   QStringLiteral("【核心】本进程已无任何悬浮层窗口残留"),
                   QStringLiteral("残留 %1 个").arg(residualOverlayWindowCount()));
    reporter.check(host.closeAll() == 0, QStringLiteral("重复 closeAll() 幂等"));
    reporter.check(host.closeOverlaysOfOwner(QStringLiteral("owner.a")) == 0,
                   QStringLiteral("回收不存在的归属者返回 0"));

    // ========================================================================
    const int total = reporter.passed() + reporter.failed();
    printLine(QString());
    printLine(QStringLiteral("──────────── 汇总 ────────────"));
    printLine(QStringLiteral("  通过 %1 / %2").arg(reporter.passed()).arg(total));
    if (reporter.failed() > 0) {
        printLine(QStringLiteral("  %1失败 %2%3")
                      .arg(QString::fromUtf8(kRed))
                      .arg(reporter.failed())
                      .arg(QString::fromUtf8(kReset)));
    }
    printLine(QStringLiteral("  结果：%1")
                  .arg(reporter.failed() == 0 ? QStringLiteral("全部通过")
                                              : QStringLiteral("存在失败项")));

    return reporter.failed() == 0 ? 0 : 1;
}
