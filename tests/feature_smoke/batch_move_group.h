#pragma once

// ============================================================================
//  batch_move_group.h —— 批量移动文件（file.batch_move）的端到端自检
//
//  验的是"引擎与插件同一份源码"那条纪律（BatchMoveEngine 被两边一起编译）：
//      · 计划（dry-run）与执行（真落盘）用的是**同一份 Plan**；
//      · 正则非法 / 目录穿越 / 目标重名这些"危险输入"必须被拦下并给出中文原因；
//      · 真落盘时断言的是**磁盘上的真实状态**（文件真的在/真的不在），
//        而不是函数返回了 true；
//      · 撤销必须真能把文件搬回原位（撤销日志是文件，不是内存里的假数据）。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 运行批量移动用例；返回本组新增的失败项数
int runBatchMoveGroupTests(Reporter &reporter,
                           WinEase::PluginManager &manager,
                           StubServices &services);

} // namespace FeatureSmoke
