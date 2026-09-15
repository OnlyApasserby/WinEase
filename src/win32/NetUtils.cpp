// winsock2.h 必须先于 windows.h（否则会先拉进旧版 winsock.h，两套定义冲突）；
// ws2tcpip.h / iphlpapi.h 必须在 winsock2.h 之后 —— 顺序颠倒时
// MIB_TCP6TABLE_OWNER_PID 这类 IPv6 结构体会"未声明"（它们依赖 ws2ipdef.h 里的类型）。
#include <winsock2.h>
#include <ws2tcpip.h>

#include "win32/NetUtils.h"

#include "win32/ProcessUtils.h"

#include <iphlpapi.h>

#include <QByteArray>
#include <QStringList>
#include <QtEndian>

#include <vector>

namespace WinEase::Win32 {

namespace {

// ---------------------------------------------------------------------------
//  地址格式化
//
//  这里**不用 QHostAddress**：它属于 QtNetwork，而平台层只依赖 Qt6::Core/Qt6::Gui
//  （多引一个 Qt 模块意味着主程序与每个插件都要多带一份 Qt6Network.dll）。
//  IPv6 的 RFC 5952 压缩也就几十行，自己写更划算。
// ---------------------------------------------------------------------------

/// 点分十进制（dwLocalAddr 是网络字节序）
QString formatIpv4(quint32 networkOrderAddress)
{
    const quint32 host = qFromBigEndian<quint32>(networkOrderAddress);
    return QStringLiteral("%1.%2.%3.%4")
        .arg((host >> 24) & 0xFFu)
        .arg((host >> 16) & 0xFFu)
        .arg((host >> 8) & 0xFFu)
        .arg(host & 0xFFu);
}

/// IPv6 字节数组 → 文本（含 RFC 5952 的 "::" 压缩）
QString formatIpv6(const unsigned char *bytes)
{
    quint16 groups[8] = { 0 };
    for (int i = 0; i < 8; ++i) {
        // 网络字节序：高字节在前
        groups[i] = static_cast<quint16>((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
    }

    // 找最长的零段（长度 ≥ 2 才压缩；并列时取靠前的那个，与 RFC 5952 一致）
    int bestStart = -1;
    int bestLength = 0;
    int runStart = -1;
    for (int i = 0; i < 9; ++i) {
        const bool isZero = (i < 8) && groups[i] == 0;
        if (isZero) {
            if (runStart < 0) {
                runStart = i;
            }
        } else if (runStart >= 0) {
            const int length = i - runStart;
            if (length > bestLength) {
                bestLength = length;
                bestStart = runStart;
            }
            runStart = -1;
        }
    }
    if (bestLength < 2) {
        bestStart = -1;
        bestLength = 0;
    }

    QStringList parts;
    for (int i = 0; i < 8; ++i) {
        if (bestStart >= 0 && i >= bestStart && i < bestStart + bestLength) {
            if (i == bestStart) {
                parts.append(QString()); // 空串代表 "::" 的中间段
            }
            continue;
        }
        parts.append(QString::number(groups[i], 16));
    }

    QString text = parts.join(QLatin1Char(':'));
    // 零段位于首/尾时，join 只留下一个冒号，需要补成 "::"
    if (bestStart == 0) {
        text.prepend(QLatin1Char(':'));
    }
    if (bestStart >= 0 && bestStart + bestLength == 8) {
        text.append(QLatin1Char(':'));
    }
    if (text.isEmpty()) {
        text = QStringLiteral("::");
    }
    return text;
}

// ---------------------------------------------------------------------------
//  GetExtended*Table 的两步调用：先问大小，再取数据
//
//  坑：两次调用之间表可能变长（新连接建立）→ 返回 ERROR_INSUFFICIENT_BUFFER，
//      所以必须允许重试，不能只试一次。
// ---------------------------------------------------------------------------

constexpr int kTableQueryAttempts = 3;

QByteArray queryTcpTable(ULONG addressFamily)
{
    for (int attempt = 0; attempt < kTableQueryAttempts; ++attempt) {
        DWORD size = 0;
        DWORD result = ::GetExtendedTcpTable(nullptr,
                                             &size,
                                             FALSE,
                                             addressFamily,
                                             TCP_TABLE_OWNER_PID_ALL,
                                             0);
        if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
            return QByteArray();
        }

        QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
        result = ::GetExtendedTcpTable(buffer.data(),
                                       &size,
                                       FALSE,
                                       addressFamily,
                                       TCP_TABLE_OWNER_PID_ALL,
                                       0);
        if (result == NO_ERROR) {
            buffer.resize(static_cast<int>(size));
            return buffer;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER) {
            return QByteArray();
        }
        // 表在两次调用之间变长了 → 重试
    }
    return QByteArray();
}

QByteArray queryUdpTable(ULONG addressFamily)
{
    for (int attempt = 0; attempt < kTableQueryAttempts; ++attempt) {
        DWORD size = 0;
        DWORD result = ::GetExtendedUdpTable(nullptr,
                                             &size,
                                             FALSE,
                                             addressFamily,
                                             UDP_TABLE_OWNER_PID,
                                             0);
        if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
            return QByteArray();
        }

        QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
        result = ::GetExtendedUdpTable(buffer.data(),
                                       &size,
                                       FALSE,
                                       addressFamily,
                                       UDP_TABLE_OWNER_PID,
                                       0);
        if (result == NO_ERROR) {
            buffer.resize(static_cast<int>(size));
            return buffer;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER) {
            return QByteArray();
        }
    }
    return QByteArray();
}

/// 端口字段是"网络字节序放在低 16 位"
quint16 portFromNetworkField(quint32 field)
{
    return qFromBigEndian<quint16>(static_cast<quint16>(field & 0xFFFFu));
}

void appendTcp4(QList<EndpointInfo> *out, const QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return;
    }
    const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.constData());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID &row = table->table[i];
        EndpointInfo info;
        info.protocol = NetProtocol::Tcp;
        info.ipv6 = false;
        info.localAddress = formatIpv4(row.dwLocalAddr);
        info.localPort = portFromNetworkField(row.dwLocalPort);
        info.remoteAddress = formatIpv4(row.dwRemoteAddr);
        info.remotePort = portFromNetworkField(row.dwRemotePort);
        info.state = row.dwState;
        info.pid = row.dwOwningPid;
        out->append(info);
    }
}

