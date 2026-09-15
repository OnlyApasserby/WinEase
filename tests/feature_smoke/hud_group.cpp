// ============================================================================
//  feature_smoke / hud_group.cpp —— P3-07 第一批端到端用例（硬件监控悬浮窗）
//
//  本组验的是"**面板上写的是不是真话**"，分两层：
//
//    ① 显示模型层（plugins/common/HudMetrics，与插件编同一份源码）：
//       · 逐项开关真的会增删行，顺序稳定；
//       · 格式化边界（0 B/s、1023 B/s、1024 B/s、截断到 100%、°C 取整）；
//       · ★ 本功能最硬的一条纪律：**读不到的指标必须写出原因，
//         绝不允许显示 0 或留空** —— 断言的是"必须非空 + 绝不像一个读数"
//         （判据见 looksLikeReading()：不许"0 / 0°C"，但允许原因里带错误码）。
//
//    ② 真实悬浮窗层（QPluginLoader 加载 build/bin/plugins 里的真 DLL）：
//       · 启用即出一个**置顶 / 分层 / 不抢焦点 / 点击穿透**的小面板，
//         位置在主屏工作区右上角（留白）；
//       · ★ 面板上的读数来自**本机真实硬件**：内存一行必须带
//         "总量 > 0 且已用 ≤ 总量"的真实量级（这恰恰是"0 占位"过不去的断言），
//         磁盘温度一行要么是 0~120 °C 的合理值、要么是**非空的不可用原因**；
//       · ★ CPU 温度必须**如实报"需要 PawnIOLib.dll"**，不得显示 0°C/空白
//         （第二里程碑接上 PawnIO 后本组会随之改成"必须有真实读数"）；
//       · 逐项开关当场改变面板行数（窗口高度跟着变，不是只在配置里改）；
//       · 位置持久化（禁用→再启用回到同一处）、复位到默认位置、
//         可拖拽 / 穿透切换、停用后**不留置顶窗口**。
//
//  ⚠ 为什么读数断言不做"与平台层逐值相等"：CPU / 网速 / GPU 是**两次计数之差**，
//     谁先采谁后采天然不同。所以本组的判据是"**量级/边界/同源性**"：
//     总量必须真实（>0 且已用 ≤ 总量）、温度必须落在合理区间、
//     而"不可用"必须是原因文本 —— 这三条已经足以让"0 占位 / 空白 / 假数据"全部变红。
// ============================================================================

#include "hud_group.h"

#include "HudMetrics.h"

#include "hud_overlay.h"

#include "win32/SystemInfo.h"
#include "win32/WindowUtils.h"

#include <windows.h>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QWidget>

#include <climits>

namespace FeatureSmoke {

namespace {

using WinEase::FeaturePlugins::Hud::HudSnapshot;
using WinEase::FeaturePlugins::Hud::MetricInput;
using WinEase::FeaturePlugins::Hud::MetricKind;
using WinEase::FeaturePlugins::Hud::MetricOptions;
using WinEase::FeaturePlugins::Hud::MetricReading;
using WinEase::OverlayWindow;
using WinEase::Win32::WindowHandle;

const QString kHudId = QStringLiteral("monitor.hardware_hud");

// 设置面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kCpuCheck = QStringLiteral("hudCpuCheck");
const QString kMemoryCheck = QStringLiteral("hudMemoryCheck");
const QString kNetworkCheck = QStringLiteral("hudNetworkCheck");
const QString kGpuCheck = QStringLiteral("hudGpuCheck");
const QString kTemperatureCheck = QStringLiteral("hudTemperatureCheck");
const QString kIntervalSpin = QStringLiteral("hudIntervalSpin");
const QString kInteractiveCheck = QStringLiteral("hudInteractiveCheck");
const QString kResetButton = QStringLiteral("hudResetPositionButton");
const QString kDetailLabel = QStringLiteral("hudDetailLabel");
const QString kUnavailableLabel = QStringLiteral("hudUnavailableLabel");

/// 面板默认宽度（HudOverlay::Style::panelWidth）——自检据此算窗口尺寸预期
constexpr int kPanelWidthLogical = 300;

/// 面板外框逻辑宽度 = 面板宽 + 左右阴影边距（各 8）
constexpr int kOuterWidthLogical = kPanelWidthLogical + 16;

/// 面板四周阴影边距（HudOverlay::Style::shadowMargin）——物理↔面板矩形换算要用
constexpr int kShadowMarginLogical = 8;

/// 从面板文本里取某一行的值（"内存=61% · 10.2 GB/15.9 GB" → "61% · 10.2 GB/15.9 GB"）。
/// 找不到返回空串。面板文本每行一条，行名为**行首标签**（与 MetricReading::label 一致）。
QString panelField(const QString &snapshotText, const QString &label)
{
    const QString prefix = label + QLatin1Char('=');
    const QStringList lines = snapshotText.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(prefix)) {
            return line.mid(prefix.size());
        }
    }
    return QString();
}

/// 面板文本里某一行是否被标成"不可用"（HudOverlay::snapshotText 的固定标记）
bool panelFieldUnavailable(const QString &snapshotText, const QString &label)
{
    return panelField(snapshotText, label).endsWith(QStringLiteral("[不可用]"));
}

/// 取文本里第一段连续数字（"41°C" → 41；"61% · 10.2 GB" → 61）。取不到返回 INT_MIN
int leadingInt(const QString &text)
{
    int start = -1;
    for (int index = 0; index < text.size(); ++index) {
        if (text.at(index).isDigit()) {
            start = index;
            break;
        }
    }
    if (start < 0) {
        return INT_MIN;
    }
    int end = start;
    while (end < text.size() && text.at(end).isDigit()) {
        ++end;
    }
    bool ok = false;
    const int value = text.mid(start, end - start).toInt(&ok);
    return ok ? value : INT_MIN;
}

