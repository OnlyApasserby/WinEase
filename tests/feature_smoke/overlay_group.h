#pragma once

// ============================================================================
//  feature_smoke / overlay_group.h —— P1-C 悬浮层组端到端用例
//                        （P1-07 屏幕标尺 / P1-08 焦点高亮）
//
//  这一组与前两组的最大不同：**被测产物是一个置顶透明窗口**，而且它"生效"的
//  证据在**合成后的屏幕像素**上。所以断言分成两层：
//    ① 窗口层：宿主的登记表（创建/回收）、窗口扩展样式（TOPMOST / TRANSPARENT /
//       NOACTIVATE / LAYERED）、几何（是否铺满显示器）、z 序（是否真的在上面）；
//    ② 像素层：用 `WinEase::Win32::colorAt()` 读**合成后**的屏幕像素（它会带
//       CAPTUREBLT 抓分层窗口），从而验证"聚光灯确实把光标周围挖亮了、
//       把别处压暗了"——这是"高亮位置与鼠标一致"这条验收唯一的硬证据。
//
//  ⚠ 悬浮层是**分层窗口**，命中测试逐像素按 alpha 判定（ROADMAP 踩坑 #5）：
//      所以"点击穿透可切换"这条只能验证到扩展样式层面；真去点屏幕属于
//      "自动化注入污染真实桌面"，本自检不做（手测项）。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 返回失败项数（0 表示全部通过）
int runOverlayGroupTests(Reporter &reporter,
                         WinEase::PluginManager &manager,
                         StubServices &services,
                         ProbeWindow &probe);

} // namespace FeatureSmoke
