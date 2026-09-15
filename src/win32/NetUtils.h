#pragma once

// ============================================================================
//  NetUtils.h —— 网络端点枚举（端口占用查看）
//
//  用途（对应路线图 P1-12）：
//      * 端口占用查看：端口 → PID → 进程名/完整路径
//      * 后续"找出占用某个文件的进程"、"网络流量悬浮窗"等也会用到
//
//  实现说明：
//      使用 GetExtendedTcpTable / GetExtendedUdpTable（iphlpapi，随系统提供），
//      表类型选 *_OWNER_PID_ALL —— 一次拿到"连接 + 监听 + 归属 PID"，
//      避免再去查每个句柄反推进程。
//
//  权限说明：
//      **枚举是只读操作，普通权限即可**（能查到所有进程的端口，含系统进程）。
//      只有"结束占用进程"需要管理员权限，那一层走提权助手，不在本模块。
//
//  无状态：本模块只有纯函数，不持有任何缓存（见 WinEaseWin32 设计原则 2）。
// ============================================================================

#include <QList>
#include <QString>

namespace WinEase::Win32 {

/// 传输层协议
enum class NetProtocol {
    Tcp = 0,
    Udp
};

/// 一条网络端点记录（GetExtendedTcpTable / GetExtendedUdpTable 的一行）
struct EndpointInfo {
    NetProtocol protocol = NetProtocol::Tcp;
    bool ipv6 = false;

    QString localAddress;   ///< 如 "0.0.0.0"、"127.0.0.1"、"::"
    quint16 localPort = 0;

    QString remoteAddress;  ///< 仅 TCP 有效；UDP 恒为空
    quint16 remotePort = 0;

    /// MIB_TCP_STATE_*（UDP 恒为 0，表示"无状态概念"）
    quint32 state = 0;
    quint32 pid = 0;

    /// "TCP" / "TCPv6" / "UDP" / "UDPv6"
    QString protocolText() const;
    /// 连接状态中文描述（UDP 返回空串）
    QString stateText() const;
    /// "127.0.0.1:8080"
    QString localText() const;
    /// "1.2.3.4:443"；无远端时返回空串
    QString remoteText() const;
    /// 是否处于监听状态（TCP listen / UDP 视为监听）
    bool isListening() const;
    /// 一行摘要，用于列表展示
    QString describe() const;
};

/// 全部 IPv4 + IPv6 的 TCP 端点（含监听与已建立连接）
QList<EndpointInfo> tcpEndpoints();
/// 全部 IPv4 + IPv6 的 UDP 端点
QList<EndpointInfo> udpEndpoints();

/// 合并查询；tcp/udp 至少有一个为 true，否则返回空
QList<EndpointInfo> allEndpoints(bool includeTcp = true, bool includeUdp = true);

/// 本地端口等于 port 的全部端点
/// @param listeningOnly 只保留监听态（排掉"连到别人 8080"这种出站连接）
QList<EndpointInfo> endpointsOnPort(quint16 port,
                                    bool includeTcp = true,
                                    bool includeUdp = true,
                                    bool listeningOnly = false);

/// 本地端口等于 port 的全部端点（按 PID 归属）
QList<EndpointInfo> endpointsOfProcess(quint32 pid, bool includeTcp = true, bool includeUdp = true);

/// 端口占用者：端点 + 进程信息（聚合后便于直接展示/结束）
struct PortOccupant {
    quint16 port = 0;
    QString protocol;        ///< "TCP" / "UDP"（含 v6 后缀）
    QString address;         ///< 本地地址
    QString state;           ///< 状态中文（UDP 为空）
    quint32 pid = 0;
    QString processName;     ///< 如 "chrome.exe"；查不到时为空
    QString processPath;     ///< 完整路径；无权限读取时为空
    bool elevated = false;   ///< 进程是否以管理员权限运行

    /// 中文摘要："8080/TCP 被 chrome.exe（PID 1234）占用"
    QString describe() const;
};

/// 查询端口占用者；无占用返回空列表
QList<PortOccupant> portOccupants(quint16 port, bool includeTcp = true, bool includeUdp = true);

/// MIB_TCP_STATE_* → 中文；未知值返回 "状态 N"
QString tcpStateText(quint32 state);

} // namespace WinEase::Win32
