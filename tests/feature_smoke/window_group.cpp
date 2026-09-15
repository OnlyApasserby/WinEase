// ============================================================================
//  feature_smoke / window_group.cpp —— P1-A 窗口组端到端用例
//                        （P1-01 置顶 / P1-02 分屏 / P1-03 透明度 / P1-04 置底）
//
//  与其它自检的**本质区别**：
//      win32_smoke 验的是平台原语，plugin_smoke 验的是插件契约；
//      本程序加载 **build/bin/plugins 下真实的插件 DLL**，用公开入口
//      （PluginManager::dispatchHotkey）触发功能，然后回到 Win32 侧读**真实窗口状态**。
//      也就是说：这里测的是"用户装上的那个功能到底有没有生效"，而不是"代码能不能跑"。
//
//  三条纪律（每个用例都遵守）：
//      ① 先做前置条件断言（光标下是不是探针窗口 / z 序关系），前置不成立就明确失败，
//         绝不在"目标不确定"的情况下动手改窗口 —— 那会改到用户自己的窗口上；
//      ② 通过 public 入口触发，断言 Win32 侧的真实状态（不是复述插件内部状态）；
//      ③ 用例结束必须停用插件并断言系统状态已还原。
// ============================================================================

#include "window_group.h"

#include "win32/WindowTarget.h"
#include "win32/WindowUtils.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QThread>

#include <cmath>

