#pragma once

// ============================================================================
//  lan_transfer_group.h —— 局域网文件传输（net.lan_transfer）的自检
//
//  分两层验：
//    ① **协议与安全（纯函数，与插件编同一份源码）**
//         HTTP 头解析、multipart 边界、URL 编解码、MIME、
//         以及最重要的 —— **目录穿越必须被挡**（"../../Windows/win.ini"、
//         "C:/Windows/x"、CON / NUL 这类保留设备名）。
//    ② **真实回环**：让插件的服务真的在 127.0.0.1 上监听，然后用 QTcpSocket
//         发真请求，断言**磁盘上的真实变化**：
//         GET /          → 手机页面里含上传表单；
//         GET /files/x   → 字节与源文件一致；
//         PUT /api/put   → 本机磁盘上真的多出那个文件；
//         GET /api/list  → JSON 里有目录清单；
//         GET /../..     → 403，且**磁盘上什么都没发生**。
// ============================================================================

#include "test_support.h"

namespace FeatureSmoke {

/// 运行局域网传输用例；返回本组新增的失败项数
int runLanTransferGroupTests(Reporter &reporter,
                             WinEase::PluginManager &manager,
                             StubServices &services);

} // namespace FeatureSmoke
