#pragma once

// ============================================================================
//  LiteMonitorBridge.h —— 原生侧对 C++/CLI 桥接层的封装（P3-07）
//
//  桥接对象：`WinEaseLiteMonitorBridge.dll`（源码在 src/bridge/，
//  用 `/clr:netcore` 编译，内部直接调用 LibreHardwareMonitorLib —— 也就是
//  refrences/LiteMonitor 用的那个硬件库）。
//
//  **为什么必须用动态加载（LoadLibrary + GetProcAddress）**：
//      ① 主程序 / 插件是原生 C++，链接期不能依赖一个托管程序集；
//      ② 桥接层的构建依赖 .NET SDK 与 ijwhost.lib，缺任一项都会跳过构建
//         —— 那时主程序必须照常启动，HUD 只是"少了温度这一类读数"；
//      ③ fail-closed：拿不到模块就如实报"桥接不可用"，绝不能假装读到 0°C。
//
//  **跨 ABI 只传 POD 与 wchar_t***（桥接层头文件里写死了三条纪律）。
//  本层负责把它们翻译成 QString / QList，插件只见 Qt 类型。
//
//  ⚠ 传感器类型数值必须与 LibreHardwareMonitorLib 的 `SensorType` 枚举一致
//    （桥接层是原样透传的），改这里等于改两边。
// ============================================================================

#include <QList>
#include <QString>

namespace WinEase::Win32 {

/// 传感器类型（与 LibreHardwareMonitorLib.SensorType 对齐）
enum class LiteSensorKind {
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

/// 一个传感器的一次读数
struct LiteSensor {
    QString hardware;    ///< 所属硬件名（"Intel Core Ultra 9 ..." / "NVIDIA ..." / 主板名）
    QString name;        ///< 传感器名（"CPU Package" / "GPU Core" / "CPU Fan"）
    QString identifier;  ///< LHM 的标识符（用于稳定选择，界面不展示）
    LiteSensorKind kind = LiteSensorKind::Temperature;
    bool hasValue = false; ///< false = 硬件库没给值（多数情况是**权限不够**）
    double value = 0.0;
};

/// 一次采样的结果
struct LiteHardwareSnapshot {
    bool available = false;   ///< 桥接层是否真的给出了读数
    QString error;            ///< available == false 时的原因（**必须非空**）
    QList<LiteSensor> sensors;
};

/// 桥接模块的文件名（只给日志/提示用，不含路径）
QString liteMonitorBridgeFileName();

/// 桥接模块是否可用（不建立会话就能问，用于"提前给原因"）
bool liteMonitorBridgeAvailable(QString *reason = nullptr);

/// 桥接层的运行时信息（".NET 8.0.28 / LibreHardwareMonitorLib 0.9.6.0"），
/// 用于 HUD 的"已知限制"与自检诊断；不可用时返回原因串。
QString liteMonitorRuntimeInfo();

/// 一个硬件采样会话。⚠ 建议整个插件只持有一个：
///    首次 sample() 会打开 LibreHardwareMonitor 的 Computer（几百毫秒到数秒），
///    反复 open/close 会让每次刷新都付出这个代价。
class LiteMonitorSession
{
public:
    LiteMonitorSession();
    ~LiteMonitorSession();

    LiteMonitorSession(const LiteMonitorSession &) = delete;
    LiteMonitorSession &operator=(const LiteMonitorSession &) = delete;

    /// 采样一次。available == false 时 error 里是中文原因（非空）。
    LiteHardwareSnapshot sample();

    /// 最后一次失败原因（空 = 还没失败过）。用于 tooltip 里解释"为什么没有温度"。
    QString lastError() const;

private:
    void *m_handle = nullptr;
    QString m_lastError;
    bool m_failed = false;
};

} // namespace WinEase::Win32
