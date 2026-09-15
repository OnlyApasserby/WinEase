#pragma once

// ============================================================================
//  display_group.h —— 显示与媒体组用例
//      P2-08 亮度 / 色温 / 护眼模式（display.brightness）
//      P2-09 放大镜增强（display.magnifier）
//      P2-10 麦克风一键静音（media.mic_mute）
//
//  这三项的共同点：改的都是**用户看得见 / 听得见**的系统状态，因此外在状态
//  不靠"插件自己报了什么"，而是直接读系统：
//      * P2-08 → **真实 gamma ramp**（`GetDeviceGammaRamp` 读回逐通道比对）；
//      * P2-09 → **真实的原生放大镜窗口**（HWND 存在性 + `MagGetWindowSource` 源矩形 +
//                 `GetWindowRgn` 遮罩区域 + 跟随鼠标后的窗口位置）；
//      * P2-10 → **真实的麦克风静音状态**（`IAudioEndpointVolume::GetMute` 读回）。
//
//  也正因为动的是系统状态，这一组的纪律是"**结束必须还原**"：
//  麦克风原静音状态、gamma ramp 全部在用例内还原 —— 用户可能正开着会 / 看片。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 运行显示与媒体组全部用例；返回本组新增的失败项数
int runDisplayGroupTests(Reporter &reporter,
                         WinEase::PluginManager &manager,
                         StubServices &services);

} // namespace FeatureSmoke
