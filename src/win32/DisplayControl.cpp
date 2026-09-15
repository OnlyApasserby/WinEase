#include "win32/DisplayControl.h"

#include "win32/ComApartment.h"
#include "win32/WindowUtils.h"
#include "win32/Win32Error.h"

#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include <windows.h>
#include <wbemidl.h>   // 内置屏亮度：WMI（IwBemLocator / IWbemServices）
#include <wrl/client.h> // ComPtr（COM 对象引用计数只能释放一次，交给它管）

// 注意：亮度/对比度这组"高层"DDC/CI 函数声明在 highlevelmonitorconfigurationapi.h 中，
// 而 lowlevelmonitorconfigurationapi.h 提供的是更底层的 VCP 原始读写（GetVCPFeatureAndVCPFeatureReply 等）。
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

namespace WinEase::Win32 {

namespace {

/// gamma ramp 的元素个数：3 个通道 × 256 级
constexpr int kGammaRampSize = 3 * 256;

/// 物理显示器句柄的 RAII 守卫（一个 HMONITOR 可能对应多个物理显示器）
class PhysicalMonitorGuard
{
public:
    PhysicalMonitorGuard() = default;

    ~PhysicalMonitorGuard() { release(); }

    PhysicalMonitorGuard(const PhysicalMonitorGuard &) = delete;
    PhysicalMonitorGuard &operator=(const PhysicalMonitorGuard &) = delete;

    bool acquire(int monitorIndex, QString *errorOut)
    {
        release();

        const QList<MonitorInfo> all = monitors();
        if (monitorIndex < 0 || monitorIndex >= all.size()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("显示器索引越界（共 %1 个显示器）").arg(all.size());
            }
            return false;
        }

        const HMONITOR native = static_cast<HMONITOR>(all.at(monitorIndex).nativeHandle);
        if (native == nullptr) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("无法获取显示器原生句柄");
            }
            return false;
        }

        DWORD count = 0;
        if (::GetNumberOfPhysicalMonitorsFromHMONITOR(native, &count) == FALSE || count == 0) {
            if (errorOut != nullptr) {
                *errorOut = describeFailure(QStringLiteral("枚举物理显示器"), lastError());
            }
            return false;
        }

        m_monitors.resize(count);
        if (::GetPhysicalMonitorsFromHMONITOR(native, count, m_monitors.data()) == FALSE) {
            if (errorOut != nullptr) {
                *errorOut = describeFailure(QStringLiteral("打开物理显示器句柄"), lastError());
            }
            m_monitors.clear();
            return false;
        }

        m_count = count;
        return true;
    }

    bool isValid() const { return m_count > 0 && !m_monitors.empty(); }

    /// 第一个物理显示器的句柄（多物理显示器场景取主显示器）
    HANDLE handle() const { return m_monitors.front().hPhysicalMonitor; }

    void release()
    {
        if (m_count > 0 && !m_monitors.empty()) {
            ::DestroyPhysicalMonitors(m_count, m_monitors.data());
        }
        m_monitors.clear();
        m_count = 0;
    }

private:
    std::vector<PHYSICAL_MONITOR> m_monitors;
    DWORD m_count = 0;
};

/// 为一个显示器创建 DC（gamma ramp 是逐显示器生效的）
HDC createMonitorDc(int monitorIndex)
{
    const QList<MonitorInfo> all = monitors();
    if (monitorIndex < 0 || monitorIndex >= all.size()) {
        return nullptr;
    }
    const QString deviceName = all.at(monitorIndex).deviceName;
    if (deviceName.isEmpty()) {
        return nullptr;
    }
    return ::CreateDCW(reinterpret_cast<LPCWSTR>(deviceName.utf16()), nullptr, nullptr, nullptr);
}

/// DC 的 RAII 守卫
class DcGuard
{
public:
    explicit DcGuard(HDC dc = nullptr) : m_dc(dc) {}
    ~DcGuard()
    {
        if (m_dc != nullptr) {
            ::DeleteDC(m_dc);
        }
    }
    DcGuard(const DcGuard &) = delete;
    DcGuard &operator=(const DcGuard &) = delete;

    bool isValid() const { return m_dc != nullptr; }
    HDC get() const { return m_dc; }

private:
    HDC m_dc = nullptr;
};

