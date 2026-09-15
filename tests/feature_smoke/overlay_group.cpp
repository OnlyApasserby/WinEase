// ============================================================================
//  feature_smoke / overlay_group.cpp —— P1-C 悬浮层组端到端用例
//                    （P1-07 屏幕标尺 / P1-08 焦点高亮）
//
//  三条纪律（与前两组一致）：
//      ① 前置条件先断言出来（光标到位了没、背景板颜色对不对）；
//      ② 只经公开入口驱动（dispatchHotkey），断言**外部可观测**的真实状态；
//      ③ 用例结束必须让桌面恢复原样（悬浮层全部回收、像素回到原色）。
//
//  这一组的两层断言：
//      * **窗口层**：宿主登记表（创建/回收）、扩展样式（TOPMOST / TRANSPARENT /
//        NOACTIVATE / LAYERED）、几何（铺满显示器）、z 序（确实在最上面）；
//      * **像素层**：用 colorAt() 读合成后的屏幕像素 —— 这是"聚光灯把光标周围挖亮、
//        把别处压暗"唯一的硬证据，也是"高亮位置与鼠标一致"这条验收的证据。
// ============================================================================

#include "overlay_group.h"

#include "app/core/PluginManager.h"
#include "sdk/OverlayHost.h"
#include "sdk/OverlayWindow.h"
#include "win32/ScreenCapture.h"
#include "win32/WindowUtils.h"

#include <QCoreApplication>
#include <QStringList>
#include <QThread>

#include <climits>

namespace FeatureSmoke {

namespace {

const QString kRulerId = QStringLiteral("display.ruler");
const QString kSpotlightId = QStringLiteral("display.focus_highlight");

using WinEase::OverlayWindow;
using WinEase::Win32::WindowHandle;

WindowHandle overlayHandle(OverlayWindow *overlay)
{
    return (overlay != nullptr) ? reinterpret_cast<HWND>(overlay->winId()) : nullptr;
}

/// 像素亮度（取三通道平均，够用且不受色相影响）
int brightnessOf(const QColor &color)
{
    return (color.red() + color.green() + color.blue()) / 3;
}

/// StatusLog 里**最后一条**含某关键词的消息。
/// 不能用 last()：跟随鼠标的插件会不断追加消息，last() 可能已经换成别的了
QString lastWith(const StatusLog &status, const QString &needle)
{
    QString found;
    for (const QString &message : status.messages) {
        if (message.contains(needle)) {
            found = message;
        }
    }
    return found;
}

/// 读一个屏幕像素（带"读不到"保护）
bool readPixel(const QPoint &physicalPoint, QColor *colorOut)
{
    bool ok = false;
    const QColor color = WinEase::Win32::colorAt(physicalPoint, &ok);
    if (ok && color.isValid() && colorOut != nullptr) {
        *colorOut = color;
    }
    return ok && color.isValid();
}

/// 插件是否已经上报过某个光标位置（"高亮位置 x,y"）
bool reportsPosition(const StatusLog &status, const QPoint &point)
{
    return lastWith(status, QStringLiteral("高亮位置"))
        .contains(QStringLiteral("%1,%2").arg(point.x()).arg(point.y()));
}

} // namespace

int runOverlayGroupTests(Reporter &reporter,
                         WinEase::PluginManager &manager,
                         StubServices &services,
                         ProbeWindow &probe)
{
    const int failuresAtStart = reporter.failures();
    const WindowHandle probeHandle = probe.handle();

    WinEase::OverlayHost *host = services.overlayHost();
    reporter.check(host != nullptr,
                   QStringLiteral("悬浮层组前置：宿主桩提供的是**真实** OverlayHost（不是 nullptr）"));
    if (host == nullptr) {
        return reporter.failures() - failuresAtStart;
    }
    reporter.check(host->overlayCount() == 0,
                   QStringLiteral("悬浮层组前置：开始前宿主登记表是空的"),
                   QStringLiteral("当前 %1 个").arg(host->overlayCount()));

    const int monitorCount = WinEase::Win32::monitors().size();

    // =======================================================================
    //  P1-07 屏幕标尺
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-07 屏幕标尺 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kRulerId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-07 插件已从 plugins 目录加载（display.ruler）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kRulerId, true), QStringLiteral("P1-07 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("就绪")),
                       QStringLiteral("P1-07 启用时给出状态提示"),
                       status.last());
        reporter.check(host->overlaysOfOwner(kRulerId).isEmpty(),
                       QStringLiteral("P1-07 启用本身不建悬浮层（要等用户按快捷键，不打扰人）"));
        status.clear();

