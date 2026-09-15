#include "win32/SystemInfo.h"

#include "win32/Win32Error.h"

#include <QDateTime>

#include <algorithm>
#include <cstddef>
#include <vector>

// windows.h 必须最先包含：下面的 SDK 头都依赖它
#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>

namespace WinEase::Win32 {

namespace {

// ---------------------------------------------------------------------------
//  PDH 封装（仅在实现文件内使用，不把 pdh.h 泄漏到公共头）
// ---------------------------------------------------------------------------

struct PdhArrayValue {
    QString instance;
    double value = 0.0;
};

/// 打开查询并绑定一个（可含通配符的）英文计数器路径。
/// 使用 PdhAddEnglishCounterW，保证中文系统上也能用英文路径。
bool pdhOpen(void **queryOut, void **counterOut, const QString &counterPath, QString *errorOut)
{
    PDH_HQUERY query = nullptr;
    PDH_STATUS status = ::PdhOpenQueryW(nullptr, 0, &query);
    if (status != ERROR_SUCCESS) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("打开性能计数器查询"), static_cast<DWORD>(status));
        }
        return false;
    }

    PDH_HCOUNTER counter = nullptr;
    status = ::PdhAddEnglishCounterW(query, reinterpret_cast<LPCWSTR>(counterPath.utf16()), 0, &counter);
    if (status != ERROR_SUCCESS) {
        ::PdhCloseQuery(query);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("当前系统不支持性能计数器「%1」（错误码 %2）")
                            .arg(counterPath)
                            .arg(static_cast<quint32>(status));
        }
        return false;
    }

    *queryOut = query;
    *counterOut = counter;
    return true;
}

void pdhClose(void *query)
{
    if (query != nullptr) {
        ::PdhCloseQuery(static_cast<PDH_HQUERY>(query));
    }
}

/// 采集一次数据；返回是否成功
bool pdhCollect(void *query)
{
    if (query == nullptr) {
        return false;
    }
    return ::PdhCollectQueryData(static_cast<PDH_HQUERY>(query)) == ERROR_SUCCESS;
}

/// 读取通配符计数器的数组结果
bool pdhReadArray(void *counter, QList<PdhArrayValue> *out, QString *errorOut)
{
    if (counter == nullptr || out == nullptr) {
        return false;
    }

    PDH_HCOUNTER handle = static_cast<PDH_HCOUNTER>(counter);

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = ::PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
    if (status == PDH_NO_DATA) {
        // 尚无数据（例如刚建立的查询），不算错误
        return true;
    }
    if (status != PDH_MORE_DATA) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("读取性能计数器"), static_cast<DWORD>(status));
        }
        return false;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
    status = ::PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
    if (status != ERROR_SUCCESS) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("读取性能计数器数据"), static_cast<DWORD>(status));
        }
        return false;
    }

    out->reserve(static_cast<qsizetype>(itemCount));
    for (DWORD index = 0; index < itemCount; ++index) {
        const PDH_FMT_COUNTERVALUE_ITEM_W &item = items[index];
        if (item.FmtValue.CStatus != ERROR_SUCCESS) {
            continue; // 单个实例无数据时跳过，不影响整体
        }
        PdhArrayValue value;
        value.instance = QString::fromWCharArray(item.szName != nullptr ? item.szName : L"");
        value.value = item.FmtValue.doubleValue;
        out->append(value);
    }
    return true;
}

/// 实例名是否代表"总计"（不同计数器的写法有 _Total / _total 等）
bool isTotalInstance(const QString &instance)
{
    return instance.contains(QStringLiteral("_Total"), Qt::CaseInsensitive);
}

} // namespace

// ============================================================================
//  内存
// ============================================================================

MemoryInfo memoryInfo()
{
    MemoryInfo info;

    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status) == FALSE) {
        return info;
    }

    info.totalBytes = static_cast<quint64>(status.ullTotalPhys);
    info.availableBytes = static_cast<quint64>(status.ullAvailPhys);
    info.usedBytes = info.totalBytes - info.availableBytes;
    info.usagePercent = (info.totalBytes > 0)
                            ? (static_cast<double>(info.usedBytes) * 100.0 / static_cast<double>(info.totalBytes))
                            : 0.0;
    info.valid = true;
    return info;
}

