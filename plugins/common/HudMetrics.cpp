#include "HudMetrics.h"

#include <QtGlobal>

namespace WinEase::FeaturePlugins::Hud {

namespace {

/// 空错误文案的兜底（不同指标的"说不出原因"含义不同）
const QString kSamplerWarmUp = QStringLiteral("首次采样中…");
const QString kMemoryFallback = QStringLiteral("内存信息不可用");
const QString kGpuFallback = QStringLiteral("GPU 利用率不可用");
const QString kNetworkFallback = QStringLiteral("网络速率不可用");
const QString kDiskFallback = QStringLiteral("未检测到物理磁盘");
const QString kDiskUnreadable = QStringLiteral("磁盘温度读不到");

/// 把一行加入快照（统一在这里保证"不可用必有文案"）
void appendReading(HudSnapshot &snapshot,
                   MetricKind kind,
                   const QString &label,
                   bool available,
                   const QString &valueText,
                   const QString &tooltip)
{
    MetricReading reading;
    reading.kind = kind;
    reading.label = label;
    reading.available = available;
    reading.valueText = valueText;
    reading.tooltip = tooltip;
    snapshot.metrics.append(reading);
}

} // namespace

// ============================================================================
//  格式化
// ============================================================================

QString formatPercent(double percent)
{
    const double clamped = qBound(0.0, percent, 100.0);
    return QStringLiteral("%1%").arg(qRound(clamped));
}

QString formatRate(quint64 bytesPerSecond)
{
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

    const double value = static_cast<double>(bytesPerSecond);
    if (value < kKiB) {
        return QStringLiteral("%1 B/s").arg(bytesPerSecond);
    }
    if (value < kMiB) {
        return QStringLiteral("%1 KB/s").arg(value / kKiB, 0, 'f', 1);
    }
    if (value < kGiB) {
        return QStringLiteral("%1 MB/s").arg(value / kMiB, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB/s").arg(value / kGiB, 0, 'f', 2);
}

QString formatBytes(quint64 bytes)
{
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kTiB = 1024.0 * 1024.0 * 1024.0 * 1024.0;

    const double value = static_cast<double>(bytes);
    if (value < kKiB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (value < kMiB) {
        return QStringLiteral("%1 KB").arg(value / kKiB, 0, 'f', 1);
    }
    if (value < kGiB) {
        return QStringLiteral("%1 MB").arg(value / kMiB, 0, 'f', 1);
    }
    if (value < kTiB) {
        return QStringLiteral("%1 GB").arg(value / kGiB, 0, 'f', 1);
    }
    return QStringLiteral("%1 TB").arg(value / kTiB, 0, 'f', 2);
}

QString formatTemperature(double celsius)
{
    return QStringLiteral("%1°C").arg(celsius, 0, 'f', 0);
}

// ============================================================================
//  不可用文案
// ============================================================================

QString cpuTempUnavailableText()
{
    // 措辞与 docs/ROADMAP-P3.md 的 P3-07 第二里程碑保持一致：
    // 未安装 PawnIOLib.dll 时**不许显示 0°C 或留空**，必须把"缺什么"说清楚
    return QStringLiteral("需要 PawnIOLib.dll（本机缺失）");
}

QString cpuTempTooltipText()
{
    return QStringLiteral(
               "CPU / 主板温度需要 PawnIO 驱动 + 用户态 PawnIOLib.dll（走提权助手加载）。\n"
               "本机已安装 PawnIO 驱动（设备 \\\\.\\PawnIO 存在），但缺 PawnIOLib.dll，"
               "因此本行暂时读不到数值。\n"
               "获取指引：从 PawnIO 官方仓库（github.com/namazso/PawnIO）下载发行包，"
               "把 PawnIOLib.dll 放到 WinEase.exe 同目录即可，无需重启。\n"
               "该能力属于 P3-07 第二里程碑，届时温度区会自动变成真实读数。\n"
               "磁盘温度不受此影响——它走 IOCTL_STORAGE_QUERY_PROPERTY，零依赖。");
}

QString unavailableText(const QString &error, const QString &fallback)
{
    const QString trimmed = error.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return fallback;
}

// ============================================================================
//  组装
// ============================================================================

HudSnapshot buildSnapshot(const MetricInput &input, const MetricOptions &options)
{
    HudSnapshot snapshot;
    snapshot.title = QStringLiteral("硬件监控");

    // ---- CPU ----
    if (options.cpu) {
        const QString tooltip =
            QStringLiteral("CPU 利用率（所有核心平均）。\n"
                           "取自 PDH 计数器 \\Processor Information(_Total)\\% Processor Time，"
                           "与任务管理器同源。\n"
                           "首次采样只建立基准，第二次起才有意义。");
        appendReading(snapshot, MetricKind::Cpu, QStringLiteral("CPU"), input.cpuValid,
                      input.cpuValid ? formatPercent(input.cpuPercent)
                                     : unavailableText(input.cpuError, kSamplerWarmUp),
                      tooltip);
    }

    // ---- 内存 ----
    if (options.memory) {
        QString tooltip = QStringLiteral("物理内存占用（GlobalMemoryStatusEx）。");
        if (input.memoryValid && input.memoryTotalBytes > 0) {
            tooltip = QStringLiteral("已用 %1 / 共 %2\n%3")
                          .arg(formatBytes(input.memoryUsedBytes),
                               formatBytes(input.memoryTotalBytes),
                               tooltip);
        }
        appendReading(snapshot, MetricKind::Memory, QStringLiteral("内存"), input.memoryValid,
                      input.memoryValid
                          ? QStringLiteral("%1 · %2/%3")
                                .arg(formatPercent(input.memoryPercent),
                                     formatBytes(input.memoryUsedBytes),
                                     formatBytes(input.memoryTotalBytes))
                          : unavailableText(input.memoryError, kMemoryFallback),
                      tooltip);
    }

    // ---- 网速 ----
    if (options.network) {
        QString tooltip = QStringLiteral("实时网络吞吐（PDH，已排除回环、隧道与虚拟适配器）。");
        if (input.networkValid
            && (input.networkTotalRxBytes > 0 || input.networkTotalTxBytes > 0)) {
            tooltip = QStringLiteral("下行 %1　上行 %2\n自面板开启以来累计：↓%3　↑%4\n%5")
                          .arg(formatRate(input.rxBytesPerSecond),
                               formatRate(input.txBytesPerSecond),
                               formatBytes(input.networkTotalRxBytes),
                               formatBytes(input.networkTotalTxBytes),
                               tooltip);
        }
        appendReading(snapshot, MetricKind::Network, QStringLiteral("网速"), input.networkValid,
                      input.networkValid
                          ? QStringLiteral("↓%1  ↑%2")
                                .arg(formatRate(input.rxBytesPerSecond),
                                     formatRate(input.txBytesPerSecond))
                          : unavailableText(input.networkError, kNetworkFallback),
                      tooltip);
    }

    // ---- GPU ----
    if (options.gpu) {
        const QString tooltip =
            QStringLiteral("GPU 利用率（3D 引擎活动）。\n"
                           "取自 PDH 计数器 \\GPU Engine(*)\\Utilization Percentage，"
                           "与任务管理器同源。\n"
                           "虚拟机、无独立显卡或精简版系统上可能整机不可用——"
                           "此时该行会如实显示原因，而不是 0%。");
        appendReading(snapshot, MetricKind::Gpu, QStringLiteral("GPU"), input.gpuValid,
                      input.gpuValid ? formatPercent(input.gpuPercent)
                                     : unavailableText(input.gpuError, kGpuFallback),
                      tooltip);
    }

    // ---- 温度区：磁盘温度（本批已可用）----
    if (options.temperature) {
        const MetricInput::DiskReading *primary = nullptr;
        const MetricInput::DiskReading *failed = nullptr;
        QStringList allLines;
        for (const MetricInput::DiskReading &disk : input.disks) {
            if (disk.valid) {
                allLines.append(QStringLiteral("%1：%2").arg(
                    disk.model.isEmpty() ? QStringLiteral("物理磁盘") : disk.model,
                    formatTemperature(disk.celsius)));
                if (primary == nullptr) {
                    primary = &disk;
                }
            } else {
                allLines.append(QStringLiteral("%1：%2").arg(
                    disk.model.isEmpty() ? QStringLiteral("物理磁盘") : disk.model,
                    unavailableText(disk.error, kDiskUnreadable)));
                if (failed == nullptr) {
                    failed = &disk;
                }
            }
        }

        bool available = false;
        QString valueText;
        if (primary != nullptr) {
            available = true;
            valueText = formatTemperature(primary->celsius);
        } else if (failed != nullptr) {
            valueText = unavailableText(failed->error, kDiskUnreadable);
        } else {
            valueText = kDiskFallback;
        }

        QString tooltip = QStringLiteral("磁盘温度（IOCTL_STORAGE_QUERY_PROPERTY，零依赖零提权）。");
        if (!allLines.isEmpty()) {
            tooltip = allLines.join(QStringLiteral("\n")) + QStringLiteral("\n") + tooltip;
        }

        appendReading(snapshot, MetricKind::DiskTemp, QStringLiteral("磁盘温度"), available,
                      valueText, tooltip);
    }

    // ---- 温度区：CPU 温度（本批固定不可用，第二里程碑接 PawnIO）----
    if (options.temperature) {
        appendReading(snapshot, MetricKind::CpuTemp, QStringLiteral("CPU 温度"),
                      input.cpuTempValid,
                      input.cpuTempValid ? formatTemperature(input.cpuTempCelsius)
                                         : unavailableText(input.cpuTempError,
                                                           cpuTempUnavailableText()),
                      cpuTempTooltipText());
    }

    return snapshot;
}

} // namespace WinEase::FeaturePlugins::Hud
