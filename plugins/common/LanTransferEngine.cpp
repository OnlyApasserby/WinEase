#include "LanTransferEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QSet>
#include <QUrl>

namespace WinEase::FeaturePlugins::LanTransfer {

namespace {

const QByteArray kCrLf = QByteArrayLiteral("\r\n");
const QByteArray kHeadEnd = QByteArrayLiteral("\r\n\r\n");

/// 响应原因短语（够用即可，不自建一张完整的表）
QString reasonFor(int statusCode)
{
    switch (statusCode) {
    case 200:
        return QStringLiteral("OK");
    case 204:
        return QStringLiteral("No Content");
    case 302:
        return QStringLiteral("Found");
    case 400:
        return QStringLiteral("Bad Request");
    case 403:
        return QStringLiteral("Forbidden");
    case 404:
        return QStringLiteral("Not Found");
    case 405:
        return QStringLiteral("Method Not Allowed");
    case 413:
        return QStringLiteral("Payload Too Large");
    case 500:
        return QStringLiteral("Internal Server Error");
    default:
        return QStringLiteral("OK");
    }
}

/// 头部一行是 "Name: value"，返回小写名与去空白的值
bool splitHeaderLine(const QByteArray &line, QString *name, QString *value)
{
    const int colon = line.indexOf(':');
    if (colon <= 0) {
        return false;
    }
    *name = QString::fromLatin1(line.left(colon)).trimmed().toLower();
    *value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
    return true;
}

/// 文件名是否命中 Windows 保留设备名（CON / NUL / LPT1 …）
bool isReservedDeviceName(const QString &name)
{
    static const QSet<QString> reserved = {
        QStringLiteral("con"),  QStringLiteral("prn"),  QStringLiteral("aux"),
        QStringLiteral("nul"),  QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
        QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
        QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9")};
    const QString stem = name.section(QLatin1Char('.'), 0, 0).toLower();
    return reserved.contains(stem);
}

QString htmlEscape(const QString &text)
{
    QString result = text;
    result.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    result.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    result.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    result.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    result.replace(QLatin1Char('\''), QLatin1String("&#39;"));
    return result;
}

/// 目录页里一个条目行的 URL（绝对路径 + URL 编码，手机上点一下就下载）
QString entryUrl(const QString &relativePath, bool asDirectory)
{
    QString path = QStringLiteral("/files/") + relativePath;
    if (asDirectory && !path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    return urlEncode(path);
}

/// 站点图标的 SVG 源（**全 ASCII、单引号属性**，见 siteIconDataUri 的说明）。
///
/// ⚠ 这里写成单引号 `'` 而不是双引号：它最终要塞进 HTML 属性的
///   `href="..."` 里，双引号会提前把属性闭合掉。
const char kSiteIconSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
    "<rect width='64' height='64' rx='14' fill='#8ab4f8'/>"
    "<text x='32' y='45' text-anchor='middle' font-family='sans-serif'"
    " font-size='34' font-weight='700' fill='#14161a'>W</text>"
    "</svg>";

} // namespace

// ============================================================================
//  HTTP
// ============================================================================

ParseStatus parseRequestHead(const QByteArray &buffer, HttpRequest *request)
{
    if (request == nullptr) {
        return ParseStatus::Invalid;
    }
    *request = HttpRequest();

    const int headEnd = buffer.indexOf(kHeadEnd);
    if (headEnd < 0) {
        // 头还没收完。加一条上限：正常请求头不可能超过 32KB，
        // 超了就是有人在灌垃圾（或者客户端坏了），直接判非法而不是无限攒内存。
        if (buffer.size() > 32 * 1024) {
            request->status = ParseStatus::Invalid;
            request->error = QStringLiteral("请求头过大（超过 32KB）");
            return ParseStatus::Invalid;
        }
        request->status = ParseStatus::NeedMoreData;
        return ParseStatus::NeedMoreData;
    }

    const QByteArray head = buffer.left(headEnd);
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) {
        request->status = ParseStatus::Invalid;
        request->error = QStringLiteral("请求行为空");
        return ParseStatus::Invalid;
    }

    // ---- 请求行 ----
    const QByteArray requestLine = lines.first().trimmed();
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 3) {
        request->status = ParseStatus::Invalid;
        request->error = QStringLiteral("请求行格式错误");
        return ParseStatus::Invalid;
    }
    request->method = QString::fromLatin1(parts.at(0)).toUpper();
    request->rawTarget = QString::fromUtf8(parts.at(1));
    if (request->rawTarget.isEmpty() || request->rawTarget.at(0) != QLatin1Char('/')) {
        request->status = ParseStatus::Invalid;
        request->error = QStringLiteral("请求路径必须以 / 开头");
        return ParseStatus::Invalid;
    }