DisplayValueCapability queryMonitorValue(int monitorIndex, DWORD requiredCapability, bool brightness)
{
    DisplayValueCapability capability;

    PhysicalMonitorGuard monitor;
    if (!monitor.acquire(monitorIndex, &capability.error)) {
        return capability;
    }

    DWORD capabilities = 0;
    DWORD temperatureCapabilities = 0;
    if (::GetMonitorCapabilities(monitor.handle(), &capabilities, &temperatureCapabilities) == FALSE) {
        capability.error = describeFailure(QStringLiteral("查询显示器 DDC/CI 能力"), lastError());
        return capability;
    }

    if ((capabilities & requiredCapability) == 0U) {
        capability.error = brightness ? QStringLiteral("该显示器不支持 DDC/CI 亮度调节"
                                                       "（笔记本内置屏通常不支持，需走 WMI 通路）")
                                      : QStringLiteral("该显示器不支持 DDC/CI 对比度调节");
        return capability;
    }

    DWORD minimum = 0;
    DWORD current = 0;
    DWORD maximum = 0;
    const BOOL ok = brightness ? ::GetMonitorBrightness(monitor.handle(), &minimum, &current, &maximum)
                               : ::GetMonitorContrast(monitor.handle(), &minimum, &current, &maximum);
    if (ok == FALSE) {
        capability.error = describeFailure(brightness ? QStringLiteral("读取显示器亮度")
                                                      : QStringLiteral("读取显示器对比度"),
                                           lastError());
        return capability;
    }

    capability.supported = true;
    capability.minimum = minimum;
    capability.current = current;
    capability.maximum = maximum;
    return capability;
}

bool setMonitorValue(int monitorIndex, quint32 value, bool brightness, QString *errorOut)
{
    PhysicalMonitorGuard monitor;
    if (!monitor.acquire(monitorIndex, errorOut)) {
        return false;
    }

    const BOOL ok = brightness ? ::SetMonitorBrightness(monitor.handle(), value)
                               : ::SetMonitorContrast(monitor.handle(), value);
    if (ok == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(brightness ? QStringLiteral("设置显示器亮度")
                                                   : QStringLiteral("设置显示器对比度"),
                                        lastError());
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  色温 → 通道增益
// ---------------------------------------------------------------------------

/// 由色温计算理想黑体辐射的 RGB 值（Tanner Helland 近似，工程上广泛使用）
void blackbodyRgb(int kelvin, double *red, double *green, double *blue)
{
    const double t = std::clamp(static_cast<double>(kelvin), 1000.0, 40000.0) / 100.0;

    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
    } else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    }

    if (t >= 66.0) {
        b = 255.0;
    } else if (t <= 19.0) {
        b = 0.0;
    } else {
        b = 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    }

    if (red != nullptr) {
        *red = std::clamp(r / 255.0, 0.0, 1.0);
    }
    if (green != nullptr) {
        *green = std::clamp(g / 255.0, 0.0, 1.0);
    }
    if (blue != nullptr) {
        *blue = std::clamp(b / 255.0, 0.0, 1.0);
    }
}

} // namespace

// ============================================================================
//  亮度 / 对比度
// ============================================================================

DisplayValueCapability queryBrightness(int monitorIndex)
{
    return queryMonitorValue(monitorIndex, MC_CAPS_BRIGHTNESS, true);
}

bool setBrightness(int monitorIndex, quint32 value, QString *errorOut)
{
    return setMonitorValue(monitorIndex, value, true, errorOut);
}

bool setBrightnessPercent(int monitorIndex, double percent, QString *errorOut)
{
    const DisplayValueCapability capability = queryBrightness(monitorIndex);
    if (!capability.supported) {
        if (errorOut != nullptr) {
            *errorOut = capability.error;
        }
        return false;
    }

    const double clamped = std::clamp(percent, 0.0, 100.0);
    const double span = static_cast<double>(capability.maximum - capability.minimum);
    const auto value = static_cast<quint32>(std::lround(
        static_cast<double>(capability.minimum) + span * clamped / 100.0));

    return setBrightness(monitorIndex, value, errorOut);
}

DisplayValueCapability queryContrast(int monitorIndex)
{
    return queryMonitorValue(monitorIndex, MC_CAPS_CONTRAST, false);
}