        // ---- 显示标尺 ----
        reporter.check(dispatchAction(manager, kRulerId, QStringLiteral("default")),
                       QStringLiteral("P1-07 触发「显示标尺」"));
        const QList<OverlayWindow *> overlays = host->overlaysOfOwner(kRulerId);
        reporter.check(overlays.size() == monitorCount,
                       QStringLiteral("P1-07 每个显示器各建一个悬浮层（跨 DPI 时只有这样才处处清晰）"),
                       QStringLiteral("悬浮层 %1 个 / 显示器 %2 个").arg(overlays.size()).arg(monitorCount));
        reporter.check(status.contains(QStringLiteral("标尺已显示")),
                       QStringLiteral("P1-07 状态文本反馈已显示"),
                       status.last());
        if (overlays.isEmpty()) {
            manager.setPluginEnabled(kRulerId, false);
            return reporter.failures() - failuresAtStart;
        }

        OverlayWindow *rulerOverlay = overlays.first();
        const WindowHandle rulerHandle = overlayHandle(rulerOverlay);
        reporter.check(OverlayWindow::isOverlayWindow(reinterpret_cast<void *>(rulerHandle)),
                       QStringLiteral("P1-07 悬浮层带 OverlayKit 身份标记（宿主/自检据此识别）"));
        reporter.check(hasExStyle(rulerHandle, WS_EX_TOPMOST),
                       QStringLiteral("P1-07 悬浮层是**置顶**窗口（验收：全屏置顶）"));
        reporter.check(hasExStyle(rulerHandle, WS_EX_LAYERED),
                       QStringLiteral("P1-07 悬浮层是分层窗口（能画半透明刻度）"));
        reporter.check(hasExStyle(rulerHandle, WS_EX_NOACTIVATE),
                       QStringLiteral("P1-07 悬浮层不抢焦点（不打断用户正在做的事）"));
        reporter.check(hasExStyle(rulerHandle, WS_EX_TRANSPARENT),
                       QStringLiteral("P1-07 默认**点击穿透**（验收：穿透开关可切换）"));

        const WinEase::Win32::MonitorInfo monitor = WinEase::Win32::monitorForWindow(rulerHandle);
        reporter.check(rectsClose(rulerOverlay->overlayGeometry(), monitor.geometry, 2),
                       QStringLiteral("P1-07 悬浮层铺满所在显示器（容差 ≤2px：非整数缩放的取整）"),
                       QStringLiteral("期望 %1 / 实测 %2")
                           .arg(rectText(monitor.geometry), rectText(rulerOverlay->overlayGeometry())));
        reporter.check(zOrderIndexOf(rulerHandle) < zOrderIndexOf(probeHandle),
                       QStringLiteral("P1-07 悬浮层在 z 序上盖住普通窗口"),
                       QStringLiteral("悬浮层下标 %1 / 普通窗口下标 %2")
                           .arg(zOrderIndexOf(rulerHandle))
                           .arg(zOrderIndexOf(probeHandle)));

        // ---- 落点：用光标的**物理**坐标 ----
        const QPoint requestedStart(420, 320);
        moveCursorTo(requestedStart);
        const QPoint actualStart = WinEase::Win32::cursorPosition();
        status.clear();
        reporter.check(dispatchAction(manager, kRulerId, QStringLiteral("setStart")),
                       QStringLiteral("P1-07 触发「落点」"));
        reporter.check(
            status.contains(QStringLiteral("起点已设为 %1,%2").arg(actualStart.x()).arg(actualStart.y())),
            QStringLiteral("P1-07 落点记录的是光标的**物理**像素坐标"),
            QStringLiteral("状态：%1（光标实际 %2,%3）")
                .arg(status.last())
                .arg(actualStart.x())
                .arg(actualStart.y()));