namespace FeatureSmoke {

namespace {

using WinEase::IFeaturePlugin;
using WinEase::PluginManager;
using WinEase::Win32::WindowHandle;
using WinEase::Win32::WindowTargetMode;

// 插件 id 与插件实现里的 id() 必须一致。
// 集中在此处：写错时用例会"看起来通过"却什么都没测到，所以只允许有一个来源。
const QString kTopId = QStringLiteral("window.always_on_top");
const QString kSnapId = QStringLiteral("window.snap_layout");
const QString kOpacityId = QStringLiteral("window.opacity");
const QString kBottomId = QStringLiteral("window.bottom");

/// 把光标移到探针窗口中心并把它抬到最前；返回"跟随鼠标"模式是否**稳定**解析到探针。
///
/// 为什么必须真的移动鼠标：四个窗口插件的默认模式就是"跟随鼠标下的窗口"，
/// 而这一步恰恰是最容易悄悄失效的地方（DPI、命中测试、顶层窗口追溯）。
///
/// 为什么是"约 1 秒内**连续稳定**"而不是"试几次就算"：
///   ① 置底插件的 z 序维持钩子是 WINEVENT_OUTOFCONTEXT —— 回调是**排队投递**的。
///      上一次 z 序实验产生的回调可能还没被处理，一旦在下面 processEvents() 时跑到，
///      就会把刚刚抬起来的窗口又压回底部，于是此刻"光标下"就不是探针了。
///      这属于被测行为的正常表现（不是缺陷）。
///   ② 这是**真实桌面**：运行期间如果有人在动鼠标，SetCursorPos 的结果会被抢回去。
///   两种情况都用"连续 3 次采样都保持目标"来吸收，**但上限就是约 1 秒**：
///   再失败就如实报**前置条件不成立**（调用方会把光标位置 / 命中窗口 / 期望窗口一起打出来），
///   绝不为了"让测试通过"无限重试，也绝不静默跳过。
bool aimCursorAtProbe(const ProbeWindow &probe)
{
    const QPoint center = probe.center();
    if (center.isNull()) {
        return false;
    }

    constexpr int kAimTimeoutMs = 1000;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kAimTimeoutMs) {
        ::SetCursorPos(center.x(), center.y());
        // 抬到最前（不激活）：保证光标所在位置命中探针而不是别人的窗口
        ::SetWindowPos(probe.handle(), HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (waitForResolvedTarget(WindowTargetMode::FollowCursor, probe.handle(), 200, 3, 30)) {
            return true;
        }
    }
    return false;
}

/// 瞄准失败时用来定位原因：光标在哪、命中了谁、期望是谁
QString aimDetail(const ProbeWindow &probe)
{
    const QPoint cursor = WinEase::Win32::cursorPosition();
    return QStringLiteral("光标 %1,%2 命中 %3，期望探针 %4")
        .arg(cursor.x())
        .arg(cursor.y())
        .arg(handleText(WinEase::Win32::resolveWindowTarget(WindowTargetMode::FollowCursor, nullptr)),
             handleText(probe.handle()));
}

bool pointsClose(const QPoint &actual, const QPoint &expected, int tolerance)
{
    return std::abs(actual.x() - expected.x()) <= tolerance
           && std::abs(actual.y() - expected.y()) <= tolerance;
}

bool approx(qreal actual, qreal expected, qreal tolerance)
{
    return std::abs(actual - expected) <= tolerance;
}

bool isLayered(WindowHandle hwnd)
{
    return WinEase::Win32::isValidWindow(hwnd)
           && (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
}

/// 期望区域 vs 实测视觉边界：
///   容差 ≤1px（非整数缩放的取整损耗有方向）+ 不超出期望区域 ±1px。
/// 分屏对"露缝/重叠"极其敏感，单纯比对相等会因为 1px 取整而误报。
void checkRect(Reporter &reporter,
               const QString &what,
               const QRect &actual,
               const QRect &expected)
{
    const bool ok = rectsClose(actual, expected, 1) && expected.adjusted(-1, -1, 1, 1).contains(actual);
    reporter.check(ok,
                   what,
                   QStringLiteral("期望 %1，实测 %2").arg(rectText(expected), rectText(actual)));
}

// 区域规格（测试侧独立写一遍"规范"，与插件实现对照；不调用插件的私有函数）
QRect specLeft(const QRect &area)
{
    return QRect(area.left(), area.top(), area.width() / 2, area.height());
}

QRect specRight(const QRect &area)
{
    const int half = area.width() / 2;
    return QRect(area.left() + half, area.top(), area.width() - half, area.height());
}

QRect specTopLeft(const QRect &area)
{
    return QRect(area.left(), area.top(), area.width() / 2, area.height() / 2);
}

QRect specBottomRight(const QRect &area)
{
    const int halfW = area.width() / 2;
    const int halfH = area.height() / 2;
    return QRect(area.left() + halfW,
                 area.top() + halfH,
                 area.width() - halfW,
                 area.height() - halfH);
}

} // namespace

int runWindowGroupTests(Reporter &reporter,
                        PluginManager &manager,
                        StubServices &services,
                        ProbeWindow &probe)
{
    const int failuresAtStart = reporter.failures();

    // 用例会把真实光标挪来挪去（"跟随鼠标"模式必须真的移光标），结束后还原
    const QPoint savedCursor = WinEase::Win32::cursorPosition();
    struct CursorRestorer {
        QPoint saved;
        ~CursorRestorer()
        {
            if (!saved.isNull()) {
                ::SetCursorPos(saved.x(), saved.y());
            }
        }
    } cursorRestorer{ savedCursor };

    const WindowHandle probeHandle = probe.handle();

    // =======================================================================
    //  P1-01 窗口置顶
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-01 窗口置顶 ----"));
    {
        IFeaturePlugin *plugin = manager.plugin(kTopId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-01 插件已从 plugins 目录加载（window.always_on_top）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        reporter.check(aimCursorAtProbe(probe),
                       QStringLiteral("P1-01 前置：光标下的顶层窗口就是探针窗口"),
                       QStringLiteral("光标 %1,%2，命中 %3")
                           .arg(WinEase::Win32::cursorPosition().x())
                           .arg(WinEase::Win32::cursorPosition().y())
                           .arg(handleText(WinEase::Win32::resolveWindowTarget(WindowTargetMode::FollowCursor,
                                                                               nullptr))));
        reporter.check(!WinEase::Win32::isTopMost(probeHandle),
                       QStringLiteral("P1-01 前置：探针窗口初始不是置顶窗口"));

        reporter.check(manager.setPluginEnabled(kTopId, true), QStringLiteral("P1-01 插件启用成功"));
        reporter.check(services.hasHotkey(kTopId + QStringLiteral("::lock")),
                       QStringLiteral("P1-01 启用时注册了「锁定目标窗口」辅助快捷键"));

        StatusLog status;
        status.attach(plugin);

        // ---- 锁定语义：锁定之后不再依赖鼠标位置 ----
        //
        // 判定顺序（每一步都要先成立，才谈得上下一步）：
        //   ① 锁定前，目标**确实是窗口 A**（探针）—— 跟随鼠标就能解析到它；
        //   ② 锁定操作返回成功（状态文本反馈）；
        //   ③ 光标移到窗口 B（探针子进程的辅助窗口）上；
        //   ④ **等窗口 B 成为"当前解析目标"并稳定住**（连续 3 次采样，见 waitForResolvedTarget）；
        //   ⑤ 执行插件动作；
        //   ⑥ 断言动作**仍作用于窗口 A**，而且窗口 B **没有**被作用到。
        //
        // ⚠ 全程**不看前台窗口是谁**：前台会自己变（探针子进程在抢、用户也在点东西），
        //   拿它当"锁定还生效"的证据就是用了一个不属于本功能的信号做断言 ——
        //   那正是 P1-04 前置偶发失败时"看起来像是锁定坏了"的根源。
        reporter.check(WinEase::Win32::resolveWindowTarget(WindowTargetMode::FollowCursor, nullptr)
                           == probeHandle,
                       QStringLiteral("P1-01 ① 锁定前：目标确实是窗口 A（跟随鼠标能稳定解析到它）"),
                       aimDetail(probe));

        reporter.check(dispatchAction(manager, kTopId, QStringLiteral("lock")),
                       QStringLiteral("P1-01 ② 触发「锁定目标窗口」"));
        reporter.check(status.contains(QStringLiteral("已锁定")),
                       QStringLiteral("P1-01 ② 锁定结果通过状态文本反馈"),
                       status.last());

        status.clear();

        // ③④ 把光标移到**另一个可操作窗口 B**（辅助窗口）上，并等它**稳定地**成为解析目标。
        //     不要用"屏幕角落/桌面"当对照：平台层在光标下拿不到可用窗口时会**回退到前台窗口**，
        //     而前台窗口很可能就是探针自己，于是这个对照条件会莫名其妙地"成立"，
        //     锁定语义就白测了。换成另一个真实窗口后，解析**成功但指向别人** —— 这才是有效对照。
        const QRect helperRect = WinEase::Win32::windowRect(probe.helperHandle());
        bool aimedAtOtherWindow = false;
        if (helperRect.isValid()) {
            constexpr int kAimOtherTimeoutMs = 1000;
            QElapsedTimer otherTimer;
            otherTimer.start();
            while (otherTimer.elapsed() < kAimOtherTimeoutMs && !aimedAtOtherWindow) {
                ::SetWindowPos(probe.helperHandle(), HWND_TOP, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ::SetCursorPos(helperRect.center().x(), helperRect.center().y());
                // ★ 等它"成为当前解析目标**并稳定住**"，而不是"某一次等于"
                aimedAtOtherWindow = waitForResolvedTarget(
                    WindowTargetMode::FollowCursor, probe.helperHandle(), 200, 3, 30);
            }
        }
        reporter.check(aimedAtOtherWindow,
                       QStringLiteral("P1-01 ③④ 光标移到窗口 B 后，「跟随鼠标」**稳定地**解析到 B"
                                      "（锁定语义的对照条件）"),
                       QStringLiteral("辅助窗口 %1 / 当前解析 = %2")
                           .arg(rectText(helperRect),
                                handleText(WinEase::Win32::resolveWindowTarget(WindowTargetMode::FollowCursor,
                                                                              nullptr))));

        // ⑤ 执行动作
        reporter.check(dispatchAction(manager, kTopId, QStringLiteral("default")),
                       QStringLiteral("P1-01 ⑤ 触发置顶（default 动作被分发）"));
        settleEvents();

        // ⑥ 断言动作落在 A 上，且 B 没被波及
        reporter.check(WinEase::Win32::isTopMost(probeHandle),
                       QStringLiteral("P1-01 ⑥ ★ 置顶仍作用在**锁定的窗口 A** 上（此时鼠标在窗口 B 上）"));
        reporter.check(!WinEase::Win32::isTopMost(probe.helperHandle()),
                       QStringLiteral("P1-01 ⑥ ★ 对照：窗口 B **没有**被置顶（动作没有被鼠标带走）"));
        reporter.check(status.contains(QStringLiteral("已置顶")),
                       QStringLiteral("P1-01 ⑤ 状态文本带上了具体窗口"),
                       status.last());

        status.clear();
        reporter.check(dispatchAction(manager, kTopId, QStringLiteral("default")),
                       QStringLiteral("P1-01 再次触发以取消置顶"));
        reporter.check(!WinEase::Win32::isTopMost(probeHandle),
                       QStringLiteral("P1-01 再次触发后置顶被取消"));

        reporter.check(dispatchAction(manager, kTopId, QStringLiteral("unpin_all")),
                       QStringLiteral("P1-01 触发「取消全部置顶」（此时已无置顶记录）"));
        reporter.check(status.contains(QStringLiteral("没有置顶窗口")),
                       QStringLiteral("P1-01 无置顶记录时状态文本如实反馈"),
                       status.last());

        // ---- 停用还原：这是 P1-01 验收里最硬的一条 ----
        status.clear();
        dispatchAction(manager, kTopId, QStringLiteral("default"));
        reporter.check(WinEase::Win32::isTopMost(probeHandle),
                       QStringLiteral("P1-01 重新置顶（为验证停用还原做准备）"));

        reporter.check(dispatchAction(manager, kTopId, QStringLiteral("lock")),
                       QStringLiteral("P1-01 触发解除锁定"));
        reporter.check(status.contains(QStringLiteral("已解除锁定")),
                       QStringLiteral("P1-01 解锁结果通过状态文本反馈"),
                       status.last());

        reporter.check(manager.setPluginEnabled(kTopId, false), QStringLiteral("P1-01 插件停用成功"));
        reporter.check(!WinEase::Win32::isTopMost(probeHandle),
                       QStringLiteral("P1-01 停用后被置顶的窗口恢复原状"));
        reporter.check(!services.hasHotkey(kTopId + QStringLiteral("::lock")),
                       QStringLiteral("P1-01 停用时注销了辅助快捷键"));
    }

    // =======================================================================
    //  P1-02 窗口快速分屏
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-02 窗口快速分屏 ----"));
    {
        IFeaturePlugin *plugin = manager.plugin(kSnapId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-02 插件已从 plugins 目录加载（window.snap_layout）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        const WinEase::Win32::MonitorInfo monitor = WinEase::Win32::monitorForWindow(probeHandle);
        reporter.check(monitor.valid && !monitor.workArea.isEmpty(),
                       QStringLiteral("P1-02 前置：查到探针窗口所在显示器的工作区"),
                       QStringLiteral("workArea = %1").arg(rectText(monitor.workArea)));
        if (!monitor.valid || monitor.workArea.isEmpty()) {
            return reporter.failures() - failuresAtStart;
        }
        const QRect area = monitor.workArea;

        // 「跟随鼠标」模式下窗口每移动一次，光标就可能落到窗口外（用户按一下把窗口
        // 甩到自己脚边是常态）→ 每个动作之前都必须重新把光标放回窗口上，并且把这个
        // 前置条件断言出来。否则动作会静默地作用到**别的**窗口上，测试变成假通过。
        const auto aim = [&reporter, &probe](const QString &what) {
            const bool ok = aimCursorAtProbe(probe);
            reporter.check(ok,
                           QStringLiteral("P1-02 前置：执行%1之前光标重新落到探针窗口上").arg(what),
                           aimDetail(probe));
            return ok;
        };
        const auto snapTo = [&](const QString &action, const QString &what, const QRect &expected) {
            if (!aim(what)) {
                return;
            }
            if (!dispatchAction(manager, kSnapId, action)) {
                reporter.check(false, QStringLiteral("P1-02 %1的快捷键被分发").arg(what));
                return;
            }
            settleEvents(); // 阶段之间显式走一遍事件队列：窗口移动后要等一帧才量得准
            checkRect(reporter, QStringLiteral("P1-02 %1").arg(what), probe.visualRect(), expected);
        };

        aim(QStringLiteral("启用插件"));
        reporter.check(manager.setPluginEnabled(kSnapId, true), QStringLiteral("P1-02 插件启用成功"));

        // ---- 左半 / 右半：同时验证"无缝拼接" ----
        snapTo(QStringLiteral("default"),
               QStringLiteral("「左半」把窗口摆到工作区左半边"),
               specLeft(area));
        const QRect gotLeft = probe.visualRect();

        snapTo(QStringLiteral("right"),
               QStringLiteral("「右半」把窗口摆到工作区右半边"),
               specRight(area));
        const QRect gotRight = probe.visualRect();

        // 奇偶宽度 / 非整数缩放下最容易露缝：一次断言把它钉死
        const bool seamless = gotLeft.isValid() && gotRight.isValid()
                              && (gotLeft.right() + 1 == gotRight.left())
                              && (gotLeft.width() + gotRight.width() == area.width());
        reporter.check(seamless,
                       QStringLiteral("P1-02 左右两半精确铺满工作区（不重叠、不留缝）"),
                       QStringLiteral("左 %1 + 右 %2，工作区宽 %3")
                           .arg(rectText(gotLeft), rectText(gotRight))
                           .arg(area.width()));

        snapTo(QStringLiteral("topleft"),
               QStringLiteral("「左上」四象限位置正确"),
               specTopLeft(area));

        snapTo(QStringLiteral("bottomright"),
               QStringLiteral("「右下」四象限位置正确"),
               specBottomRight(area));

        // ---- 居中：把**视觉边界**摆到工作区中心，且不改变视觉尺寸 ----
        // 断言用视觉矩形而不是窗口矩形：二者相差不可见投影边框，
        // 拿窗口矩形断言会把"边框缩了一圈"这种缺陷当成正常。
        const QRect beforeCenter = probe.visualRect();
        if (aim(QStringLiteral("「居中」"))) {
            if (dispatchAction(manager, kSnapId, QStringLiteral("center"))) {
                const QRect centered = probe.visualRect();
                reporter.check(pointsClose(centered.center(), area.center(), 1),
                               QStringLiteral("P1-02 「居中」把窗口视觉边界移到工作区中心"),
                               QStringLiteral("窗口视觉中心 %1,%2 / 工作区中心 %3,%4")
                                   .arg(centered.center().x())
                                   .arg(centered.center().y())
                                   .arg(area.center().x())
                                   .arg(area.center().y()));
                reporter.check(centered.size() == beforeCenter.size(),
                               QStringLiteral("P1-02 「居中」不改变窗口尺寸（预览过一次的缺陷：每居中一次缩 ~18x9）"),
                               QStringLiteral("%1x%2 → %3x%4")
                                   .arg(beforeCenter.width())
                                   .arg(beforeCenter.height())
                                   .arg(centered.width())
                                   .arg(centered.height()));
            } else {
                reporter.check(false, QStringLiteral("P1-02 「居中」快捷键被分发"));
            }
        }

        // ---- 停用：分屏只移动窗口，用户随时可再改 → 有意**不做**还原 ----
        const QRect beforeDisable = probe.visualRect();
        reporter.check(manager.setPluginEnabled(kSnapId, false), QStringLiteral("P1-02 插件停用成功"));
        reporter.check(rectsClose(probe.visualRect(), beforeDisable, 1),
                       QStringLiteral("P1-02 停用不移动窗口（移动窗口属于用户可再改的操作，有意不还原）"),
                       QStringLiteral("停用前 %1，停用后 %2")
                           .arg(rectText(beforeDisable), rectText(probe.visualRect())));
    }

    // =======================================================================
    //  P1-03 窗口透明度
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-03 窗口透明度 ----"));
    {
        IFeaturePlugin *plugin = manager.plugin(kOpacityId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-03 插件已从 plugins 目录加载（window.opacity）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        reporter.check(aimCursorAtProbe(probe), QStringLiteral("P1-03 前置：光标下的窗口是探针窗口"));
        reporter.check(approx(WinEase::Win32::opacity(probeHandle), 1.0, 1e-6)
                           && !isLayered(probeHandle),
                       QStringLiteral("P1-03 前置：探针窗口初始完全不透明且不是分层窗口"));

        // 订阅必须挂在启用**之前**：启用瞬间那条"当前步长 x%"正是配置生效的证据，
        // 挂在后面就只能收到后续动作的消息（第一版就是这么漏掉这条断言的）
        StatusLog status;
        status.attach(plugin);

        reporter.check(manager.setPluginEnabled(kOpacityId, true), QStringLiteral("P1-03 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("步长 25%")),
                       QStringLiteral("P1-03 启用时读取配置文件里的步长（stepPercent=25）"),
                       status.last());
        status.clear();

        // 期望值按 alpha 量化误差留 0.01 余量：0.75 → alpha 191 → 0.749
        if (dispatchAction(manager, kOpacityId, QStringLiteral("default"))) {
            settleEvents(); // 阶段之间把事件队列走空（分层属性要经过一次窗口消息才生效）
            reporter.check(approx(WinEase::Win32::opacity(probeHandle), 0.75, 0.01),
                           QStringLiteral("P1-03 「变淡一档」把 100% 降到 75%（步长来自配置）"),
                           QStringLiteral("实测 %1%").arg(qRound(WinEase::Win32::opacity(probeHandle) * 100)));
        } else {
            reporter.check(false, QStringLiteral("P1-03 「变淡一档」快捷键被分发"));
        }
        reporter.check(isLayered(probeHandle),
                       QStringLiteral("P1-03 为应用 alpha 加上了 WS_EX_LAYERED（SetLayeredWindowAttributes 的前提）"));

        dispatchAction(manager, kOpacityId, QStringLiteral("default"));
        settleEvents();
        reporter.check(approx(WinEase::Win32::opacity(probeHandle), 0.50, 0.01),
                       QStringLiteral("P1-03 连续变淡按步长累加（75% → 50%）"),
                       QStringLiteral("实测 %1%").arg(qRound(WinEase::Win32::opacity(probeHandle) * 100)));

        dispatchAction(manager, kOpacityId, QStringLiteral("increase"));
        settleEvents();
        reporter.check(approx(WinEase::Win32::opacity(probeHandle), 0.75, 0.01),
                       QStringLiteral("P1-03 「变回一档」按步长回退（50% → 75%）"),
                       QStringLiteral("实测 %1%").arg(qRound(WinEase::Win32::opacity(probeHandle) * 100)));

        dispatchAction(manager, kOpacityId, QStringLiteral("reset"));
        settleEvents();
        reporter.check(approx(WinEase::Win32::opacity(probeHandle), 1.0, 1e-6) && !isLayered(probeHandle),
                       QStringLiteral("P1-03 「恢复不透明」把窗口还原成普通窗口（清掉分层样式，而非只把 alpha 设回 255）"));

        // ---- 停用还原 ----
        dispatchAction(manager, kOpacityId, QStringLiteral("default"));
        dispatchAction(manager, kOpacityId, QStringLiteral("default"));
        settleEvents();
        reporter.check(approx(WinEase::Win32::opacity(probeHandle), 0.50, 0.01),
                       QStringLiteral("P1-03 停用前再次调淡（为验证还原做准备）"),
                       QStringLiteral("实测 %1%").arg(qRound(WinEase::Win32::opacity(probeHandle) * 100)));

        reporter.check(manager.setPluginEnabled(kOpacityId, false), QStringLiteral("P1-03 插件停用成功"));
        reporter.check(approx(WinEase::Win32::opacity(probeHandle), 1.0, 1e-6) && !isLayered(probeHandle),
                       QStringLiteral("P1-03 停用后被调整过的窗口恢复完全不透明（不留副作用）"));
    }

    // =======================================================================
    //  P1-04 窗口置底
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-04 窗口置底 ----"));
    {
        IFeaturePlugin *plugin = manager.plugin(kBottomId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-04 插件已从 plugins 目录加载（window.bottom）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        const int indexBefore = zOrderIndexOf(probeHandle);
        reporter.check(indexBefore > 0 && !isNoActivate(probeHandle),
                       QStringLiteral("P1-04 前置：探针窗口可见、未被压制焦点，且上方还有其它窗口"),
                       QStringLiteral("z 序下标 %1（0 为最上）").arg(indexBefore));

        // 同 P1-02：动作会把窗口压到 z 序最底，之后光标底下就不再是探针窗口了，
        // 每个动作之前都要重新建立"光标在探针上"这个前提
        const auto aim = [&reporter, &probe](const QString &what) {
            const bool ok = aimCursorAtProbe(probe);
            reporter.check(ok,
                           QStringLiteral("P1-04 前置：执行%1之前光标重新落到探针窗口上").arg(what),
                           aimDetail(probe));
            return ok;
        };

        reporter.check(manager.setPluginEnabled(kBottomId, true), QStringLiteral("P1-04 插件启用成功"));

        StatusLog status;
        status.attach(plugin);

        aim(QStringLiteral("置底"));
        reporter.check(dispatchAction(manager, kBottomId, QStringLiteral("default")),
                       QStringLiteral("P1-04 触发置底（default 动作被分发）"));
        const int indexPinned = zOrderIndexOf(probeHandle);
        reporter.check(indexPinned > indexBefore,
                       QStringLiteral("P1-04 窗口被压到 z 序底部（会被其它窗口正常遮挡）"),
                       QStringLiteral("置底前下标 %1 → 置底后 %2").arg(indexBefore).arg(indexPinned));
        reporter.check(isNoActivate(probeHandle),
                       QStringLiteral("P1-04 置底窗口带 WS_EX_NOACTIVATE（点击不激活到最前）"));
        reporter.check(status.contains(QStringLiteral("已钉到底部")),
                       QStringLiteral("P1-04 状态文本反馈置底成功"),
                       status.last());

        // ---- z 序维持：前台切换必须来自**别的进程** ----
        // WINEVENT_SKIPOWNPROCESS 会过滤掉装钩子的进程自己产生的事件，
        // 所以这段实验只能交给探针子进程做（它先把窗口抬到最顶，再激活自己的辅助窗口）。
        status.clear();
        probe.requestRaise();
        // ★ 判定"前台已切到辅助窗口"用**连续稳定**（1500ms 内连续 3 次采样都保持目标），
        //   而不是"某一次等于"：子进程是"先抬目标窗口、再激活辅助窗口"，
        //   中间必然经过瞬态；一次命中就往下走，后面那条"z 序维持"就在没稳的状态上做断言。
        //   两次尝试各自 1500ms（第二次是为了错开子进程那两轮尝试的时序）；
        //   仍失败就**如实报前置不成立** —— 不无限重试，也不静默跳过。
        bool activated = waitForForeground(probe.helperHandle(), 1500);
        if (!activated) {
            probe.requestRaise();
            activated = waitForForeground(probe.helperHandle(), 1500);
        }
        reporter.check(activated,
                       QStringLiteral("P1-04 前置：子进程成功把辅助窗口切到前台（前台事件必须来自别的进程）"),
                       QStringLiteral("辅助窗口 %1，当前前台 %2。"
                                      "若这条失败而紧随其后的「z 序维持」通过，说明本机**前台锁定**"
                                      "拒绝了切换（通常是运行期间有人在操作窗口）——"
                                      "此时那条「维持生效」是**假通过**，请重跑自检。")
                           .arg(handleText(probe.helperHandle()),
                                handleText(WinEase::Win32::foregroundWindow())));

        // 阶段之间显式把事件队列走空：z 序维持的钩子回调是**排队投递**的，
        // 不等它跑完就断言，等于在"还没轮到"的状态上做判断（踩坑 #27 的同族）
        settleEvents();
        const bool pushedBack = waitFor(
            [&] { return zOrderIndexOf(probeHandle) >= indexPinned; }, 3000);
        reporter.check(pushedBack,
                       QStringLiteral("P1-04 前台切换后置底窗口被自动压回底部（z 序维持钩子生效）"),
                       QStringLiteral("置底后下标 %1，实际 %2")
                           .arg(indexPinned)
                           .arg(zOrderIndexOf(probeHandle)));

        aim(QStringLiteral("「解除置底」"));
        reporter.check(dispatchAction(manager, kBottomId, QStringLiteral("release")),
                       QStringLiteral("P1-04 触发「解除置底」"));
        reporter.check(!isNoActivate(probeHandle),
                       QStringLiteral("P1-04 解除后扩展样式已还原（窗口重新可以被激活）"));

        // ---- 停用还原 ----
        aim(QStringLiteral("再次置底"));
        dispatchAction(manager, kBottomId, QStringLiteral("default"));
        reporter.check(isNoActivate(probeHandle),
                       QStringLiteral("P1-04 再次置底（为验证停用还原做准备）"));
        reporter.check(manager.setPluginEnabled(kBottomId, false), QStringLiteral("P1-04 插件停用成功"));
        reporter.check(!isNoActivate(probeHandle),
                       QStringLiteral("P1-04 停用后窗口扩展样式还原（不留副作用）"));
    }

    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