bool setContrast(int monitorIndex, quint32 value, QString *errorOut)
{
    return setMonitorValue(monitorIndex, value, false, errorOut);
}

// ============================================================================
//  色温（gamma ramp）
// ============================================================================

GammaSnapshot captureGamma(int monitorIndex)
{
    GammaSnapshot snapshot;
    snapshot.monitorIndex = monitorIndex;

    DcGuard dc(createMonitorDc(monitorIndex));
    if (!dc.isValid()) {
        snapshot.error = QStringLiteral("无法为显示器 %1 创建设备上下文").arg(monitorIndex);
        return snapshot;
    }

    // Win32 的 ramp 布局是 WORD[3][256]，与 QList<quint16> 的 768 个元素内存布局一致
    std::array<WORD, kGammaRampSize> ramp{};
    if (::GetDeviceGammaRamp(dc.get(), ramp.data()) == FALSE) {
        snapshot.error = describeFailure(QStringLiteral("读取 gamma ramp"), lastError());
        return snapshot;
    }

    snapshot.ramp.reserve(kGammaRampSize);
    for (const WORD value : ramp) {
        snapshot.ramp.append(static_cast<quint16>(value));
    }
    snapshot.valid = true;
    return snapshot;
}

bool restoreGamma(const GammaSnapshot &snapshot, QString *errorOut)
{
    if (!snapshot.valid || snapshot.ramp.size() != kGammaRampSize) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("gamma 快照无效，无法还原");
        }
        return false;
    }

    DcGuard dc(createMonitorDc(snapshot.monitorIndex));
    if (!dc.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法为显示器 %1 创建设备上下文").arg(snapshot.monitorIndex);
        }
        return false;
    }

    std::array<WORD, kGammaRampSize> ramp{};
    for (int index = 0; index < kGammaRampSize; ++index) {
        ramp[static_cast<size_t>(index)] = snapshot.ramp.at(index);
    }

    if (::SetDeviceGammaRamp(dc.get(), ramp.data()) == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("还原 gamma ramp"), lastError());
        }
        return false;
    }
    return true;
}

bool isGammaSupported(int monitorIndex)
{
    DcGuard dc(createMonitorDc(monitorIndex));
    if (!dc.isValid()) {
        return false;
    }
    std::array<WORD, kGammaRampSize> ramp{};
    return ::GetDeviceGammaRamp(dc.get(), ramp.data()) != FALSE;
}

bool applyGammaGains(int monitorIndex,
                     double redGain,
                     double greenGain,
                     double blueGain,
                     const GammaSnapshot &baseline,
                     QString *errorOut)
{
    if (!baseline.valid || baseline.ramp.size() != kGammaRampSize) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("需要有效的原始 gamma 基准（请先调用 captureGamma）");
        }
        return false;
    }

    DcGuard dc(createMonitorDc(monitorIndex));
    if (!dc.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法为显示器 %1 创建设备上下文").arg(monitorIndex);
        }
        return false;
    }

    const std::array<double, 3> gains{
        std::clamp(redGain, 0.0, 1.0),
        std::clamp(greenGain, 0.0, 1.0),
        std::clamp(blueGain, 0.0, 1.0),
    };

    // 始终以 baseline 为基准重新计算，避免多次调节层层叠加造成失真
    std::array<WORD, kGammaRampSize> ramp{};
    for (int channel = 0; channel < 3; ++channel) {
        for (int level = 0; level < 256; ++level) {
            const int index = channel * 256 + level;
            const double base = static_cast<double>(baseline.ramp.at(index));
            const double scaled = std::clamp(base * gains[static_cast<size_t>(channel)],
                                             0.0,
                                             65535.0);
            ramp[static_cast<size_t>(index)] = static_cast<WORD>(std::lround(scaled));
        }
    }

    if (::SetDeviceGammaRamp(dc.get(), ramp.data()) == FALSE) {
        // ⚠ Windows 会**静默拒绝**幅度过大的 ramp：返回 FALSE 但 GetLastError() 是 0，
        //   照原样报出去就成了"设置 gamma ramp 失败：成功"，用户根本读不懂。
        //   实测本机能写 4000K，3200K 就被拒（同一台机器、同一份基准）。
        const DWORD code = lastError();
        if (errorOut != nullptr) {
            *errorOut = code == ERROR_SUCCESS
                            ? QStringLiteral("设置 gamma ramp 被系统拒绝"
                                             "（Windows 限制了应用程序可调节的幅度，"
                                             "这个色温超出了本机允许的范围）")
                            : describeFailure(QStringLiteral("设置 gamma ramp"), code);
        }
        return false;
    }
    return true;
}

