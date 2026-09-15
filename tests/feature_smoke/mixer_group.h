#pragma once

// ============================================================================
//  mixer_group.h —— P3-09 音量混合器 的端到端自检
//
//  验收标准原文是"对**3 个同时播放的应用**分别调节互不影响"。
//  所以这一组不能拿"系统声音"那一份会话当被测对象（那只能测到三分之一的行为），
//  而是自检**自己起子进程**去当播放器（见 test_support.h 的 AudioSessionProbe）：
//  两个不同进程、同一个进程名 → 系统里就有了两份**真实的**音频会话，
//  正好覆盖本功能最容易写错的那条路径 —— "一个应用多份会话"。
//
//  断言一律回 Win32 侧逐份读真实音量/静音，另加一条分界线：
//  **调会话音量绝不能动系统主音量**。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

int runMixerGroupTests(Reporter &reporter,
                       WinEase::PluginManager &manager,
                       StubServices &services);

} // namespace FeatureSmoke
