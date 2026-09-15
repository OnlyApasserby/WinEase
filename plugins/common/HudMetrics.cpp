#include "HudMetrics.h"

#include <QtGlobal>

#include <climits>

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

QString bridgeUnavailableText()
{
    return QStringLiteral("桥接组件不可用");
}

QString cpuTempUnavailableText()
{
    // 桥接层整个没装上时用它。措辞纪律不变：**不许显示 0°C 或留空**，
    // 必须把"缺什么"说清楚（自检会断言非空）
    return QStringLiteral("需要 C++/CLI 桥接组件");
}

QString cpuTempNoReadingText()
{
    return QStringLiteral("无读数（需要管理员权限）");
}

QString cpuTempTooltipText()
{
    return QStringLiteral(
               "CPU / 主板 / 风扇转速来自 C++/CLI 桥接程序集（WinEaseLiteMonitorBridge.dll），\n"
               "它直接调用 LibreHardwareMonitorLib —— 与 refrences/LiteMonitor 同一个硬件库。\n"
               "\n"
               "读数前提：LibreHardwareMonitor 要经过内核驱动读 CPU 的 MSR / 主板 SuperIO，\n"
               "因此**多数机器需要以管理员身份运行 WinEase** 才能拿到 CPU 封装温度与风扇转速；\n"
               "GPU 温度不走这条路（NVIDIA / AMD 的用户态接口），普通权限即可读到。\n"
               "\n"
               "拿到不读数时本行会如实写明原因，绝不显示 0°C。\n"
               "磁盘温度不受影响 —— 它走 IOCTL_STORAGE_QUERY_PROPERTY，零依赖零提权。");
}

QString gpuTempUnavailableText()
{
    return QStringLiteral("GPU 温度不可用");
}

QString boardTempUnavailableText()
{
    return QStringLiteral("主板温度不可用");
}

QString fanUnavailableText()
{
    return QStringLiteral("风扇转速不可用");
}