void colorTemperatureGains(int kelvin, double *redGain, double *greenGain, double *blueGain)
{
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
    blackbodyRgb(kelvin, &r, &g, &b);

    // 归一化：让最大通道保持 1.0，避免调冷暖时整体亮度跟着变化
    const double maximum = std::max({ r, g, b });
    if (maximum > 0.0) {
        r /= maximum;
        g /= maximum;
        b /= maximum;
    }

    if (redGain != nullptr) {
        *redGain = r;
    }
    if (greenGain != nullptr) {
        *greenGain = g;
    }
    if (blueGain != nullptr) {
        *blueGain = b;
    }
}

bool applyColorTemperature(int monitorIndex,
                           int kelvin,
                           const GammaSnapshot &baseline,
                           QString *errorOut)
{
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    colorTemperatureGains(kelvin, &red, &green, &blue);
    return applyGammaGains(monitorIndex, red, green, blue, baseline, errorOut);
}

int applyColorTemperatureToAll(int kelvin, const QList<GammaSnapshot> &baselines)
{
    int applied = 0;
    for (const GammaSnapshot &baseline : baselines) {
        if (!baseline.valid) {
            continue;
        }
        if (applyColorTemperature(baseline.monitorIndex, kelvin, baseline)) {
            ++applied;
        }
    }
    return applied;
}

// ============================================================================
//  内置屏亮度（WMI root\WMI）
// ============================================================================

namespace {

constexpr wchar_t kWmiNamespace[] = L"ROOT\\WMI";
constexpr wchar_t kWmiBrightnessQuery[] = L"SELECT * FROM WmiMonitorBrightness";
constexpr wchar_t kWmiMethodsQuery[] = L"SELECT * FROM WmiMonitorBrightnessMethods";
constexpr wchar_t kWmiMethodsClass[] = L"WmiMonitorBrightnessMethods";
constexpr wchar_t kWmiSetBrightnessMethod[] = L"WmiSetBrightness";

/// BSTR 的 RAII 守卫：`SysAllocString` 出来的每一个 BSTR 都必须 `SysFreeString` 归还
class Bstr
{
public:
    explicit Bstr(const wchar_t *text) : m_value(::SysAllocString(text)) {}
    ~Bstr()
    {
        if (m_value != nullptr) {
            ::SysFreeString(m_value);
        }
    }

    Bstr(const Bstr &) = delete;
    Bstr &operator=(const Bstr &) = delete;

    BSTR get() const { return m_value; }

private:
    BSTR m_value = nullptr;
};

/// VARIANT 的 RAII 守卫：WMI 读出来的值里可能带 BSTR / SAFEARRAY，
/// 不 `VariantClear` 就是泄漏（COM 的规矩，见踩坑 #15）
class ScopedVariant
{
public:
    ScopedVariant() { ::VariantInit(&m_value); }
    ~ScopedVariant() { ::VariantClear(&m_value); }

    ScopedVariant(const ScopedVariant &) = delete;
    ScopedVariant &operator=(const ScopedVariant &) = delete;

    VARIANT *out() { return &m_value; }
    const VARIANT &get() const { return m_value; }

private:
    VARIANT m_value{};
};

/// WMI 的 HRESULT 有些不属于 Win32 错误域，`FormatMessage` 给不出人话 —— 这里补上常见的
QString wmiFailure(const QString &action, HRESULT hr)
{
    switch (static_cast<DWORD>(hr)) {
    case WBEM_E_ACCESS_DENIED:
        return QStringLiteral("%1 失败：WMI 拒绝访问（该机型的内置屏亮度需要管理员权限）").arg(action);
    case WBEM_E_NOT_FOUND:
        return QStringLiteral("%1 失败：这台机器没有提供该 WMI 实例（可能不是笔记本内置屏）").arg(action);
    case WBEM_E_INVALID_PARAMETER:
        return QStringLiteral("%1 失败：亮度值超出该面板支持的范围").arg(action);
    default:
        break;
    }
    return describeHresultFailure(action, hr);
}

/// 连接 ROOT\WMI 命名空间（COM 前置不满足时直接说清楚，不硬着头皮往下走）
bool connectWmi(Microsoft::WRL::ComPtr<IWbemServices> *services, QString *errorOut)
{
    if (!isThreadComReady()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("当前线程未初始化 COM，无法查询内置屏亮度");
        }
        return false;
    }