/// 取快照里某一行的值（找不到返回 nullptr）
const MetricReading *readingOf(const HudSnapshot &snapshot, MetricKind kind)
{
    for (const MetricReading &reading : snapshot.metrics) {
        if (reading.kind == kind) {
            return &reading;
        }
    }
    return nullptr;
}

/// 一行是否**看起来像一个读数**（"0" / "0°C" / "39°C" / "61%"）
///
/// ⚠ 为什么不是"含数字就算"：真实的不可用原因**允许带数字**——
///   例如"打开磁盘设备 失败：…（错误码 32）"，那个错误码是要给用户看的诊断信息，
///   不许为了让断言好看而把它藏掉。真正必须拦死的是"**伪装成读数**"：
///   0 / 0°C / 12% 这类一眼像数值的文本（"显示 0 或留空"正是本功能的红线）。
///   所以判据收紧成"整段文本 = 一个可选带单位的数"，而不是"文本里有没有数字"。
bool looksLikeReading(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    int index = 0;
    if (trimmed.at(index) == QLatin1Char('+') || trimmed.at(index) == QLatin1Char('-')) {
        ++index;
    }

    bool hasDigit = false;
    while (index < trimmed.size()
           && (trimmed.at(index).isDigit() || trimmed.at(index) == QLatin1Char('.'))) {
        hasDigit = hasDigit || trimmed.at(index).isDigit();
        ++index;
    }
    if (!hasDigit) {
        return false;
    }

    // 数字之后只允许跟一个单位（或什么都没有）
    const QString unit = trimmed.mid(index).trimmed();
    static const QStringList kUnits = {
        QString(),      QStringLiteral("°C"), QStringLiteral("℃"), QStringLiteral("%"),
        QStringLiteral("B"),    QStringLiteral("B/s"),   QStringLiteral("KB"),
        QStringLiteral("KB/s"), QStringLiteral("MB"),    QStringLiteral("MB/s"),
        QStringLiteral("GB"),   QStringLiteral("GB/s"),  QStringLiteral("TB"),
        QStringLiteral("TB/s"),
    };
    return kUnits.contains(unit, Qt::CaseInsensitive);
}

/// 解析 "12.3 GB" 这类文本的量级（返回单位换算后的字节数；解析不出返回 0）。
/// 用途：**给"读数"设一个真实量级**，而不是只断言"非空"——两者拦住的东西完全不同。
/// ⚠ 面板上的内存一行是 "61% · 10.2 GB/15.9 GB"（斜杠两侧各一个量级），
///   所以这里同时承担"从一段文本里挑出量级"的活：按斜杠切，取第一段。
quint64 magnitudeBytes(const QString &text)
{
    const QString head = text.section(QLatin1Char('/'), 0, 0);
    const QStringList parts = head.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 2) {
        return 0;
    }
    bool numberOk = false;
    const double value = parts.at(0).toDouble(&numberOk);
    if (!numberOk) {
        return 0;
    }

    const QString unit = parts.at(1).toUpper();
    double scale = 0.0;
    if (unit == QLatin1String("B")) {
        scale = 1.0;
    } else if (unit == QLatin1String("KB")) {
        scale = 1024.0;
    } else if (unit == QLatin1String("MB")) {
        scale = 1024.0 * 1024.0;
    } else if (unit == QLatin1String("GB")) {
        scale = 1024.0 * 1024.0 * 1024.0;
    } else if (unit == QLatin1String("TB")) {
        scale = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    }
    if (scale <= 0.0) {
        return 0;
    }
    return static_cast<quint64>(value * scale);
}

/// 一行读数的文字形式（断言失败时打全现场）
QString readingText(const MetricReading *reading)
{
    if (reading == nullptr) {
        return QStringLiteral("（无此行）");
    }
    return QStringLiteral("%1=%2%3")
        .arg(reading->label, reading->valueText,
             reading->available ? QString() : QStringLiteral("（不可用）"));
}

QString metricsText(const HudSnapshot &snapshot)
{
    QStringList parts;
    for (const MetricReading &reading : snapshot.metrics) {
        parts.append(readingText(&reading));
    }
    return parts.join(QStringLiteral(" | "));
}

/// 悬浮层句柄
WindowHandle overlayHandle(OverlayWindow *overlay)
{
    return (overlay != nullptr) ? reinterpret_cast<HWND>(overlay->winId()) : nullptr;
}

/// 面板在自检这一侧的**只读视图**。
///
/// ⚠ 为什么走 QObject 属性而不是直接调 `HudOverlay::snapshot()`：
///   插件是独立 DLL，**不导出 C++ 符号**（面板类只在插件内部），直接调成员函数
///   会在链接期报 LNK2019（本组第一版就是这么失败的）。Qt 的**运行期元对象**
///   天然跨 DLL，所以"面板现在几行、写的是什么、落在哪"只从这里读。
///   这与踩坑 #18/#48 的既有约定一致：跨 DLL 边界只走运行期元对象。
///   类型识别用 `qobject_cast<OverlayWindow*>`（Q_INTERFACES 生效的标准跨 DLL 转换），
///   再核对 objectName 确认拿到的就是本插件那个面板。
class HudPanelView
{
public:
    explicit HudPanelView(WinEase::OverlayHost *host)
    {
        const QList<OverlayWindow *> overlays = host->overlaysOfOwner(kHudId);
        if (overlays.isEmpty()) {
            return;
        }
        OverlayWindow *overlay = overlays.first();
        // Q_OBJECT 类的运行期类型名跨 DLL 可查：拿它确认"这是硬件监控面板"，
        // 而不是靠 include 插件私有头文件
        if (overlay->metaObject()->className()
            != QLatin1String("WinEase::FeaturePlugins::HudOverlay")) {
            return;
        }
        m_overlay = overlay;
        m_object = overlay;
    }

    bool valid() const { return m_object != nullptr; }
    OverlayWindow *overlay() const { return m_overlay; }
    QObject *object() const { return m_object; }

