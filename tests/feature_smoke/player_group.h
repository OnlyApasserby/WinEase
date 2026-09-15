#pragma once

// ============================================================================
//  player_group.h —— P3-11 媒体控制面板 的端到端自检
//
//  验收标准原文是"控制 Chrome / Spotify / 系统播放器均可识别"。
//  自检机器上**通常一个播放器都没开着**，所以这一组和 P3-09 一个套路：
//  自检**自己造一份真实的媒体会话**（见 test_support.h 的 MediaSessionProbe：子进程
//  用桌面互操作接口 `ISystemMediaTransportControlsInterop::GetForWindow` 注册 SMTC
//  发布方），然后：
//
//      * 父进程走 `GlobalSystemMediaTransportControlsSessionManager` 把它**读出来**
//        （曲目/艺术家/专辑/时长/位置/封面都要对得上）；
//      * 真的下暂停/播放/跳转命令，并回读**播放器改过的**真实状态
//        （探针把收到的键与跳转请求逐条落盘 —— 这是"请求确实送到了"的硬证据）；
//      * 最后点面板上的按钮，验的是"用户点按钮"这条完整链路。
//
//  三条纪律各自有断言钉住：读不到写原因（纯函数层）、停用不改播放状态、两组控制分开。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

int runPlayerGroupTests(Reporter &reporter,
                        WinEase::PluginManager &manager,
                        StubServices &services);

} // namespace FeatureSmoke