        // ---- 跟随鼠标量长度：垂直拉开 400 物理像素（垂距是精确值，不受斜向取整影响）----
        const QPoint requestedEnd(actualStart.x(), actualStart.y() + 400);
        moveCursorTo(requestedEnd);
        const QPoint actualEnd = WinEase::Win32::cursorPosition();
        const int expectedLength = qAbs(actualEnd.y() - actualStart.y());

        const bool lengthReported = waitFor(
            [&status] { return firstNumberAfter(lastWith(status, QStringLiteral("标尺长度")),
                                                QStringLiteral("标尺长度"))
                               != INT_MIN; },
            2500);
        const int measured =
            firstNumberAfter(lastWith(status, QStringLiteral("标尺长度")), QStringLiteral("标尺长度"));
        const bool lengthOk = lengthReported && measured != INT_MIN
                              && qAbs(measured - expectedLength) <= 2;
        reporter.check(lengthOk,
                       QStringLiteral("P1-07 标尺长度按物理像素量出来（容差 ±2px，容忍真实鼠标抖动）"),
                       QStringLiteral("期望 %1 px / 实测 %2 px（状态：%3）")
                           .arg(expectedLength)
                           .arg(measured)
                           .arg(lastWith(status, QStringLiteral("标尺长度"))));

        // ---- 单位切换：px → cm ----
        status.clear();
        reporter.check(dispatchAction(manager, kRulerId, QStringLiteral("unit")),
                       QStringLiteral("P1-07 触发「切换单位」"));
        reporter.check(status.contains(QStringLiteral("单位已切换为 cm")),
                       QStringLiteral("P1-07 单位切到厘米（按系统 DPI 估算，界面上已如实标注）"),
                       status.last());
        const bool cmReported = waitFor(
            [&status] { return lastWith(status, QStringLiteral("标尺长度")).contains(QStringLiteral("cm")); },
            2500);
        reporter.check(cmReported,
                       QStringLiteral("P1-07 切单位后长度立即按新单位重算"),
                       lastWith(status, QStringLiteral("标尺长度")));

        // ---- 点击穿透开关 ----
        status.clear();
        reporter.check(dispatchAction(manager, kRulerId, QStringLiteral("interactive")),
                       QStringLiteral("P1-07 触发「切换可拖拽」"));
        reporter.check(!hasExStyle(rulerHandle, WS_EX_TRANSPARENT),
                       QStringLiteral("P1-07 可拖拽模式关闭了点击穿透（改的是扩展样式，不重建窗口）"),
                       QStringLiteral("状态：%1").arg(status.last()));
        reporter.check(dispatchAction(manager, kRulerId, QStringLiteral("interactive")),
                       QStringLiteral("P1-07 再触发一次切回"));
        reporter.check(hasExStyle(rulerHandle, WS_EX_TRANSPARENT),
                       QStringLiteral("P1-07 切回后恢复点击穿透"));

        // ---- 清除：桌面必须干净 ----
        status.clear();
        reporter.check(dispatchAction(manager, kRulerId, QStringLiteral("clear")),
                       QStringLiteral("P1-07 触发「清除全部标尺」"));
        reporter.check(host->overlaysOfOwner(kRulerId).isEmpty(),
                       QStringLiteral("P1-07 清除后悬浮层被回收（桌面上不留一层点不掉的窗口）"),
                       QStringLiteral("残留 %1 个").arg(host->overlaysOfOwner(kRulerId).size()));
        reporter.check(status.contains(QStringLiteral("已清除")),
                       QStringLiteral("P1-07 状态文本反馈清除结果"),
                       status.last());