    // ---- 路径与查询串 ----
    const int question = request->rawTarget.indexOf(QLatin1Char('?'));
    if (question < 0) {
        request->path = urlDecode(request->rawTarget);
    } else {
        request->path = urlDecode(request->rawTarget.left(question));
        const QString query = request->rawTarget.mid(question + 1);
        for (const QString &pair : query.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
            const int equals = pair.indexOf(QLatin1Char('='));
            if (equals < 0) {
                request->query.insert(urlDecode(pair), QString());
            } else {
                request->query.insert(urlDecode(pair.left(equals)),
                                      urlDecode(pair.mid(equals + 1)));
            }
        }
    }

    // ---- 头字段 ----
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i);
        QString name;
        QString value;
        if (splitHeaderLine(line, &name, &value)) {
            request->headers.insert(name, value);
        }
    }

    if (request->headers.contains(QStringLiteral("content-length"))) {
        bool ok = false;
        const qint64 length =
            request->headers.value(QStringLiteral("content-length")).toLongLong(&ok);
        if (!ok || length < 0) {
            request->status = ParseStatus::Invalid;
            request->error = QStringLiteral("Content-Length 非法");
            return ParseStatus::Invalid;
        }
        request->contentLength = length;
    }

    request->headBytes = headEnd + kHeadEnd.size();
    request->status = ParseStatus::Complete;
    return ParseStatus::Complete;
}

QByteArray buildResponse(int statusCode,
                         const QString &reasonPhrase,
                         const QString &contentType,
                         const QByteArray &body,
                         const QMap<QString, QString> &extraHeaders)
{
    const QString reason = reasonPhrase.isEmpty() ? reasonFor(statusCode) : reasonPhrase;

    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reason.toUtf8() + kCrLf;
    head += "Content-Type: " + contentType.toUtf8() + kCrLf;
    head += "Content-Length: " + QByteArray::number(body.size()) + kCrLf;
    head += "Cache-Control: no-store" + kCrLf;
    // 一律关连接：这是"发完就走"的小服务，keep-alive 只会让连接生命周期更难收尾
    head += "Connection: close" + kCrLf;
    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it) {
        head += it.key().toUtf8() + ": " + it.value().toUtf8() + kCrLf;
    }
    head += kCrLf;

    QByteArray response = head;
    response += body;
    return response;
}

QByteArray buildHeadOnlyResponse(int statusCode,
                                 const QString &reasonPhrase,
                                 const QString &contentType,
                                 qint64 contentLength,
                                 const QMap<QString, QString> &extraHeaders)
{
    const QString reason = reasonPhrase.isEmpty() ? reasonFor(statusCode) : reasonPhrase;
    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reason.toUtf8() + kCrLf;
    head += "Content-Type: " + contentType.toUtf8() + kCrLf;
    head += "Content-Length: " + QByteArray::number(contentLength) + kCrLf;
    head += "Connection: close" + kCrLf;
    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it) {
        head += it.key().toUtf8() + ": " + it.value().toUtf8() + kCrLf;
    }
    head += kCrLf;
    return head;
}

