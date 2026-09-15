#pragma once

// ============================================================================
//  LanHttpServer —— 局域网文件传输的接收端（QTcpServer 上的极小 HTTP 服务）
//
//  它**不是**通用 Web 服务器，只做这几件事：
//      GET  /                 → 手机友好的目录页（含上传表单，页面自带内联图标）
//      GET  /files/<相对路径>  → 下载分享目录里的文件（流式）
//      GET  /api/list?path=   → 目录清单（JSON；给另一台 WinEase 用）
//      POST /upload           → 手机浏览器上传（multipart/form-data，流式落盘）
//      PUT  /api/put?name=    → 点对点发送（WinEase → WinEase，裸 body 流式落盘）
//      GET  /favicon.ico 等   → **204**（浏览器自动探测站点图标，不能回 404，见踩坑 #93）
//
//  ★ 三条工程纪律：
//      ① **流式**：上传/下载都不把整个文件读进内存（要能传几个 GB 的电影）；
//      ② **路径一律经 LanTransfer::resolveSharePath** 才落到文件系统上，
//         越界请求回 403 并记日志（手机浏览器地址栏也是不可信输入）；
//      ③ 每个连接独立状态机，出错只断自己那一条，不牵连服务器。
//
//  ⚠ 明文 HTTP、无鉴权：定位是"自家局域网内互相传个文件"。
//    插件默认**只有用户点了开启才监听**，停用即关端口（见 onDisable）。
//
//  ⚠ LanHttpConnection 之所以是**顶层类**而不是 LanHttpServer 的嵌套类：
//    moc 不支持嵌套类的元对象（AutoMoc 会直接报
//    "Meta object features not supported for nested classes"）。
// ============================================================================

#include "LanTransferEngine.h"

#include <QFile>
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

namespace WinEase::FeaturePlugins {

class LanHttpServer;

/// 一条连接的状态机（每个手机 / 每台对端电脑一个）
class LanHttpConnection : public QObject
{
    Q_OBJECT

public:
    LanHttpConnection(QTcpSocket *socket, LanHttpServer *server);
    void abort();

Q_SIGNALS:
    /// 连接结束（服务器据此把自己从列表里摘掉并回收对象）
    void closed();

private Q_SLOTS:
    void onReadyRead();
    void onBytesWritten(qint64 bytes);
    void onDisconnected();

private:
    enum class Phase {
        Head,             ///< 收请求头
        Download,         ///< 正在往对方发文件
        RawUpload,        ///< 正在收 PUT 的裸 body
        MultipartUpload,  ///< 正在收 multipart（手机表单）
        Finished          ///< 响应已写完，等着断开
    };

    void handleRequest();
    void routeRequest(const LanTransfer::HttpRequest &request);
    void servePage(const LanTransfer::HttpRequest &request);
    void serveListing(const LanTransfer::HttpRequest &request);
    void serveDownload(const LanTransfer::HttpRequest &request, bool headOnly);
    void beginRawUpload(const LanTransfer::HttpRequest &request);
    void beginMultipartUpload(const LanTransfer::HttpRequest &request);
    void consumeMultipart();

    void pumpDownload();
    void sendResponse(const QByteArray &response);
    void replyError(int status, const QString &title, const QString &detail);

    QTcpSocket *m_socket = nullptr;
    LanHttpServer *m_server = nullptr;

    Phase m_phase = Phase::Head;
    QByteArray m_buffer;
    qint64 m_bodyRemaining = 0;    ///< 裸 body 上传还剩多少字节
    qint64 m_declaredLength = 0;   ///< 请求声明的 Content-Length（multipart 进度用）

    // 下载侧
    QFile m_file;
    qint64 m_fileRemaining = 0;

    // 上传侧
    QFile m_upload;
    QString m_uploadName;
    qint64 m_uploadBytes = 0;
    bool m_inFilePart = false;
    bool m_seenFirstPart = false;
    QByteArray m_boundary;
};

class LanHttpServer : public QObject
{
    Q_OBJECT

public:
    explicit LanHttpServer(QObject *parent = nullptr);
    ~LanHttpServer() override;

    /// 开始监听（任意 IPv4 地址）。失败时 `error` 是中文原因。
    bool start(quint16 port, QString *error);
    void stop();
    bool isRunning() const;
    quint16 port() const;

    void setShareDirectory(const QString &directory);
    QString shareDirectory() const;

    void setDeviceName(const QString &name);
    QString deviceName() const;

    /// 已经接待过多少次请求（界面上的计数）
    int requestCount() const;

    /// 连接计数（自检用来确认连接被回收，不会随每次请求泄漏）
    int activeConnections() const;

Q_SIGNALS:
    void transferStarted(const QString &fileName, qint64 totalBytes, bool uploading);
    void transferProgress(const QString &fileName, qint64 bytesDone);
    void transferFinished(const QString &fileName, bool ok, const QString &detail);
    void logMessage(const QString &line);

private Q_SLOTS:
    void onNewConnection();

private:
    friend class LanHttpConnection;

    void countRequest();
    void forgetConnection(LanHttpConnection *connection);

    QTcpServer m_server;
    QString m_shareDirectory;
    QString m_deviceName;
    int m_requestCount = 0;
    QList<LanHttpConnection *> m_connections;
};

} // namespace WinEase::FeaturePlugins
