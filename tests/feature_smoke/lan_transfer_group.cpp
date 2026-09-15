// ============================================================================
//  lan_transfer_group.cpp —— 局域网文件传输（net.lan_transfer）的自检
//
//  ⚠ 这一组**只连 127.0.0.1 的回环地址**：不起广播、不碰真实局域网，
//    因此可以在任何机器上稳定重复执行（不依赖"这台机器有没有连着 Wi-Fi"）。
// ============================================================================

#include "lan_transfer_group.h"

#include "LanTransferEngine.h"
#include "app/core/PluginManager.h"
#include "sdk/IFeaturePlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWidget>

namespace FeatureSmoke {

namespace {

// 引擎住在 WinEase::FeaturePlugins::LanTransfer 里；自检自己也在 FeatureSmoke 命名空间，
// 短名找不到嵌套命名空间，这里给它一个**命名空间别名**。
namespace LanTransfer = WinEase::FeaturePlugins::LanTransfer;

using LanTransfer::HttpRequest;
using LanTransfer::ParseStatus;

const QString kPluginId = QStringLiteral("net.lan_transfer");

/// 发一个请求并把整个响应读回来。
///
/// ⚠ 关键点：**服务端和客户端在同一个线程里**（插件与自检同进程），
///   所以不能死等 `waitForReadyRead` —— 那样事件循环停摆，QTcpServer
///   根本没机会 accept 和回包，结果永远是"读不到任何字节"（状态 0）。
///   正确姿势：等待期间持续 processEvents（并给每次等待一个很短的超时）。
QByteArray httpRequest(quint16 port,
                       const QByteArray &head,
                       const QByteArray &body = QByteArray())
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);

    QElapsedTimer timer;
    timer.start();
    while (socket.state() != QAbstractSocket::ConnectedState && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        socket.waitForConnected(20);
    }
    if (socket.state() != QAbstractSocket::ConnectedState) {
        return QByteArray();
    }

    socket.write(head);
    if (!body.isEmpty()) {
        socket.write(body);
    }
    socket.flush();

    QByteArray response;
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        socket.waitForReadyRead(20);
        response += socket.readAll();
        // "Connection: close"：对方发完就断开，读到断开即结束
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            break;
        }
    }
    response += socket.readAll();
    return response;
}

/// 响应头与响应体切开
QByteArray responseBody(const QByteArray &response)
{
    const int split = response.indexOf(QByteArrayLiteral("\r\n\r\n"));
    return split < 0 ? QByteArray() : response.mid(split + 4);
}

int responseStatus(const QByteArray &response)
{
    if (!response.startsWith(QByteArrayLiteral("HTTP/1.1 "))) {
        return 0;
    }
    return response.mid(9, 3).toInt();
}

QByteArray get(const quint16 port, const QByteArray &target, const QByteArray &headers = QByteArray())
{
    return httpRequest(port,
                       "GET " + target + " HTTP/1.1\r\nHost: winease\r\n" + headers + "\r\n");
}

/// 把服务真的跑起来（通过插件，而不是直接 new 服务器）
bool enablePlugin(WinEase::PluginManager &manager,
                  StubServices &services,
                  const QString &shareDirectory,
                  quint16 port)
{
    services.setConfigValue(kPluginId, QStringLiteral("shareDirectory"), shareDirectory);
    services.setConfigValue(kPluginId, QStringLiteral("httpPort"), static_cast<int>(port));
    services.setConfigValue(kPluginId, QStringLiteral("deviceName"), QStringLiteral("WinEase 自检"));
    return manager.setPluginEnabled(kPluginId, true);
}

} // namespace

