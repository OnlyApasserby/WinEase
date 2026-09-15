#pragma once

// ============================================================================
//  VolumeSteps.h —— 滚轮格数与音量的换算（P2-05 滚轮增强共用）
//
//  为什么这点逻辑值得单独成文件（而不是写在插件里）：
//      它是本功能**唯一有算术的地方**，而且全是边界情况 ——
//        * 一格 = 多少百分比、四舍五入还是截断；
//        * 到 0% 再往下滚、到 100% 再往上滚，必须**停在边界**（不能回绕、不能过冲）；
//        * 高精度滚轮/触控板会送来 ±40 这类**比 WHEEL_DELTA 小的 delta**，
//          直接除以 120 会得到 0 → 手感变成"滚十下才动一下"，
//          必须把余量攒起来（`consumeNotches`）。
//      插件与端到端自检**共用同一份实现**，断言才有意义（架构约定：可复用逻辑放
//      plugins/common）。
//
//  ⚠ 纯头文件、纯函数、无状态：不碰任何设备，也不读写配置。
// ============================================================================

#include <QString>

#include <cmath>

namespace WinEase::FeaturePlugins::VolumeSteps {

/// 一个"滚轮格"的标准 delta（Win32 WHEEL_DELTA）
constexpr int kWheelDelta = 120;

/// 把本次 delta 累加进 accumulator，取出整数格数，余量留在 accumulator 里。
/// 例：连续三次 delta = 40 → 0 格、0 格、1 格（而不是三格 0）。
/// @return 整数格数（正数 = 向上/向右，负数 = 向下/向左）
inline int consumeNotches(int &accumulator, int delta)
{
    accumulator += delta;
    const int notches = accumulator / kWheelDelta; // C++ 整数除法向零取整，正负都对
    accumulator -= notches * kWheelDelta;
    return notches;
}

/// 收敛到 [0, 1]（同时防住 NaN —— NaN 传下去会让整个音量控件失灵）
inline float clampVolume(float volume)
{
    if (!(volume == volume)) { // NaN
        return 0.0f;
    }
    if (volume < 0.0f) {
        return 0.0f;
    }
    if (volume > 1.0f) {
        return 1.0f;
    }
    return volume;
}

/// 按格数调整音量并收敛到 [0, 1]。
/// @param current 当前音量（0.0 ~ 1.0）
/// @param notches 格数（正 = 调大）
/// @param stepPercent 每格多少个百分点（例如 5 → 5%）
inline float applyNotches(float current, int notches, float stepPercent)
{
    if (notches == 0 || stepPercent <= 0.0f) {
        return clampVolume(current);
    }
    const float next = current + static_cast<float>(notches) * stepPercent / 100.0f;
    return clampVolume(next);
}

/// 0.45 → 45（四舍五入，用在界面文案与断言里）
inline int percentOf(float volume)
{
    const float clamped = clampVolume(volume);
    return static_cast<int>(std::lround(clamped * 100.0f));
}

/// 0.45 → "45%"
inline QString percentText(float volume)
{
    return QStringLiteral("%1%").arg(percentOf(volume));
}

} // namespace WinEase::FeaturePlugins::VolumeSteps
