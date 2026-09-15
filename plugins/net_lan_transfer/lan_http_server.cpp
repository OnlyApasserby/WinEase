// ============================================================================
//  lan_http_server.cpp —— 局域网文件传输的接收端
//
//  为什么不用现成框架：Qt 里能做 HTTP 服务的模块（Qt HttpServer）是附加模块，
//  本工程只依赖 Core/Gui/Widgets（多引一个模块，每个插件 DLL 都要跟着变重）。
//  我们需要的 HTTP 子集非常小（四个路由、不需要 chunked、不需要 keep-alive），
//  自己写反而更可控 —— 尤其是"路径必须落在分享目录内"这条安全线。
// ============================================================================

#include "lan_http_server.h"

#include "sdk/PluginServices.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUrl>

#include <utility>

namespace WinEase::FeaturePlugins {

using LanTransfer::HttpRequest;
using LanTransfer::ParseStatus;

namespace {

constexpr int kReadChunk = 64 * 1024;      ///< 每次读写的大小（下载节流粒度）
constexpr qint64 kMaxMultipartBytes = 64ll * 1024 * 1024 * 1024; ///< 单次上传上限（64GB 足够）

QString peerText(QTcpSocket *socket)
{
    if (socket == nullptr) {
        return QStringLiteral("?");
    }
    const QString address = socket->peerAddress().toString();
    const QString normalized =
        address.startsWith(QLatin1String("::ffff:")) ? address.mid(7) : address;
    return QStringLiteral("%1:%2").arg(normalized).arg(socket->peerPort());
}

/// 浏览器自己会发的"站点图标探测请求"（与用户操作无关）。
///
/// ⚠ 这些路径**不是**用户想访问的地址：`/favicon.ico` 是浏览器渲染任何页面之后
///   自动补发的（页面没声明图标时的约定回落路径），iOS/Safari 与部分国产浏览器
///   还会额外探测 `/apple-touch-icon*.png`。见踩坑 #93。
bool isIconProbePath(const QString &path)
{
    return path == QLatin1String("/favicon.ico")
        || path == QLatin1String("/apple-touch-icon.png")
        || path == QLatin1String("/apple-touch-icon-precomposed.png");
}

} // namespace

// ============================================================================
//  LanHttpServer
// ============================================================================

LanHttpServer::LanHttpServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &LanHttpServer::onNewConnection);
}

LanHttpServer::~LanHttpServer()
{
    stop();
}

bool LanHttpServer::start(quint16 port, QString *error)
{
    if (m_server.isListening()) {
        return true;
    }
    // 只听 AnyIPv4：监听 IPv6 通配在某些家用路由器上会遇到 IPv4 映射地址的怪问题，
    // 而"局域网传文件"用 IPv4 地址最省事（手机上输入的也是 IPv4）。
    if (!m_server.listen(QHostAddress::AnyIPv4, port)) {
        if (error != nullptr) {
            *error = QStringLiteral("端口 %1 无法监听：%2").arg(port).arg(m_server.errorString());
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    Q_EMIT logMessage(QStringLiteral("已在 %1 端口开始监听（分享目录：%2）")
                          .arg(m_server.serverPort())
                          .arg(QDir::toNativeSeparators(m_shareDirectory)));
    return true;
}

void LanHttpServer::stop()
{
    const QList<LanHttpConnection *> connections = m_connections;
    for (LanHttpConnection *connection : connections) {
        connection->abort();
    }
    m_connections.clear();
    if (m_server.isListening()) {
        m_server.close();
        Q_EMIT logMessage(QStringLiteral("已停止监听"));
    }
}

bool LanHttpServer::isRunning() const
{
    return m_server.isListening();
}

quint16 LanHttpServer::port() const
{
    return m_server.serverPort();
}

void LanHttpServer::setShareDirectory(const QString &directory)
{
    m_shareDirectory = directory;
}

QString LanHttpServer::shareDirectory() const
{
    return m_shareDirectory;
}

void LanHttpServer::setDeviceName(const QString &name)
{
    m_deviceName = name;
}

QString LanHttpServer::deviceName() const
{
    return m_deviceName;
}

int LanHttpServer::requestCount() const
{
    return m_requestCount;
}

void LanHttpServer::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        if (socket == nullptr) {
            break;
        }
        auto *connection = new LanHttpConnection(socket, this);
        m_connections.append(connection);
        connect(connection, &LanHttpConnection::closed, this,
                [this, connection] { forgetConnection(connection); });
    }
}

