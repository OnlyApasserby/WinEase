#pragma once

// ============================================================================
//  LanTransferEngine.h —— 局域网跨平台文件传输的"协议与页面"引擎（纯函数）
//
//  场景：WinEase 在本机开一个极小的 HTTP 服务，
//      · 手机（iOS / Android / 鸿蒙）用浏览器直接访问 → 双向传文件，**不需要装 App**
//      · 同一局域网里的另一台装了 WinEase 的电脑 → 出现在"发现设备"列表里，
//        可以直接互发（走同一套 HTTP 接口）
//
//  ★ 为什么把协议解析放在"纯函数"里：
//      HTTP 头解析、multipart 边界、路径穿越拦截这些**最容易出安全问题的部分**
//      必须能被自检用构造数据反复捶（例如 "GET /../../Windows/win.ini" 必须被挡），
//      而不是只能靠"起个服务器拿真手机试"。
//
//  ★ 本文件不依赖 QObject / 不碰任何 socket：只有字符串与字节。
//    真正的收发在插件里的 LanHttpServer / LanDiscovery（那部分要 QObject）。
// ============================================================================

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::LanTransfer {

/// 请求头解析结果
enum class ParseStatus {
    NeedMoreData, ///< 还没收到完整的头（继续读）
    Complete,     ///< 头完整，可以开始处理
    Invalid,      ///< 报文体不合法（400）
};

/// 解析出来的请求头（不包含 body）
struct HttpRequest {
    ParseStatus status = ParseStatus::NeedMoreData;
    QString error;                          ///< status == Invalid 时的原因（中文）
    QString method;                         ///< GET / POST / PUT / HEAD
    QString path;                           ///< 已 URL 解码的路径，形如 "/files/a.txt"
    QString rawTarget;                      ///< 原始 target（含 query）
    QMap<QString, QString> query;           ///< 已解码的查询参数
    QMap<QString, QString> headers;         ///< 头名一律小写
    qint64 contentLength = -1;              ///< 没有 Content-Length 时为 -1
    int headBytes = 0;                      ///< 请求头占用的字节数（body 从这里开始）
};

/// 解析请求头；`buffer` 是连接上收到的全部字节（可能只有一半）
ParseStatus parseRequestHead(const QByteArray &buffer, HttpRequest *request);

/// 组装一个响应（自动补 Content-Length / Connection: close）
QByteArray buildResponse(int statusCode,
                         const QString &reasonPhrase,
                         const QString &contentType,
                         const QByteArray &body,
                         const QMap<QString, QString> &extraHeaders = QMap<QString, QString>());

/// 组装"只发头不发体"的响应（HEAD 请求 / 302 跳转）
QByteArray buildHeadOnlyResponse(int statusCode,
                                 const QString &reasonPhrase,
                                 const QString &contentType,
                                 qint64 contentLength,
                                 const QMap<QString, QString> &extraHeaders =
                                     QMap<QString, QString>());

/// 响应体是 JSON 时的便捷封装
QByteArray buildJsonResponse(int statusCode,
                             const QString &reasonPhrase,
                             const QByteArray &jsonBody);

// ---------------------------------------------------------------------------
//  URL / MIME
// ---------------------------------------------------------------------------

QString urlDecode(const QString &text);
QString urlEncode(const QString &text);
QString mimeTypeForFile(const QString &fileName);
QString formatBytes(quint64 bytes);
QString formatSpeed(double bytesPerSecond);

// ---------------------------------------------------------------------------
//  文件共享（服务端视角）
// ---------------------------------------------------------------------------

/// 分享目录里的一个条目
struct ShareEntry {
    QString name;         ///< 仅名字（界面显示）
    QString relativePath; ///< 相对分享根的路径（正斜杠；用于拼 URL）
    quint64 size = 0;     ///< 字节；目录为 0
    bool isDirectory = false;
};

/// 列出分享目录（`relative` 为空表示根；非递归，目录排在前面）
QList<ShareEntry> listShareEntries(const QString &shareRoot, const QString &relative);

/// ★ 安全边界：把客户端请求的路径解析到分享目录内。
///   - 拒绝 `..` 穿越（"../../Windows/win.ini"）、绝对路径、盘符
///   - 拒绝 Windows 保留设备名（CON / NUL / COM1 …）
///   返回 false 时 `error` 是中文原因，调用方一律回 403，**绝不放行**
bool resolveSharePath(const QString &shareRoot,
                      const QString &requestPath,
                      QString *absolutePath,
                      QString *error);

/// 目标目录里已有同名文件时，给一个不冲突的名字："a.txt" → "a (1).txt"
QString uniqueFileName(const QString &directory, const QString &fileName);

/// 移动端友好的目录页（HTML）。`deviceName` 会显示在标题里，方便手机上确认"连对了机器"
QString buildListingPage(const QString &deviceName,
                         const QString &currentRelativePath,
                         const QList<ShareEntry> &entries,
                         const QStringList &banners);

/// 极简错误页（手机上也不至于看到一片空白）
QString buildErrorPage(const QString &title, const QString &detail);

// ---------------------------------------------------------------------------
//  局域网发现（PC ↔ PC）
// ---------------------------------------------------------------------------

/// 发现协议的魔数与端口（同一局域网内的 WinEase 靠它互相认出来）
constexpr quint16 kDiscoveryPort = 27182;
const QString kDiscoveryMagic = QStringLiteral("WINEASE-LAN/1");

/// 组装一条公告："WINEASE-LAN/1|主机名|端口|设备名"
QByteArray buildAnnounce(const QString &hostName, quint16 httpPort, const QString &deviceName);

/// 解析公告；不是本协议的包返回 false
bool parseAnnounce(const QByteArray &datagram,
                   QString *hostName,
                   quint16 *httpPort,
                   QString *deviceName);

/// 本机默认设备名（"WinEase@主机名"），手机页面与发现列表都用它
QString defaultDeviceName();

// ---------------------------------------------------------------------------
//  multipart/form-data（手机浏览器上传文件走这条）
// ---------------------------------------------------------------------------

/// 从 Content-Type 里取出 boundary；取不到返回空
QByteArray multipartBoundary(const QString &contentType);

/// 从一段 part 的头部里取出文件名（`filename="xxx"`），没有则返回空
QString multipartFileName(const QByteArray &partHead);

/// 从一段 part 的头部里取出字段名（`name="xxx"`）
QString multipartFieldName(const QByteArray &partHead);

} // namespace WinEase::FeaturePlugins::LanTransfer
