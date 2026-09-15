#pragma once

// ============================================================================
//  convert_group.h —— P3-03 批量格式转换（file.convert）用例
//
//  这一组有两条主线，缺一条就等于没测：
//    * **引擎级**：`plan()` / `apply()` 是插件与自检**共用的同一份源码** ——
//      验"计划与落盘是同一份计算"、"绝不原地覆盖"、"冲突三态"、
//      "取消只在开工前生效、磁盘上不留半张图"。这些都能做到**确定性**断言
//      （取消由进度回调在第 N 张之后触发，不靠掐时间）；
//    * **面板级**：走 build/bin/plugins 下真实的 file_convert.dll ——
//      拖入 / 剪贴板两条入口、模式切换、计划行、进度条、逐条结果、停用。
//
//  ⚠ 全程只用自检自己造的临时素材（临时目录里的图片与 GBK 文本），
//    不碰用户的任何文件。本组**不声明全局快捷键**（裁剪 C11），
//    所以面板级用例直接拿 createSettingsWidget() 出来的面板驱动。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 运行 P3-03 全部用例，返回本组新增的失败项数。
/// @param sandboxDir 自检的临时目录（素材都造在它下面）
int runConvertGroupTests(Reporter &reporter,
                         WinEase::PluginManager &manager,
                         const QString &sandboxDir);

} // namespace FeatureSmoke
