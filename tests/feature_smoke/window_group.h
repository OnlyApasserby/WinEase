#pragma once

// ============================================================================
//  feature_smoke / window_group.h —— P1-A 窗口组（P1-01 / P1-02 / P1-03 / P1-04）
//
//  所有用例都遵守同一条纪律：
//    1. 先做**前置条件断言**（光标下的窗口 / z 序关系），前置不成立就跳过并失败，
//       绝不在"目标不确定"的情况下动手改用户自己的窗口
//    2. 通过公开入口 driver（IFeaturePlugin::dispatchHotkey）触发功能，
//       再回到 Win32 侧读**真实状态**做断言 —— 而不是在测试里再实现一遍逻辑
//    3. 测完必须停用插件并断言系统状态已还原
// ============================================================================

#include "test_support.h"

#include "app/core/PluginManager.h"

namespace FeatureSmoke {

/// 返回失败项数（0 表示全部通过）
///
/// probe 是非 const 的：P1-04 的"置底维持"必须由**子进程**发起一次前台切换
/// （装钩子的进程自己产生的 z 序变化会被 WINEVENT_SKIPOWNPROCESS 过滤掉）。
int runWindowGroupTests(Reporter &reporter,
                        WinEase::PluginManager &manager,
                        StubServices &services,
                        ProbeWindow &probe);

} // namespace FeatureSmoke