int runLanTransferGroupTests(Reporter &reporter,
                             WinEase::PluginManager &manager,
                             StubServices &services)
{
    const int failuresAtStart = reporter.failures();
    reporter.info(QStringLiteral("---- net.lan_transfer 局域网文件传输 ----"));

    // =======================================================================
    //  ① 协议解析（纯函数）
    // =======================================================================
    {
        const QByteArray raw =
            "GET /files/%E4%B8%AD%E6%96%87.txt?a=1&b=%20x HTTP/1.1\r\n"
            "Host: example\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        HttpRequest request;
        const ParseStatus status = LanTransfer::parseRequestHead(raw, &request);
        reporter.check(status == ParseStatus::Complete && request.method == QStringLiteral("GET")
                           && request.path == QStringLiteral("/files/中文.txt")
                           && request.query.value(QStringLiteral("a")) == QStringLiteral("1")
                           && request.query.value(QStringLiteral("b")) == QStringLiteral(" x")
                           && request.contentLength == 5 && request.headBytes == raw.size() - 5,
                       QStringLiteral("HTTP 请求头解析：方法与 URL 解码路径、查询串、"
                                      "Content-Length、body 起点都对"),
                       QStringLiteral("%1 | %2").arg(request.method, request.path));
    }
    {
        HttpRequest request;
        const ParseStatus status =
            LanTransfer::parseRequestHead(QByteArrayLiteral("GET / HTTP/1.1\r\nHost: a\r\n"),
                                          &request);
        reporter.check(status == ParseStatus::NeedMoreData,
                       QStringLiteral("请求头没收全时返回 NeedMoreData（不误判成完整请求）"));
    }
    {
        HttpRequest request;
        const ParseStatus status = LanTransfer::parseRequestHead(QByteArrayLiteral("garbage\r\n\r\n"),
                                                                 &request);
        reporter.check(status == ParseStatus::Invalid && !request.error.isEmpty(),
                       QStringLiteral("垃圾请求头判非法并给出原因（不会当成合法请求处理）"),
                       request.error);
    }
    {
        const QByteArray json = LanTransfer::buildJsonResponse(
            200, QString(), QByteArrayLiteral("{\"ok\":true}"));
        reporter.check(json.startsWith("HTTP/1.1 200 OK")
                           && json.contains("Content-Length: 11")
                           && json.contains("application/json"),
                       QStringLiteral("响应组装：状态行 / Content-Length / Content-Type 都写对了"));
    }

    // =======================================================================
    //  ② ★ 目录穿越必须被挡（这是本功能唯一的安全红线）
    // =======================================================================
    {
        QTemporaryDir share;
        const QString root = share.path();
        QString resolved;
        QString error;

        const QStringList attacks = {QStringLiteral("/files/../../Windows/win.ini"),
                                     QStringLiteral("/files/..%2f..%2fsecret.txt"),
                                     QStringLiteral("/files/sub/%2e%2e/%2e%2e/boot.ini"),
                                     QStringLiteral("/files/C:/Windows/win.ini"),
                                     QStringLiteral("/files/CON"),
                                     QStringLiteral("/files/a/../../b.txt")};
        QStringList allowed;
        for (const QString &attack : attacks) {
            if (LanTransfer::resolveSharePath(root, attack, &resolved, &error)) {
                allowed.append(attack);
            }
        }
        reporter.check(allowed.isEmpty(),
                       QStringLiteral("★ 目录穿越 / 盘符 / 保留设备名全部被拒（403 那种）"),
                       QStringLiteral("竟然放行了：%1").arg(allowed.join(QStringLiteral(", "))));

        const bool normalOk =
            LanTransfer::resolveSharePath(root, QStringLiteral("/files/docs/report.pdf"),
                                          &resolved, &error)
            && QDir::cleanPath(resolved)
                   == QDir::cleanPath(QDir(root).absoluteFilePath(QStringLiteral("docs/report.pdf")));
        reporter.check(normalOk,
                       QStringLiteral("正常路径仍然解析成功（拦的是越界，不是把功能拦死）"),
                       resolved);
    }

    // =======================================================================
    //  ③ multipart（手机表单）
    // =======================================================================
    {
        const QByteArray boundary = LanTransfer::multipartBoundary(
            QStringLiteral("multipart/form-data; boundary=----WebKitFormBoundaryABC123"));
        const QByteArray partHead =
            "Content-Disposition: form-data; name=\"file\"; filename=\"照片 1.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n";
        reporter.check(boundary == QByteArrayLiteral("----WebKitFormBoundaryABC123")
                           && LanTransfer::multipartFileName(partHead) == QStringLiteral("照片 1.jpg")
                           && LanTransfer::multipartFieldName(partHead) == QStringLiteral("file"),
                       QStringLiteral("multipart 边界与文件名/字段名解析正确（含中文与空格）"),
                       QString::fromUtf8(boundary));
    }
    {
        const QString html = LanTransfer::buildListingPage(
            QStringLiteral("WinEase@测试"),
            QString(),
            {{QStringLiteral("报告.pdf"), QStringLiteral("报告.pdf"), 1024, false},
             {QStringLiteral("照片"), QStringLiteral("照片"), 0, true}},
            {QStringLiteral("别关 WinEase")});
        reporter.check(html.contains(QStringLiteral("<form method=\"POST\" action=\"/upload\""))
                           && html.contains(QStringLiteral("enctype=\"multipart/form-data\""))
                           && html.contains(QStringLiteral("上传"))
                           && html.contains(QStringLiteral("报告.pdf"))
                           && html.contains(QStringLiteral("别关 WinEase")),
                       QStringLiteral("手机页面：带上传表单、列出文件、把提醒写在页面上"));

        // ---- ★ 站点图标：页面必须自己声明，浏览器才不会去探测 /favicon.ico（踩坑 #93）----
        const QString icon = LanTransfer::siteIconDataUri();
        reporter.check(html.contains(QStringLiteral("<link rel=\"icon\" href=\"data:image/svg+xml,"))
                           && html.contains(QStringLiteral("<link rel=\"apple-touch-icon\"")),
                       QStringLiteral("★ 手机页面自己声明了图标（内联 data URI）→ "
                                      "浏览器不再去请求 /favicon.ico"),
                       icon.left(40));
        reporter.check(icon.startsWith(QStringLiteral("data:image/svg+xml,"))
                           && !icon.contains(QLatin1Char('#'))
                           && !icon.contains(QLatin1Char('<'))
                           && !icon.contains(QLatin1Char('"')),
                       QStringLiteral("★ 图标 data URI：scheme 明文未编码、正文已 percent-encoding"
                                      "（`#` / `<` / `\"` 都不许原样出现，否则会被 HTML 属性截断）"),
                       icon.left(80));
    }
    {
        const QByteArray noContent = LanTransfer::buildNoContentResponse();
        reporter.check(noContent.startsWith("HTTP/1.1 204")
                           && noContent.endsWith("\r\n\r\n")
                           && !noContent.contains("Content-Length"),
                       QStringLiteral("★ 204 响应：状态行正确、不带响应体、"
                                      "**不带 Content-Length**（RFC 7230 对 204 的硬要求）"),
                       QString::fromUtf8(noContent.left(60)).trimmed());
    }

    // =======================================================================
    //  ④ 真实回环：让插件真的监听，然后发真请求
    // =======================================================================
    {
        QTemporaryDir share;
        const QString root = share.path();
        const QString sharedFile = QDir(root).absoluteFilePath(QStringLiteral("shared.txt"));
        QFile file(sharedFile);
        file.open(QIODevice::WriteOnly);
        file.write("winease-lan-probe");
        file.close();

        // 挑一个不太可能被占用的高位端口（自检不依赖固定端口）
        const quint16 port = 28720;
        const bool enabled = enablePlugin(manager, services, root, port);
        reporter.check(enabled, QStringLiteral("局域网传输插件能启用（分享目录已配置）"));
        if (enabled) {
            const QByteArray page = get(port, "/");
            reporter.check(responseStatus(page) == 200
                               && page.contains(QStringLiteral("WinEase 局域网传输").toUtf8())
                               && page.contains(QByteArrayLiteral("shared.txt")),
                           QStringLiteral("回环 GET /：手机页面 200 且列出了分享目录里的文件"),
                           QStringLiteral("状态 %1").arg(responseStatus(page)));

            const QByteArray download = get(port, "/files/shared.txt");
            reporter.check(responseStatus(download) == 200
                               && responseBody(download) == QByteArrayLiteral("winease-lan-probe")
                               && download.contains("attachment"),
                           QStringLiteral("回环 GET /files/：下载内容与源文件**逐字节一致**"
                                          "（且带下载用的 Content-Disposition）"));

            const QByteArray listing = get(port, "/api/list");
            reporter.check(responseStatus(listing) == 200
                               && listing.contains("shared.txt")
                               && listing.contains("\"entries\""),
                           QStringLiteral("回环 GET /api/list：JSON 清单里有文件（给对端 WinEase 用）"));

            // ---- 浏览器的站点图标探测：必须 204，绝不能是 404 错误页（踩坑 #93）----
            bool probesOk = true;
            QString probeDetail;
            for (const QByteArray &target : {QByteArrayLiteral("/favicon.ico"),
                                            QByteArrayLiteral("/apple-touch-icon.png"),
                                            QByteArrayLiteral("/apple-touch-icon-precomposed.png")}) {
                const QByteArray probe = get(port, target);
                const bool ok = responseStatus(probe) == 204 && responseBody(probe).isEmpty()
                                && !probe.contains(QStringLiteral("没有这个地址").toUtf8());
                if (!ok) {
                    probesOk = false;
                    probeDetail += QStringLiteral("%1→%2 ")
                                       .arg(QString::fromLatin1(target))
                                       .arg(responseStatus(probe));
                }
            }
            reporter.check(probesOk,
                           QStringLiteral("★ 回环 GET /favicon.ico 与 apple-touch-icon*："
                                          "一律 204 且**不是**「没有这个地址」错误页"
                                          "（手机浏览器不会因此报错、面板日志不再出现假故障）"),
                           probeDetail);

            // ---- 反向对照：真的写错的路径仍然必须 404（不要为了消掉日志把 404 全改成 204）----
            const QByteArray unknown = get(port, "/nope-not-a-route");
            reporter.check(responseStatus(unknown) == 404
                               && unknown.contains(QStringLiteral("没有这个地址").toUtf8()),
                           QStringLiteral("反向对照：未知路径仍然 404 并如实说明（拦的是浏览器探测，"
                                          "不是把错误处理一起关掉）"),
                           QStringLiteral("状态 %1").arg(responseStatus(unknown)));

            // ---- 对端发送：PUT 裸 body（WinEase → WinEase 走的就是这条）----
            const QByteArray payload = "put-by-peer";
            const QByteArray putResponse =
                httpRequest(port,
                            "PUT /api/put?name=from-peer.txt HTTP/1.1\r\nHost: winease\r\n"
                            "Content-Length: "
                                + QByteArray::number(payload.size()) + "\r\n\r\n",
                            payload);
            const QString received = QDir(root).absoluteFilePath(QStringLiteral("from-peer.txt"));
            reporter.check(responseStatus(putResponse) == 200 && QFile::exists(received),
                           QStringLiteral("★ 回环 PUT /api/put：**磁盘上真的多出**对端发来的文件"),
                           QStringLiteral("状态 %1").arg(responseStatus(putResponse)));
            if (QFile::exists(received)) {
                QFile receivedFile(received);
                receivedFile.open(QIODevice::ReadOnly);
                reporter.check(receivedFile.readAll() == payload,
                               QStringLiteral("收到的文件内容与发出的**逐字节一致**（没有截断/损坏）"));
            }

            // ---- 同名再发一次：必须自动改名，绝不覆盖 ----
            const QByteArray second =
                httpRequest(port,
                            "PUT /api/put?name=from-peer.txt HTTP/1.1\r\nHost: winease\r\n"
                            "Content-Length: 6\r\n\r\n",
                            QByteArrayLiteral("second"));
            reporter.check(responseStatus(second) == 200
                               && QFile::exists(QDir(root).absoluteFilePath(
                                   QStringLiteral("from-peer (1).txt"))),
                           QStringLiteral("同名文件再传一次 → 自动改名「from-peer (1).txt」"
                                          "（**绝不静默覆盖**）"),
                           QStringLiteral("目录里现有：%1")
                               .arg(QDir(root).entryList(QDir::Files).join(QStringLiteral(","))));

            // ---- 越界请求：必须 403，且目录里什么都没多 ----
            const QStringList filesBefore = QDir(root).entryList(QDir::Files);
            const QByteArray attack = get(port, "/files/../../Windows/win.ini");
            const QStringList filesAfter = QDir(root).entryList(QDir::Files);
            reporter.check(responseStatus(attack) == 403 && filesBefore == filesAfter,
                           QStringLiteral("★ 回环越界请求 GET /files/../../Windows/win.ini → 403，"
                                          "分享目录没有变化"),
                           QStringLiteral("状态 %1").arg(responseStatus(attack)));

            reporter.check(manager.setPluginEnabled(kPluginId, false),
                           QStringLiteral("停用局域网传输插件成功"));
            const QByteArray afterStop = get(port, "/");
            reporter.check(afterStop.isEmpty(),
                           QStringLiteral("★ 停用后端口立刻关闭（局域网里不再有这台机器的服务）"),
                           QStringLiteral("还读到了 %1 字节").arg(afterStop.size()));
        }
    }

    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