void LanHttpServer::countRequest()
{
    ++m_requestCount;
}

int LanHttpServer::activeConnections() const
{
    return m_connections.size();
}

void LanHttpServer::forgetConnection(LanHttpConnection *connection)
{
    m_connections.removeAll(connection);
    connection->deleteLater();
}

// ============================================================================
//  LanHttpConnection
// ============================================================================

LanHttpConnection::LanHttpConnection(QTcpSocket *socket, LanHttpServer *server)
    : QObject(server)
    , m_socket(socket)
    , m_server(server)
{
    socket->setParent(this);
    connect(socket, &QTcpSocket::readyRead, this, &LanHttpConnection::onReadyRead);
    connect(socket, &QTcpSocket::bytesWritten, this, &LanHttpConnection::onBytesWritten);
    connect(socket, &QTcpSocket::disconnected, this, &LanHttpConnection::onDisconnected);
}

void LanHttpConnection::abort()
{
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
    if (m_upload.isOpen()) {
        m_upload.close();
    }
    Q_EMIT closed();
}

void LanHttpConnection::onDisconnected()
{
    Q_EMIT closed();
}

void LanHttpConnection::sendResponse(const QByteArray &response)
{
    m_phase = Phase::Finished;
    if (m_socket == nullptr) {
        return;
    }
    m_socket->write(response);
    m_socket->disconnectFromHost();
}

void LanHttpConnection::replyError(int status, const QString &title, const QString &detail)
{
    if (m_server != nullptr) {
        // ⚠ 这里的地址是**对端**（手机）的地址与临时端口，不是本服务的访问地址。
        //   必须写成"来自 …的请求"，否则 `192.168.21.5:43480：没有这个地址` 会被
        //   读成"要访问的地址不存在"（用户视角的假故障，见踩坑 #93）。
        Q_EMIT m_server->logMessage(QStringLiteral("来自 %1 的请求（%2）：%3")
                                        .arg(peerText(m_socket), QString::number(status), title));
        Q_EMIT m_server->logMessage(detail);
    }
    const QString page = LanTransfer::buildErrorPage(title, detail);
    sendResponse(LanTransfer::buildResponse(status, QString(), QStringLiteral("text/html; charset=utf-8"),
                                           page.toUtf8()));
}

void LanHttpConnection::onReadyRead()
{
    if (m_socket == nullptr) {
        return;
    }
    m_buffer += m_socket->readAll();

    if (m_phase == Phase::Head) {
        handleRequest();
        return;
    }
    if (m_phase == Phase::RawUpload) {
        const QByteArray chunk =
            m_buffer.left(static_cast<int>(qMin<qint64>(m_buffer.size(), m_bodyRemaining)));
        m_buffer.remove(0, chunk.size());
        if (!chunk.isEmpty()) {
            m_upload.write(chunk);
            m_uploadBytes += chunk.size();
            m_bodyRemaining -= chunk.size();
            if (m_server != nullptr) {
                Q_EMIT m_server->transferProgress(m_uploadName, m_uploadBytes);
            }
        }
        if (m_bodyRemaining <= 0) {
            m_upload.close();
            if (m_server != nullptr) {
                Q_EMIT m_server->transferFinished(m_uploadName, true,
                                                 QStringLiteral("来自 %1").arg(peerText(m_socket)));
            }
            sendResponse(LanTransfer::buildJsonResponse(
                200, QString(), QByteArrayLiteral("{\"ok\":true}")));
        }
        return;
    }
    if (m_phase == Phase::MultipartUpload) {
        consumeMultipart();
    }
}

void LanHttpConnection::onBytesWritten(qint64)
{
    if (m_phase == Phase::Download) {
        pumpDownload();
    }
}

// ---------------------------------------------------------------------------
//  请求分发
// ---------------------------------------------------------------------------

