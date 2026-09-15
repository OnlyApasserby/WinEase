#pragma once

// ============================================================================
//  device_group.h —— 安全与隐私组用例
//      P2-11 摄像头/麦克风使用提醒（security.device_alert）
//
//  这一项的"外在状态"很特殊：**系统隐私页面里的那份使用记录**。
//  它是本工程第一个**只读**系统的功能（`ConsentStore` 一个字节都不写），
//  所以自检的难点不是"改完有没有还原"，而是**怎么在不真的开摄像头的前提下，
//  让系统里出现一条"正在使用"的记录**。
//
//  做法：在 `ConsentStore\<设备>\NonPackaged` 下造一个**自检专用条目**
//  （键名是编码后的 exe 路径），写完读回确认、用完立刻删除并断言不留残留。
//  ⚠ 这是写用户真实注册表（HKCU）：系统「隐私」页面会短暂看到这个假条目。
//    夹具只写 `LastUsedTimeStart` / `LastUsedTimeStop` 两个时间戳，
//    **不写 Value**（不碰任何权限语义），用完删得干干净净。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 运行安全与隐私组（P2-11）全部用例；返回本组新增的失败项数
int runDeviceGroupTests(Reporter &reporter,
                        WinEase::PluginManager &manager,
                        StubServices &services);

} // namespace FeatureSmoke