QByteArray buildJsonResponse(int statusCode, const QString &reasonPhrase, const QByteArray &jsonBody)
{
    return buildResponse(statusCode, reasonPhrase,
                         QStringLiteral("application/json; charset=utf-8"), jsonBody);
}

QByteArray buildNoContentResponse()
{
    // 204 **不能**带 Content-Length（RFC 7230 §3.3.2），所以不复用 buildResponse/buildHeadOnlyResponse
    QByteArray head;
    head += "HTTP/1.1 204 " + reasonFor(204).toUtf8() + kCrLf;
    head += "Cache-Control: no-store" + kCrLf;
    head += "Connection: close" + kCrLf;
    head += kCrLf;
    return head;
}

// ============================================================================
//  URL / MIME
// ============================================================================

QString urlDecode(const QString &text)
{
    return QUrl::fromPercentEncoding(text.toUtf8());
}

QString urlEncode(const QString &text)
{
    // 逐段编码：'/' 要保留（否则多层路径会被拍平成一个文件名）
    QStringList encodedSegments;
    for (const QString &segment : text.split(QLatin1Char('/'))) {
        encodedSegments.append(QString::fromLatin1(
            QUrl::toPercentEncoding(segment, QByteArrayLiteral("!$&'()*+,;=:@"))));
    }
    return encodedSegments.join(QLatin1Char('/'));
}

