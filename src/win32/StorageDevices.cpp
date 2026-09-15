#include "win32/StorageDevices.h"

#include "win32/Win32Error.h"

#include <QByteArray>

#include <algorithm>
#include <vector>

#include <windows.h>

// ⚠ 包含顺序有讲究：`winioctl.h` 会带入 `devioctl.h` / `ntddstor.h` 依赖的类型，
//    必须先于 `ntddstor.h`（反过来会让 ntddstor.h 里的结构体报"未知重写说明符"）
#include <winioctl.h> // STORAGE_DEVICE_DESCRIPTOR / StorageDeviceProperty / IOCTL_STORAGE_*
#include <cfgmgr32.h> // CM_Locate_DevNode / CM_Request_Device_Eject（PnP 弹出）
#include <setupapi.h>

namespace WinEase::Win32 {

namespace {

// 设备接口 GUID 手写在这里（省掉 initguid.h / uuid.lib 的链接依赖）。
// 这两个 GUID 是 Windows 的稳定约定，不会随版本变。
//   GUID_DEVINTERFACE_DISK   = {53f56307-b6bf-11d0-94f2-00a0c91efb8b}
//   GUID_DEVINTERFACE_VOLUME = {53f5630d-b6bf-11d0-94f2-00a0c91efb8b}
const GUID kGuidDevInterfaceDisk = { 0x53f56307, 0xb6bf, 0x11d0,
                                     { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

struct PhysicalDisk {
    int number = -1;
    QString devicePath;
    QString pnpDeviceId;
    QString productName;
    RemovalClass removal = RemovalClass::Unknown;
    int busType = 0;
};

struct VolumeRecord {
    QString mountPoint;
    QString letter;
    QString label;
    QString fileSystem;
    quint64 totalBytes = 0;
    quint64 freeBytes = 0;
    bool ready = false;
    int diskNumber = -1;
    int partitionNumber = 0;
};

/// 打开设备路径（**share 读写、不要任何访问权限**：只查询属性，不占用设备）
HANDLE openDevice(const QString &path)
{
    return ::CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                         0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr,
                         OPEN_EXISTING,
                         0,
                         nullptr);
}

QString busText(int busType)
{
    switch (busType) {
    case BusTypeUsb:       return QStringLiteral("USB");
    case BusTypeNvme:      return QStringLiteral("NVMe");
    case BusTypeSata:      return QStringLiteral("SATA");
    case BusTypeAta:       return QStringLiteral("ATA");
    case BusTypeScsi:      return QStringLiteral("SCSI");
    case BusTypeSas:       return QStringLiteral("SAS");
    case BusTypeSd:        return QStringLiteral("SD");
    case BusTypeMmc:       return QStringLiteral("MMC");
    case BusTypeVirtual:   return QStringLiteral("虚拟");
    case BusTypeFileBackedVirtual: return QStringLiteral("虚拟（文件）");
    default:               break;
    }
    return QStringLiteral("总线 %1").arg(busType);
}

QList<PhysicalDisk> enumeratePhysicalDisks()
{
    QList<PhysicalDisk> disks;

    const HDEVINFO info = ::SetupDiGetClassDevsW(&kGuidDevInterfaceDisk,
                                                 nullptr,
                                                 nullptr,
                                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        return disks;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (::SetupDiEnumDeviceInterfaces(info, nullptr, &kGuidDevInterfaceDisk, index,
                                          &interfaceData)
            == FALSE) {
            break;
        }

        DWORD required = 0;
        ::SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        QByteArray buffer(static_cast<int>(required), Qt::Uninitialized);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (::SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, required, nullptr,
                                               &deviceInfo)
            == FALSE) {
            continue;
        }

        PhysicalDisk disk;
        disk.devicePath = QString::fromWCharArray(detail->DevicePath);

        // ① PnP 的"可移除策略"：这是判断"要不要安全弹出"最权威的一处
        DWORD removalPolicy = 0;
        DWORD size = 0;
        DWORD type = 0;
        if (::SetupDiGetDeviceRegistryPropertyW(info, &deviceInfo, SPDRP_REMOVAL_POLICY, &type,
                                                reinterpret_cast<PBYTE>(&removalPolicy),
                                                sizeof(removalPolicy), &size)
            != FALSE) {
            switch (removalPolicy) {
            case CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL:
                disk.removal = RemovalClass::SurpriseRemoval;
                break;
            case CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL:
                disk.removal = RemovalClass::OrderlyRemoval;
                break;
            case CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL:
                disk.removal = RemovalClass::NoRemoval;
                break;
            default:
                break;
            }
        }

        // 设备实例 ID（PnP 弹出要用它定位 devnode）
        wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
        if (::SetupDiGetDeviceInstanceIdW(info, &deviceInfo, instanceId, MAX_DEVICE_ID_LEN, nullptr)
            != FALSE) {
            disk.pnpDeviceId = QString::fromWCharArray(instanceId);
        }

        // ② 总线类型 + 产品名 + 设备号
        const HANDLE handle = openDevice(disk.devicePath);
        if (handle != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceProperty;
            query.QueryType = PropertyStandardQuery;

            QByteArray descriptorBuffer(1024, Qt::Uninitialized);
            DWORD returned = 0;
            if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                  descriptorBuffer.data(),
                                  static_cast<DWORD>(descriptorBuffer.size()), &returned, nullptr)
                != FALSE) {
                const auto *descriptor =
                    reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(descriptorBuffer.constData());
                disk.busType = static_cast<int>(descriptor->BusType);
                if (descriptor->ProductIdOffset != 0
                    && static_cast<int>(descriptor->ProductIdOffset) < descriptorBuffer.size()) {
                    disk.productName =
                        QString::fromLatin1(descriptorBuffer.constData()
                                            + descriptor->ProductIdOffset)
                            .trimmed();
                }
            }

            STORAGE_DEVICE_NUMBER number{};
            if (::DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number,
                                  sizeof(number), &returned, nullptr)
                != FALSE) {
                disk.number = static_cast<int>(number.DeviceNumber);
            }
            ::CloseHandle(handle);
        }

