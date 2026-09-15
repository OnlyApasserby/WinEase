#include "DeviceUsageMonitor.h"

#include "win32/RegistryUtils.h"

#include <QFileInfo>

#include <algorithm>

namespace WinEase::Common {

namespace {

using WinEase::Win32::RegistryKey;
using WinEase::Win32::RegistryRoot;
using WinEase::Win32::RegistryView;

/// FILETIME 与 Unix 纪元之间的 100ns 差（1601-01-01 → 1970-01-01）
constexpr qulonglong kFileTimeUnixEpochDiff = 116444736000000000ULL;

constexpr wchar_t kConsentStoreSubPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore";

} // namespace

QString deviceKindKey(DeviceKind kind)
{
    return kind == DeviceKind::Webcam ? QStringLiteral("webcam") : QStringLiteral("microphone");
}

QString deviceKindText(DeviceKind kind)
{
    return kind == DeviceKind::Webcam ? QStringLiteral("摄像头") : QStringLiteral("麦克风");
}

DeviceKind deviceKindFromKey(const QString &key, bool *ok)
{
    if (ok != nullptr) {
        *ok = true;
    }
    if (key == QLatin1String("webcam")) {
        return DeviceKind::Webcam;
    }
    if (key == QLatin1String("microphone")) {
        return DeviceKind::Microphone;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return DeviceKind::Webcam;
}

QList<DeviceKind> allDeviceKinds()
{
    return { DeviceKind::Webcam, DeviceKind::Microphone };
}

QString consentStorePath(DeviceKind kind, bool nonPackaged)
{
    QString path = QString::fromWCharArray(kConsentStoreSubPath);
    path += QLatin1Char('\\') + deviceKindKey(kind);
    if (nonPackaged) {
        path += QStringLiteral("\\NonPackaged");
    }
    return path;
}

QString appPathFromKeyName(const QString &keyName)
{
    // 注册表把路径里的 `\` 换成了 `#`（盘符的冒号保留），反向换回来即可。
    // 注意有些条目本来就是"包标识"（如 Microsoft.WindowsCamera_8wekyb3d8bbwe），
    // 里面没有 `#` —— 那种原样返回，显示名就是它自己
    QString path = keyName;
    path.replace(QLatin1Char('#'), QLatin1Char('\\'));
    return path;
}

QString keyNameFromAppPath(const QString &appPath)
{
    QString key = appPath;
    key.replace(QLatin1Char('\\'), QLatin1Char('#'));
    return key;
}

QString displayNameForAppKey(const QString &keyName)
{
    if (keyName.isEmpty()) {
        return QString();
    }
    const QString path = appPathFromKeyName(keyName);
    const QString fileName = QFileInfo(path).fileName();
    return fileName.isEmpty() ? keyName : fileName;
}

qint64 fileTimeToUnixMs(qulonglong fileTime)
{
    if (fileTime == 0 || fileTime < kFileTimeUnixEpochDiff) {
        return -1;
    }
    return static_cast<qint64>((fileTime - kFileTimeUnixEpochDiff) / 10000ULL);
}

bool usageInProgress(qulonglong lastUsedTimeStart, qulonglong lastUsedTimeStop)
{
    // ⚠ 本功能唯一的判据：开始时间非 0（说明真的用过）且停止时间为 0（说明还没停）
    return lastUsedTimeStart != 0 && lastUsedTimeStop == 0;
}

QList<DeviceUsageEntry> buildUsageEntries(const QList<RawUsage> &raw,
                                          const QStringList &approvedKeys,
                                          const QDateTime &now)
{
    Q_UNUSED(now)

    QList<DeviceUsageEntry> entries;
    entries.reserve(raw.size());

    for (const RawUsage &item : raw) {
        if (item.keyName.isEmpty() || item.lastUsedTimeStart == 0) {
            // 两个时间戳都是 0 的空键（"从没用过"）直接丢掉：
            // 把它们当成"正在使用"会让用户一启用功能就被假警报淹没
            continue;
        }

        DeviceUsageEntry entry;
        entry.keyName = item.keyName;
        entry.appPath = appPathFromKeyName(item.keyName);
        entry.displayName = displayNameForAppKey(item.keyName);
        entry.lastUsedTimeStart = item.lastUsedTimeStart;
        entry.lastUsedTimeStop = item.lastUsedTimeStop;
        entry.inUse = usageInProgress(item.lastUsedTimeStart, item.lastUsedTimeStop);
        entry.approved = approvedKeys.contains(item.keyName);
        entry.lastUsedUnixMs = fileTimeToUnixMs(item.lastUsedTimeStop);
        entries.append(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const DeviceUsageEntry &left,
                                                 const DeviceUsageEntry &right) {
        if (left.inUse != right.inUse) {
            return left.inUse; // 正在使用的排最前
        }
        if (left.inUse) {
            // 都在用：**刚开始的排前面**（用户最关心"谁刚刚打开了摄像头"）
            if (left.lastUsedTimeStart != right.lastUsedTimeStart) {
                return left.lastUsedTimeStart > right.lastUsedTimeStart;
            }
        } else if (left.lastUsedUnixMs != right.lastUsedUnixMs) {
            return left.lastUsedUnixMs > right.lastUsedUnixMs; // 最近用过的靠前
        }
        return left.keyName < right.keyName; // 稳定：同名时按键名，结果可复现
    });
    return entries;
}

QList<DeviceUsageEntry> alertEntries(const QList<DeviceUsageEntry> &entries)
{
    QList<DeviceUsageEntry> alerts;
    for (const DeviceUsageEntry &entry : entries) {
        if (entry.inUse && !entry.approved) {
            alerts.append(entry);
        }
    }
    return alerts;
}

QString alertSummary(DeviceKind kind, const QList<DeviceUsageEntry> &alerts)
{
    if (alerts.isEmpty()) {
        return QString();
    }

    QStringList names;
    for (const DeviceUsageEntry &entry : alerts) {
        names << entry.displayName;
    }
    return QStringLiteral("%1正在被使用：%2")
        .arg(deviceKindText(kind), names.join(QStringLiteral("、")));
}

QString deviceBadgeText(const QList<DeviceKind> &activeKinds)
{
    QString text;
    for (const DeviceKind kind : activeKinds) {
        text += kind == DeviceKind::Webcam ? QStringLiteral("摄") : QStringLiteral("麦");
    }
    return text;
}

QString humanizeUsageDuration(const DeviceUsageEntry &entry, const QDateTime &now)
{
    if (entry.inUse) {
        if (entry.lastUsedTimeStart == 0) {
            return QStringLiteral("正在使用");
        }
        const QDateTime started = QDateTime::fromMSecsSinceEpoch(
            fileTimeToUnixMs(entry.lastUsedTimeStart));
        if (!started.isValid()) {
            return QStringLiteral("正在使用");
        }
        const qint64 seconds = started.secsTo(now);
        if (seconds < 3) {
            return QStringLiteral("刚刚开始");
        }
        if (seconds < 60) {
            return QStringLiteral("已使用 %1 秒").arg(seconds);
        }
        return QStringLiteral("已使用 %1 分").arg(seconds / 60);
    }

    if (entry.lastUsedUnixMs <= 0) {
        return QStringLiteral("未使用过");
    }
    const qint64 seconds = QDateTime::fromMSecsSinceEpoch(entry.lastUsedUnixMs).secsTo(now);
    if (seconds < 60) {
        return QStringLiteral("刚刚用过");
    }
    if (seconds < 3600) {
        return QStringLiteral("%1 分钟前用过").arg(seconds / 60);
    }
    return QStringLiteral("%1 小时前用过").arg(seconds / 3600);
}

ConsentStoreSnapshot readConsentStore(DeviceKind kind)
{
    ConsentStoreSnapshot snapshot;

    const QString path = consentStorePath(kind, true);
    QString error;
    RegistryKey root = RegistryKey::open(RegistryRoot::CurrentUser,
                                         path,
                                         true,
                                         RegistryView::Default,
                                         &error);
    if (!root.isValid()) {
        // ⚠ 父键不存在是**正常**的（这台机器还没有任何应用碰过这个设备）——
        //   它和"读取出错"必须区分开：前者是"没有警报"，后者是"看不到"
        snapshot.ok = true;
        snapshot.error.clear();
        return snapshot;
    }

    const QStringList subKeys = root.subKeyNames();
    for (const QString &subKey : subKeys) {
        RegistryKey app = RegistryKey::open(RegistryRoot::CurrentUser,
                                            path + QLatin1Char('\\') + subKey,
                                            true,
                                            RegistryView::Default);
        if (!app.isValid()) {
            continue; // 单个子键读不到不致命（权限/竞态），跳过它继续
        }

        RawUsage usage;
        usage.keyName = subKey;
        usage.lastUsedTimeStart =
            app.value(QStringLiteral("LastUsedTimeStart")).toULongLong();
        usage.lastUsedTimeStop = app.value(QStringLiteral("LastUsedTimeStop")).toULongLong();
        snapshot.entries.append(usage);
    }

    snapshot.ok = true;
    return snapshot;
}

} // namespace WinEase::Common