    Microsoft::WRL::ComPtr<IWbemLocator> locator;
    HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator,
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(locator.GetAddressOf()));
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("创建 WMI 定位器"), hr);
        }
        return false;
    }

    const Bstr nameSpace(kWmiNamespace);
    hr = locator->ConnectServer(nameSpace.get(),
                                nullptr,
                                nullptr,
                                nullptr,
                                0,
                                nullptr,
                                nullptr,
                                services->GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = wmiFailure(QStringLiteral("连接 WMI 命名空间 ROOT\\WMI"), hr);
        }
        return false;
    }

    // 给代理设置默认安全级别：不设的话部分系统上取实例会被拒。
    // 失败不致命（后续调用多半仍能成功），但保留错误信息供排障。
    ::CoSetProxyBlanket(services->Get(),
                        RPC_C_AUTHN_WINNT,
                        RPC_C_AUTHZ_NONE,
                        nullptr,
                        RPC_C_AUTHN_LEVEL_CALL,
                        RPC_C_IMP_LEVEL_IMPERSONATE,
                        nullptr,
                        EOAC_NONE);
    return true;
}

/// 遍历一个 WQL 查询的所有实例；visit 返回 false 表示提前收工。
/// 一个实例都没查到时按失败处理（"这台机型没有内置屏亮度"本身就是结论）
bool forEachInstance(IWbemServices *services,
                     const wchar_t *queryText,
                     const std::function<bool(IWbemClassObject *)> &visit,
                     QString *errorOut)
{
    const Bstr language(L"WQL");
    const Bstr query(queryText);

    Microsoft::WRL::ComPtr<IEnumWbemClassObject> enumerator;
    HRESULT hr = services->ExecQuery(language.get(),
                                     query.get(),
                                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                     nullptr,
                                     enumerator.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = wmiFailure(QStringLiteral("查询内置屏实例"), hr);
        }
        return false;
    }

    int seen = 0;
    for (;;) {
        IWbemClassObject *raw = nullptr;
        ULONG returned = 0;
        hr = enumerator->Next(WBEM_INFINITE, 1, &raw, &returned);
        if (FAILED(hr) || returned == 0 || raw == nullptr) {
            break;
        }
        // ComPtr 接管后禁手工 Release（踩坑 #15）
        Microsoft::WRL::ComPtr<IWbemClassObject> instance(raw);
        ++seen;
        if (!visit(instance.Get())) {
            return true;
        }
    }

    if (seen == 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("系统没有提供内置屏亮度实例"
                                       "（虚拟机显卡 / 远程桌面 / 纯外接屏机型均为正常现象）");
        }
        return false;
    }
    return true;
}

/// 读一个数值属性（WMI 里同一个概念可能是 UI1/UI2/I4/UI4，按实际类型取）
bool readNumberProperty(IWbemClassObject *object, const wchar_t *name, int *valueOut)
{
    ScopedVariant variant;
    if (FAILED(object->Get(name, 0, variant.out(), nullptr, nullptr))) {
        return false;
    }

    const VARIANT &value = variant.get();
    switch (value.vt) {
    case VT_UI1: *valueOut = static_cast<int>(value.bVal); return true;
    case VT_I1:  *valueOut = static_cast<int>(value.cVal); return true;
    case VT_UI2: *valueOut = static_cast<int>(value.uiVal); return true;
    case VT_I2:  *valueOut = static_cast<int>(value.iVal); return true;
    case VT_UI4: *valueOut = static_cast<int>(value.ulVal); return true;
    case VT_I4:  *valueOut = static_cast<int>(value.lVal); return true;
    default: return false;
    }
}