        disks.append(disk);
    }

    ::SetupDiDestroyDeviceInfoList(info);
    return disks;
}

QList<VolumeRecord> enumerateVolumes()
{
    QList<VolumeRecord> volumes;

    const DWORD mask = ::GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if ((mask & (1UL << index)) == 0) {
            continue;
        }

        VolumeRecord volume;
        volume.letter = QString(QChar(static_cast<char16_t>(L'A' + index))) + QLatin1Char(':');
        volume.mountPoint = volume.letter + QStringLiteral("\\");

        const std::wstring root = volume.mountPoint.toStdWString();
        const UINT driveType = ::GetDriveTypeW(root.c_str());
        if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN) {
            // 盘符在、但没有可访问的介质（空读卡器）—— 仍然记下来，
            // "设备在但没插卡"本身就是用户需要知道的信息
            volumes.append(volume);
            continue;
        }

        wchar_t labelBuffer[MAX_PATH + 1] = {};
        wchar_t fileSystemBuffer[MAX_PATH + 1] = {};
        DWORD serial = 0;
        DWORD maxComponent = 0;
        DWORD flags = 0;
        if (::GetVolumeInformationW(root.c_str(), labelBuffer, MAX_PATH, &serial, &maxComponent,
                                    &flags, fileSystemBuffer, MAX_PATH)
            != FALSE) {
            volume.label = QString::fromWCharArray(labelBuffer);
            volume.fileSystem = QString::fromWCharArray(fileSystemBuffer);
        }

        ULARGE_INTEGER freeForCaller{};
        ULARGE_INTEGER total{};
        ULARGE_INTEGER totalFree{};
        if (::GetDiskFreeSpaceExW(root.c_str(), &freeForCaller, &total, &totalFree) != FALSE) {
            volume.totalBytes = total.QuadPart;
            volume.freeBytes = totalFree.QuadPart;
            volume.ready = true;
        }

        // 卷 → 物理盘号：拿 IOCTL_STORAGE_GET_DEVICE_NUMBER 直接问
        const HANDLE handle = openDevice(QStringLiteral("\\\\.\\") + volume.letter);
        if (handle != INVALID_HANDLE_VALUE) {
            STORAGE_DEVICE_NUMBER number{};
            DWORD returned = 0;
            if (::DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number,
                                  sizeof(number), &returned, nullptr)
                != FALSE) {
                volume.diskNumber = static_cast<int>(number.DeviceNumber);
                volume.partitionNumber = static_cast<int>(number.PartitionNumber);
            }
            ::CloseHandle(handle);
        }

        volumes.append(volume);
    }

    return volumes;
}

