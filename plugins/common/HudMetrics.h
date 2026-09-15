#pragma once

// ============================================================================
//  HudMetrics.h —— 硬件监控悬浮窗的显示模型（P3-07 第一批）
//
//  为什么放在 plugins/common（而不是插件私有）：
//      本功能的硬指标是"**读不到的指标必须在面板上如实写出原因，绝不允许显示 0
//      或空白**"。这条逻辑必须由**插件与自检编译同一份源码**，
//      否则自检断言的是另一套实现，等于没验（同 TextTools / RenameEngine 的纪律）。
//
//  ⚠ 本文件与实现**不包含任何 Win32 头**，只吃纯数据：
//      取数在 src/win32/SystemInfo（平台层）完成，插件负责把取数结果填进 MetricInput，
//      本层只做"组装显示模型 + 格式化 + 不可用文案"。
//
//  ⚠ 本文件不含 Q_OBJECT（与 OverlayGeometry.h 一致），可以被任意插件/自检直接链接。
// ============================================================================

#include <QList>
#include <QString>

namespace WinEase::FeaturePlugins::Hud {

// ============================================================================
//  显示模型
// ============================================================================

/// 指标种类。面板行序即此枚举顺序
enum class MetricKind {
    Cpu,      ///< CPU 利用率（PDH，与任务管理器同源）
    Memory,   ///< 内存占用（GlobalMemoryStatusEx）
    Network,  ///< 网络上下行速率（PDH，排除回环/虚拟适配器）
    Gpu,      ///< GPU 利用率（PDH \GPU Engine，可能整机不可用）
    DiskTemp, ///< 磁盘温度（IOCTL_STORAGE_QUERY_PROPERTY，零依赖零提权）
    CpuTemp,  ///< CPU 温度（C++/CLI 桥接 + LibreHardwareMonitor，常需管理员）
    GpuTemp,  ///< GPU 温度（同一桥接层，NVIDIA/AMD 走厂商接口，通常不需管理员）
    BoardTemp,///< 主板温度（桥接层 SuperIO / 主板传感器）
    Fan,      ///< 风扇转速（桥接层 Fan 类传感器，取读得出来的第一个）
};

/// 桥接层（C++/CLI + LibreHardwareMonitor）的传感器类型。
/// ⚠ 数值必须与 LibreHardwareMonitorLib 的 `SensorType` 一致：
///    桥接层是原样透传的，改这里等于改两边（详见 src/win32/LiteMonitorBridge.h）
enum class SensorKind {
    Voltage = 0,
    Current = 1,
    Power = 2,
    Clock = 3,
    Temperature = 4,
    Fan = 5,
    Flow = 6,
    Control = 7,
    Level = 8,
    Factor = 9,
    Data = 10,
    SmallData = 11,
    Throughput = 12,
    Load = 13,
};

/// 桥接层给出的一个传感器读数（纯数据：自检可以直接构造，不需要真硬件）
struct BridgeSensor {
    QString hardware;    ///< 硬件名（"Intel Core Ultra 9 ..." / "NVIDIA ..."）
    QString name;        ///< 传感器名（"CPU Package" / "GPU Core" / "CPU Fan"）
    QString identifier;  ///< LHM 标识符（"/intelcpu/0/temperature/0"）——**靠它判断归属**
    SensorKind kind = SensorKind::Temperature;
    bool hasValue = false; ///< false = 硬件库没给值（多数情况是权限不够）
    double value = 0.0;
};

/// 面板上的一行
struct MetricReading {
    MetricKind kind = MetricKind::Cpu;
    QString label;      ///< 行首名称：CPU / 内存 / 网速 / GPU / 磁盘温度 / CPU 温度
    QString valueText;  ///< 行尾数值。
                        ///< ⚠ available == false 时这里**必须是不可用原因**，
                        ///<   不得为空字符串、不得为 "0"（自检会按此断言）
    QString tooltip;    ///< 完整说明（含不可用原因与获取指引）
    bool available = false; ///< 本行是否有真实读数（false → 面板用警示色）
};

/// 逐项开关。
/// `temperature` 管"磁盘温度 + CPU 温度"两行（沿用第一批的语义），
/// 桥接层带来的三项（GPU 温度 / 主板温度 / 风扇）各自独立开关 —— 它们依赖
/// 外部组件（C++/CLI 桥接 + 硬件库），用户可能需要单独关掉。
struct MetricOptions {
    bool cpu = true;
    bool memory = true;
    bool network = true;
    bool gpu = true;
    bool temperature = true;
    bool gpuTemp = true;
    bool boardTemp = true;
    bool fan = true;

