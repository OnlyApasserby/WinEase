#pragma once

// ============================================================================
//  DisplayControl.h —— 显示器亮度 / 对比度 / 色温控制
//
//  用途：屏幕亮度色温调节 + 护眼模式（P2-08，二者合并为一个插件）
//
//  两条独立通路，能力差异很大，必须分别探测：
//      1. **亮度/对比度** —— DDC/CI（VESA 标准，走显卡 I2C 总线）
//         支持：绝大多数外接显示器（DP/HDMI/DVI）
//         不支持：笔记本内置屏（面板由 eDP 直连，不经过 DDC/CI）、
//                 部分 USB-C/雷电扩展坞、部分虚拟机显示适配器
//         内置屏需要 WMI（WmiMonitorBrightness），属于另一条通路，后续按需补充
//      2. **色温** —— 显卡 gamma ramp（影响整屏输出，无需显示器支持）
//         代价：影响所有颜色（含截图之外的观感），且会被显示模式切换/锁屏重置
//
//  ⚠ gamma 使用约定（很重要）：
//      调节必须基于**功能启用时捕获的原始 ramp**，否则多次调节会层层叠加导致失真。
//      标准用法：
//          const auto baseline = captureGamma(monitorIndex);   // 启用时捕获一次
//          applyColorTemperature(monitorIndex, 4500, baseline); // 每次调节都基于 baseline
//          restoreGamma(baseline);                              // 停用时还原
// ============================================================================

#include <QList>
#include <QString>

namespace WinEase::Win32 {

// ============================================================================
//  亮度 / 对比度（DDC/CI）
// ============================================================================

struct DisplayValueCapability {
    bool supported = false;
    quint32 current = 0;
    quint32 minimum = 0;
    quint32 maximum = 100;
    QString error;

