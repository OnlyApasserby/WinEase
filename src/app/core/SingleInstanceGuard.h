#pragma once

// ============================================================================
//  SingleInstanceGuard.h —— 单实例守卫（QSharedMemory 实现）
//
//  为什么必须单实例：
//    本工具常驻托盘并抢占全局快捷键，重复启动会导致快捷键注册互相失败、
//    托盘图标重复、配置并发写入等问题。
//
//  实现原理：
//    1. QSharedMemory —— 单实例判定
//       以固定 key 创建 1 字节共享内存；create() 成功 == 本进程是唯一实例，
//       失败且 error()==AlreadyExists == 已有实例在运行。
//       Windows 下共享内存在最后一个句柄关闭时由系统回收，进程崩溃不会残留锁。
//    2. QLocalServer / QLocalSocket —— 唤醒通道
//       二次启动时把 "activate" 发给主实例，主实例收到后把窗口拉到前台。
//       （共享内存只能判重，不能传消息，所以需要一个轻量 IPC 通道）
//
//  注意：QSharedMemory 成员必须与进程同生命周期，析构时才释放"锁"。
// ============================================================================

#include <QLocalServer>
#include <QObject>
#include <QSharedMemory>
#include <QString>

namespace WinEase {

class SingleInstanceGuard : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstanceGuard(const QString &instanceKey, QObject *parent = nullptr);
    ~SingleInstanceGuard() override;

    SingleInstanceGuard(const SingleInstanceGuard &) = delete;
    SingleInstanceGuard &operator=(const SingleInstanceGuard &) = delete;

    /// 是否为本机唯一实例（true 时程序正常启动，false 时应通知已有实例后退出）
    bool isPrimaryInstance() const;

    /// 通知已运行的实例"激活主窗口"（仅在 isPrimaryInstance()==false 时调用）
    bool notifyExistingInstance(const QString &message = QStringLiteral("activate"));

    /// 主实例侧：最近一次收到的消息
    QString lastMessage() const;

    /// 状态描述，便于写入日志排障
    QString statusText() const;

Q_SIGNALS:
    /// 收到其它实例的激活请求（主实例侧发出）
    void activationRequested(const QString &message);

private:
    void startNotificationServer();
    void stopNotificationServer();
    QString notificationServerName() const;

    QString m_instanceKey;
    QSharedMemory m_sharedMemory; ///< 单实例"锁"（1 字节）
    QLocalServer m_server;        ///< 激活通知通道
    QString m_lastMessage;
    bool m_primary = false;
};

} // namespace WinEase