        reporter.check(manager.setPluginEnabled(kRulerId, false), QStringLiteral("P1-07 插件停用成功"));
        reporter.check(host->overlaysOfOwner(kRulerId).isEmpty(),
                       QStringLiteral("P1-07 停用后没有残留悬浮层（宿主按 ownerId 兜底回收）"));
    }

    // =======================================================================
    //  P1-08 焦点高亮
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-08 焦点高亮 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kSpotlightId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-08 插件已从 plugins 目录加载（display.focus_highlight）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // 背景板：一块**非置顶**纯色窗口，用来给"采样点"提供一个位置确定的静止内容。
        //
        // ⚠ 用例**不假设**能看到它本身的颜色：真实桌面上可能有别的窗口盖在上面
        //   （实测就被控制台盖过一次，导致"背景板色断言"整片假失败）。
        //   因此下面所有像素断言都改成**同一个像素相对自己**的 A/B 比较：
        //   开灯前后、洞在不在上面 —— 只依赖"这块像素没变"这个前提。
        const QColor cardColor(0x33, 0x66, 0xCC);
        const SolidColorWindow card(cardColor, QPoint(320, 300), false);
        reporter.check(WinEase::Win32::isValidWindow(card.handle()),
                       QStringLiteral("P1-08 前置：背景板窗口已上屏（非置顶）"));
        if (!WinEase::Win32::isValidWindow(card.handle())) {
            return reporter.failures() - failuresAtStart;
        }
        ::SetWindowPos(card.handle(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        const QPoint samplePoint = card.physicalCenter();

        // 前置：采样点的像素必须**稳定**（连续两次读到同一个颜色），
        //       否则后面的"变暗/变亮"可能是别的东西在动
        QColor baseline;
        bool baselineStable = false;
        for (int attempt = 0; attempt < 10 && !baselineStable; ++attempt) {
            ::SetWindowPos(card.handle(), HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            QColor first;
            QColor second;
            if (readPixel(samplePoint, &first)) {
                QCoreApplication::processEvents();
                QThread::msleep(60);
                if (readPixel(samplePoint, &second) && first == second) {
                    baseline = first;
                    baselineStable = true;
                }
            }
        }
        const int baselineLuma = qMax(1, brightnessOf(baseline));
        reporter.check(baselineStable,
                       QStringLiteral("P1-08 前置：采样点像素稳定（开灯前的基准，后面全靠它做 A/B）"),
                       QStringLiteral("采样点 %1,%2 → %3（亮度 %4）")
                           .arg(samplePoint.x())
                           .arg(samplePoint.y())
                           .arg(baseline.name(QColor::HexRgb).toUpper())
                           .arg(baselineLuma));

        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kSpotlightId, true), QStringLiteral("P1-08 插件启用成功"));
        status.clear();

        reporter.check(dispatchAction(manager, kSpotlightId, QStringLiteral("default")),
                       QStringLiteral("P1-08 触发「开启聚光灯」"));
        const QList<OverlayWindow *> overlays = host->overlaysOfOwner(kSpotlightId);
        reporter.check(overlays.size() == monitorCount,
                       QStringLiteral("P1-08 每个显示器各建一个悬浮层"),
                       QStringLiteral("悬浮层 %1 个 / 显示器 %2 个").arg(overlays.size()).arg(monitorCount));
        reporter.check(status.contains(QStringLiteral("聚光灯已开启")),
                       QStringLiteral("P1-08 状态文本反馈已开启（含半径与样式）"),
                       status.last());
        if (overlays.isEmpty()) {
            manager.setPluginEnabled(kSpotlightId, false);
            return reporter.failures() - failuresAtStart;
        }

        const WindowHandle spotlightHandle = overlayHandle(overlays.first());
        reporter.check(OverlayWindow::isOverlayWindow(reinterpret_cast<void *>(spotlightHandle))
                           && hasExStyle(spotlightHandle, WS_EX_TOPMOST)
                           && hasExStyle(spotlightHandle, WS_EX_LAYERED)
                           && hasExStyle(spotlightHandle, WS_EX_TRANSPARENT)
                           && hasExStyle(spotlightHandle, WS_EX_NOACTIVATE),
                       QStringLiteral("P1-08 悬浮层：置顶 + 分层 + **点击穿透** + 不抢焦点"
                                      "（验收：不干扰被点击的窗口）"));

        // ---- 步骤 1：光标**刻意放到远处** → 采样点应当被压暗 ----
        const QPoint farPoint(samplePoint.x() + 520, samplePoint.y() + 260);
        moveCursorTo(farPoint);
        const QPoint farActual = WinEase::Win32::cursorPosition();
        const bool movedAway = waitFor(
            [&status, &farActual] { return reportsPosition(status, farActual); }, 2500);
        reporter.check(movedAway,
                       QStringLiteral("P1-08 前置：光标已移到远处，状态文本跟上（上报物理坐标）"),
                       QStringLiteral("状态：%1（光标 %2,%3）")
                           .arg(lastWith(status, QStringLiteral("高亮位置")))
                           .arg(farActual.x())
                           .arg(farActual.y()));

        const bool dimmed = waitFor(
            [&samplePoint, baselineLuma] {
                QColor color;
                return readPixel(samplePoint, &color)
                       && brightnessOf(color) * 100 < baselineLuma * 75;
            },
            3000);
        QColor dimmedPixel;
        readPixel(samplePoint, &dimmedPixel);
        reporter.check(dimmed,
                       QStringLiteral("P1-08 开灯后采样点被压暗（相对自己的基准变暗 ≥25%）"),
                       QStringLiteral("基准亮度 %1 → 实测亮度 %2（%3）")
                           .arg(baselineLuma)
                           .arg(brightnessOf(dimmedPixel))
                           .arg(dimmedPixel.name(QColor::HexRgb).toUpper()));

        // ---- 步骤 2：把光标移到采样点上 → 洞挪过来了 → 该像素应当**重新变亮** ----
        //      这是"高亮位置与鼠标一致"唯一的硬证据：同一个像素、只有光标位置不同
        moveCursorTo(samplePoint);
        const QPoint holeAt = WinEase::Win32::cursorPosition();
        const bool positionReported = waitFor(
            [&status, &holeAt] { return reportsPosition(status, holeAt); }, 2500);
        reporter.check(positionReported,
                       QStringLiteral("P1-08 高亮位置跟着光标走（状态文本给出物理坐标）"),
                       QStringLiteral("状态：%1（光标 %2,%3）")
                           .arg(lastWith(status, QStringLiteral("高亮位置")))
                           .arg(holeAt.x())
                           .arg(holeAt.y()));

        const bool litAgain = waitFor(
            [&samplePoint, baselineLuma] {
                QColor color;
                return readPixel(samplePoint, &color)
                       && brightnessOf(color) >= baselineLuma - 12;
            },
            3000);
        QColor litPixel;
        readPixel(samplePoint, &litPixel);
        reporter.check(litAgain,
                       QStringLiteral("P1-08 ★ 把光标移到采样点上后该像素恢复亮度"
                                      "（**只挪了光标** → 证明洞真的挖在光标处）"),
                       QStringLiteral("基准亮度 %1 / 实测亮度 %2（%3）")
                           .arg(baselineLuma)
                           .arg(brightnessOf(litPixel))
                           .arg(litPixel.name(QColor::HexRgb).toUpper()));

        // ---- 步骤 3：切「只画圆环」→ 不再压暗整屏 ----
        status.clear();
        reporter.check(dispatchAction(manager, kSpotlightId, QStringLiteral("style")),
                       QStringLiteral("P1-08 触发「切换样式」"));
        reporter.check(status.contains(QStringLiteral("只画圆环")),
                       QStringLiteral("P1-08 状态文本反馈样式已切换"),
                       status.last());
        const bool veilRemoved = waitFor(
            [&samplePoint, baselineLuma] {
                QColor color;
                return readPixel(samplePoint, &color)
                       && brightnessOf(color) >= baselineLuma - 12;
            },
            3000);
        reporter.check(veilRemoved,
                       QStringLiteral("P1-08 「只画圆环」样式下整屏不再被压暗（像素回到基准亮度）"),
                       QStringLiteral("基准亮度 %1 / 状态：%2").arg(baselineLuma).arg(status.last()));

        // ---- 停用：像素必须恢复（不留残影）----
        reporter.check(manager.setPluginEnabled(kSpotlightId, false),
                       QStringLiteral("P1-08 插件停用成功"));
        reporter.check(host->overlaysOfOwner(kSpotlightId).isEmpty(),
                       QStringLiteral("P1-08 停用后悬浮层被回收（无残留置顶窗口）"));

        // ⚠ 判据是"每通道 ≤8/255"而不是严格相等：实测出现过
        //   `#3366CC → #3264C7`（每通道差 1~5）且**重跑即消失**的偏差 —— 那是 DWM 合成
        //   最后一两帧的时序噪声，不是残影。真残影是"整层压暗"或"圆环描边"，
        //   量级 ≥30~40/255，8/255 对它有 4 倍以上余量（诊断信息照旧打全）。
        const auto maxChannelDelta = [](const QColor &left, const QColor &right) {
            return qMax(qMax(qAbs(left.red() - right.red()), qAbs(left.green() - right.green())),
                        qAbs(left.blue() - right.blue()));
        };
        const bool fullyRestored = waitFor(
            [&samplePoint, &baseline, &maxChannelDelta] {
                QColor color;
                return readPixel(samplePoint, &color) && maxChannelDelta(baseline, color) <= 8;
            },
            3000);
        QColor finalPixel;
        readPixel(samplePoint, &finalPixel);

        // 诊断（失败时才有价值，成本只有几次读像素）：
        //   ① 邻域像素 —— 软边（阴影/光标）会呈现梯度，整体色偏则处处一样；
        //   ② 该点顶层窗口与探针窗口矩形 —— 谁在这附近、阴影可能是谁投的；
        //   ③ 把光标移开后再读一次 —— 区分"光标引起的"与"窗口/合成引起的"。
        QString neighbors;
        for (int offset = -12; offset <= 12; offset += 6) {
            QColor color;
            readPixel(samplePoint + QPoint(offset, 0), &color);
            neighbors += color.name(QColor::HexRgb).toUpper() + QLatin1Char(' ');
        }
        const QRect probeRect = WinEase::Win32::windowRect(probe.handle());
        const QRect probeVisual = WinEase::Win32::visualWindowRect(probe.handle());
        const QPoint cursorOnPoint = WinEase::Win32::cursorPosition();
        moveCursorTo(samplePoint + QPoint(0, 240));
        QCoreApplication::processEvents();
        QThread::msleep(120);
        QColor pixelWithoutCursor;
        readPixel(samplePoint, &pixelWithoutCursor);

        const bool pixelClose = fullyRestored && finalPixel.isValid()
                                && maxChannelDelta(baseline, finalPixel) <= 8;
        reporter.check(pixelClose,
                       QStringLiteral("P1-08 停用后屏幕像素恢复（分层窗口没有留下残影）"),
                       QStringLiteral("基准 %1 / 实测 %2（最大通道差 %3）· 该点顶层窗口 %4（背景板 %5）"
                                      " · 光标 %6,%7 · 移开光标后 %8 · 横向邻域 %9 · 探针矩形 %10/%11")
                           .arg(baseline.name(QColor::HexRgb).toUpper(),
                                finalPixel.name(QColor::HexRgb).toUpper())
                           .arg(maxChannelDelta(baseline, finalPixel))
                           .arg(handleText(WinEase::Win32::topLevelWindowAt(samplePoint)),
                                handleText(card.handle()))
                           .arg(cursorOnPoint.x())
                           .arg(cursorOnPoint.y())
                           .arg(pixelWithoutCursor.name(QColor::HexRgb).toUpper())
                           .arg(neighbors,
                                rectText(probeRect),
                                rectText(probeVisual)));
    }

    reporter.check(host->overlayCount() == 0,
                   QStringLiteral("悬浮层组收尾：宿主登记表已清空（用例不留置顶窗口）"),
                   QStringLiteral("残留 %1 个").arg(host->overlayCount()));

    reporter.info(QStringLiteral("悬浮层组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