void LanHttpConnection::handleRequest()
{
    HttpRequest request;
    const ParseStatus status = LanTransfer::parseRequestHead(m_buffer, &request);
    if (status == ParseStatus::NeedMoreData) {
        return;
    }
    if (status == ParseStatus::Invalid) {
        replyError(400, QStringLiteral("请求不合法"), request.error);
        return;
    }

    m_buffer.remove(0, request.headBytes);
    if (m_server != nullptr) {
        m_server->countRequest();
    }
    routeRequest(request);
}

void LanHttpConnection::routeRequest(const HttpRequest &request)
{
    const QString path = request.path;
    const bool isGet = (request.method == QLatin1String("GET"));
    const bool isHead = (request.method == QLatin1String("HEAD"));

    if ((isGet || isHead) && (path == QLatin1String("/") || path.isEmpty())) {
        servePage(request);
        return;
    }
    if ((isGet || isHead) && path == QLatin1String("/api/list")) {
        serveListing(request);
        return;
    }
    if ((isGet || isHead) && path.startsWith(QLatin1String("/files/"))) {
        serveDownload(request, isHead);
        return;
    }
    if (request.method == QLatin1String("POST") && path == QLatin1String("/upload")) {
        beginMultipartUpload(request);
        return;
    }
    if (request.method == QLatin1String("PUT") && path == QLatin1String("/api/put")) {
        beginRawUpload(request);
        return;
    }
    // ★ 浏览器的站点图标探测：回 204（"这里没有图标"）而**不是** 404 错误页。
    //   回 404 时手机上看不出问题，但 WinEase 面板的「传输记录」会多出一行
    //   "…：没有这个地址（GET /favicon.ico …）"，很容易被误读成"手机连不上"（踩坑 #93）。
    //   正常路径不该走到这里：页面已经声明了内联图标，多数浏览器根本不会发这个请求。
    if ((isGet || isHead) && isIconProbePath(path)) {
        sendResponse(LanTransfer::buildNoContentResponse());
        return;
    }

    replyError(404, QStringLiteral("没有这个地址"),
               QStringLiteral("%1 %2 不在本服务的路由表里。").arg(request.method, path));
}

void LanHttpConnection::servePage(const HttpRequest &request)
{
    const QString relative = request.query.value(QStringLiteral("path"));
    QString absolute;
    QString error;
    if (!LanTransfer::resolveSharePath(m_server->shareDirectory(), relative, &absolute, &error)) {
        replyError(403, QStringLiteral("拒绝访问"), error);
        return;
    }
    if (!QFileInfo(absolute).isDir()) {
        replyError(404, QStringLiteral("目录不存在"), absolute);
        return;
    }

    QStringList banners;
    if (m_server->shareDirectory().isEmpty() || !QFileInfo(m_server->shareDirectory()).isDir()) {
        banners.append(QStringLiteral("这台电脑还没有设置分享目录，请先在 WinEase 里选一个。"));
    }
    banners.append(QStringLiteral("同一局域网内直连，不经过任何服务器。传输中请勿关闭 WinEase。"));

    const QString page =
        LanTransfer::buildListingPage(m_server->deviceName(), relative,
                                      LanTransfer::listShareEntries(m_server->shareDirectory(),
                                                                    relative),
                                      banners);
    if (request.method == QLatin1String("HEAD")) {
        sendResponse(LanTransfer::buildHeadOnlyResponse(200, QString(),
                                                        QStringLiteral("text/html; charset=utf-8"),
                                                        page.toUtf8().size()));
        return;
    }
    sendResponse(LanTransfer::buildResponse(200, QString(),
                                            QStringLiteral("text/html; charset=utf-8"),
                                            page.toUtf8()));
}

void LanHttpConnection::serveListing(const HttpRequest &request)
{
    const QString relative = request.query.value(QStringLiteral("path"));
    QString absolute;
    QString error;
    if (!LanTransfer::resolveSharePath(m_server->shareDirectory(), relative, &absolute, &error)) {
        replyError(403, QStringLiteral("拒绝访问"), error);
        return;
    }

    QJsonArray entries;
    for (const LanTransfer::ShareEntry &entry :
         LanTransfer::listShareEntries(m_server->shareDirectory(), relative)) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), entry.name);
        object.insert(QStringLiteral("path"), entry.relativePath);
        object.insert(QStringLiteral("size"), static_cast<double>(entry.size));
        object.insert(QStringLiteral("directory"), entry.isDirectory);
        entries.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("device"), m_server->deviceName());
    root.insert(QStringLiteral("path"), relative);
    root.insert(QStringLiteral("mime"), QJsonValue::Null);
    root.insert(QStringLiteral("entries"), entries);

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    sendResponse(LanTransfer::buildJsonResponse(200, QString(), json));
}