QList<StorageDeviceInfo> buildDeviceList(bool onlyRemovable, QString *errorOut)
{
    Q_UNUSED(errorOut)

    const QList<PhysicalDisk> disks = enumeratePhysicalDisks();
    const QList<VolumeRecord> volumes = enumerateVolumes();

    QList<StorageDeviceInfo> result;
    QList<int> consumedVolumes;

    for (const PhysicalDisk &disk : disks) {
        QList<VolumeRecord> own;
        for (int index = 0; index < volumes.size(); ++index) {
            if (volumes.at(index).diskNumber == disk.number) {
                own.append(volumes.at(index));
                consumedVolumes.append(index);
            }
        }

        StorageDeviceInfo base;
        base.devicePath = disk.devicePath;
        base.pnpDeviceId = disk.pnpDeviceId;
        base.productName = disk.productName;
        base.removal = disk.removal;
        base.busType = disk.busType;
        base.busText = busText(disk.busType);
        base.isUsbStorage = (disk.busType == static_cast<int>(BusTypeUsb));

        if (own.isEmpty()) {
            // 未格式化 / 无盘符的设备也要列出来：用户需要知道"有这么个东西插着"
            if (!onlyRemovable || base.removable()) {
                result.append(base);
            }
            continue;
        }

        for (const VolumeRecord &volume : own) {
            StorageDeviceInfo info = base;
            info.volumePath = volume.mountPoint;
            info.driveLetter = volume.letter;
            info.label = volume.label;
            info.fileSystem = volume.fileSystem;
            info.totalBytes = volume.totalBytes;
            info.freeBytes = volume.freeBytes;
            info.isReady = volume.ready;
            info.partitionNumber = volume.partitionNumber;
            if (!onlyRemovable || info.removable()) {
                result.append(info);
            }
        }
    }

    // 兜底：有盘符但映射不到物理盘（网络盘 / 虚拟盘）——
    // 只读展示时列出来并标注（它们**不该**出现在"可移除设备"列表里）
    if (!onlyRemovable) {
        for (int index = 0; index < volumes.size(); ++index) {
            if (consumedVolumes.contains(index)) {
                continue;
            }
            StorageDeviceInfo info;
            info.volumePath = volumes.at(index).mountPoint;
            info.driveLetter = volumes.at(index).letter;
            info.label = volumes.at(index).label;
            info.fileSystem = volumes.at(index).fileSystem;
            info.totalBytes = volumes.at(index).totalBytes;
            info.freeBytes = volumes.at(index).freeBytes;
            info.isReady = volumes.at(index).ready;
            info.removal = RemovalClass::Unknown;
            info.busText = QStringLiteral("未映射到物理盘");
            result.append(info);
        }
    }

    // 稳定排序：先按盘符，再按设备路径（结果可复现，方便断言）
    std::sort(result.begin(), result.end(),
              [](const StorageDeviceInfo &left, const StorageDeviceInfo &right) {
                  if (left.driveLetter != right.driveLetter) {
                      return left.driveLetter < right.driveLetter;
                  }
                  return left.devicePath < right.devicePath;
              });
    return result;
}

QString describeVeto(PNP_VETO_TYPE type, const QString &name)
{
    switch (type) {
    case PNP_VetoOutstandingOpen:
        return QStringLiteral("还有未关闭的文件句柄（有程序正在使用该设备）");
    case PNP_VetoWindowsApp:
        return name.isEmpty() ? QStringLiteral("有应用程序阻止了弹出")
                              : QStringLiteral("应用程序「%1」正在使用该设备").arg(name);
    case PNP_VetoWindowsService:
        return name.isEmpty() ? QStringLiteral("有服务阻止了弹出")
                              : QStringLiteral("服务「%1」正在使用该设备").arg(name);
    case PNP_VetoLegacyDevice:
    case PNP_VetoLegacyDriver:
        return QStringLiteral("旧式驱动阻止了弹出");
    case PNP_VetoDevice:
        return name.isEmpty() ? QStringLiteral("设备自身拒绝弹出")
                              : QStringLiteral("设备「%1」拒绝弹出").arg(name);
    case PNP_VetoPendingClose:
        return QStringLiteral("有操作尚未完成（稍后再试）");
    case PNP_VetoInsufficientPower:
        return QStringLiteral("电源不足");
    case PNP_VetoNonDisableable:
        return QStringLiteral("该设备不可被停用（系统关键设备）");
    case PNP_VetoInsufficientRights:
        return QStringLiteral("权限不足（需要管理员）");
    case PNP_VetoIllegalDeviceRequest:
        return QStringLiteral("设备请求非法");
    case PNP_VetoTypeUnknown:
    default:
        break;
    }
    return name.isEmpty() ? QStringLiteral("原因未知") : name;
}

} // namespace

QString removalClassText(RemovalClass value)
{
    switch (value) {
    case RemovalClass::SurpriseRemoval:
        return QStringLiteral("可随时拔出");
    case RemovalClass::OrderlyRemoval:
        return QStringLiteral("需安全弹出");
    case RemovalClass::NoRemoval:
        return QStringLiteral("固定盘");
    case RemovalClass::Unknown:
    default:
        break;
    }
    return QStringLiteral("未知");
}

QString StorageDeviceInfo::describe() const
{
    QStringList parts;
    parts << (driveLetter.isEmpty() ? QStringLiteral("（无盘符）") : driveLetter);
    if (!label.isEmpty()) {
        parts << label;
    }
    if (!productName.isEmpty()) {
        parts << productName;
    }
    if (!fileSystem.isEmpty()) {
        parts << fileSystem;
    }
    if (totalBytes > 0) {
        parts << humanizeBytes(totalBytes);
    }
    if (!busText.isEmpty()) {
        parts << busText;
    }
    parts << removalClassText(removal);
    return parts.join(QStringLiteral(" · "));
}

