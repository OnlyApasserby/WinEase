// ============================================================================
//  LiteMonitorBridge.cpp —— 原生侧对 C++/CLI 桥接层的封装（P3-07）
//
//  见 LiteMonitorBridge.h 的设计说明。这里只做三件事：
//      1. 动态加载桥接模块并解析纯 C ABI 的函数指针（模块缺失 → 给出原因）
//      2. 把 POD + wchar_t* 的快照翻译成 QString / QList
//      3. 任何失败都换算成"非空中文原因"，绝不把"读不到"翻译成 0
// ============================================================================

#include "win32/LiteMonitorBridge.h"

#include <QCoreApplication>
#include <QDir>

#include <windows.h>

namespace WinEase::Win32 {
namespace {

// ---- 桥接层的纯 C ABI（必须与 src/bridge/WinEaseLiteMonitorBridge.cpp 一致）----
struct WeLmSensor {
    const wchar_t *hardware;
    const wchar_t *name;
    const wchar_t *identifier;
    int kind;
    int hasValue;
    double value;
};

using FnOpen = void *(*)();
using FnClose = void (*)(void *);
using FnRefresh = int (*)(void *);
using FnCount = int (*)(void *);
using FnAt = int (*)(void *, int, WeLmSensor *);
using FnLastError = int (*)(void *, wchar_t *, int);
using FnRuntimeInfo = int (*)(wchar_t *, int);

struct BridgeModule {
    HMODULE dll = nullptr;
    FnOpen open = nullptr;
    FnClose close = nullptr;
    FnRefresh refresh = nullptr;
    FnCount count = nullptr;
    FnAt at = nullptr;
    FnLastError lastError = nullptr;
    FnRuntimeInfo runtimeInfo = nullptr;
    QString failureReason;
};

/// 模块句柄的解析结果。
///
/// ⚠ 这里是**刻意的进程级缓存**：CLR 一旦被加载进进程就无法卸载
/// （`FreeLibrary` 对 IJW 程序集无效，还会给以后的加载留下隐患），
/// 所以解析一次之后就不再释放。它是一份"只在首次调用时写入、之后只读"的
/// 常量状态，不是会在多个模块间分叉的业务状态 —— 这也是平台层禁用全局状态
/// 那条纪律的真正边界（见 ROADMAP P0-1）。
const BridgeModule &bridgeModule()
{
    static BridgeModule module = [] {
        BridgeModule m;
        const QString dir = QCoreApplication::applicationDirPath();
        const std::wstring path = QDir::toNativeSeparators(dir + QStringLiteral("/WinEaseLiteMonitorBridge.dll")).toStdWString();
        m.dll = LoadLibraryW(path.c_str());
        if (!m.dll) {
            m.failureReason = QStringLiteral("桥接模块 WinEaseLiteMonitorBridge.dll 未加载（错误码 %1）。"
                                             "构建它需要 .NET 8 SDK 与 MSVC 的 C++/CLI 支持；"
                                             "本机没有 .NET 8 运行时也会加载失败。")
                                  .arg(static_cast<qulonglong>(GetLastError()));
            return m;
        }
        m.open = reinterpret_cast<FnOpen>(GetProcAddress(m.dll, "we_lm_open"));
        m.close = reinterpret_cast<FnClose>(GetProcAddress(m.dll, "we_lm_close"));
        m.refresh = reinterpret_cast<FnRefresh>(GetProcAddress(m.dll, "we_lm_refresh"));
        m.count = reinterpret_cast<FnCount>(GetProcAddress(m.dll, "we_lm_sensor_count"));
        m.at = reinterpret_cast<FnAt>(GetProcAddress(m.dll, "we_lm_sensor_at"));
        m.lastError = reinterpret_cast<FnLastError>(GetProcAddress(m.dll, "we_lm_last_error"));
        m.runtimeInfo = reinterpret_cast<FnRuntimeInfo>(GetProcAddress(m.dll, "we_lm_runtime_info"));
        if (!m.open || !m.close || !m.refresh || !m.count || !m.at) {
            m.failureReason = QStringLiteral("桥接模块缺少导出函数（版本不匹配）。");
        }
        return m;
    }();
    return module;
}

QString fromWide(const wchar_t *text)
{
    return text ? QString::fromWCharArray(text) : QString();
}

QString readBridgeError(void *handle)
{
    const BridgeModule &m = bridgeModule();
    if (!m.lastError) return QString();
    wchar_t buffer[1024] = {0};
    m.lastError(handle, buffer, 1024);
    return QString::fromWCharArray(buffer);
}

} // namespace

QString liteMonitorBridgeFileName()
{
    return QStringLiteral("WinEaseLiteMonitorBridge.dll");
}

bool liteMonitorBridgeAvailable(QString *reason)
{
    const BridgeModule &m = bridgeModule();
    if (m.dll && m.open && m.refresh && m.count && m.at) return true;
    if (reason) *reason = m.failureReason;
    return false;
}

QString liteMonitorRuntimeInfo()
{
    const BridgeModule &m = bridgeModule();
    if (!m.dll) return m.failureReason;
    if (!m.runtimeInfo) return QStringLiteral("桥接模块未提供运行时信息。");
    wchar_t buffer[512] = {0};
    m.runtimeInfo(buffer, 512);
    return QString::fromWCharArray(buffer);
}

LiteMonitorSession::LiteMonitorSession() = default;

LiteMonitorSession::~LiteMonitorSession()
{
    const BridgeModule &m = bridgeModule();
    if (m_handle && m.close) m.close(m_handle);
    m_handle = nullptr;
}

LiteHardwareSnapshot LiteMonitorSession::sample()
{
    LiteHardwareSnapshot snapshot;
    const BridgeModule &m = bridgeModule();
    if (!m.dll || !m.open || !m.refresh) {
        snapshot.error = m.failureReason.isEmpty() ? QStringLiteral("桥接模块不可用。") : m.failureReason;
        m_lastError = snapshot.error;
        m_failed = true;
        return snapshot;
    }

    if (!m_handle) {
        m_handle = m.open();
        if (!m_handle) {
            snapshot.error = QStringLiteral("桥接层无法创建硬件会话（可能缺少 .NET 8 运行时）。");
            m_lastError = snapshot.error;
            m_failed = true;
            return snapshot;
        }
    }

    const int refreshed = m.refresh(m_handle);
    if (refreshed < 0) {
        const QString reason = readBridgeError(m_handle);
        snapshot.error = reason.isEmpty() ? QStringLiteral("硬件库刷新失败（未给出原因）。") : reason;
        m_lastError = snapshot.error;
        m_failed = true;
        return snapshot;
    }

    const int total = m.count ? m.count(m_handle) : 0;
    snapshot.sensors.reserve(total);
    for (int i = 0; i < total; ++i) {
        WeLmSensor raw;
        if (!m.at(m_handle, i, &raw)) continue;
        LiteSensor sensor;
        sensor.hardware = fromWide(raw.hardware);
        sensor.name = fromWide(raw.name);
        sensor.identifier = fromWide(raw.identifier);
        sensor.kind = static_cast<LiteSensorKind>(raw.kind);
        sensor.hasValue = raw.hasValue != 0;
        sensor.value = raw.value;
        snapshot.sensors.append(sensor);
    }
    snapshot.available = true;
    m_failed = false;
    m_lastError.clear();
    return snapshot;
}

QString LiteMonitorSession::lastError() const
{
    return m_lastError;
}

} // namespace WinEase::Win32