// ============================================================================
//  CPU
// ============================================================================

CpuSampler::CpuSampler()
{
    // "Processor Information" 提供每核心与 _Total，且比旧版 "Processor" 更准确
    pdhOpen(&m_query, &m_counter,
            QStringLiteral("\\Processor Information(*)\\% Processor Time"),
            &m_initError);
}

CpuSampler::~CpuSampler()
{
    release();
}

void CpuSampler::release()
{
    pdhClose(m_query);
    m_query = nullptr;
    m_counter = nullptr;
}

CpuSampler::Sample CpuSampler::sample()
{
    Sample result;

    if (m_query == nullptr) {
        result.error = m_initError.isEmpty() ? QStringLiteral("CPU 性能计数器不可用") : m_initError;
        return result;
    }

    if (!pdhCollect(m_query)) {
        result.error = QStringLiteral("采集 CPU 性能数据失败");
        return result;
    }
    ++m_collectCount;

    QList<PdhArrayValue> values;
    if (!pdhReadArray(m_counter, &values, &result.error)) {
        return result;
    }

    // PDH 的百分比需要两次采集之间的差值，首次结果无意义
    if (m_collectCount < 2) {
        result.error = QStringLiteral("正在建立采样基准（下一次即可用）");
        return result;
    }

    double overall = -1.0;
    double sum = 0.0;
    int coreCount = 0;

    for (const PdhArrayValue &entry : values) {
        const double percent = std::clamp(entry.value, 0.0, 100.0);
        if (isTotalInstance(entry.instance)) {
            overall = percent;
            continue;
        }
        result.perCorePercent.append(percent);
        sum += percent;
        ++coreCount;
    }

    if (overall < 0.0 && coreCount > 0) {
        overall = sum / static_cast<double>(coreCount);
    }

    result.overallPercent = std::clamp(overall < 0.0 ? 0.0 : overall, 0.0, 100.0);
    result.valid = true;
    return result;
}

// ============================================================================
//  网络吞吐
//
//  实现选择说明：
//      这里用 PDH 计数器 "\Network Interface(*)\Bytes Received/sec"，
//      而不是 GetIfTable2(MIB_IF_ROW2)。原因：
//        1. PDH 直接给出**每秒速率**，不需要自己保存上次计数再做差分；
//        2. 与 CPU/GPU 采样共用同一套 PDH 封装，代码面更小；
//        3. 避免依赖 netioapi.h 的 WINAPI_FAMILY / NTDDI 嵌套条件
//           （实测在本工程配置下 MIB_IF_TABLE2 不可见，排查成本高于收益）。
//      代价：累计值只能由速率积分得到，字段语义已在头文件中说明。
// ============================================================================

namespace {

/// 需要从网络统计中排除的适配器关键字（回环、隧道、虚拟网卡）
const QStringList &ignoredNetworkInstanceKeywords()
{
    static const QStringList keywords{
        QStringLiteral("loopback"),
        QStringLiteral("isatap"),
        QStringLiteral("teredo"),
        QStringLiteral("6to4"),
        QStringLiteral("pseudo"),
        QStringLiteral("virtual"),
        QStringLiteral("vethernet"),
        QStringLiteral("vmware"),
        QStringLiteral("hyper-v"),
        QStringLiteral("tap-"),
        QStringLiteral("wan miniport"),
    };
    return keywords;
}

bool isIgnoredNetworkInstance(const QString &instance)
{
    const QString lower = instance.toLower();
    for (const QString &keyword : ignoredNetworkInstanceKeywords()) {
        if (lower.contains(keyword)) {
            return true;
        }
    }
    return false;
}

/// 对通配符计数器求和（跳过应忽略的实例）
bool sumNetworkCounter(void *counter, quint64 *totalOut, QString *errorOut)
{
    QList<PdhArrayValue> values;
    if (!pdhReadArray(counter, &values, errorOut)) {
        return false;
    }

    double sum = 0.0;
    for (const PdhArrayValue &entry : values) {
        if (isIgnoredNetworkInstance(entry.instance)) {
            continue;
        }
        sum += std::max(0.0, entry.value);
    }
    if (totalOut != nullptr) {
        *totalOut = static_cast<quint64>(sum);
    }
    return true;
}

} // namespace

