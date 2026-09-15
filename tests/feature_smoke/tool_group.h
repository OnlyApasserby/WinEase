#pragma once

// ============================================================================
//  feature_smoke / tool_group.h —— P1-B 工具组端到端用例
//                        （P1-06 取色 / P1-10 文本格式化 / P1-11 编码转换 / P1-12 端口占用）
//
//  与窗口组（window_group）的差异：
//    * 窗口组要操作**别的进程的窗口**，所以需要探针子进程与瞄准纪律；
//    * 工具组大部分是"纯数据进出"（剪贴板、文本转换、网络表），
//      因此断言的粒度可以做到**逐字节**（编码/哈希），
//      而且不需要动用户的窗口。
//
//  仍然属于"端到端"：驱动方式一律是 PluginManager::dispatchHotkey()，
//  断言读的是真实外部状态（剪贴板、GetExtendedTcpTable、屏幕像素）。
// ============================================================================

#include "app/core/PluginManager.h"
#include "test_support.h"

namespace FeatureSmoke {

/// 返回失败项数（0 表示全部通过）
int runToolGroupTests(Reporter &reporter,
                      WinEase::PluginManager &manager,
                      StubServices &services,
                      ProbeWindow &probe);

} // namespace FeatureSmoke