    int rowCount() const { return m_object->property("metricRowCount").toInt(); }
    QString snapshotText() const { return m_object->property("snapshotText").toString(); }
    QPoint anchor() const { return m_object->property("anchorPhysical").toPoint(); }

    /// 面板本体的物理矩形。
    /// 面板左上角 = 锚点，尺寸 = 窗口尺寸 - 四周阴影边距（阴影边距是面板自己的样式常量）
    QRect panelPhysicalRect() const
    {
        const QRect outer = m_overlay->overlayGeometry();
        const int margin = qMax(0, qRound(kShadowMarginLogical * m_overlay->overlayScaleFactor()));
        return outer.adjusted(margin, margin, -margin, -margin);
    }

private:
    OverlayWindow *m_overlay = nullptr;
    QObject *m_object = nullptr;
};

/// 插件当前那个面板（没开/被回收时返回 invalid 的视图）
HudPanelView hudPanelOf(WinEase::OverlayHost *host)
{
    return HudPanelView(host);
}

/// 一次采样的原始读数（**全部标成可用**）：用来验"不可用时只改那一行"，
/// 而不是把整份模型换掉
MetricInput availableInput()
{
    MetricInput input;

    input.cpuValid = true;
    input.cpuPercent = 37.4;

    input.memoryValid = true;
    input.memoryPercent = 61.7;
    input.memoryTotalBytes = 16ull * 1024 * 1024 * 1024;
    input.memoryUsedBytes = 10ull * 1024 * 1024 * 1024;

    input.networkValid = true;
    input.rxBytesPerSecond = 912 * 1024;
    input.txBytesPerSecond = 130 * 1024;
    input.networkTotalRxBytes = 3ull * 1024 * 1024 * 1024;
    input.networkTotalTxBytes = 512ull * 1024 * 1024;

    input.gpuValid = true;
    input.gpuPercent = 12.0;

    MetricInput::DiskReading disk;
    disk.model = QStringLiteral("WinEase 自检磁盘");
    disk.celsius = 41.0;
    disk.valid = true;
    input.disks.append(disk);

    input.cpuTempValid = true;
    input.cpuTempCelsius = 55.0;

    return input;
}

/// 把某一行改成"读不到"（保留其余读数）
void makeUnavailable(MetricInput *input, MetricKind kind)
{
    switch (kind) {
    case MetricKind::Cpu:
        input->cpuValid = false;
        input->cpuError = QStringLiteral("CPU 计数器打不开");
        break;
    case MetricKind::Memory:
        input->memoryValid = false;
        input->memoryError = QStringLiteral("内存信息打不开");
        break;
    case MetricKind::Network:
        input->networkValid = false;
        input->networkError = QStringLiteral("网络计数器打不开");
        break;
    case MetricKind::Gpu:
        input->gpuValid = false;
        input->gpuError = QStringLiteral("GPU 计数器打不开");
        break;
    case MetricKind::DiskTemp:
        for (MetricInput::DiskReading &disk : input->disks) {
            disk.valid = false;
            disk.error = QStringLiteral("该设备不支持温度查询");
        }
        break;
    case MetricKind::CpuTemp:
        input->cpuTempValid = false;
        input->cpuTempError = QStringLiteral("缺 PawnIOLib.dll");
        break;
    }
}

} // namespace