/// 读一个字符串属性（BSTR → QString）
QString readStringProperty(IWbemClassObject *object, const wchar_t *name)
{
    ScopedVariant variant;
    if (FAILED(object->Get(name, 0, variant.out(), nullptr, nullptr))) {
        return QString();
    }
    const VARIANT &value = variant.get();
    if (value.vt != VT_BSTR || value.bstrVal == nullptr) {
        return QString();
    }
    return QString::fromWCharArray(value.bstrVal);
}

/// `Level` 是"该面板支持的亮度档位"数组（形如 0,1,2,…,100），末位即最大亮度
void readBrightnessLevels(IWbemClassObject *object, int *maximumOut)
{
    ScopedVariant variant;
    if (FAILED(object->Get(L"Level", 0, variant.out(), nullptr, nullptr))) {
        return;
    }
    const VARIANT &value = variant.get();
    if ((value.vt & VT_ARRAY) == 0 || (value.vt & VT_UI1) == 0 || value.parray == nullptr) {
        return;
    }

    SAFEARRAY *array = value.parray;
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(::SafeArrayGetLBound(array, 1, &lower))
        || FAILED(::SafeArrayGetUBound(array, 1, &upper)) || upper < lower) {
        return;
    }

    BYTE last = 0;
    if (SUCCEEDED(::SafeArrayGetElement(array, &upper, &last))) {
        *maximumOut = static_cast<int>(last);
    }
}

/// 实例名形如 `DISPLAY\SDC420B\5&4702920&0&UID8448_0`，第 2 段是面板编码（SDC420B = 三星面板）
QString readInstanceLabel(IWbemClassObject *object)
{
    const QString instance = readStringProperty(object, L"InstanceName");
    if (instance.isEmpty()) {
        return QString();
    }
    const QStringList parts = instance.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    if (parts.size() >= 2) {
        return parts.at(1);
    }
    return parts.isEmpty() ? instance : parts.first();
}

/// 第一块内置面板的亮度控制实例路径（写方法要用它）
QString brightnessMethodsPath(IWbemServices *services, QString *errorOut)
{
    QString path;
    forEachInstance(services,
                    kWmiMethodsQuery,
                    [&path](IWbemClassObject *instance) {
                        path = readStringProperty(instance, L"__PATH");
                        return false; // 非笔记本机型可能有多块面板，取第一块
                    },
                    errorOut);
    return path;
}

} // namespace

InternalBrightnessState queryInternalBrightness()
{
    InternalBrightnessState state;

    Microsoft::WRL::ComPtr<IWbemServices> services;
    QString error;
    if (!connectWmi(&services, &error)) {
        state.error = error;
        return state;
    }

    bool found = false;
    const bool queried = forEachInstance(
        services.Get(),
        kWmiBrightnessQuery,
        [&state, &found](IWbemClassObject *instance) {
            int current = 0;
            if (!readNumberProperty(instance, L"CurrentBrightness", &current)) {
                return true; // 这个实例没有当前值，换下一个
            }
            int maximum = 100;
            readBrightnessLevels(instance, &maximum);

            state.supported = true;
            state.current = current;
            state.maximum = maximum > 0 ? maximum : 100;
            state.target = readInstanceLabel(instance);
            found = true;
            return false;
        },
        &error);

    if (!found) {
        state.supported = false;
        state.error = queried ? QStringLiteral("内置屏亮度实例里没有 CurrentBrightness 属性")
                              : error;
    }
    return state;
}