void LanHttpConnection::serveDownload(const HttpRequest &request, bool headOnly)
{
    QString absolute;
    QString error;
    if (!LanTransfer::resolveSharePath(m_server->shareDirectory(), request.path, &absolute, &error)) {
        replyError(403, QStringLiteral("拒绝访问"), error);
        return;
    }
    const QFileInfo info(absolute);
    if (!info.exists() || info.isDir() || !info.isFile()) {
        replyError(404, QStringLiteral("文件不存在"), QStringLiteral("请求的文件不在分享目录里。"));
        return;
    }

    const QString downloadName = info.fileName();
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("Content-Disposition"),
                   QStringLiteral("attachment; filename*=UTF-8''%1")
                       .arg(QString::fromLatin1(QUrl::toPercentEncoding(downloadName))));
    headers.insert(QStringLiteral("Accept-Ranges"), QStringLiteral("none"));

    if (headOnly) {
        sendResponse(LanTransfer::buildHeadOnlyResponse(
            200, QString(), LanTransfer::mimeTypeForFile(downloadName), info.size(), headers));
        return;
    }

    m_file.setFileName(absolute);
    if (!m_file.open(QIODevice::ReadOnly)) {
        replyError(500, QStringLiteral("读不出来"),
                   QStringLiteral("文件被占用或没有读取权限：%1").arg(info.fileName()));
        return;
    }
    m_fileRemaining = info.size();
    m_phase = Phase::Download;

    if (m_server != nullptr) {
        Q_EMIT m_server->transferStarted(downloadName, m_fileRemaining, false);
    }

    // 先把响应头写出去，随后按 64KB 一块一块喂（见 pumpDownload）
    if (m_socket != nullptr) {
        m_socket->write(LanTransfer::buildHeadOnlyResponse(
            200, QString(), LanTransfer::mimeTypeForFile(downloadName), info.size(), headers));
    }
    pumpDownload();
}

void LanHttpConnection::pumpDownload()
{
    if (m_phase != Phase::Download || m_socket == nullptr) {
        return;
    }
    // 只在没有积压时继续喂，避免大文件把内存顶起来（这是"流式"的关键）
    while (m_socket->bytesToWrite() < kReadChunk * 4 && m_fileRemaining > 0) {
        const QByteArray chunk = m_file.read(qMin<qint64>(kReadChunk, m_fileRemaining));
        if (chunk.isEmpty()) {
            break;
        }
        m_fileRemaining -= chunk.size();
        m_socket->write(chunk);
        if (m_server != nullptr) {
            Q_EMIT m_server->transferProgress(m_file.fileName(), m_file.size() - m_fileRemaining);
        }
    }
    if (m_fileRemaining <= 0) {
        m_file.close();
        m_phase = Phase::Finished;
        if (m_server != nullptr) {
            Q_EMIT m_server->transferFinished(QFileInfo(m_file.fileName()).fileName(), true,
                                             QStringLiteral("发给 %1").arg(peerText(m_socket)));
        }
        m_socket->disconnectFromHost();
    }
}

