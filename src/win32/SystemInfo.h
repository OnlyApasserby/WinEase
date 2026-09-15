#pragma once

// ============================================================================
//  SystemInfo.h —— 系统与硬件状态采集
//
//  用途：硬件监控悬浮窗（P3-07）、系统监控类功能
//
//  重要设计约束：**本层不使用任何全局变量**。
//      采样类指标（CPU/GPU/网络吞吐）本质上是"两次采样求差"，天然需要保存上一次的状态。
//      该状态由**调用方**（插件）持有，这样每个插件各自独立，
//      也避免了静态库被链接进多个 DLL 时状态分裂的问题。
//
//  用法示例：
//      WinEase::Win32::CpuSampler cpu;          // 插件成员变量
//      ... 每秒调用一次 ...
//      const auto sample = cpu.sample();
//      if (sample.valid) { 使用 sample.overallPercent }
//
//  已知限制：
//      * GPU 利用率依赖 PDH 计数器 "\GPU Engine(*)\Utilization Percentage"，
//        在虚拟机、无独显或精简系统上可能不可用 —— 此时 valid=false 并给出原因
//      * 温度只覆盖**存储设备**；CPU/主板温度需要 PawnIO 驱动（见 P3-07）
//      * PDH 的 "% Processor Time" 首次采集仅建立基准，第二次起才有意义
// ============================================================================

#include <QList>
#include <QString>

namespace WinEase::Win32 {

// ============================================================================
//  内存
// ============================================================================

struct MemoryInfo {
    quint64 totalBytes = 0;
    quint64 availableBytes = 0;
    quint64 usedBytes = 0;
    double usagePercent = 0.0;
    bool valid = false;
};

MemoryInfo memoryInfo();

// ============================================================================
//  CPU
// ============================================================================

class CpuSampler
{
public:
    CpuSampler();
    ~CpuSampler();

    CpuSampler(const CpuSampler &) = delete;
    CpuSampler &operator=(const CpuSampler &) = delete;

    struct Sample {
        double overallPercent = 0.0;
        QList<double> perCorePercent;
        bool valid = false;    ///< 首次采样为 false（只建立了基准）
        QString error;         ///< 不可用原因
    };

    /// 采样一次。建议调用间隔 ≥ 500ms，否则百分比波动噪点很大。
    Sample sample();

private:
    void release();

    void *m_query = nullptr;   ///< PDH_HQUERY
    void *m_counter = nullptr; ///< PDH_HCOUNTER
    int m_collectCount = 0;
    QString m_initError;
};

// ============================================================================
//  网络吞吐
// ============================================================================

class NetworkSampler
{
public:
    NetworkSampler();
    ~NetworkSampler();

    NetworkSampler(const NetworkSampler &) = delete;
    NetworkSampler &operator=(const NetworkSampler &) = delete;

    struct Sample {
        quint64 rxBytesPerSecond = 0;
        quint64 txBytesPerSecond = 0;
        /// 由速率积分得到的累计值（自本采样器创建以来），用于悬浮窗展示"本次开机已下载"
        quint64 totalRxBytes = 0;
        quint64 totalTxBytes = 0;
        bool valid = false;    ///< 首次采样为 false（只建立了基准）
        QString error;
    };

    /// 采样一次。统计范围：排除回环、隧道与常见虚拟适配器（PDH 计数器实例名过滤）
    Sample sample();

private:
    void release();

    void *m_rxQuery = nullptr;
    void *m_txQuery = nullptr;
    void *m_rxCounter = nullptr;
    void *m_txCounter = nullptr;
    int m_collectCount = 0;
    QString m_initError;
    quint64 m_totalRx = 0;
    quint64 m_totalTx = 0;
    qint64 m_lastTimestampMs = 0;
};

// ============================================================================
//  GPU
// ============================================================================

class GpuSampler
{
public:
    GpuSampler();
    ~GpuSampler();

    GpuSampler(const GpuSampler &) = delete;
    GpuSampler &operator=(const GpuSampler &) = delete;

    struct Sample {
        double utilizationPercent = 0.0;
        bool valid = false;
        QString error;
    };

    /// 采样一次（与任务管理器同源，取 3D 引擎活动；多引擎取和并截断到 100%）
    Sample sample();

private:
    void release();

    void *m_query = nullptr;
    void *m_counter = nullptr;
    int m_collectCount = 0;
    QString m_initError;
};

// ============================================================================
//  存储设备温度（不需要额外驱动）
// ============================================================================

struct DiskTemperature {
    QString model;      ///< 设备描述，例如 "NVMe UMIS UPJYJ1TBMNV1QWY"
    QString devicePath; ///< \\.\PhysicalDriveN（磁盘类驱动的名字，不是 SetupDi 接口路径）
    double celsius = 0.0;
    bool valid = false;
    QString error;      ///< 不可用原因（该设备不支持 / 权限不足等）
};

/// 枚举物理磁盘温度（NVMe / SATA，走 IOCTL_STORAGE_QUERY_PROPERTY）
///
/// ⚠ 为什么走 `\\.\PhysicalDriveN` 而不是 SetupDi 的磁盘接口路径：
///   接口路径（`\\?\scsi#disk&ven_...`）开出来的是 **PDO** 句柄，本机实测
///   温度查询在上面直接返回 ERROR_INVALID_FUNCTION；同一个 IOCTL 在
///   `\\.\PhysicalDrive0`（磁盘类驱动的 FDO 名字）上返回真实温度。
///   详见 SystemInfo.cpp 里的实现注释。
QList<DiskTemperature> diskTemperatures();

// ============================================================================
//  其他
// ============================================================================

/// 系统运行时间（秒）
quint64 uptimeSeconds();

struct BatteryInfo {
    bool present = false;
    bool onAcPower = false;
    int percent = -1;          ///< 0~100；未知为 -1
    int secondsRemaining = -1; ///< 未知为 -1
};

BatteryInfo batteryInfo();

} // namespace WinEase::Win32