bool setInternalBrightness(int value, QString *errorOut)
{
    Microsoft::WRL::ComPtr<IWbemServices> services;
    QString error;
    if (!connectWmi(&services, &error)) {
        if (errorOut != nullptr) {
            *errorOut = error;
        }
        return false;
    }

    // ① 找到"能写亮度"的实例路径
    const QString path = brightnessMethodsPath(services.Get(), &error);
    if (path.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = error.isEmpty() ? QStringLiteral("找不到内置屏亮度控制实例") : error;
        }
        return false;
    }

    // ② 从类定义取 InParameters 模板，再派生一个可写的参数实例（WMI 写方法的固定套路）
    const Bstr className(kWmiMethodsClass);
    Microsoft::WRL::ComPtr<IWbemClassObject> classObject;
    HRESULT hr = services->GetObject(className.get(),
                                     0,
                                     nullptr,
                                     classObject.GetAddressOf(),
                                     nullptr);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = wmiFailure(QStringLiteral("读取亮度控制类定义"), hr);
        }
        return false;
    }

    // 方法签名要用 GetMethod 取（`Get(L"InParameters")` 只在直接对方法对象操作时才有效，
    // 对类定义调用取不到 —— 这里踩过一次）
    Microsoft::WRL::ComPtr<IWbemClassObject> parametersTemplate;
    Microsoft::WRL::ComPtr<IWbemClassObject> outSignature;
    hr = classObject->GetMethod(kWmiSetBrightnessMethod,
                                0,
                                parametersTemplate.GetAddressOf(),
                                outSignature.GetAddressOf());
    if (FAILED(hr) || parametersTemplate == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = FAILED(hr) ? wmiFailure(QStringLiteral("读取亮度方法签名"), hr)
                                   : QStringLiteral("WmiMonitorBrightnessMethods 没有"
                                                    " WmiSetBrightness 方法签名");
        }
        return false;
    }

    Microsoft::WRL::ComPtr<IWbemClassObject> parameters;
    hr = parametersTemplate->SpawnInstance(0, parameters.GetAddressOf());
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = wmiFailure(QStringLiteral("构造亮度参数"), hr);
        }
        return false;
    }

    ScopedVariant timeout;
    timeout.out()->vt = VT_I4;
    timeout.out()->lVal = 0; // 0 = 立即生效，不等渐变
    if (FAILED(parameters->Put(L"Timeout", 0, timeout.out(), CIM_UINT32))) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("亮度参数里的 Timeout 写入失败");
        }
        return false;
    }

    ScopedVariant brightness;
    brightness.out()->vt = VT_UI1;
    brightness.out()->bVal = static_cast<BYTE>(std::clamp(value, 0, 100));
    if (FAILED(parameters->Put(L"Brightness", 0, brightness.out(), CIM_UINT8))) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("亮度值写入参数失败");
        }
        return false;
    }

    // ③ 执行 WmiSetBrightness
    const Bstr objectPath(reinterpret_cast<const wchar_t *>(path.utf16()));
    const Bstr methodName(kWmiSetBrightnessMethod);
    Microsoft::WRL::ComPtr<IWbemClassObject> outParameters;
    hr = services->ExecMethod(objectPath.get(),
                              methodName.get(),
                              0,
                              nullptr,
                              parameters.Get(),
                              outParameters.GetAddressOf(),
                              nullptr);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = wmiFailure(QStringLiteral("设置内置屏亮度"), hr);
        }
        return false;
    }
    return true;
}

bool setInternalBrightnessPercent(double percent, QString *errorOut)
{
    const InternalBrightnessState state = queryInternalBrightness();
    if (!state.supported) {
        if (errorOut != nullptr) {
            *errorOut = state.error;
        }
        return false;
    }

    const double clamped = std::clamp(percent, 0.0, 100.0);
    const int value = static_cast<int>(std::lround(
        clamped * static_cast<double>(state.maximum) / 100.0));
    return setInternalBrightness(value, errorOut);
}

// ============================================================================
//  亮度后端的自动选择
// ============================================================================

namespace {

/// 显示器的人类可读名字（deviceName 形如 "\\.\DISPLAY1"，去掉前缀更像用户看到的东西）
QString monitorLabel(int monitorIndex)
{
    const QList<MonitorInfo> all = monitors();
    if (monitorIndex < 0 || monitorIndex >= all.size()) {
        return QStringLiteral("显示器 %1").arg(monitorIndex);
    }
    QString name = all.at(monitorIndex).deviceName;
    name.remove(QStringLiteral("\\\\.\\"));
    if (name.isEmpty()) {
        name = QStringLiteral("显示器 %1").arg(monitorIndex);
    }
    return all.at(monitorIndex).primary ? QStringLiteral("%1（主显示器）").arg(name) : name;
}

} // namespace