void LanHttpConnection::beginRawUpload(const HttpRequest &request)
{
    if (m_server->shareDirectory().isEmpty()) {
        replyError(500, QStringLiteral("没有分享目录"),
                   QStringLiteral("请先在 WinEase 里选一个接收目录。"));
        return;
    }
    if (request.contentLength <= 0) {
        replyError(400, QStringLiteral("缺少 Content-Length"),
                   QStringLiteral("裸 body 上传必须带 Content-Length。"));
        return;
    }
    if (request.contentLength > kMaxMultipartBytes) {
        replyError(413, QStringLiteral("文件太大"),
                   QStringLiteral("单次上传上限 64GB。"));
        return;
    }

    QString name = request.query.value(QStringLiteral("name")).trimmed();
    if (name.isEmpty()) {
        replyError(400, QStringLiteral("缺少 name 参数"),
                   QStringLiteral("PUT /api/put?name=文件名"));
        return;
    }
    name = QFileInfo(name).fileName();
    QString absolute;
    QString error;
    if (!LanTransfer::resolveSharePath(m_server->shareDirectory(), name, &absolute, &error)) {
        replyError(403, QStringLiteral("拒绝访问"), error);
        return;
    }

    const QString finalName =
        LanTransfer::uniqueFileName(QFileInfo(absolute).absolutePath(), QFileInfo(absolute).fileName());
    absolute = QFileInfo(absolute).absolutePath() + QLatin1Char('/') + finalName;

    m_upload.setFileName(absolute);
    if (!m_upload.open(QIODevice::WriteOnly)) {
        replyError(500, QStringLiteral("写不进去"),
                   QStringLiteral("无法在接收目录写文件（权限或磁盘满）。"));
        return;
    }
    m_uploadName = finalName;
    m_uploadBytes = 0;
    m_bodyRemaining = request.contentLength;
    m_phase = Phase::RawUpload;

    if (m_server != nullptr) {
        Q_EMIT m_server->logMessage(QStringLiteral("%1 开始发送 %2（%3）")
                                        .arg(peerText(m_socket), finalName,
                                             LanTransfer::formatBytes(
                                                 static_cast<quint64>(request.contentLength))));
        Q_EMIT m_server->transferStarted(finalName, request.contentLength, true);
    }
    // body 可能已经在缓冲区里（onReadyRead 里继续处理）
    onReadyRead();
}

void LanHttpConnection::beginMultipartUpload(const HttpRequest &request)
{
    if (m_server->shareDirectory().isEmpty() || !QFileInfo(m_server->shareDirectory()).isDir()) {
        replyError(500, QStringLiteral("没有分享目录"),
                   QStringLiteral("请先在 WinEase 里选一个接收目录。"));
        return;
    }
    m_boundary = LanTransfer::multipartBoundary(
        request.headers.value(QStringLiteral("content-type")));
    if (m_boundary.isEmpty()) {
        replyError(400, QStringLiteral("不是 multipart 表单"),
                   QStringLiteral("上传接口要求 enctype=\"multipart/form-data\"。"));
        return;
    }
    m_phase = Phase::MultipartUpload;
    m_inFilePart = false;
    m_seenFirstPart = false;
    m_uploadBytes = 0;
    m_declaredLength = (request.contentLength > 0) ? request.contentLength : 0;

    // 起始分隔符是 "--boundary"，QByteArray::indexOf 需要完整的前缀比较
    consumeMultipart();
}