    /// 当前值换算为百分比（0~100）
    double percent() const
    {
        if (!supported || maximum <= minimum) {
            return 0.0;
        }
        return (static_cast<double>(current - minimum) * 100.0)
               / static_cast<double>(maximum - minimum);
    }
};

/// 查询亮度能力与当前值（monitorIndex 对应 monitors() 的顺序）
DisplayValueCapability queryBrightness(int monitorIndex);

/// 设置亮度（原始 VCP 值域，配合 queryBrightness 的 min/max 使用）
bool setBrightness(int monitorIndex, quint32 value, QString *errorOut = nullptr);

/// 按百分比设置亮度（0~100）
bool setBrightnessPercent(int monitorIndex, double percent, QString *errorOut = nullptr);

DisplayValueCapability queryContrast(int monitorIndex);
bool setContrast(int monitorIndex, quint32 value, QString *errorOut = nullptr);

// ============================================================================
//  色温（gamma ramp）
// ============================================================================

struct GammaSnapshot {
    bool valid = false;
    int monitorIndex = -1;
    /// 768 个 WORD：先 256 个红、再 256 个绿、最后 256 个蓝（与 Win32 的 WORD[3][256] 内存布局一致）
    QList<quint16> ramp;
    QString error;
};

/// 捕获当前 gamma 作为基准（功能启用时调用一次）
GammaSnapshot captureGamma(int monitorIndex);

/// 还原到快照（功能停用时调用，满足"不留副作用"要求）
bool restoreGamma(const GammaSnapshot &snapshot, QString *errorOut = nullptr);

/// 该显示器是否支持 gamma 调节（远程桌面、部分虚拟显示适配器不支持）
bool isGammaSupported(int monitorIndex);

/// 以 baseline 为基准，按通道增益应用 gamma（1.0 = 不改变）
bool applyGammaGains(int monitorIndex,
                     double redGain,
                     double greenGain,
                     double blueGain,
                     const GammaSnapshot &baseline,
                     QString *errorOut = nullptr);

/// 以 baseline 为基准应用色温（开尔文；6500 约为中性，越低越暖）
bool applyColorTemperature(int monitorIndex,
                           int kelvin,
                           const GammaSnapshot &baseline,
                           QString *errorOut = nullptr);

/// 由色温计算三通道增益（已归一化：最大通道增益恒为 1.0，避免整体变暗）
/// 供界面预览色块使用
void colorTemperatureGains(int kelvin, double *redGain, double *greenGain, double *blueGain);

/// 逐个显示器应用同一色温；返回成功的显示器数量
int applyColorTemperatureToAll(int kelvin, const QList<GammaSnapshot> &baselines);

// ============================================================================
//  内置屏亮度（WMI `root\WMI`）—— 笔记本面板的唯一通路
//
//  为什么必须单独一条通路：DDC/CI 走的是显卡的 I2C 总线，而笔记本内置屏由
//  eDP 直连显示控制器，**根本不在那条总线上**。本机实测就是这种情况：物理显示器
//  句柄能打开，但 `GetMonitorCapabilities` 直接报"I2C 传输失败"。
//  内置屏的亮度归 Windows 显示/电源栈管，对外暴露的就是 WMI 的两个类：
//      WmiMonitorBrightness         读：CurrentBrightness / Level
//      WmiMonitorBrightnessMethods  写：WmiSetBrightness(Timeout, Brightness)
//  —— 设置应用里的那个亮度滑块走的也是这条路。
// ============================================================================

struct InternalBrightnessState {
    bool supported = false;
    int current = 0;
    int maximum = 100;
    /// 命中面板的实例名（多面板机型上能看出是哪一块）
    QString target;
    QString error;
};

/// 读内置屏当前亮度（不改任何状态，随时可调）
InternalBrightnessState queryInternalBrightness();

/// 写内置屏亮度（原始值域，配合 queryInternalBrightness 的 maximum）
bool setInternalBrightness(int value, QString *errorOut = nullptr);

/// 写内置屏亮度（百分比 0~100）
bool setInternalBrightnessPercent(double percent, QString *errorOut = nullptr);

// ============================================================================
//  亮度后端的自动选择（"能力探测 + 逐级回退"的唯一决策点）
//
//  逐屏解析：内置屏走 WMI、外接屏走 DDC/CI，同一台机器上可能两种同时存在。
//  两条路的**失败原因都留在结果里**（即使最终有一个成功了），
//  因为"为什么没走那条路"正是用户在扩展坞/远程桌面上排障时需要看到的。
// ============================================================================

enum class BrightnessBackend {
    None = 0,    ///< 两条路都不通（远程桌面、虚拟显示适配器、显示器不支持）
    DdcCi,       ///< 外接屏：DDC/CI（VESA 标准，走显卡 I2C 总线）
    WmiInternal, ///< 内置屏：WMI `root\WMI`
};

struct BrightnessChannel {
    BrightnessBackend backend = BrightnessBackend::None;
    int monitorIndex = -1;
    bool supported = false;
    double percent = 0.0;
    /// 该通路的原始值域上限（DDC/CI 与 WMI 都能回读出来，换算百分比要用它）
    int maximum = 100;
    /// 用户可读的目标（显示器名 / 面板实例名）
    QString target;
    /// "走的哪条路"或"为什么不行" —— 直接显示给用户的那句话
    QString detail;
    /// 诊断：DDC/CI 那边失败的原因（即便最终走了 WMI 也留着）
    QString ddcError;
    /// 诊断：WMI 那边失败的原因
    QString wmiError;
};

/// 解析某个显示器该走哪条亮度通路（monitorIndex 对应 monitors() 的顺序）
BrightnessChannel resolveBrightnessChannel(int monitorIndex);

/// 把百分比写到由 channel 指定的那条通路上（不重新探测，避免每条通路各探一次）
bool setBrightnessPercentOn(const BrightnessChannel &channel,
                            double percent,
                            QString *errorOut = nullptr);

/// 只读回"当前亮度百分比"（不重新探测通路）—— 写硬件之后的回读确认用它。
/// 与麦克风静音同一条纪律：**不信自己刚写进去的参数**，真值永远问系统。
bool readBrightnessPercent(const BrightnessChannel &channel,
                           double *percentOut,
                           QString *errorOut = nullptr);

/// 通路的中文名（界面/日志用）
QString brightnessBackendText(BrightnessBackend backend);

/// 通路的中文名（配置持久化用的稳定键："ddc"/"wmi"/"none"）
QString brightnessBackendKey(BrightnessBackend backend);

} // namespace WinEase::Win32
