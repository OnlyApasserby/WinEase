#include "core/SingleInstanceGuard.h"

#include "core/Logging.h"

#include <QLocalSocket>

namespace WinEase {

namespace {
/// 通知通道的连接/读写超时（毫秒）
constexpr int kIpcTimeoutMs = 500;
} // namespace

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------

SingleInstanceGuard::SingleInstanceGuard(const QString &instanceKey, QObject *parent)
    : QObject(parent)
    , m_instanceKey(instanceKey)
    , m_sharedMemory(instanceKey)
{
    // 尝试创建 1 字节共享内存；成功即代表本进程拿到的"锁"
    if (m_sharedMemory.create(1, QSharedMemory::ReadWrite)) {
        m_primary = true;
        qCInfo(lcApp) << "单实例守卫：本进程为主实例";
        startNotificationServer();
        return;
    }

    if (m_sharedMemory.error() == QSharedMemory::AlreadyExists) {
        m_primary = false;
        qCInfo(lcApp) << "检测到已在运行的 WinEase 实例，将激活已有窗口后退出";
        return;
    }

    // 其它错误（权限不足 / 系统资源紧张）：保守按主实例继续运行，
    // 避免因为单实例机制本身出错导致用户完全无法启动程序。
    qCWarning(lcApp) << "共享内存创建失败:" << m_sharedMemory.errorString()
                     << "，将按主实例继续运行";
    m_primary = true;
    startNotificationServer();
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    stopNotificationServer();

    if (m_sharedMemory.isAttached()) {
        m_sharedMemory.detach();
    }
}

QString SingleInstanceGuard::notificationServerName() const
{
    // 与共享内存 key 区分开，避免命名冲突
    return m_instanceKey + QStringLiteral(".Notify");
}

// ---------------------------------------------------------------------------
//  通知通道（QLocalServer）
// ---------------------------------------------------------------------------

void SingleInstanceGuard::startNotificationServer()
{
    const QString serverName = notificationServerName();

    // 清理上一次异常退出可能残留的服务节点
    QLocalServer::removeServer(serverName);

    if (!m_server.listen(serverName)) {
        // 通知通道失败不影响单实例判定，仅失去"二次启动唤醒"能力
        qCWarning(lcApp) << "单实例通知通道监听失败:" << m_server.errorString();
        return;
    }

    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                const QString message = QString::fromUtf8(socket->readAll());
                m_lastMessage = message;
                qCInfo(lcApp) << "收到其它实例的激活请求:" << message;
                Q_EMIT activationRequested(message);
                socket->disconnectFromServer();
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

void SingleInstanceGuard::stopNotificationServer()
{
    if (!m_server.isListening()) {
        return;
    }
    m_server.close();
    QLocalServer::removeServer(notificationServerName());
}

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------

bool SingleInstanceGuard::isPrimaryInstance() const
{
    return m_primary;
}

bool SingleInstanceGuard::notifyExistingInstance(const QString &message)
{
    QLocalSocket socket;
    socket.connectToServer(notificationServerName());
    if (!socket.waitForConnected(kIpcTimeoutMs)) {
        qCWarning(lcApp) << "通知已有实例失败:" << socket.errorString();
        return false;
    }

    socket.write(message.toUtf8());
    socket.flush();
    socket.waitForBytesWritten(kIpcTimeoutMs);
    socket.disconnectFromServer();
    return true;
}

QString SingleInstanceGuard::lastMessage() const
{
    return m_lastMessage;
}

QString SingleInstanceGuard::statusText() const
{
    if (!m_primary) {
        return QStringLiteral("非主实例（已有 WinEase 在运行）");
    }
    if (!m_server.isListening()) {
        return QStringLiteral("主实例（激活通道不可用）");
    }
    return QStringLiteral("主实例（激活通道已就绪）");
}

} // namespace WinEase
