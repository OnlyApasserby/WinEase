#pragma once

// ============================================================================
//  feature_smoke / file_group.h —— P2-A 文件组端到端用例
//      （P2-01 批量重命名 / P2-02 快速文件预览 / P2-03 重复文件查找 / P2-12 敏感文件粉碎）
//
//  与前几组的不同之处：
//    * 窗口组要操作**别的进程的窗口**，工具组多数是"纯数据进出"，
//      本组的外在状态是**磁盘本身**：文件名、文件内容、文件还在不在。
//    * 前四组都没有需要"点按钮"的界面（走的是快捷键 + 剪贴板）；
//      本组的面板是**工具窗口**，必须隔着 DLL 边界按 `objectName` 找控件来驱动
//      （插件是独立 DLL、不导出 C++ 符号，测试拿不到它的具体类）。
//
//  仍然是端到端：入口一律是 `PluginManager::dispatchHotkey()` 或面板上真实的按钮，
//  断言读的是磁盘 / 回收站 / 界面上真正画出来的内容。
// ============================================================================

#include "app/core/PluginManager.h"
#include "test_support.h"

namespace FeatureSmoke {

/// 返回失败项数（0 表示全部通过）
int runFileGroupTests(Reporter &reporter,
                      WinEase::PluginManager &manager,
                      StubServices &services,
                      ProbeWindow &probe);

} // namespace FeatureSmoke