void appendTcp6(QList<EndpointInfo> *out, const QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return;
    }
    const auto *table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID *>(buffer.constData());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCP6ROW_OWNER_PID &row = table->table[i];
        EndpointInfo info;
        info.protocol = NetProtocol::Tcp;
        info.ipv6 = true;
        info.localAddress = formatIpv6(row.ucLocalAddr);
        info.localPort = portFromNetworkField(row.dwLocalPort);
        info.remoteAddress = formatIpv6(row.ucRemoteAddr);
        info.remotePort = portFromNetworkField(row.dwRemotePort);
        info.state = row.dwState;
        info.pid = row.dwOwningPid;
        out->append(info);
    }
}

void appendUdp4(QList<EndpointInfo> *out, const QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return;
    }
    const auto *table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID *>(buffer.constData());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_PID &row = table->table[i];
        EndpointInfo info;
        info.protocol = NetProtocol::Udp;
        info.ipv6 = false;
        info.localAddress = formatIpv4(row.dwLocalAddr);
        info.localPort = portFromNetworkField(row.dwLocalPort);
        info.pid = row.dwOwningPid;
        out->append(info);
    }
}

void appendUdp6(QList<EndpointInfo> *out, const QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return;
    }
    const auto *table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID *>(buffer.constData());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_UDP6ROW_OWNER_PID &row = table->table[i];
        EndpointInfo info;
        info.protocol = NetProtocol::Udp;
        info.ipv6 = true;
        info.localAddress = formatIpv6(row.ucLocalAddr);
        info.localPort = portFromNetworkField(row.dwLocalPort);
        info.pid = row.dwOwningPid;
        out->append(info);
    }
}

} // namespace

// ---------------------------------------------------------------------------
//  EndpointInfo
// ---------------------------------------------------------------------------

QString EndpointInfo::protocolText() const
{
    const QString base = (protocol == NetProtocol::Tcp) ? QStringLiteral("TCP") : QStringLiteral("UDP");
    return ipv6 ? base + QStringLiteral("v6") : base;
}

QString EndpointInfo::stateText() const
{
    return (protocol == NetProtocol::Tcp) ? tcpStateText(state) : QString();
}

QString EndpointInfo::localText() const
{
    return QStringLiteral("%1:%2").arg(localAddress).arg(localPort);
}

QString EndpointInfo::remoteText() const
{
    if (protocol != NetProtocol::Tcp || remotePort == 0) {
        return QString();
    }
    return QStringLiteral("%1:%2").arg(remoteAddress).arg(remotePort);
}

bool EndpointInfo::isListening() const
{
    // UDP 没有连接概念：表里出现即意味着某个套接字绑定了该端口
    if (protocol == NetProtocol::Udp) {
        return true;
    }
    return state == MIB_TCP_STATE_LISTEN;
}