void LanHttpConnection::consumeMultipart()
{
    const QByteArray delimiter = QByteArrayLiteral("\r\n--") + m_boundary;
    const QByteArray firstDelimiter = QByteArrayLiteral("--") + m_boundary;

    bool progressed = true;
    while (progressed) {
        progressed = false;

        if (!m_inFilePart) {
            // 找 part 头结束（空行）；第一段可能没有前置 \r\n
            int headerEnd = m_buffer.indexOf(QByteArrayLiteral("\r\n\r\n"));
            if (headerEnd < 0 && !m_seenFirstPart) {
                headerEnd = m_buffer.indexOf(QByteArrayLiteral("\n\n"));
            }
            if (headerEnd < 0) {
                // 结束分隔符 "--boundary--" 后面紧跟 CRLF，没有 part 头 → 表单结束
                const int finish = m_buffer.indexOf(firstDelimiter + QByteArrayLiteral("--"));
                if (finish >= 0) {
                    m_upload.close();
                    m_phase = Phase::Finished;
                    if (m_server != nullptr && !m_uploadName.isEmpty()) {
                        Q_EMIT m_server->transferFinished(m_uploadName, true,
                                                         QStringLiteral("来自 %1")
                                                             .arg(peerText(m_socket)));
                    }
                    sendResponse(LanTransfer::buildResponse(
                        200, QString(), QStringLiteral("text/html; charset=utf-8"),
                        QStringLiteral("<!DOCTYPE html><html lang=\"zh-CN\"><head>"
                                       "<meta charset=\"utf-8\"><meta name=\"viewport\" "
                                       "content=\"width=device-width,initial-scale=1\"></head>"
                                       "<body style=\"font-family:sans-serif;background:#14161a;"
                                       "color:#e8eaed;padding:20px\"><h2>上传完成</h2>"
                                       "<p>文件已经存到电脑上的分享目录里。</p>"
                                       "<p><a style=\"color:#8ab4f8\" href=\"/\">返回文件列表</a>"
                                       "</p></body></html>")
                            .toUtf8()));
                    return;
                }
                if (m_buffer.size() > 1024 * 1024) {
                    // 头部都不该有这么长：直接断开，避免无限攒
                    replyError(400, QStringLiteral("表单异常"),
                               QStringLiteral("multipart 头部过大。"));
                }
                return;
            }

            const QByteArray partHead = m_buffer.left(headerEnd);
            m_buffer.remove(0, headerEnd + 4);
            m_seenFirstPart = true;

            const QString fileName = LanTransfer::multipartFileName(partHead);
            if (fileName.isEmpty()) {
                // 非文件字段（例如空字段）：把它的 body 丢掉，继续找下一段
                m_uploadName.clear();
                continue;
            }
            if (m_server->shareDirectory().isEmpty()) {
                replyError(500, QStringLiteral("没有分享目录"),
                           QStringLiteral("请先在 WinEase 里选一个接收目录。"));
                return;
            }
            QString absolute;
            QString error;
            if (!LanTransfer::resolveSharePath(m_server->shareDirectory(), fileName, &absolute,
                                              &error)) {
                replyError(403, QStringLiteral("拒绝访问"), error);
                return;
            }
            const QString finalName = LanTransfer::uniqueFileName(
                QFileInfo(absolute).absolutePath(), QFileInfo(absolute).fileName());
            absolute = QFileInfo(absolute).absolutePath() + QLatin1Char('/') + finalName;

            m_upload.setFileName(absolute);
            if (!m_upload.open(QIODevice::WriteOnly)) {
                replyError(500, QStringLiteral("写不进去"),
                           QStringLiteral("无法在接收目录写文件（权限或磁盘满）。"));
                return;
            }
            m_uploadName = finalName;
            m_uploadBytes = 0;
            m_inFilePart = true;
            if (m_server != nullptr) {
                Q_EMIT m_server->transferStarted(finalName, m_declaredLength, true);
                Q_EMIT m_server->logMessage(
                    QStringLiteral("%1 开始上传 %2").arg(peerText(m_socket), finalName));
            }
            progressed = true;
            continue;
        }

        // 正在收文件内容：找分隔符
        const int position = m_buffer.indexOf(delimiter);
        if (position < 0) {
            // 没找到：把安全的一段写出去，最后 delimiter.size() 字节留着（可能被截断）
            const int keep = delimiter.size();
            if (m_buffer.size() > keep) {
                const QByteArray chunk = m_buffer.left(m_buffer.size() - keep);
                m_buffer.remove(0, chunk.size());
                m_upload.write(chunk);
                m_uploadBytes += chunk.size();
                if (m_server != nullptr) {
                    Q_EMIT m_server->transferProgress(m_uploadName, m_uploadBytes);
                }
            }
            return;
        }

        // 找到分隔符：它前面的内容（去掉前导 \r\n）属于文件
        QByteArray content = m_buffer.left(position);
        m_buffer.remove(0, position + delimiter.size());
        m_upload.write(content);
        m_uploadBytes += content.size();
        m_upload.close();
        if (m_server != nullptr) {
            Q_EMIT m_server->transferFinished(m_uploadName, true,
                                             QStringLiteral("来自 %1").arg(peerText(m_socket)));
            Q_EMIT m_server->logMessage(QStringLiteral("已接收 %1（%2）")
                                            .arg(m_uploadName,
                                                 LanTransfer::formatBytes(
                                                     static_cast<quint64>(m_uploadBytes))));
        }
        m_inFilePart = false;
        progressed = true;
    }
}

} // namespace WinEase::FeaturePlugins
