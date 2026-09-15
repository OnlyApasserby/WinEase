#pragma once

// ============================================================================
//  StorageDevices.h —— 存储设备枚举 / 属性 / 安全弹出
//
//  给谁用：P3-14 USB 设备管控（列设备 + 安全弹出）、P3-07 硬件监控（磁盘列表）。
//
//  ---------------------------------------------------------------------------
//  ⚠ 为什么不能用 `GetDriveType() == DRIVE_REMOVABLE` 判断"可移动"
//
//  这是本模块存在的全部理由。**移动硬盘在 Windows 里几乎总是被报告成
//  `DRIVE_FIXED`**（NTFS/exFAT + USB 桥接芯片，系统把它当固定盘），
//  只有 U 盘、读卡器、部分外置 SSD 才是 `DRIVE_REMOVABLE`。
//  用 DRIVE_ 类型去判断，用户插着的移动硬盘会被**静默漏掉** ——
//  而它恰恰是最需要"安全弹出"的那类设备。
//
//  正确的判据有两条，本模块都取：
//    ① **PnP 的 RemovalPolicy**（`SPDRP_REMOVAL_POLICY`）：
//       `EXPECT_SURPRISE_REMOVAL` = 可随时拔（U 盘）；`EXPECT_ORDERLY_REMOVAL` = 需要安全弹出（移动硬盘）；
//       `EXPECT_NO_REMOVAL` = 固定盘；
//    ② **总线类型**（`IOCTL_STORAGE_QUERY_PROPERTY` → `STORAGE_DEVICE_DESCRIPTOR.BusType == BusTypeUsb`）：
//       最贴近用户心智的判据 —— "插在 USB 上的存储"就是他要弹出的东西。
//  两者只要有**一条**成立就算"可移除设备"（宁可多列一个也不漏）。
//
//  ---------------------------------------------------------------------------
//  ⚠ 关于"弹出失败时能不能说清是谁在占用"
//
//  `CM_Request_Device_Eject` 的 **veto 机制能给出阻止者的名字**
//  （`PNP_VetoWindowsApp` / `PNP_VetoWindowsService` 时会带回应用名或服务名，
//   `PNP_VetoOutstandingOpen` 表示还有未关闭的句柄）。
//  这比自己扫全系统句柄便宜得多、也稳得多 —— 本模块把它翻译成人话。
//
//  ⚠ 弹出是**真的会把设备摘掉**的操作，调用方必须先做二次确认；
//     本模块不提供"强制弹出"（不调 `CM_Request_Device_Eject` 的 force 路径）。
//
//  ⚠ 平台层约定：只依赖 Qt6::Core（QString/QList），只放无状态原语。
// ============================================================================

#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 设备的可移除性质（来自 PnP RemovalPolicy，比卷的 DRIVE_ 类型可靠得多）
enum class RemovalClass {
    Unknown = 0,
    SurpriseRemoval, ///< 可随时拔（U 盘 / 读卡器）
    OrderlyRemoval,  ///< 需要"安全弹出"（移动硬盘 / 外置 SSD）
    NoRemoval,       ///< 固定盘
};

QString removalClassText(RemovalClass value);

/// 一台存储设备（或其上的一个卷）
struct StorageDeviceInfo {
    /// 物理盘路径（`\\.\PhysicalDriveN` 形式，可直接 CreateFile）；拿不到时为空
    QString devicePath;
    /// 卷挂载点（"E:\\"）；无卷（未格式化/未分配）时为空
    QString volumePath;
    QString driveLetter; ///< "E:"
    QString label;       ///< 卷标
    QString fileSystem;  ///< NTFS / exFAT / FAT32 / 空
    QString productName; ///< 设备产品名（如 "Elements 25A3"）

    RemovalClass removal = RemovalClass::Unknown;
    int busType = 0;           ///< STORAGE_BUS_TYPE 原值
    QString busText;           ///< "USB" / "NVMe" / "RAID" / …
    bool isUsbStorage = false; ///< 总线是 USB —— 用户眼里的"可移动设备"

    quint64 totalBytes = 0;
    quint64 freeBytes = 0;
    /// 有介质且可访问（空读卡器 / 未格式化盘为 false）
    bool isReady = false;

    /// PnP 设备实例 ID（弹出用；拿不到时为空）
    QString pnpDeviceId;
    /// 卷是否由该设备提供（多卷设备会有多条记录）
    int partitionNumber = 0;

    /// 是否应当出现在"可移动设备"列表里（USB 总线 或 PnP 判为可移除）
    bool removable() const
    {
        return isUsbStorage || removal == RemovalClass::SurpriseRemoval
               || removal == RemovalClass::OrderlyRemoval;
    }

    /// 界面/日志用的一句话描述（"E: Elements 25A3 · NTFS · 931 GB · USB"）
    QString describe() const;
};

/// 枚举"用户眼里的可移动存储"：USB 总线上的盘 + PnP 判为可移除的盘。
/// **这是 P3-14 列表的数据源**（不要再自己拿 DRIVE_ 类型过滤一遍）。
QList<StorageDeviceInfo> removableStorageDevices(QString *errorOut = nullptr);

/// 枚举**全部**存储设备（含内置盘），只读展示 / P3-07 用
QList<StorageDeviceInfo> allStorageDevices(QString *errorOut = nullptr);

/// 请求安全弹出。
/// 顺序：U 盘/光驱先试 `IOCTL_STORAGE_EJECT_MEDIA`；失败或设备不支持时
/// 退到 `CM_Request_Device_Eject`（等价于系统的"安全删除硬件"，**移动硬盘走的就是这条**）。
/// @param vetoReasonOut 失败时回传"谁在阻止"（如"资源管理器正在使用该设备"）
bool ejectStorageDevice(const StorageDeviceInfo &device,
                        QString *errorOut = nullptr,
                        QString *vetoReasonOut = nullptr);

/// 把弹出失败的错误码翻译成用户能懂的原因
QString ejectFailureReason(unsigned long errorCode);

/// 人类可读的容量（"931 GB"）
QString humanizeBytes(quint64 bytes);

} // namespace WinEase::Win32