QString EndpointInfo::describe() const
{
    if (protocol == NetProtocol::Udp) {
        return QStringLiteral("%1 %2（PID %3）").arg(protocolText(), localText()).arg(pid);
    }
    const QString stateLabel = stateText();
    if (stateLabel.isEmpty()) {
        return QStringLiteral("%1 %2（PID %3）").arg(protocolText(), localText()).arg(pid);
    }
    return QStringLiteral("%1 %2 %3（PID %4）").arg(protocolText(), localText(), stateLabel).arg(pid);
}

// ---------------------------------------------------------------------------
//  枚举
// ---------------------------------------------------------------------------

QList<EndpointInfo> tcpEndpoints()
{
    QList<EndpointInfo> result;
    appendTcp4(&result, queryTcpTable(AF_INET));
    appendTcp6(&result, queryTcpTable(AF_INET6));
    return result;
}

QList<EndpointInfo> udpEndpoints()
{
    QList<EndpointInfo> result;
    appendUdp4(&result, queryUdpTable(AF_INET));
    appendUdp6(&result, queryUdpTable(AF_INET6));
    return result;
}

QList<EndpointInfo> allEndpoints(bool includeTcp, bool includeUdp)
{
    QList<EndpointInfo> result;
    if (includeTcp) {
        result += tcpEndpoints();
    }
    if (includeUdp) {
        result += udpEndpoints();
    }
    return result;
}

QList<EndpointInfo> endpointsOnPort(quint16 port,
                                    bool includeTcp,
                                    bool includeUdp,
                                    bool listeningOnly)
{
    QList<EndpointInfo> result;
    if (port == 0) {
        return result;
    }
    const auto matches = [port, listeningOnly](const EndpointInfo &info) {
        if (info.localPort != port) {
            return false;
        }
        return !listeningOnly || info.isListening();
    };

    for (const EndpointInfo &info : allEndpoints(includeTcp, includeUdp)) {
        if (matches(info)) {
            result.append(info);
        }
    }
    return result;
}

QList<EndpointInfo> endpointsOfProcess(quint32 pid, bool includeTcp, bool includeUdp)
{
    QList<EndpointInfo> result;
    if (pid == 0) {
        return result;
    }
    for (const EndpointInfo &info : allEndpoints(includeTcp, includeUdp)) {
        if (info.pid == pid) {
            result.append(info);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
//  端口占用者
// ---------------------------------------------------------------------------

QString PortOccupant::describe() const
{
    QString who;
    if (!processName.isEmpty()) {
        who = QStringLiteral("%1（PID %2）").arg(processName).arg(pid);
    } else if (pid == 0) {
        who = QStringLiteral("系统/未知进程");
    } else {
        who = QStringLiteral("PID %1（无法读取进程名）").arg(pid);
    }
    return QStringLiteral("%1/%2 被 %3 占用%4")
        .arg(port)
        .arg(protocol, who)
        .arg(elevated ? QStringLiteral("（该进程以管理员权限运行）") : QString());
}

QList<PortOccupant> portOccupants(quint16 port, bool includeTcp, bool includeUdp)
{
    QList<PortOccupant> result;
    // 只看监听态：否则"本机连到别人的 443"会被误判成 443 被占用
    for (const EndpointInfo &info : endpointsOnPort(port, includeTcp, includeUdp, true)) {
        PortOccupant occupant;
        occupant.port = info.localPort;
        occupant.protocol = info.protocolText();
        occupant.address = info.localAddress;
        occupant.state = info.stateText();
        occupant.pid = info.pid;
        occupant.processName = processName(info.pid);
        occupant.processPath = processPath(info.pid);
        occupant.elevated = isProcessElevated(info.pid);
        result.append(occupant);
    }
    return result;
}

QString tcpStateText(quint32 stateCode)
{
    switch (stateCode) {
    case MIB_TCP_STATE_CLOSED:
        return QStringLiteral("已关闭");
    case MIB_TCP_STATE_LISTEN:
        return QStringLiteral("监听中");
    case MIB_TCP_STATE_SYN_SENT:
        return QStringLiteral("正在连接");
    case MIB_TCP_STATE_SYN_RCVD:
        return QStringLiteral("收到连接请求");
    case MIB_TCP_STATE_ESTAB:
        return QStringLiteral("已连接");
    case MIB_TCP_STATE_FIN_WAIT1:
    case MIB_TCP_STATE_FIN_WAIT2:
    case MIB_TCP_STATE_CLOSE_WAIT:
    case MIB_TCP_STATE_CLOSING:
    case MIB_TCP_STATE_LAST_ACK:
        return QStringLiteral("正在断开");
    case MIB_TCP_STATE_TIME_WAIT:
        return QStringLiteral("等待关闭");
    case MIB_TCP_STATE_DELETE_TCB:
        return QStringLiteral("已删除");
    default:
        return QStringLiteral("状态 %1").arg(stateCode);
    }
}

} // namespace WinEase::Win32