    /// 是否一个指标都没开（面板需要给出空态提示）
    bool noneSelected() const
    {
        return !cpu && !memory && !network && !gpu && !temperature && !gpuTemp && !boardTemp
               && !fan;
    }
};

/// 一次采样的原始读数。由插件从 Win32::SystemInfo 填充（纯数据，自检可直接构造）
struct MetricInput {
    struct DiskReading {
        QString model;
        double celsius = 0.0;
        bool valid = false;
        QString error;
    };

    bool cpuValid = false;
    double cpuPercent = 0.0;
    QString cpuError;

    bool memoryValid = false;
    double memoryPercent = 0.0;
    quint64 memoryUsedBytes = 0;
    quint64 memoryTotalBytes = 0;
    QString memoryError;

    bool networkValid = false;
    quint64 rxBytesPerSecond = 0;
    quint64 txBytesPerSecond = 0;
    /// 由速率积分得到的累计值（自采样器创建以来）——只是 tooltip 里的参考信息
    quint64 networkTotalRxBytes = 0;
    quint64 networkTotalTxBytes = 0;
    QString networkError;

    bool gpuValid = false;
    double gpuPercent = 0.0;
    QString gpuError;

    /// 全部物理磁盘；面板取"第一块读得出来的"作为主显示，其余进 Tooltip
    QList<DiskReading> disks;

    bool cpuTempValid = false;
    double cpuTempCelsius = 0.0;
    QString cpuTempError;

    /// ---- C++/CLI 桥接层（P3-07 改版：LibreHardwareMonitor）----
    /// 桥接层是否可用。false 时 bridgeError 给出原因（模块缺失 / 无 .NET 运行时 / 刷新失败）
    bool bridgeAvailable = false;
    QString bridgeError;
    /// 桥接层这次采样给出的全部传感器（原样透传，选择逻辑在本层做，便于自检构造数据断言）
    QList<BridgeSensor> bridgeSensors;
};

/// 面板显示模型
struct HudSnapshot {
    QString title;                ///< 标题条文字
    QList<MetricReading> metrics; ///< 已按 MetricOptions 过滤；顺序即绘制行序
};

// ============================================================================
//  格式化（纯函数）
// ============================================================================

/// 百分比："37%"；超过 100 截断（多引擎 GPU 相加可能越界）
QString formatPercent(double percent);

/// 速率："812 KB/s" / "12.4 MB/s" / "0 B/s"
QString formatRate(quint64 bytesPerSecond);

/// 容量："1.2 GB" / "15.9 GB"
QString formatBytes(quint64 bytes);

/// 温度："39°C"
QString formatTemperature(double celsius);

// ============================================================================
//  不可用文案（统一口径，插件、面板、帮助页共用）
// ============================================================================

/// 桥接层整体不可用时的统一口径（模块缺失 / 无 .NET 8 运行时）
QString bridgeUnavailableText();

/// CPU 温度的固定不可用原因（桥接层都没装上时用）
QString cpuTempUnavailableText();

/// 桥接层在跑、但 CPU 温度就是没有值（LibreHardwareMonitor 需要管理员权限读 MSR / SuperIO）
QString cpuTempNoReadingText();

/// 桥接层说明（真实原理 + 权限前提 + 本机运行时信息），进 Tooltip / 帮助页
QString cpuTempTooltipText();

/// GPU 温度 / 主板温度 / 风扇的不可用文案
QString gpuTempUnavailableText();
QString boardTempUnavailableText();
QString fanUnavailableText();

/// 从桥接层传感器里挑一个：
///   kind           —— 传感器类型（温度 / 风扇 …）
///   identifierHint —— 标识符里必须含的子串（"cpu" / "gpu" / "mainboard" …），
///                     大小写不敏感；用**标识符**而不是硬件名判断归属，
///                     因为中文系统里硬件名会被本地化，标识符是稳定的英文路径
///   nameCandidates —— 传感器名优先级（"CPU Package" 优先于 "Core Average" …），
///                     空表表示"随便第一个有值的"
/// 返回 nullptr = 没有匹配项；返回的项 hasValue == false 表示"有探头但没读数"。
const BridgeSensor *pickSensor(const QList<BridgeSensor> &sensors, SensorKind kind,
                               const QString &identifierHint,
                               const QStringList &nameCandidates = QStringList());

/// 不可用行的显示文本：优先用真实错误，空则用 fallback。
/// ⚠ 保证返回**非空**——这是"绝不显示 0 / 空白"的最后一道闸
QString unavailableText(const QString &error, const QString &fallback);

// ============================================================================
//  组装
// ============================================================================

/// 由原始读数组装面板显示模型（纯函数：同样输入必得同样输出，便于自检断言）
HudSnapshot buildSnapshot(const MetricInput &input, const MetricOptions &options);

} // namespace WinEase::FeaturePlugins::Hud
