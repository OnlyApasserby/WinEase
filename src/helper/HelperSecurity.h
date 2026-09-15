#pragma once

// ============================================================================
//  HelperSecurity.h —— 提权助手的调用方校验（P0-4 的安全核心）
//
//  威胁模型：任意第三方进程都可以尝试连接命名管道。
//  因此 helper **不采信客户端自报的任何身份信息**（PID、路径、签名描述全都不可信），
//  只使用内核侧的管道元数据：
//
//    1. GetNamedPipeClientProcessId() 拿到**对端真实 PID** —— 这是命名管道的
//       内核元数据，客户端无法伪造（谎报没有意义，校验用的根本不是自报值）
//    2. OpenProcess + QueryFullProcessImageNameW 反查对端 exe 完整路径
//    3. 路径必须与 helper 自身**同目录**（同一安装位置，第三方进程天然不满足）
//    4. 若对端 exe 带 Authenticode 签名，则必须验证通过；
//       无签名允许通过（开发期产物未签名），但记录日志
//
//  任何一步失败都按"恶意调用"处理：拒绝并断开（fail-closed）。
//
//  ⚠ 管道本身的 DACL/完整性标签由 HelperService::createPipeInstance() 负责：
//    提权进程创建的管道默认带 High 完整性标签，普通权限（Medium）进程会被
//    MIC 拒绝写入 —— 必须显式放行（这是本次实测踩到的坑，见 ROADMAP #24）。
// ============================================================================

#include <QtGlobal>

#include <windows.h>

namespace WinEase::Helper {

enum class CallerCheckResult {
    Accepted,
    Rejected
};

/// 校验管道对端的调用方身份
/// @param pipe         服务端管道实例句柄（已连接）
/// @param callerPidOut 校验通过时输出对端 PID（供"禁止自杀"等规则使用）
/// @param errorOut     拒绝时输出中文原因（会原样回给调用方）
CallerCheckResult verifyCaller(HANDLE pipe,
                               quint32 *callerPidOut,
                               QString *errorOut);

} // namespace WinEase::Helper