int runHudGroupTests(Reporter &reporter,
                     WinEase::PluginManager &manager,
                     StubServices &services)
{
    const int failuresAtStart = reporter.failures();

    WinEase::OverlayHost *host = services.overlayHost();
    reporter.check(host != nullptr,
                   QStringLiteral("P3-07 前置：宿主提供的是**真实** OverlayHost（不是 nullptr）"));
    if (host == nullptr) {
        return reporter.failures() - failuresAtStart;
    }

    // =======================================================================
    //  第一部分：显示模型（纯函数，与插件同一份源码）
    // =======================================================================
    reporter.info(QStringLiteral("---- P3-07 显示模型（HudMetrics，纯函数）----"));
    {
        // ---- 格式化边界 ----
        reporter.check(WinEase::FeaturePlugins::Hud::formatPercent(37.4) == QStringLiteral("37%")
                           && WinEase::FeaturePlugins::Hud::formatPercent(99.6)
                                  == QStringLiteral("100%"),
                       QStringLiteral("P3-07 百分比四舍五入到整数"));
        // 多引擎 GPU 相加可能越界 —— 越界必须截断，不能画出 "437%"
        reporter.check(WinEase::FeaturePlugins::Hud::formatPercent(437.0)
                               == QStringLiteral("100%")
                           && WinEase::FeaturePlugins::Hud::formatPercent(-5.0)
                                  == QStringLiteral("0%"),
                       QStringLiteral("P3-07 百分比越界被截断到 0~100（GPU 多引擎相加会越界）"));
        reporter.check(WinEase::FeaturePlugins::Hud::formatRate(0) == QStringLiteral("0 B/s")
                           && WinEase::FeaturePlugins::Hud::formatRate(1023)
                                  == QStringLiteral("1023 B/s")
                           && WinEase::FeaturePlugins::Hud::formatRate(1024)
                                  == QStringLiteral("1.0 KB/s")
                           && WinEase::FeaturePlugins::Hud::formatRate(1024 * 1024)
                                  == QStringLiteral("1.0 MB/s"),
                       QStringLiteral("P3-07 速率单位换算的四个边界（0 / 1023B / 1KB / 1MB）"),
                       QStringLiteral("0=%1 1023=%2 1024=%3 1M=%4")
                           .arg(WinEase::FeaturePlugins::Hud::formatRate(0),
                                WinEase::FeaturePlugins::Hud::formatRate(1023),
                                WinEase::FeaturePlugins::Hud::formatRate(1024),
                                WinEase::FeaturePlugins::Hud::formatRate(1024 * 1024)));
        // 内存一行的量级必须真实（后面端到端还会用它判"0 占位"）
        reporter.check(magnitudeBytes(WinEase::FeaturePlugins::Hud::formatBytes(
                          16ull * 1024 * 1024 * 1024))
                           >= 15ull * 1024 * 1024 * 1024
                       && magnitudeBytes(WinEase::FeaturePlugins::Hud::formatBytes(
                              10ull * 1024 * 1024 * 1024))
                              >= 9ull * 1024 * 1024 * 1024,
                   QStringLiteral("P3-07 容量文本可被换算回真实量级（16GB / 10GB 不失真）"));
        reporter.check(WinEase::FeaturePlugins::Hud::formatTemperature(39.4)
                           == QStringLiteral("39°C"),
                       QStringLiteral("P3-07 温度取整并带 °C 单位"));

        // ---- 逐项开关 ----
        const MetricInput input = availableInput();

        MetricOptions all;
        const HudSnapshot full = WinEase::FeaturePlugins::Hud::buildSnapshot(input, all);
        // 温度开关同时管"磁盘温度"与"CPU 温度"两行，所以五项全开是 **6 行**
        const bool orderOk = full.metrics.size() == 6
                             && full.metrics.at(0).kind == MetricKind::Cpu
                             && full.metrics.at(1).kind == MetricKind::Memory
                             && full.metrics.at(2).kind == MetricKind::Network
                             && full.metrics.at(3).kind == MetricKind::Gpu
                             && full.metrics.at(4).kind == MetricKind::DiskTemp
                             && full.metrics.at(5).kind == MetricKind::CpuTemp;
        reporter.check(orderOk,
                       QStringLiteral("P3-07 五项全开：行序为 CPU / 内存 / 网速 / GPU / "
                                      "磁盘温度 / CPU 温度（温度开关管两行）"),
                       metricsText(full));

        // 关掉"温度" → 那两行要一起消失（一个开关管两行，不能只藏一行）
        MetricOptions noTemperature = all;
        noTemperature.temperature = false;
        const HudSnapshot withoutTemperature =
            WinEase::FeaturePlugins::Hud::buildSnapshot(input, noTemperature);
        reporter.check(withoutTemperature.metrics.size() == 4
                           && readingOf(withoutTemperature, MetricKind::DiskTemp) == nullptr
                           && readingOf(withoutTemperature, MetricKind::CpuTemp) == nullptr,
                       QStringLiteral("P3-07 关掉「温度」→ 磁盘温度与 CPU 温度**两行一起消失**"
                                      "（一个开关管两行，不会出现「只藏掉一半」）"),
                       metricsText(withoutTemperature));

        MetricOptions onlyCpu;
        onlyCpu.memory = false;
        onlyCpu.network = false;
        onlyCpu.gpu = false;
        onlyCpu.temperature = false;
        const HudSnapshot cpuOnly =
            WinEase::FeaturePlugins::Hud::buildSnapshot(input, onlyCpu);
        reporter.check(cpuOnly.metrics.size() == 1
                           && cpuOnly.metrics.first().kind == MetricKind::Cpu
                           && cpuOnly.metrics.first().available
                           && cpuOnly.metrics.first().valueText == QStringLiteral("37%"),
                       QStringLiteral("P3-07 只开 CPU：面板上只剩一行，且是真的读数"),
                       metricsText(cpuOnly));

        MetricOptions none;
        none.cpu = false;
        none.memory = false;
        none.network = false;
        none.gpu = false;
        none.temperature = false;
        const HudSnapshot empty = WinEase::FeaturePlugins::Hud::buildSnapshot(input, none);
        reporter.check(empty.metrics.isEmpty() && none.noneSelected(),
                       QStringLiteral("P3-07 一个都不开：模型返回空（面板据此画空态提示，"
                                      "而不是崩在空列表上）"));
        reporter.check(!full.title.isEmpty(),
                       QStringLiteral("P3-07 快照带标题（面板标题条要有字可画）"),
                       full.title);

        // ---- ★ 核心纪律：读不到 → 必须写出原因，不许是 0/空白 ----
        const MetricKind kinds[] = {MetricKind::Cpu, MetricKind::Memory, MetricKind::Network,
                                    MetricKind::Gpu, MetricKind::DiskTemp, MetricKind::CpuTemp};
        QString unavailableReport;
        bool unavailableOk = true;
        for (MetricKind kind : kinds) {
            MetricInput broken = input;
            makeUnavailable(&broken, kind);

            MetricOptions onlyThis;
            onlyThis.cpu = (kind == MetricKind::Cpu);
            onlyThis.memory = (kind == MetricKind::Memory);
            onlyThis.network = (kind == MetricKind::Network);
            onlyThis.gpu = (kind == MetricKind::Gpu);
            onlyThis.temperature =
                (kind == MetricKind::DiskTemp || kind == MetricKind::CpuTemp);

            const HudSnapshot snapshot =
                WinEase::FeaturePlugins::Hud::buildSnapshot(broken, onlyThis);
            const MetricReading *reading = readingOf(snapshot, kind);
            const bool rowOk = reading != nullptr && !reading->available
                               && !reading->valueText.trimmed().isEmpty()
                               && !looksLikeReading(reading->valueText);
            if (!rowOk) {
                unavailableOk = false;
            }
            unavailableReport += QStringLiteral("%1→%2；").arg(readingText(reading));
        }
        reporter.check(unavailableOk,
                       QStringLiteral("P3-07 ★ 六项指标各自不可用时，面板上写的是**原因文本**"
                                      "（非空、且绝不长得像一个读数 —— 不许显示 0 或留空）"),
                       unavailableReport);

        // ---- 不可用项不影响别的行 ----
        MetricInput halfBroken = input;
        makeUnavailable(&halfBroken, MetricKind::Gpu);
        const HudSnapshot mixed = WinEase::FeaturePlugins::Hud::buildSnapshot(halfBroken, all);
        const MetricReading *gpu = readingOf(mixed, MetricKind::Gpu);
        const MetricReading *cpu = readingOf(mixed, MetricKind::Cpu);
        reporter.check(gpu != nullptr && !gpu->available && cpu != nullptr && cpu->available
                           && cpu->valueText == QStringLiteral("37%"),
                       QStringLiteral("P3-07 只有 GPU 读不到时，CPU 那一行照旧是真读数"
                                      "（不可用是**逐行**的，不会把整面板拖垮）"),
                       metricsText(mixed));

        // ---- CPU 温度的固定口径（第二里程碑的判定点）----
        const QString cpuTempReason = WinEase::FeaturePlugins::Hud::cpuTempUnavailableText();
        reporter.check(cpuTempReason.contains(QStringLiteral("PawnIOLib"))
                           && !looksLikeReading(cpuTempReason),
                       QStringLiteral("P3-07 CPU 温度不可用时说的是「需要 PawnIOLib.dll」"
                                      "（缺什么写什么，而不是一句「不可用」）"),
                       cpuTempReason);
        const QString cpuTempTip = WinEase::FeaturePlugins::Hud::cpuTempTooltipText();
        reporter.check(cpuTempTip.contains(QStringLiteral("PawnIO"))
                           && cpuTempTip.contains(QStringLiteral(".dll")),
                       QStringLiteral("P3-07 CPU 温度的完整说明里有获取指引（去哪拿这个 DLL）"),
                       cpuTempTip.left(80) + QStringLiteral("…"));

        // ---- unavailableText 的最后一道闸 ----
        reporter.check(WinEase::FeaturePlugins::Hud::unavailableText(QString(), QStringLiteral("兜底"))
                               == QStringLiteral("兜底")
                           && WinEase::FeaturePlugins::Hud::unavailableText(
                                  QStringLiteral("   "), QStringLiteral("兜底"))
                                  == QStringLiteral("兜底")
                           && WinEase::FeaturePlugins::Hud::unavailableText(
                                  QStringLiteral("真实原因"), QStringLiteral("兜底"))
                                  == QStringLiteral("真实原因"),
                       QStringLiteral("P3-07 unavailableText：真实原因优先、空/全空白时兜底"
                                      "（返回**永不为空**是「绝不显示空白」的最后一道闸）"));
    }

    // =======================================================================
    //  第二部分：真实悬浮窗（build/bin/plugins 里那个 DLL）
    // =======================================================================
    WinEase::IFeaturePlugin *plugin = manager.plugin(kHudId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P3-07 插件已从 plugins 目录加载（monitor.hardware_hud）"));
    if (plugin == nullptr) {
        reporter.info(QStringLiteral("P3-07 组失败项：%1").arg(reporter.failures() - failuresAtStart));
        return reporter.failures() - failuresAtStart;
    }

    reporter.check(plugin->supportsHotkey()
                       && plugin->defaultHotkey() == QKeySequence(QStringLiteral("Ctrl+Alt+I")),
                   QStringLiteral("P3-07 插件声明主快捷键 Ctrl+Alt+I（风扇开关面板）"),
                   plugin->defaultHotkey().toString(QKeySequence::PortableText));
    reporter.check(services.hasHotkey(QStringLiteral("monitor.hardware_hud::default")),
                   QStringLiteral("P3-07 宿主已按插件声明注册主快捷键（用户按下去有东西发生）"),
                   services.registeredHotkeys().join(QStringLiteral(", ")));
    reporter.check(plugin->hasSettings(),
                   QStringLiteral("P3-07 插件声明有设置面板"));

    // ---- 初始配置：五项全开、1 秒刷新、点击穿透、位置已定（自检自己定，不依赖默认值）----
    services.preset(kHudId, QStringLiteral("showCpu"), true);
    services.preset(kHudId, QStringLiteral("showMemory"), true);
    services.preset(kHudId, QStringLiteral("showNetwork"), true);
    services.preset(kHudId, QStringLiteral("showGpu"), true);
    services.preset(kHudId, QStringLiteral("showTemperature"), true);
    services.preset(kHudId, QStringLiteral("intervalMs"), 1000);
    services.preset(kHudId, QStringLiteral("interactive"), false);
    services.preset(kHudId, QStringLiteral("anchorX"), 100);
    services.preset(kHudId, QStringLiteral("anchorY"), 120);

    const WinEase::Win32::MonitorInfo monitor = WinEase::Win32::primaryMonitor();
    QRect workArea = monitor.valid ? monitor.workArea : QRect();
    if (workArea.isEmpty() && monitor.valid) {
        workArea = monitor.geometry;
    }
    const qreal scale = (monitor.valid && monitor.scaleFactor > 0.0) ? monitor.scaleFactor : 1.0;
    reporter.check(!workArea.isEmpty(),
                   QStringLiteral("P3-07 前置：取到了主显示器工作区（位置断言全靠它）"),
                   rectText(workArea));

    StatusLog status;
    status.attach(plugin);

    reporter.check(manager.setPluginEnabled(kHudId, true),
                   QStringLiteral("P3-07 插件启用成功"));
    reporter.check(status.contains(QStringLiteral("已开启")),
                   QStringLiteral("P3-07 启用时给出状态提示（并说明当前是穿透还是可拖拽）"),
                   status.last());

    const QList<OverlayWindow *> overlays = host->overlaysOfOwner(kHudId);
    reporter.check(overlays.size() == 1,
                   QStringLiteral("P3-07 启用即出一个面板（常驻 HUD：不是「要按快捷键才出现」）"),
                   QStringLiteral("悬浮层 %1 个").arg(overlays.size()));
    HudPanelView panel = hudPanelOf(host);
    reporter.check(panel.valid(),
                   QStringLiteral("P3-07 ★ 拿到的确实是插件那个面板（运行期类名 = "
                                  "WinEase::FeaturePlugins::HudOverlay），且状态能从它的"
                                  "只读属性读出来 —— 自检读的是**真面板**，不是自己造的影子对象"),
                   QStringLiteral("悬浮层类名 %1")
                       .arg(overlays.isEmpty() ? QStringLiteral("（无）")
                                               : QString::fromLatin1(
                                                     overlays.first()->metaObject()->className())));
    if (!panel.valid()) {
        manager.setPluginEnabled(kHudId, false);
        reporter.info(QStringLiteral("P3-07 组失败项：%1").arg(reporter.failures() - failuresAtStart));
        return reporter.failures() - failuresAtStart;
    }

    const WindowHandle panelHandle = overlayHandle(panel.overlay());
    reporter.check(hasExStyle(panelHandle, WS_EX_TOPMOST)
                       && hasExStyle(panelHandle, WS_EX_LAYERED)
                       && hasExStyle(panelHandle, WS_EX_NOACTIVATE),
                   QStringLiteral("P3-07 面板是置顶 + 分层 + **不抢焦点**的悬浮层"
                                  "（常驻面板绝不能把用户正在打字的光标抢走）"));
    reporter.check(hasExStyle(panelHandle, WS_EX_TRANSPARENT),
                   QStringLiteral("P3-07 默认**点击穿透**（不挡下层窗口的点击 —— 这是"
                                  "常驻 HUD 能长期待在桌面上的前提）"),
                   status.last());

    // ---- 几何：指定的锚点 + 只占面板大小 ----
    const QRect panelRect = panel.panelPhysicalRect();
    reporter.check(qAbs(panelRect.left() - 100) <= 2 && qAbs(panelRect.top() - 120) <= 2,
                   QStringLiteral("P3-07 面板落在配置指定的锚点（100,120；容差 ≤2px 取整）"),
                   QStringLiteral("实测 %1").arg(rectText(panelRect)));

    const int expectedWidth = qRound(kOuterWidthLogical * scale);
    const QRect outerRect = panel.overlay()->overlayGeometry();
    reporter.check(qAbs(outerRect.width() - expectedWidth) <= 2,
                   QStringLiteral("P3-07 ★ 窗口就是**面板大小**，不是铺满屏幕"
                                  "（常驻 HUD 若铺满整屏，等于让 DWM 每帧白合成一整屏）"),
                   QStringLiteral("期望宽 %1（逻辑 %2 × 缩放 %3）/ 实测 %4")
                       .arg(expectedWidth)
                       .arg(kOuterWidthLogical)
                       .arg(scale)
                       .arg(rectText(outerRect)));

    // ---- 读数：等第一次真实刷新落地（PDH 首次只建基准，插件 0.6s 后补采一次）----
    const bool refreshed = waitFor(
        [&panel] {
            const QString text = panel.snapshotText();
            return !text.isEmpty() && !text.contains(QStringLiteral("首次采样中"))
                   && !panelField(text, QStringLiteral("内存")).isEmpty();
        },
        5000);
    const QString panelText = panel.snapshotText();
    reporter.check(refreshed,
                   QStringLiteral("P3-07 面板在几秒内刷出真实读数（不是永远停在「首次采样中…」）"),
                   panelText);

    // ★ 内存一行：真实量级（"0 占位"过不去这条）
    //   面板文本形如 "内存=61% · 10.2 GB/15.9 GB"
    const QString memoryValue = panelField(panelText, QStringLiteral("内存"));
    QString memoryUsedText;
    QString memoryTotalText;
    {
        // 内存一行的形状是 "61% · 10.2 GB/15.9 GB"：
        // 先按 "·" 去掉百分比，剩下的 "已用/总量" 才是量级 ——
        // ⚠ 不能拿"最后一个空格"切（那会切在 "10.2" 和 "GB" 之间，只切出 "GB"）
        const int dot = memoryValue.indexOf(QStringLiteral("·"));
        const QString usedAndTotal =
            (dot >= 0) ? memoryValue.mid(dot + 1).trimmed() : memoryValue.trimmed();
        const int slash = usedAndTotal.indexOf(QLatin1Char('/'));
        if (slash > 0) {
            memoryTotalText = usedAndTotal.mid(slash + 1).trimmed();
            memoryUsedText = usedAndTotal.left(slash).trimmed();
        }
    }
    const quint64 reportedTotal = magnitudeBytes(memoryTotalText);
    const quint64 reportedUsed = magnitudeBytes(memoryUsedText);
    const WinEase::Win32::MemoryInfo realMemory = WinEase::Win32::memoryInfo();
    reporter.check(!memoryValue.isEmpty() && !panelFieldUnavailable(panelText, QStringLiteral("内存"))
                       && reportedTotal > 0 && reportedUsed > 0 && reportedUsed <= reportedTotal,
                   QStringLiteral("P3-07 ★ 内存一行是**本机真实量级**：已用 > 0 且 ≤ 总量"
                                  "（这条断言专门拦「显示 0 / 显示占位符」）"),
                   QStringLiteral("面板：%1 ／ 平台层实测：%2 / %3")
                       .arg(memoryValue,
                            WinEase::FeaturePlugins::Hud::formatBytes(realMemory.usedBytes),
                            WinEase::FeaturePlugins::Hud::formatBytes(realMemory.totalBytes)));
    reporter.check(realMemory.valid && reportedTotal >= realMemory.totalBytes / 2
                       && reportedTotal <= realMemory.totalBytes * 2,
                   QStringLiteral("P3-07 面板上的内存总量与平台层读数是同一个量级"
                                  "（不是「另一套自己算的内存」）"),
                   QStringLiteral("面板 %1 / 平台层 %2")
                       .arg(reportedTotal)
                       .arg(realMemory.totalBytes));

    // ★ 磁盘温度一行：真实值 or 非空原因，二者必居其一
    const QString diskValue = panelField(panelText, QStringLiteral("磁盘温度"));
    const QList<WinEase::Win32::DiskTemperature> disks = WinEase::Win32::diskTemperatures();
    bool diskOk = !diskValue.isEmpty();
    if (diskOk && !panelFieldUnavailable(panelText, QStringLiteral("磁盘温度"))) {
        const int celsius = leadingInt(diskValue);
        diskOk = diskValue.contains(QStringLiteral("°C")) && celsius != INT_MIN && celsius >= 0
                 && celsius <= 120 && !disks.isEmpty();
    } else if (diskOk) {
        // 不可用时值文本绝不能**长得像一个读数**（"0°C" 或 "0" 都过不去）
        diskOk = !looksLikeReading(diskValue);
    }
    reporter.check(diskOk,
                   QStringLiteral("P3-07 ★ 磁盘温度一行：要么是 0~120°C 的合理值，"
                                  "要么是**非空的不可用原因**（绝不显示 0°C）"),
                   QStringLiteral("面板：%1 ／ 平台层枚举到 %2 块盘")
                       .arg(diskValue.isEmpty() ? QStringLiteral("（无此行）") : diskValue)
                       .arg(disks.size()));

    // ★ CPU 温度：本批固定不可用，且必须说清缺什么
    const QString cpuTempValue = panelField(panelText, QStringLiteral("CPU 温度"));
    reporter.check(!cpuTempValue.isEmpty()
                       && panelFieldUnavailable(panelText, QStringLiteral("CPU 温度"))
                       && cpuTempValue.contains(QStringLiteral("PawnIOLib"))
                       && !looksLikeReading(cpuTempValue),
                   QStringLiteral("P3-07 ★ CPU 温度**如实报缺 PawnIOLib.dll**"
                                  "（第二里程碑接上 PawnIO 后，本组会改成「必须有真实读数」）"),
                   cpuTempValue.isEmpty() ? QStringLiteral("（无此行）") : cpuTempValue);

    // ★ 面板上每一行都必须有文案（含不可用原因）——不存在空白行
    QString blankReport;
    bool noBlankRows = true;
    const QStringList panelLines = panelText.split(QLatin1Char('\n'));
    for (const QString &line : panelLines) {
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || line.mid(equals + 1).trimmed().isEmpty()) {
            noBlankRows = false;
            blankReport += line + QStringLiteral("；");
        }
    }
    reporter.check(noBlankRows && !panelLines.isEmpty(),
                   QStringLiteral("P3-07 ★ 面板上没有**空白行**（每一行要么是读数、要么是原因）"),
                   blankReport.isEmpty() ? panelText : blankReport);

    // ---- 逐项开关：当场改行数，窗口高度跟着变 ----
    const int fullHeight = panel.overlay()->overlayGeometry().height();
    reporter.check(panel.rowCount() == 6,
                   QStringLiteral("P3-07 五项全开时面板上是 6 行"
                                  "（CPU / 内存 / 网速 / GPU / 磁盘温度 / CPU 温度）"),
                   QStringLiteral("%1 行：%2").arg(panel.rowCount()).arg(panelText));

    services.setConfigValue(kHudId, QStringLiteral("showGpu"), false);
    services.setConfigValue(kHudId, QStringLiteral("showTemperature"), false);
    reporter.check(manager.setPluginEnabled(kHudId, false),
                   QStringLiteral("P3-07 停用插件（准备验「配置变了会不会真的少画两行」）"));
    reporter.check(host->overlaysOfOwner(kHudId).isEmpty(),
                   QStringLiteral("P3-07 ★ 停用后**不留置顶窗口**（用户关掉功能，桌面上不能"
                                  "还压着一条点不掉的浮窗）"),
                   QStringLiteral("残留 %1 个").arg(host->overlaysOfOwner(kHudId).size()));

    reporter.check(manager.setPluginEnabled(kHudId, true),
                   QStringLiteral("P3-07 改完配置再启用成功"));
    HudPanelView shrunk = hudPanelOf(host);
    const bool shrunkOk = waitFor(
        [&shrunk] { return shrunk.valid() && shrunk.rowCount() == 3; }, 4000);
    reporter.check(shrunkOk,
                   QStringLiteral("P3-07 ★ 关掉 GPU 与温度后，面板上只剩 3 行"
                                  "（逐项开关真的作用到面板，而不是只写进配置）"),
                   shrunk.valid() ? QStringLiteral("%1 行：%2")
                                        .arg(shrunk.rowCount())
                                        .arg(shrunk.snapshotText())
                                  : QStringLiteral("（无面板）"));
    if (shrunk.valid()) {
        reporter.check(shrunk.overlay()->overlayGeometry().height() < fullHeight,
                       QStringLiteral("P3-07 ★ 行少了窗口也跟着变矮（尺寸由行数决定，"
                                      "不是留一片空白）"),
                       QStringLiteral("6 行 %1 px → 3 行 %2 px")
                           .arg(fullHeight)
                           .arg(shrunk.overlay()->overlayGeometry().height()));
    }

    // ---- 位置持久化：面板位置是被记住的（用户拖到哪，下次开还在哪）----
    const QPoint anchorBefore = shrunk.valid() ? shrunk.anchor() : QPoint();
    reporter.check(manager.setPluginEnabled(kHudId, false),
                   QStringLiteral("P3-07 再次停用成功"));
    reporter.check(manager.setPluginEnabled(kHudId, true),
                   QStringLiteral("P3-07 再次启用成功"));
    HudPanelView restored = hudPanelOf(host);
    reporter.check(restored.valid() && restored.anchor() == anchorBefore,
                   QStringLiteral("P3-07 ★ 停用→启用后回到同一处（位置真的被持久化，"
                                  "不是每次开都跳回默认角落）"),
                   QStringLiteral("期望 %1,%2 / 实测 %3")
                       .arg(anchorBefore.x())
                       .arg(anchorBefore.y())
                       .arg(restored.valid() ? QStringLiteral("%1,%2")
                                                   .arg(restored.anchor().x())
                                                   .arg(restored.anchor().y())
                                             : QStringLiteral("（无面板）")));

    // ---- 复位：回到主屏工作区右上角（带留白），并且**落在工作区内** ----
    status.clear();
    reporter.check(dispatchAction(manager, kHudId, QStringLiteral("reset")),
                   QStringLiteral("P3-07 触发「复位到默认位置」快捷键"));
    reporter.check(status.contains(QStringLiteral("复位")),
                   QStringLiteral("P3-07 复位后给出状态提示"),
                   status.last());
    if (restored.valid() && !workArea.isEmpty()) {
        const QPoint after = restored.anchor();
        const QSize panelSize = restored.panelPhysicalRect().size();
        const QRect placed(after, panelSize);
        const bool inside = workArea.contains(placed);
        // 右上角：右边缘留白 24 逻辑像素（默认边距），所以左右不该差太远
        const int expectedLeft = workArea.right() + 1 - panelSize.width()
                                 - qRound(24 * scale);
        reporter.check(inside && qAbs(after.x() - expectedLeft) <= 4,
                       QStringLiteral("P3-07 ★ 复位后面板落在**主屏工作区右上角**"
                                      "（不会跑到屏幕外，也不压任务栏）"),
                       QStringLiteral("锚点 %1,%2 面板 %3×%4 期望左边 %5 / 工作区 %6")
                           .arg(after.x())
                           .arg(after.y())
                           .arg(panelSize.width())
                           .arg(panelSize.height())
                           .arg(expectedLeft)
                           .arg(rectText(workArea)));
    }

    // ---- 可拖拽 / 穿透切换：真的改了扩展样式，不重建窗口 ----
    const WindowHandle stableHandle = overlayHandle(restored.overlay());
    status.clear();
    reporter.check(dispatchAction(manager, kHudId, QStringLiteral("interactive")),
                   QStringLiteral("P3-07 触发「切换可拖拽」快捷键"));
    reporter.check(!hasExStyle(stableHandle, WS_EX_TRANSPARENT),
                   QStringLiteral("P3-07 ★ 可拖拽模式关掉了点击穿透（改的是扩展样式，"
                                  "面板本体还在原窗口上）"),
                   status.last());
    reporter.check(dispatchAction(manager, kHudId, QStringLiteral("interactive")),
                   QStringLiteral("P3-07 再触发一次切回穿透"));
    reporter.check(hasExStyle(stableHandle, WS_EX_TRANSPARENT),
                   QStringLiteral("P3-07 切回后恢复点击穿透"));

    // ---- 设置面板：控件齐全 + 「已知限制」照实写着 ----
    QWidget *settings = manager.createSettingsWidget(kHudId);
    reporter.check(settings != nullptr,
                   QStringLiteral("P3-07 设置面板可创建（跨 DLL 边界不崩）"));
    if (settings != nullptr) {
        const QStringList missing = [settings] {
            QStringList missingNames;
            for (const QString &name : {kCpuCheck, kMemoryCheck, kNetworkCheck, kGpuCheck,
                                        kTemperatureCheck, kIntervalSpin, kInteractiveCheck,
                                        kResetButton, kDetailLabel, kUnavailableLabel}) {
                if (settings->findChild<QObject *>(name) == nullptr) {
                    missingNames.append(name);
                }
            }
            return missingNames;
        }();
        reporter.check(missing.isEmpty(),
                       QStringLiteral("P3-07 设置面板控件齐全（5 个指标开关 / 刷新间隔 / "
                                      "可拖拽 / 复位 / 明细 / 已知限制）"),
                       QStringLiteral("缺：%1").arg(missing.join(QStringLiteral(", "))));
        auto *unavailableLabel = settings->findChild<QLabel *>(kUnavailableLabel);
        reporter.check(unavailableLabel != nullptr
                           && unavailableLabel->text().contains(QStringLiteral("PawnIOLib")),
                       QStringLiteral("P3-07 ★ 设置面板上照实写着「CPU 温度需要 PawnIOLib.dll」"
                                      "（能力边界要摆在用户看得到的地方，不能只藏在源码注释里）"),
                       unavailableLabel != nullptr ? unavailableLabel->text().left(60) : QString());
        auto *intervalSpin = settings->findChild<QSpinBox *>(kIntervalSpin);
        reporter.check(intervalSpin != nullptr && intervalSpin->minimum() >= 500
                           && intervalSpin->maximum() <= 10000
                           && intervalSpin->value() == 1000,
                       QStringLiteral("P3-07 刷新间隔有护栏（≥500ms：CPU/网速是两次计数之差，"
                                      "太快既测不准也费电）"),
                       intervalSpin != nullptr
                           ? QStringLiteral("%1~%2，当前 %3")
                                 .arg(intervalSpin->minimum())
                                 .arg(intervalSpin->maximum())
                                 .arg(intervalSpin->value())
                           : QString());
        delete settings;
    }

    // ---- 收尾：停用后一切干净 ----
    reporter.check(manager.setPluginEnabled(kHudId, false),
                   QStringLiteral("P3-07 收尾停用成功"));
    reporter.check(host->overlaysOfOwner(kHudId).isEmpty(),
                   QStringLiteral("P3-07 ★ 收尾：宿主登记表里没有本插件的悬浮层"
                                  "（用例不留置顶窗口）"),
                   QStringLiteral("残留 %1 个").arg(host->overlaysOfOwner(kHudId).size()));
    reporter.check(!services.hasHotkey(QStringLiteral("monitor.hardware_hud::interactive"))
                       && !services.hasHotkey(QStringLiteral("monitor.hardware_hud::reset")),
                   QStringLiteral("P3-07 停用后插件自己的两条快捷键被注销"
                                  "（不留「按了没反应」的注册）"),
                   services.registeredHotkeys().join(QStringLiteral(", ")));
    reporter.check(host->overlayCount() == 0,
                   QStringLiteral("P3-07 收尾：宿主登记表已清空"),
                   QStringLiteral("残留 %1 个").arg(host->overlayCount()));

    reporter.info(QStringLiteral("P3-07 组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
