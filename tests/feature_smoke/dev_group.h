#pragma once

// ============================================================================
//  dev_group.h —— 开发运维组用例
//      P2-13 Hosts 快速编辑（dev.hosts_editor）
//
//  这一项的"外在状态"是**系统配置文件本身**（`System32\drivers\etc\hosts`），
//  而它恰好是"写错一行就让用户上不了网"的那种文件。所以自检的重心不是
//  "能不能保存"，而是：
//      * **校验不过时必须真的不写**（连提权请求都不发出去）；
//      * 写出去的字节**往返无损**（BOM / CRLF / 制表符 / 缩进都不许被格式化掉）；
//      * 备份还原送出去的**就是那份备份的字节**。
//
//  ⚠ 自检**绝不真的写 hosts**：提权请求打在记录型桩上（`RecordingElevationService`），
//     链路（读文件 → 校验 → 组装 base64 → 提交）全部真实跑，只在"真的落盘"之前截住。
//     读文件那半段是真的（普通权限可读），所以"打开面板看到的是真实内容"这条是真断言。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 运行开发运维组（P2-13）全部用例；返回本组新增的失败项数
int runDevGroupTests(Reporter &reporter,
                     WinEase::PluginManager &manager,
                     StubServices &services);

} // namespace FeatureSmoke