NetworkSampler::NetworkSampler()
{
    QString rxError;
    QString txError;
    const bool rxOk = pdhOpen(&m_rxQuery, &m_rxCounter,
                              QStringLiteral("\\Network Interface(*)\\Bytes Received/sec"),
                              &rxError);
    const bool txOk = pdhOpen(&m_txQuery, &m_txCounter,
                              QStringLiteral("\\Network Interface(*)\\Bytes Sent/sec"),
                              &txError);

    if (!rxOk || !txOk) {
        m_initError = !rxError.isEmpty() ? rxError : txError;
        release();
    }
}

NetworkSampler::~NetworkSampler()
{
    release();
}

void NetworkSampler::release()
{
    pdhClose(m_rxQuery);
    pdhClose(m_txQuery);
    m_rxQuery = nullptr;
    m_txQuery = nullptr;
    m_rxCounter = nullptr;
    m_txCounter = nullptr;
}

NetworkSampler::Sample NetworkSampler::sample()
{
    Sample result;

    if (m_rxQuery == nullptr || m_txQuery == nullptr) {
        result.error = m_initError.isEmpty() ? QStringLiteral("网络性能计数器不可用") : m_initError;
        return result;
    }

    if (!pdhCollect(m_rxQuery) || !pdhCollect(m_txQuery)) {
        result.error = QStringLiteral("采集网络性能数据失败");
        return result;
    }
    ++m_collectCount;

    quint64 rxPerSecond = 0;
    quint64 txPerSecond = 0;
    if (!sumNetworkCounter(m_rxCounter, &rxPerSecond, &result.error)) {
        return result;
    }
    if (!sumNetworkCounter(m_txCounter, &txPerSecond, &result.error)) {
        return result;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (m_collectCount < 2) {
        // "/sec" 计数器首次采集没有基准
        m_lastTimestampMs = now;
        result.error = QStringLiteral("正在建立采样基准（下一次即可用）");
        return result;
    }

    if (m_lastTimestampMs > 0 && now > m_lastTimestampMs) {
        const double seconds = static_cast<double>(now - m_lastTimestampMs) / 1000.0;
        m_totalRx += static_cast<quint64>(static_cast<double>(rxPerSecond) * seconds);
        m_totalTx += static_cast<quint64>(static_cast<double>(txPerSecond) * seconds);
    }
    m_lastTimestampMs = now;

    result.rxBytesPerSecond = rxPerSecond;
    result.txBytesPerSecond = txPerSecond;
    result.totalRxBytes = m_totalRx;
    result.totalTxBytes = m_totalTx;
    result.valid = true;
    return result;
}

// ============================================================================
//  GPU
// ============================================================================

GpuSampler::GpuSampler()
{
    pdhOpen(&m_query, &m_counter,
            QStringLiteral("\\GPU Engine(*)\\Utilization Percentage"),
            &m_initError);
}

GpuSampler::~GpuSampler()
{
    release();
}

void GpuSampler::release()
{
    pdhClose(m_query);
    m_query = nullptr;
    m_counter = nullptr;
}

GpuSampler::Sample GpuSampler::sample()
{
    Sample result;

    if (m_query == nullptr) {
        result.error = m_initError.isEmpty() ? QStringLiteral("GPU 性能计数器不可用") : m_initError;
        return result;
    }

    if (!pdhCollect(m_query)) {
        result.error = QStringLiteral("采集 GPU 性能数据失败");
        return result;
    }
    ++m_collectCount;

    QList<PdhArrayValue> values;
    if (!pdhReadArray(m_counter, &values, &result.error)) {
        return result;
    }

    if (m_collectCount < 2) {
        result.error = QStringLiteral("正在建立采样基准（下一次即可用）");
        return result;
    }

    // 实例名形如 "pid_1234_luid_0x00000000_0x0000C4BB_phys_0_eng_0_engtype_3D"
    // 任务管理器的 GPU 占用取各引擎活动，这里简化为：累加 3D 引擎在各进程上的占用
    double utilization = 0.0;
    bool foundAny = false;
    for (const PdhArrayValue &entry : values) {
        if (!entry.instance.contains(QStringLiteral("engtype_3D"), Qt::CaseInsensitive)) {
            continue;
        }
        utilization += std::max(0.0, entry.value);
        foundAny = true;
    }

    if (!foundAny) {
        result.error = QStringLiteral("当前系统未报告 3D 引擎计数器实例");
        return result;
    }

    result.utilizationPercent = std::clamp(utilization, 0.0, 100.0);
    result.valid = true;
    return result;
}

// ============================================================================
//  存储设备温度
// ============================================================================

namespace {

/// 从描述符尾部那一串"以 0 结尾的 ASCII 串"里取一个（偏移量相对描述符开头）
QString descriptorString(const BYTE *buffer, DWORD returnedBytes, DWORD offset)
{
    if (offset == 0 || offset >= returnedBytes) {
        return QString();
    }
    // 最后一个串可能没有终止符 —— 按缓冲区剩余长度兜底，绝不越界读
    // （不用 strnlen：MSVC 的 <cstring> 不保证提供 std::strnlen）
    const char *start = reinterpret_cast<const char *>(buffer + offset);
    const DWORD maxLength = returnedBytes - offset;
    DWORD length = 0;
    while (length < maxLength && start[length] != '\0') {
        ++length;
    }
    return QString::fromLatin1(start, static_cast<int>(length)).trimmed();
}

/// 在一个已打开的磁盘句柄上读型号（STORAGE_DEVICE_DESCRIPTOR 的厂商串 + 产品串）。
/// 读不到就返回空串（面板会退化成"物理磁盘"）——型号只是 tooltip 里的补充信息
QString diskModelOf(HANDLE device)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty; // 0：设备描述符（SDK 自带枚举，无需兜底）
    query.QueryType = PropertyStandardQuery;

    BYTE buffer[1024] = {};
    DWORD returnedBytes = 0;
    if (::DeviceIoControl(device,
                          IOCTL_STORAGE_QUERY_PROPERTY,
                          &query,
                          sizeof(query),
                          buffer,
                          sizeof(buffer),
                          &returnedBytes,
                          nullptr)
            == FALSE
        || returnedBytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return QString();
    }

    const auto *descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer);
    const QString vendor = descriptorString(buffer, returnedBytes, descriptor->VendorIdOffset);
    const QString product = descriptorString(buffer, returnedBytes, descriptor->ProductIdOffset);
    return vendor.isEmpty() ? product : (vendor + QLatin1Char(' ') + product);
}

