#pragma once

// ============================================================================
//  DeviceUsageMonitor.h —— 摄像头 / 麦克风"谁在用"的判定
//
//  数据来源与系统"设置 → 隐私和安全性 → 摄像头/麦克风"页面**同源**：
//      HKCU\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\
//          ConsentStore\<webcam|microphone>\NonPackaged\<编码后的 exe 路径>
//  每个应用一个子键，里面两个 REG_QWORD（FILETIME）：
//      LastUsedTimeStart  开始使用的时间
//      LastUsedTimeStop   停止使用的时间 —— **0 表示"还在用"**（这是本功能的全部依据）
//
//  ---------------------------------------------------------------------------
//  三个容易踩的点，都在这里定死：
//
//   1. **"正在使用"只有一个判据**：`LastUsedTimeStart != 0 && LastUsedTimeStop == 0`。
//      从没用过的键两个值都是 0（有些机型会留下空键）—— 把它们当成"正在使用"
//      是最容易犯的错，那会让用户一启用功能就被一堆假警报淹没。
//   2. **键名是编码过的路径**：注册表把路径里的 `\` 换成了 `#`
//      （`C:\工具\cam.exe` → `C:#工具#cam.exe`）。要显示给用户看就得还原回来，
//      否则界面上出现的是一串 `C:#Users#...` 的天书。
//   3. **"读不到"不等于"没人用"**：父键不存在是**正常**的（这台机器还没有任何应用
//      碰过这个设备）；但读取过程中的其它错误必须如实报出来 ——
//      "没有警报"和"看不到"在安全类功能里是两件完全不同的事。
//
//  ⚠ 本文件不含 Q_OBJECT；读注册表那部分是 IO，其余全是纯函数（好断言）。
// ============================================================================

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::Common {

/// 设备种类（枚举顺序即界面展示顺序）
enum class DeviceKind {
    Webcam = 0, ///< 摄像头
    Microphone, ///< 麦克风
};

QString deviceKindKey(DeviceKind kind);  ///< "webcam" / "microphone"（配置与日志用）
QString deviceKindText(DeviceKind kind); ///< "摄像头" / "麦克风"（界面用）
DeviceKind deviceKindFromKey(const QString &key, bool *ok = nullptr);
QList<DeviceKind> allDeviceKinds();

/// 注册表路径（不含根键），例如 `...\ConsentStore\webcam\NonPackaged`
QString consentStorePath(DeviceKind kind, bool nonPackaged = true);

// ---------------------------------------------------------------------------
//  键名 ⇄ 路径
// ---------------------------------------------------------------------------

/// 解码注册表键名 → exe 完整路径（把 `#` 还原成 `\`；已经是路径的原样返回）
QString appPathFromKeyName(const QString &keyName);
/// 编码 exe 路径 → 注册表键名（把 `\` 换成 `#`）
QString keyNameFromAppPath(const QString &appPath);
/// 展示名（可执行文件名；还原不出路径时退回原键名）
QString displayNameForAppKey(const QString &keyName);

// ---------------------------------------------------------------------------
//  时间
// ---------------------------------------------------------------------------

/// FILETIME（1601-01-01 起的 100ns 数）→ Unix 毫秒；0 或异常返回 -1
qint64 fileTimeToUnixMs(qulonglong fileTime);

/// 是否正在使用：**开始时间非 0 且停止时间为 0**
bool usageInProgress(qulonglong lastUsedTimeStart, qulonglong lastUsedTimeStop);

// ---------------------------------------------------------------------------
//  条目与警报
// ---------------------------------------------------------------------------

/// 从注册表读到的原始三元组
struct RawUsage {
    QString keyName;
    qulonglong lastUsedTimeStart = 0;
    qulonglong lastUsedTimeStop = 0;
};

struct DeviceUsageEntry {
    QString keyName;     ///< 注册表键名（"已批准"名单里存的就是它）
    QString appPath;     ///< 还原出的 exe 路径
    QString displayName; ///< 展示名（"cam.exe"）
    qulonglong lastUsedTimeStart = 0;
    qulonglong lastUsedTimeStop = 0;
    bool inUse = false;
    bool approved = false;
    qint64 lastUsedUnixMs = -1; ///< 最近一次停止使用的时间（从未用过 = -1）
};

/// 把原始记录变成条目：丢掉"从没用过"的空键，正在使用的排在最前，
/// 其余按最近使用倒序（同名时按键名稳定排序，保证结果可复现）
QList<DeviceUsageEntry> buildUsageEntries(const QList<RawUsage> &raw,
                                          const QStringList &approvedKeys,
                                          const QDateTime &now);

/// 需要提醒的条目：**正在使用 且 不在"已批准"名单里**
QList<DeviceUsageEntry> alertEntries(const QList<DeviceUsageEntry> &entries);

/// 一句话警报（"摄像头正在被 cam.exe 使用"；多个应用时都列出来）
QString alertSummary(DeviceKind kind, const QList<DeviceUsageEntry> &alerts);

/// 托盘徽标文字：哪些设备正在被使用（"摄"/"麦"/"摄麦"；都没有则空串）
QString deviceBadgeText(const QList<DeviceKind> &activeKinds);

/// 人类可读的"用了多久"（"刚刚开始" / "已使用 3 分"）
QString humanizeUsageDuration(const DeviceUsageEntry &entry, const QDateTime &now);

// ---------------------------------------------------------------------------
//  读注册表（插件与端到端自检共用同一份实现）
// ---------------------------------------------------------------------------

struct ConsentStoreSnapshot {
    bool ok = false;             ///< 读取动作本身成功（**父键不存在也算成功**，只是列表为空）
    QList<RawUsage> entries;
    QString error;               ///< ok == false 时的中文原因
};

/// 读一个设备的 NonPackaged 使用记录
ConsentStoreSnapshot readConsentStore(DeviceKind kind);

} // namespace WinEase::Common
