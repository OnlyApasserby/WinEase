#pragma once

// ============================================================================
//  HelperService.h —— 提权助手服务（P0-4，Win32 原生命名管道实现）
//
//  为什么不用 QLocalServer：提权进程创建的管道默认带 High 完整性标签（MIC），
//  普通权限（Medium）进程会被拒绝写入；QLocalServer（Qt 6.8）没有暴露
//  设置 DACL/完整性标签的入口。因此这里直接用 CreateNamedPipeW 创建管道，
//  显式指定安全描述符：
//      D:(A;;GRGW;;;WD)S:(ML;;NW;;;ME)
//      → DACL：Everyone 可读写；SACL：完整性标签 Medium（No-Write-Up）
//  客户端仍用 QLocalSocket 连接（对端无需任何改动）。
//
//  线程模型：
//      * serverLoop() 运行在专用线程：创建监听实例 → 等待连接（可取消）
//        → 客户端到达后开独立线程处理，并立刻创建下一个监听实例
//      * 每个连接线程串行读取 JSON 行请求并回写应答，直到对端断开
//      * 单实例由 FILE_FLAG_FIRST_PIPE_INSTANCE 保证（第二次创建必失败）
// ============================================================================

#include "HelperOps.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

#include <windows.h>

namespace WinEase::Helper {

class HelperService : public QObject
{
    Q_OBJECT

public:
    explicit HelperService(QObject *parent = nullptr);
    ~HelperService() override;

    /// 开始监听（异步：专用线程）。已有实例在运行时返回 false（启动方应直接退出）
    bool start(QString *errorOut);

private:
    void serverLoop();
    /// 创建一个监听实例；firstInstance 时带 FILE_FLAG_FIRST_PIPE_INSTANCE
    HANDLE createPipeInstance(bool firstInstance, QString *errorOut);
    /// 与客户端会话（运行在独立线程，直到对端断开/读超时/服务停止）
    void handleClient(HANDLE pipe);
    void processRequest(HANDLE pipe, quint32 callerPid, const QByteArray &line);

    std::thread m_acceptThread;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_quitRequested{false};
    /// 当前处于监听状态的管道实例（供停止时 CancelIoEx 解除阻塞）
    std::atomic<HANDLE> m_listener{nullptr};
    std::atomic_bool m_listenerReady{false};
    QString m_startError;

    OpDispatcher m_ops;
};

} // namespace WinEase::Helper