/// 把"温度属性拿不到"翻译成人话
QString temperatureFailureText(const QString &action, DWORD error)
{
    // ⚠ ERROR_INVALID_FUNCTION 在这里通常是"这个驱动不处理该查询"，
    //   **不是**"我们参数传错了"——照字面翻成"函数不正确（参数或调用方式有误）"
    //   会把用户往沟里带（本机就是在属性 ID 抄错时撞到这个错误码的）
    if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED) {
        return action + QStringLiteral("：该磁盘或驱动不提供温度属性（驱动未实现此查询）");
    }
    QString text = describeFailure(action, error);
    if (isAccessDenied(error)) {
        text += QStringLiteral("；该查询可能需要管理员权限");
    }
    return text;
}

} // namespace

QList<DiskTemperature> diskTemperatures()
{
    // 枚举 `\\.\PhysicalDriveN`：与 p3_spike 里已验证可用的写法同源，
    //   而且不再需要 setupapi（平台层每多一个依赖，每个插件 DLL 都得多带一份）。
    //   代价是拿不到 SetupDi 的"友好名"，型号改由 StorageDeviceProperty 的厂商 /
    //   产品串拼出（见 diskModelOf）
    QList<DiskTemperature> result;

    for (DWORD index = 0; index < 16; ++index) {
        const QString path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(index);

        // 只查询属性，不需要读写权限 → desiredAccess 传 0（零提权即可枚举）
        const HANDLE device = ::CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()),
                                            0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            0,
                                            nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            const DWORD openError = lastError();
            // 索引超出（没有这块盘）：这是"不存在"，不该在面板上占一行"不可用"
            if (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            DiskTemperature entry;
            entry.devicePath = path;
            entry.error = describeFailure(QStringLiteral("打开磁盘设备"), openError);
            if (isAccessDenied(openError)) {
                entry.error += QStringLiteral("；打开物理磁盘可能需要管理员权限");
            }
            result.append(entry);
            continue;
        }

        DiskTemperature entry;
        entry.devicePath = path;

        STORAGE_PROPERTY_QUERY query{};
        // ⚠ 必须用 SDK 的枚举 **StorageDeviceTemperatureProperty**，不要手抄数字：
        //   本机 SDK 里它是 52，而"凭记忆写成 22"这一版让 IOCTL 直接返回
        //   ERROR_INVALID_FUNCTION（于是磁盘温度被误判成"该驱动不支持"）。
        //   枚举值随 SDK 版本会变，只有枚举本身是可靠的（踩坑 #72）
        query.PropertyId = StorageDeviceTemperatureProperty;
        query.QueryType = PropertyStandardQuery;

        // 描述符是变长的（后面跟着若干传感器读数），512 字节足够装下几十个传感器
        BYTE outputBuffer[512] = {};
        DWORD returnedBytes = 0;
        const BOOL queried = ::DeviceIoControl(device,
                                               IOCTL_STORAGE_QUERY_PROPERTY,
                                               &query,
                                               sizeof(query),
                                               outputBuffer,
                                               sizeof(outputBuffer),
                                               &returnedBytes,
                                               nullptr);
        // 错误码必须取一次就存下来：后续任何调用都可能覆盖 GetLastError()
        const DWORD queryError = lastError();

        // 型号：同一个句柄上再查一次设备描述符即可（与温度查询互不干扰）
        entry.model = diskModelOf(device);
        ::CloseHandle(device);

        if (queried == FALSE) {
            entry.error = temperatureFailureText(QStringLiteral("查询磁盘温度"), queryError);
            result.append(entry);
            continue;
        }

        if (returnedBytes < offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo)) {
            entry.error = QStringLiteral("设备返回的温度数据结构不完整");
            result.append(entry);
            continue;
        }

        const auto *descriptor =
            reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR *>(outputBuffer);
        if (descriptor->InfoCount == 0) {
            entry.error = QStringLiteral("设备未提供温度传感器信息");
            result.append(entry);
            continue;
        }

        // 取第一个传感器的当前温度；-1 或越界值视为无效
        const SHORT celsius = descriptor->TemperatureInfo[0].Temperature;
        if (celsius > 0 && celsius < 200) {
            entry.celsius = static_cast<double>(celsius);
            entry.valid = true;
        } else {
            entry.error = QStringLiteral("设备未报告有效温度值");
        }
        result.append(entry);
    }

    return result;
}

// ============================================================================
//  其他
// ============================================================================

quint64 uptimeSeconds()
{
    return static_cast<quint64>(::GetTickCount64()) / 1000ULL;
}

BatteryInfo batteryInfo()
{
    BatteryInfo info;

    SYSTEM_POWER_STATUS status{};
    if (::GetSystemPowerStatus(&status) == FALSE) {
        return info;
    }

    // BatteryFlag 为 128 表示系统没有电池（台式机）
    info.present = (status.BatteryFlag & 128) == 0 && status.BatteryFlag != 255;
    info.onAcPower = (status.ACLineStatus == 1);

    if (status.BatteryLifePercent != 255) {
        info.percent = status.BatteryLifePercent;
    }
    if (status.BatteryLifeTime != static_cast<DWORD>(-1)) {
        info.secondsRemaining = static_cast<int>(status.BatteryLifeTime);
    }
    return info;
}

} // namespace WinEase::Win32