QList<StorageDeviceInfo> removableStorageDevices(QString *errorOut)
{
    return buildDeviceList(true, errorOut);
}

QList<StorageDeviceInfo> allStorageDevices(QString *errorOut)
{
    return buildDeviceList(false, errorOut);
}

QString humanizeBytes(quint64 bytes)
{
    constexpr double kKb = 1024.0;
    constexpr double kMb = kKb * 1024.0;
    constexpr double kGb = kMb * 1024.0;
    constexpr double kTb = kGb * 1024.0;

    const double value = static_cast<double>(bytes);
    if (value >= kTb) {
        return QStringLiteral("%1 TB").arg(value / kTb, 0, 'f', 2);
    }
    if (value >= kGb) {
        return QStringLiteral("%1 GB").arg(value / kGb, 0, 'f', 1);
    }
    if (value >= kMb) {
        return QStringLiteral("%1 MB").arg(value / kMb, 0, 'f', 1);
    }
    if (value >= kKb) {
        return QStringLiteral("%1 KB").arg(value / kKb, 0, 'f', 0);
    }
    return QStringLiteral("%1 字节").arg(bytes);
}

QString ejectFailureReason(unsigned long errorCode)
{
    switch (errorCode) {
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("权限不足（换用管理员权限重试）");
    case ERROR_DEVICE_IN_USE:
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION:
        return QStringLiteral("有程序正在使用该设备（关闭相关程序/窗口后重试）");
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return QStringLiteral("该设备不支持程序化弹出（可能是内置盘或虚拟盘）");
    case ERROR_WRITE_PROTECT:
        return QStringLiteral("设备处于写保护状态");
    case ERROR_NOT_READY:
        return QStringLiteral("设备未就绪（可能没有介质）");
    case ERROR_NO_MEDIA_IN_DRIVE:
        return QStringLiteral("驱动器中没有介质");
    default:
        break;
    }
    return describeFailure(QStringLiteral("弹出设备"), errorCode);
}

bool ejectStorageDevice(const StorageDeviceInfo &device, QString *errorOut, QString *vetoReasonOut)
{
    DWORD lastError = ERROR_SUCCESS;

    // ① 先试"介质弹出"：U 盘 / 光驱的有效路径。
    //    对移动硬盘通常返回 ERROR_NOT_SUPPORTED —— 那不是失败，只是**这条不适用**
    if (!device.driveLetter.isEmpty()) {
        const QString volumeDevice = QStringLiteral("\\\\.\\") + device.driveLetter;
        const HANDLE handle = ::CreateFileW(reinterpret_cast<LPCWSTR>(volumeDevice.utf16()),
                                            GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            0,
                                            nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD returned = 0;
            const BOOL ok = ::DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0,
                                              nullptr, 0, &returned, nullptr);
            if (ok != FALSE) {
                ::CloseHandle(handle);
                return true;
            }
            lastError = ::GetLastError();
            ::CloseHandle(handle);
        } else {
            lastError = ::GetLastError();
        }
    }

    // ② 退到 PnP 弹出（等价于系统的"安全删除硬件"）——**移动硬盘走的就是这条**。
    //    它的 veto 机制会告诉我们"是谁在阻止弹出"，比自己扫全系统句柄便宜得多
    if (!device.pnpDeviceId.isEmpty()) {
        const std::wstring instanceId = device.pnpDeviceId.toStdWString();
        std::vector<wchar_t> mutableId(instanceId.begin(), instanceId.end());
        mutableId.push_back(L'\0');

        DEVINST devNode = 0;
        if (::CM_Locate_DevNodeW(&devNode, mutableId.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            PNP_VETO_TYPE vetoType = PNP_VetoTypeUnknown;
            wchar_t vetoName[MAX_PATH] = {};
            const CONFIGRET result =
                ::CM_Request_Device_EjectW(devNode, &vetoType, vetoName, MAX_PATH, 0);
            if (result == CR_SUCCESS) {
                return true;
            }

            if (vetoReasonOut != nullptr) {
                *vetoReasonOut = describeVeto(vetoType, QString::fromWCharArray(vetoName));
            }
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("系统拒绝弹出：%1")
                                .arg(vetoReasonOut != nullptr ? *vetoReasonOut
                                                              : QStringLiteral("原因未知"));
            }
            return false;
        }
    }

    if (errorOut != nullptr) {
        *errorOut = lastError != ERROR_SUCCESS
                        ? ejectFailureReason(lastError)
                        : QStringLiteral("该设备既没有可弹出的卷，也没有可定位的 PnP 设备节点");
    }
    return false;
}

} // namespace WinEase::Win32