namespace {

/// 名称优先级打分：命中 nameCandidates 越靠前分越小（返回 -1 = 不在候选里）
int nameRank(const QString &name, const QStringList &nameCandidates)
{
    if (nameCandidates.isEmpty()) {
        return 0;
    }
    for (int i = 0; i < nameCandidates.size(); ++i) {
        if (name.compare(nameCandidates.at(i), Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    for (int i = 0; i < nameCandidates.size(); ++i) {
        if (name.contains(nameCandidates.at(i), Qt::CaseInsensitive)) {
            return nameCandidates.size() + i;
        }
    }
    return -1;
}

/// 桥接层来源的指标（GPU 温度 / 主板温度 / 风扇）不可用时的统一口径。
/// 三种情况必须区分开，否则用户不知道自己是"没装组件"还是"权限不够"：
///   ① 有探头没读数 → 权限问题（写"无读数（需要管理员权限）"）
///   ② 桥接在跑、但没有这类探头 → 这台机器本来就没有（用传入的默认文案）
///   ③ 桥接不可用 → 说清缺哪个组件
QString bridgeMetricUnavailableText(const MetricInput &input, const BridgeSensor *sensor,
                                    const QString &fallback)
{
    if (sensor != nullptr) {
        return cpuTempNoReadingText();
    }
    if (input.bridgeAvailable) {
        return fallback;
    }
    return unavailableText(input.bridgeError, fallback);
}

} // namespace

const BridgeSensor *pickSensor(const QList<BridgeSensor> &sensors, SensorKind kind,
                               const QString &identifierHint, const QStringList &nameCandidates)
{
    const BridgeSensor *bestWithValue = nullptr;
    int bestRank = INT_MAX;
    const BridgeSensor *firstMatch = nullptr;

    for (const BridgeSensor &sensor : sensors) {
        if (sensor.kind != kind) {
            continue;
        }
        if (!identifierHint.isEmpty()
            && !sensor.identifier.contains(identifierHint, Qt::CaseInsensitive)) {
            continue;
        }
        if (firstMatch == nullptr) {
            firstMatch = &sensor;
        }
        if (!sensor.hasValue) {
            continue;
        }
        const int rank = nameRank(sensor.name, nameCandidates);
        if (rank < 0) {
            // 名称不在候选里：只在"没有更合适的"时用它
            if (bestWithValue == nullptr) {
                bestWithValue = &sensor;
                bestRank = INT_MAX;
            }
            continue;
        }
        if (rank < bestRank) {
            bestRank = rank;
            bestWithValue = &sensor;
        }
    }

    if (bestWithValue != nullptr) {
        return bestWithValue;
    }
    // 有探头但没读数：把第一个匹配项交回去，让面板写"无读数（需要管理员权限）"
    return firstMatch;
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

    // ---- 温度区：CPU 温度（C++/CLI 桥接 + LibreHardwareMonitor）----
    //
    // 取值优先级：
    //   ① input.cpuTempValid —— 由插件预先填好的读数（将来若再接别的通道，这里不用改）
    //   ② 桥接层里标识符含 "cpu" 的温度传感器（"CPU Package" > "Core Average" > "Core Max"）
    //   ③ 都没有 → **如实写原因**：
    //        - 桥接层不可用 → 说清"缺哪个组件"
    //        - 桥接层在跑但没值 → "无读数（需要管理员权限）"
    if (options.temperature) {
        bool available = input.cpuTempValid;
        double celsius = input.cpuTempCelsius;
        QString valueText;
        QString tooltip = cpuTempTooltipText();

        if (!available) {
            const BridgeSensor *cpuTemp = pickSensor(input.bridgeSensors, SensorKind::Temperature,
                                                     QStringLiteral("cpu"),
                                                     {QStringLiteral("CPU Package"),
                                                      QStringLiteral("Core Average"),
                                                      QStringLiteral("Core Max"),
                                                      QStringLiteral("CPU")});
            if (cpuTemp != nullptr && cpuTemp->hasValue) {
                available = true;
                celsius = cpuTemp->value;
                tooltip = QStringLiteral("%1 · %2\n%3")
                              .arg(cpuTemp->hardware, cpuTemp->name, tooltip);
            } else if (cpuTemp != nullptr) {
                valueText = cpuTempNoReadingText();
                tooltip = QStringLiteral("硬件库列出了温度探头（%1 · %2），但这次没给读数。\n\n%3")
                              .arg(cpuTemp->hardware, cpuTemp->name, tooltip);
            } else if (input.bridgeAvailable) {
                valueText = unavailableText(input.cpuTempError, cpuTempNoReadingText());
            } else {
                valueText = unavailableText(
                    input.cpuTempError.isEmpty() ? input.bridgeError : input.cpuTempError,
                    cpuTempUnavailableText());
            }
        }

        appendReading(snapshot, MetricKind::CpuTemp, QStringLiteral("CPU 温度"), available,
                      available ? formatTemperature(celsius) : valueText, tooltip);
    }

    // ---- 温度区：GPU 温度（桥接层；厂商用户态接口，通常不需要管理员）----
    if (options.gpuTemp) {
        const BridgeSensor *gpuTemp = pickSensor(input.bridgeSensors, SensorKind::Temperature,
                                                 QStringLiteral("gpu"),
                                                 {QStringLiteral("GPU Core"),
                                                  QStringLiteral("GPU Hot Spot"),
                                                  QStringLiteral("GPU")});
        const QString tooltip =
            QStringLiteral("GPU 温度来自 C++/CLI 桥接层（LibreHardwareMonitorLib）。\n"
                           "NVIDIA / AMD 的用户态接口即可读取，一般不需要管理员权限。\n"
                           "取 \"GPU Core\" 优先，其次 \"GPU Hot Spot\"（结温）。");
        appendReading(snapshot, MetricKind::GpuTemp, QStringLiteral("GPU 温度"),
                      gpuTemp != nullptr && gpuTemp->hasValue,
                      (gpuTemp != nullptr && gpuTemp->hasValue)
                          ? formatTemperature(gpuTemp->value)
                          : bridgeMetricUnavailableText(input, gpuTemp, gpuTempUnavailableText()),
                      tooltip);
    }

    // ---- 温度区：主板温度（桥接层 SuperIO / 主板传感器；通常需要管理员）----
    if (options.boardTemp) {
        const BridgeSensor *boardTemp =
            pickSensor(input.bridgeSensors, SensorKind::Temperature, QStringLiteral("mainboard"),
                       {QStringLiteral("Temperature")});
        if (boardTemp == nullptr) {
            boardTemp = pickSensor(input.bridgeSensors, SensorKind::Temperature,
                                   QStringLiteral("superio"));
        }
        const QString tooltip =
            QStringLiteral("主板温度来自 C++/CLI 桥接层（LibreHardwareMonitorLib 的 SuperIO"
                           "访问）。\n"
                           "与 CPU 温度同理，多数机器需要以管理员身份运行才能读到。");
        appendReading(snapshot, MetricKind::BoardTemp, QStringLiteral("主板温度"),
                      boardTemp != nullptr && boardTemp->hasValue,
                      (boardTemp != nullptr && boardTemp->hasValue)
                          ? formatTemperature(boardTemp->value)
                          : bridgeMetricUnavailableText(input, boardTemp,
                                                        boardTempUnavailableText()),
                      tooltip);
    }

    // ---- 风扇转速（桥接层；取读得出来的第一个）----
    if (options.fan) {
        const BridgeSensor *fan = pickSensor(input.bridgeSensors, SensorKind::Fan, QString());
        const QString tooltip =
            QStringLiteral("风扇转速来自 C++/CLI 桥接层（LibreHardwareMonitorLib）。\n"
                           "台式机主板风扇需要 SuperIO 访问（多数需管理员），"
                           "笔记本 / 显卡的风扇往往读不到 —— 此时本行如实写原因。");
        appendReading(snapshot, MetricKind::Fan, QStringLiteral("风扇"),
                      fan != nullptr && fan->hasValue,
                      (fan != nullptr && fan->hasValue)
                          ? QStringLiteral("%1 RPM").arg(qRound(fan->value))
                          : bridgeMetricUnavailableText(input, fan, fanUnavailableText()),
                      tooltip);
    }

    return snapshot;
}

} // namespace WinEase::FeaturePlugins::Hud
