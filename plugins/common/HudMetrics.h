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
    CpuTemp,  ///< CPU 温度（需 PawnIOLib.dll，第二里程碑）
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

/// 逐项开关（面板上的五个勾选框；温度开关同时管"磁盘温度"与"CPU 温度"两行）
struct MetricOptions {
    bool cpu = true;
    bool memory = true;
    bool network = true;
    bool gpu = true;
    bool temperature = true;

    /// 是否一个指标都没开（面板需要给出空态提示）
    bool noneSelected() const { return !cpu && !memory && !network && !gpu && !temperature; }
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

/// CPU / 主板温度的固定不可用原因（第二里程碑接上 PawnIO 后才会变）
QString cpuTempUnavailableText();

/// 上面那句的完整说明（含 PawnIOLib.dll 是什么、去哪拿）
QString cpuTempTooltipText();

/// 不可用行的显示文本：优先用真实错误，空则用 fallback。
/// ⚠ 保证返回**非空**——这是"绝不显示 0 / 空白"的最后一道闸
QString unavailableText(const QString &error, const QString &fallback);

// ============================================================================
//  组装
// ============================================================================

/// 由原始读数组装面板显示模型（纯函数：同样输入必得同样输出，便于自检断言）
HudSnapshot buildSnapshot(const MetricInput &input, const MetricOptions &options);

} // namespace WinEase::FeaturePlugins::Hud