QString mimeTypeForFile(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    static const QMap<QString, QString> table = {
        {QStringLiteral("txt"), QStringLiteral("text/plain; charset=utf-8")},
        {QStringLiteral("md"), QStringLiteral("text/plain; charset=utf-8")},
        {QStringLiteral("log"), QStringLiteral("text/plain; charset=utf-8")},
        {QStringLiteral("csv"), QStringLiteral("text/csv; charset=utf-8")},
        {QStringLiteral("html"), QStringLiteral("text/html; charset=utf-8")},
        {QStringLiteral("htm"), QStringLiteral("text/html; charset=utf-8")},
        {QStringLiteral("json"), QStringLiteral("application/json; charset=utf-8")},
        {QStringLiteral("xml"), QStringLiteral("application/xml")},
        {QStringLiteral("pdf"), QStringLiteral("application/pdf")},
        {QStringLiteral("zip"), QStringLiteral("application/zip")},
        {QStringLiteral("jpg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("jpeg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("png"), QStringLiteral("image/png")},
        {QStringLiteral("gif"), QStringLiteral("image/gif")},
        {QStringLiteral("webp"), QStringLiteral("image/webp")},
        {QStringLiteral("bmp"), QStringLiteral("image/bmp")},
        {QStringLiteral("svg"), QStringLiteral("image/svg+xml")},
        {QStringLiteral("mp4"), QStringLiteral("video/mp4")},
        {QStringLiteral("mkv"), QStringLiteral("video/x-matroska")},
        {QStringLiteral("mp3"), QStringLiteral("audio/mpeg")},
        {QStringLiteral("wav"), QStringLiteral("audio/wav")},
        {QStringLiteral("flac"), QStringLiteral("audio/flac")},
        {QStringLiteral("apk"), QStringLiteral("application/vnd.android.package-archive")},
    };
    return table.value(suffix, QStringLiteral("application/octet-stream"));
}

QString formatBytes(quint64 bytes)
{
    constexpr double kKiB = 1024.0;
    const double value = static_cast<double>(bytes);
    if (value < kKiB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (value < kKiB * kKiB) {
        return QStringLiteral("%1 KB").arg(value / kKiB, 0, 'f', 1);
    }
    if (value < kKiB * kKiB * kKiB) {
        return QStringLiteral("%1 MB").arg(value / (kKiB * kKiB), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(value / (kKiB * kKiB * kKiB), 0, 'f', 2);
}

QString formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond < 0.0) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1/s").arg(formatBytes(static_cast<quint64>(bytesPerSecond)));
}

// ============================================================================
//  文件共享
// ============================================================================

QList<ShareEntry> listShareEntries(const QString &shareRoot, const QString &relative)
{
    QList<ShareEntry> entries;
    QString base = shareRoot;
    if (!relative.trimmed().isEmpty() && relative != QLatin1String(".")) {
        base = QDir(shareRoot).absoluteFilePath(relative);
    }

    const QDir dir(base);
    const QFileInfoList infos = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                                 QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &info : infos) {
        if (info.isSymLink()) {
            continue; // 符号链接可能指到分享目录之外 —— 直接不列
        }
        ShareEntry entry;
        entry.name = info.fileName();
        entry.isDirectory = info.isDir();
        entry.size = entry.isDirectory ? 0 : static_cast<quint64>(info.size());
        const QString prefix = relative.trimmed().isEmpty() ? QString() : relative + QLatin1Char('/');
        entry.relativePath = prefix + entry.name;
        entries.append(entry);
    }
    return entries;
}

bool resolveSharePath(const QString &shareRoot,
                      const QString &requestPath,
                      QString *absolutePath,
                      QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    QString relative = requestPath;
    // 去掉前导的 "files/" 前缀（请求路径形如 /files/a/b.txt）
    if (relative.startsWith(QLatin1String("/"))) {
        relative.remove(0, 1);
    }
    if (relative.startsWith(QLatin1String("files/"))) {
        relative.remove(0, 6);
    }
    relative = QUrl::fromPercentEncoding(relative.toUtf8());
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (relative.contains(QStringLiteral(".."))) {
        return fail(QStringLiteral("路径里含 .. （目录穿越已拦下）"));
    }
    if (relative.contains(QLatin1Char(':'))) {
        return fail(QStringLiteral("路径里含冒号（盘符 / ADS 已拦下）"));
    }

    const QString root = QDir::cleanPath(QDir(shareRoot).absolutePath());
    QString combined = root;
    if (!relative.isEmpty()) {
        combined += QLatin1Char('/') + relative;
    }
    const QString cleaned = QDir::cleanPath(combined);

    // 逐段检查保留设备名，并确认清理后仍在分享目录内
    const QString insidePrefix = root + QLatin1Char('/');
    if (cleaned.compare(root, Qt::CaseInsensitive) != 0
        && !cleaned.startsWith(insidePrefix, Qt::CaseInsensitive)) {
        return fail(QStringLiteral("路径越出了分享目录"));
    }
    for (const QString &segment : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (isReservedDeviceName(segment)) {
            return fail(QStringLiteral("路径命中系统保留设备名：%1").arg(segment));
        }
        if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' '))) {
            return fail(QStringLiteral("路径段以点或空格结尾：%1").arg(segment));
        }
    }

    if (absolutePath != nullptr) {
        *absolutePath = QDir::toNativeSeparators(cleaned);
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

QString uniqueFileName(const QString &directory, const QString &fileName)
{
    const QDir dir(directory);
    if (!dir.exists(fileName)) {
        return fileName;
    }
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int index = 1; index < 10000; ++index) {
        const QString candidate = suffix.isEmpty()
                                      ? QStringLiteral("%1 (%2)").arg(base).arg(index)
                                      : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix);
        if (!dir.exists(candidate)) {
            return candidate;
        }
    }
    return QStringLiteral("%1-%2").arg(base).arg(QDateTime::currentMSecsSinceEpoch());
}

QString siteIconDataUri()
{
    // ⚠ 只对 SVG 正文做 percent-encoding：`data:image/svg+xml,` 这个 scheme 前缀
    //   绝不能被编码（编码后变成 `data%3A...`，浏览器直接当无效 URL 忽略）。
    static const QString uri =
        QStringLiteral("data:image/svg+xml,")
        + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(kSiteIconSvg)));
    return uri;
}

