#pragma once

// ============================================================================
//  input_group.h —— 输入组用例（P2-04 剪贴板历史 / P2-05 滚轮增强）
//
//  这一组盯着的是"用户正在输入的那一刻"，而这两件事的**外在状态**完全不一样：
//    * P2-04 的外在状态是**磁盘上的历史文件**（索引 JSON + 正文分片）与**面板控件**；
//    * P2-05 的外在状态是**真实系统音量**（IAudioEndpointVolume 读回）与
//      **钩子拦截计数**（HookService::stats().consumedEvents）。
//
//  两者都**不能**靠调用插件方法验证：插件是 DLL，不导出 C++ 符号（这是分层的有意结果）。
//  所以本组一律隔着"系统状态"与"控件对象名"断言。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 剪贴板历史的自检沙盒目录。
/// main() 用它预设插件配置、本组用它读磁盘 —— 两边共用同一个函数，避免写错路径
/// 导致"插件写 A、断言读 B"这种最难查的假失败。
QString clipboardHistoryDir(const QString &fixtureRoot);

/// @param fixtureRoot 自检沙盒（剪贴板历史目录钉在这里，不让它写进真实 %APPDATA%）
int runInputGroupTests(Reporter &reporter,
                       WinEase::PluginManager &manager,
                       StubServices &services,
                       ProbeWindow &probe,
                       const QString &fixtureRoot);

} // namespace FeatureSmoke