BrightnessChannel resolveBrightnessChannel(int monitorIndex)
{
    BrightnessChannel channel;
    channel.monitorIndex = monitorIndex;

    // ---- 第 1 条路：DDC/CI（外接屏，走显卡 I2C 总线）----
    const DisplayValueCapability ddc = queryBrightness(monitorIndex);
    if (ddc.supported) {
        channel.backend = BrightnessBackend::DdcCi;
        channel.supported = true;
        channel.percent = ddc.percent();
        channel.maximum = static_cast<int>(ddc.maximum);
        channel.target = monitorLabel(monitorIndex);
        channel.detail = QStringLiteral("DDC/CI（外接屏，走显卡 I2C 总线）");
        // DDC 通了就不再问 WMI：一次探测给一个结论，不让两条路互相打架
        return channel;
    }
    channel.ddcError = ddc.error;

    // ---- 第 2 条路：WMI（笔记本内置屏）----
    const InternalBrightnessState internal = queryInternalBrightness();
    channel.wmiError = internal.error;
    if (internal.supported) {
        channel.backend = BrightnessBackend::WmiInternal;
        channel.supported = true;
        channel.maximum = internal.maximum;
        channel.percent = internal.maximum > 0
                              ? static_cast<double>(internal.current) * 100.0
                                    / static_cast<double>(internal.maximum)
                              : 0.0;
        channel.target = internal.target.isEmpty() ? monitorLabel(monitorIndex) : internal.target;
        channel.detail = QStringLiteral("WMI 内置屏（笔记本面板，eDP 直连不进 DDC/CI 总线）");
        return channel;
    }

    channel.backend = BrightnessBackend::None;
    channel.detail = QStringLiteral("该显示器不支持亮度调节（DDC/CI 与内置屏 WMI 都不可用）");
    return channel;
}

bool setBrightnessPercentOn(const BrightnessChannel &channel, double percent, QString *errorOut)
{
    const double clamped = std::clamp(percent, 0.0, 100.0);

    switch (channel.backend) {
    case BrightnessBackend::DdcCi:
        return setBrightnessPercent(channel.monitorIndex, clamped, errorOut);
    case BrightnessBackend::WmiInternal: {
        const int maximum = channel.maximum > 0 ? channel.maximum : 100;
        const int value = static_cast<int>(std::lround(clamped * static_cast<double>(maximum) / 100.0));
        return setInternalBrightness(value, errorOut);
    }
    case BrightnessBackend::None:
    default:
        break;
    }

    if (errorOut != nullptr) {
        *errorOut = channel.detail.isEmpty() ? QStringLiteral("该显示器没有可用的亮度通路")
                                            : channel.detail;
    }
    return false;
}

bool readBrightnessPercent(const BrightnessChannel &channel, double *percentOut, QString *errorOut)
{
    if (percentOut == nullptr) {
        return false;
    }

    switch (channel.backend) {
    case BrightnessBackend::DdcCi: {
        const DisplayValueCapability current = queryBrightness(channel.monitorIndex);
        if (!current.supported) {
            if (errorOut != nullptr) {
                *errorOut = current.error;
            }
            return false;
        }
        *percentOut = current.percent();
        return true;
    }
    case BrightnessBackend::WmiInternal: {
        const InternalBrightnessState current = queryInternalBrightness();
        if (!current.supported) {
            if (errorOut != nullptr) {
                *errorOut = current.error;
            }
            return false;
        }
        *percentOut = current.maximum > 0
                          ? static_cast<double>(current.current) * 100.0
                                / static_cast<double>(current.maximum)
                          : 0.0;
        return true;
    }
    case BrightnessBackend::None:
    default:
        break;
    }

    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("该显示器没有可用的亮度通路，读不出当前亮度");
    }
    return false;
}

QString brightnessBackendText(BrightnessBackend backend)
{
    switch (backend) {
    case BrightnessBackend::DdcCi:
        return QStringLiteral("DDC/CI（外接屏）");
    case BrightnessBackend::WmiInternal:
        return QStringLiteral("WMI（内置屏）");
    case BrightnessBackend::None:
    default:
        return QStringLiteral("不可用");
    }
}

QString brightnessBackendKey(BrightnessBackend backend)
{
    switch (backend) {
    case BrightnessBackend::DdcCi:
        return QStringLiteral("ddc");
    case BrightnessBackend::WmiInternal:
        return QStringLiteral("wmi");
    case BrightnessBackend::None:
    default:
        return QStringLiteral("none");
    }
}

} // namespace WinEase::Win32