QString buildListingPage(const QString &deviceName,
                         const QString &currentRelativePath,
                         const QList<ShareEntry> &entries,
                         const QStringList &banners)
{
    QString html;
    html += QStringLiteral(
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>WinEase 文件传输</title>");
    // ★ 在这里**声明**站点图标（内联 data URI，零额外请求）：
    //   只要页面自己声明了图标，浏览器就不会再去请求 `/favicon.ico`（踩坑 #93）。
    //   apple-touch-icon 一并声明，iOS Safari / 国产浏览器"添加到桌面/书签"时也不会另发请求。
    html += QStringLiteral("<link rel=\"icon\" href=\"%1\">"
                           "<link rel=\"apple-touch-icon\" href=\"%1\">")
                .arg(siteIconDataUri());
    html += QStringLiteral(
        "<style>"
        "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;margin:0;padding:16px;"
        "background:#14161a;color:#e8eaed}"
        "h1{font-size:18px;margin:0 0 4px}"
        ".sub{color:#9aa0a6;font-size:13px;margin-bottom:14px}"
        ".card{background:#1e2126;border-radius:10px;padding:14px;margin-bottom:14px}"
        "a{color:#8ab4f8;text-decoration:none;display:block;padding:10px 4px;"
        "border-bottom:1px solid #2a2e35;word-break:break-all}"
        "a:last-child{border-bottom:none}"
        ".size{color:#9aa0a6;font-size:12px;margin-left:6px}"
        "input[type=file]{width:100%;margin:8px 0;color:#e8eaed}"
        "button{background:#8ab4f8;color:#14161a;border:0;border-radius:8px;"
        "padding:10px 16px;font-size:15px;font-weight:600}"
        ".warn{background:#3a2a12;color:#fdd663;border-radius:8px;padding:10px;"
        "font-size:13px;margin-bottom:10px}"
        "</style></head><body>");
    html += QStringLiteral("<h1>WinEase 局域网传输</h1>");
    html += QStringLiteral("<div class=\"sub\">%1 · 当前目录：%2</div>")
                .arg(htmlEscape(deviceName),
                     htmlEscape(currentRelativePath.isEmpty() ? QStringLiteral("/")
                                                              : currentRelativePath));
    for (const QString &banner : banners) {
        html += QStringLiteral("<div class=\"warn\">%1</div>").arg(htmlEscape(banner));
    }

    // ---- 上传（手机 → 电脑）----
    html += QStringLiteral(
        "<div class=\"card\"><form method=\"POST\" action=\"/upload\" "
        "enctype=\"multipart/form-data\"><b>传文件到这台电脑</b>"
        "<input type=\"file\" name=\"file\" multiple>"
        "<button type=\"submit\">上传</button></form></div>");

    // ---- 下载（电脑 → 手机）----
    html += QStringLiteral("<div class=\"card\"><b>这台电脑分享的文件</b>");
    if (!currentRelativePath.isEmpty()) {
        QString parent = currentRelativePath;
        const int slash = parent.lastIndexOf(QLatin1Char('/'));
        parent = (slash > 0) ? parent.left(slash) : QString();
        html += QStringLiteral("<a href=\"%1\">⬆ 返回上级</a>").arg(entryUrl(parent, true));
    }
    if (entries.isEmpty()) {
        html += QStringLiteral("<div class=\"sub\">这个目录是空的。</div>");
    }
    for (const ShareEntry &entry : entries) {
        html += QStringLiteral("<a href=\"%1\">%2<span class=\"size\">%3</span></a>")
                    .arg(entryUrl(entry.relativePath, entry.isDirectory),
                         htmlEscape(entry.isDirectory ? entry.name + QLatin1Char('/') : entry.name),
                         entry.isDirectory ? QStringLiteral("目录")
                                           : formatBytes(entry.size));
    }
    html += QStringLiteral("</div></body></html>");
    return html;
}

QString buildErrorPage(const QString &title, const QString &detail)
{
    return QStringLiteral(
               "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>%1</title></head><body style=\"font-family:sans-serif;background:#14161a;"
               "color:#e8eaed;padding:20px\"><h2>%1</h2><p>%2</p></body></html>")
        .arg(htmlEscape(title), htmlEscape(detail));
}

// ============================================================================
//  局域网发现
// ============================================================================

QByteArray buildAnnounce(const QString &hostName, quint16 httpPort, const QString &deviceName)
{
    const QString payload = QStringLiteral("%1|%2|%3|%4")
                                .arg(kDiscoveryMagic, hostName)
                                .arg(httpPort)
                                .arg(deviceName);
    return payload.toUtf8();
}

bool parseAnnounce(const QByteArray &datagram,
                   QString *hostName,
                   quint16 *httpPort,
                   QString *deviceName)
{
    const QString text = QString::fromUtf8(datagram).trimmed();
    const QStringList parts = text.split(QLatin1Char('|'));
    if (parts.size() < 4 || parts.at(0) != kDiscoveryMagic) {
        return false;
    }
    bool ok = false;
    const uint port = parts.at(2).toUInt(&ok);
    if (!ok || port == 0 || port > 65535) {
        return false;
    }
    if (hostName != nullptr) {
        *hostName = parts.at(1);
    }
    if (httpPort != nullptr) {
        *httpPort = static_cast<quint16>(port);
    }
    if (deviceName != nullptr) {
        *deviceName = parts.mid(3).join(QLatin1Char('|'));
    }
    return true;
}

QString defaultDeviceName()
{
    QString host = QHostInfo::localHostName();
    if (host.isEmpty()) {
        host = QCoreApplication::applicationName();
    }
    return QStringLiteral("WinEase@%1").arg(host);
}

// ============================================================================
//  multipart/form-data
// ============================================================================

QByteArray multipartBoundary(const QString &contentType)
{
    const QString lower = contentType.toLower();
    const int index = lower.indexOf(QStringLiteral("boundary="));
    if (index < 0) {
        return QByteArray();
    }
    QString boundary = contentType.mid(index + 9).trimmed();
    if (boundary.startsWith(QLatin1Char('"'))) {
        boundary = boundary.mid(1);
        const int end = boundary.indexOf(QLatin1Char('"'));
        if (end >= 0) {
            boundary = boundary.left(end);
        }
    } else {
        const int semi = boundary.indexOf(QLatin1Char(';'));
        if (semi >= 0) {
            boundary = boundary.left(semi);
        }
    }
    return boundary.trimmed().toUtf8();
}

namespace {

/// 从形如 `name="file"; filename="a b.txt"` 的段里抠出某个键的值
QString quotedAttribute(const QByteArray &partHead, const QByteArray &key)
{
    const QByteArray needle = key + "=\"";
    const int index = partHead.indexOf(needle);
    if (index < 0) {
        return QString();
    }
    const int start = index + needle.size();
    const int end = partHead.indexOf('"', start);
    if (end < 0) {
        return QString();
    }
    // 浏览器对非 ASCII 文件名有两种写法：原样 UTF-8，或 filename*=utf-8''%E4%B8%AD
    return QString::fromUtf8(partHead.mid(start, end - start));
}

} // namespace

QString multipartFileName(const QByteArray &partHead)
{
    QString name = quotedAttribute(partHead, QByteArrayLiteral("filename"));
    if (name.isEmpty()) {
        return QString();
    }
    // 去掉客户端可能带上的路径部分（老 IE / 某些安卓浏览器会带完整路径）
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        name = name.mid(slash + 1);
    }
    return name;
}

QString multipartFieldName(const QByteArray &partHead)
{
    return quotedAttribute(partHead, QByteArrayLiteral("name"));
}

} // namespace WinEase::FeaturePlugins::LanTransfer
