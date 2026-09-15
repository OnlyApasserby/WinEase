// ============================================================================
//  lan_transfer_plugin.cpp —— 局域网跨平台文件传输（插件本体 + 对端发送器）
// ============================================================================

#include "lan_transfer_plugin.h"

#include "sdk/PluginServices.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace WinEase::FeaturePlugins {

using LanTransfer::kDiscoveryPort;

namespace {

using LanTransfer::formatBytes;
using LanTransfer::formatSpeed;

// ---------------- 配置键 ----------------
const QString kKeyDirectory = QStringLiteral("shareDirectory");
const QString kKeyPort = QStringLiteral("httpPort");
const QString kKeyDeviceName = QStringLiteral("deviceName");
const QString kKeyAllowUpload = QStringLiteral("allowUpload");

constexpr quint16 kDefaultPort = 8720;
constexpr int kAnnounceIntervalMs = 3000;

/// 发送时的读块大小
constexpr qint64 kSendChunk = 256 * 1024;

} // namespace

// ============================================================================
//  LanFileSender
// ============================================================================

LanFileSender::LanFileSender(QObject *parent)
    : QObject(parent)
{
}

bool LanFileSender::isRunning() const
{
    return m_running;
}

bool LanFileSender::start(const QHostAddress &address, quint16 port, const QString &filePath)
{
    abort();

    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        Q_EMIT finished(QFileInfo(filePath).fileName(), false,
                        QStringLiteral("要发送的文件不存在：%1").arg(filePath));
        return false;
    }

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        Q_EMIT finished(info.fileName(), false,
                        QStringLiteral("文件打不开（可能被占用）：%1").arg(info.fileName()));
        return false;
    }

    m_fileName = info.fileName();
    m_total = info.size();
    m_sent = 0;
    m_headerSent = false;
    m_running = true;

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &LanFileSender::onConnected);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &LanFileSender::onBytesWritten);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &LanFileSender::onErrorOccurred);
    m_socket->connectToHost(address, port);
    return true;
}

void LanFileSender::abort()
{
    m_running = false;
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_file.isOpen()) {
        m_file.close();
    }
}

void LanFileSender::onConnected()
{
    if (m_socket == nullptr) {
        return;
    }
    // 目标端只认 PUT /api/put?name=xxx：裸 body 流式落盘，不需要 multipart
    const QByteArray encodedName =
        QUrl::toPercentEncoding(m_fileName);
    QByteArray head;
    head += "PUT /api/put?name=" + encodedName + " HTTP/1.1\r\n";
    head += "Host: winEase-lan\r\n";
    head += "Content-Type: application/octet-stream\r\n";
    head += "Content-Length: " + QByteArray::number(m_total) + "\r\n";
    head += "Connection: close\r\n\r\n";
    m_socket->write(head);
    m_headerSent = true;
}

void LanFileSender::onBytesWritten(qint64)
{
    if (!m_running || m_socket == nullptr || !m_headerSent) {
        return;
    }
    while (m_socket->bytesToWrite() < kSendChunk * 4 && m_sent < m_total) {
        const QByteArray chunk = m_file.read(qMin<qint64>(kSendChunk, m_total - m_sent));
        if (chunk.isEmpty()) {
            break;
        }
        m_socket->write(chunk);
        m_sent += chunk.size();
        Q_EMIT progress(m_fileName, m_sent, m_total);
    }
    if (m_sent >= m_total) {
        m_running = false;
        m_file.close();
        m_socket->disconnectFromHost(); // 半关闭：对方读完 body 就知道结束了
    }
}

void LanFileSender::onErrorOccurred()
{
    if (!m_running) {
        return;
    }
    fail(m_socket != nullptr ? m_socket->errorString()
                             : QStringLiteral("连接失败（对方可能没开传输服务）"));
}

void LanFileSender::fail(const QString &detail)
{
    m_running = false;
    if (m_socket != nullptr) {
        m_socket->abort();
    }
    m_file.close();
    Q_EMIT finished(m_fileName, false, detail);
}

// ============================================================================
//  LanTransferPlugin
// ============================================================================

LanTransferPlugin::LanTransferPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
    , m_server(this)
{
    m_announceTimer.setInterval(kAnnounceIntervalMs);
    connect(&m_announceTimer, &QTimer::timeout, this, &LanTransferPlugin::broadcastAnnounce);

    connect(&m_server, &LanHttpServer::logMessage, this, &LanTransferPlugin::appendLog);
    connect(&m_server, &LanHttpServer::transferStarted, this,
            [this](const QString &name, qint64 total, bool uploading) {
                ++m_transferCount;
                appendLog(QStringLiteral("▶ %1 %2（%3）")
                              .arg(uploading ? QStringLiteral("接收") : QStringLiteral("发送"),
                                   name, formatBytes(static_cast<quint64>(qMax<qint64>(0, total)))));
            });
    connect(&m_server, &LanHttpServer::transferFinished, this,
            [this](const QString &name, bool ok, const QString &detail) {
                appendLog(QStringLiteral("%1 %2：%3")
                              .arg(ok ? QStringLiteral("✔") : QStringLiteral("✘"), name, detail));
            });
    connect(&m_sender, &LanFileSender::progress, this,
            [this](const QString &name, qint64 sent, qint64 total) {
                if (m_statusLabel != nullptr) {
                    m_statusLabel->setText(QStringLiteral("发送中 %1：%2 / %3")
                                               .arg(name, formatBytes(static_cast<quint64>(sent)),
                                                    formatBytes(static_cast<quint64>(total))));
                }
            });
    connect(&m_sender, &LanFileSender::finished, this,
            [this](const QString &name, bool ok, const QString &detail) {
                appendLog(QStringLiteral("%1 发送 %2：%3")
                              .arg(ok ? QStringLiteral("✔") : QStringLiteral("✘"), name, detail));
                if (m_statusLabel != nullptr) {
                    m_statusLabel->setText(ok ? QStringLiteral("已发送 %1").arg(name)
                                              : QStringLiteral("发送失败：%1").arg(detail));
                }
            });
}

LanTransferPlugin::~LanTransferPlugin() = default;

QString LanTransferPlugin::id() const
{
    return QStringLiteral("net.lan_transfer");
}

QString LanTransferPlugin::name() const
{
    return QStringLiteral("局域网文件传输");
}

QString LanTransferPlugin::description() const
{
    return QStringLiteral("手机/电脑与这台电脑互传文件：手机扫码免装 App，电脑之间发现即发");
}

QString LanTransferPlugin::detailedDescription() const
{
    return QStringLiteral(
        "开启后 WinEase 会在本机开一个极小的 HTTP 服务（默认 8720 端口），"
        "同一局域网内的设备都能连上来传文件：\n"
        "\n"
        "① 手机（iOS / Android / 鸿蒙，**不用装任何 App**）\n"
        "   在浏览器里打开面板上显示的地址（形如 http://192.168.1.23:8720）：\n"
        "   · 页面上列出的是你选的分享目录，点文件名即下载到手机（电脑 → 手机）\n"
        "   · 页面上的「上传」按钮选文件，即可把手机里的照片/文件传到电脑（手机 → 电脑）\n"
        "\n"
        "② 另一台装了 WinEase 的电脑\n"
        "   两台机器在同一网段时会互相出现在「发现设备」列表里，选中对端 + 选文件即可直发；\n"
        "   发现不到也可以手动填对方地址（广播被路由器挡住时用）。\n"
        "\n"
        "边界与安全（说清楚，免得误用）：\n"
        "· **明文 HTTP、无鉴权**：定位是自家局域网内互传，不要在公共 Wi-Fi 上开着\n"
        "· 只监听 IPv4，且**只有你启用本插件时才监听**，停用立刻关端口\n"
        "· 所有请求路径都会先过目录穿越检查（`..` / 盘符 / CON、NUL 这类保留名一律拒绝）\n"
        "· 手机传上来的同名文件会自动改名为「名字 (1).ext」，绝不静默覆盖\n"
        "· 传输是**流式**的：传几个 GB 的录像也不会把内存吃爆\n"
        "· 传输记录（谁、传了什么、成功还是失败）都留在面板上，失败会写清原因");
}

QIcon LanTransferPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory LanTransferPlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList LanTransferPlugin::tags() const
{
    return {QStringLiteral("局域网"), QStringLiteral("传输"), QStringLiteral("文件"),
            QStringLiteral("手机"), QStringLiteral("互传"), QStringLiteral("HTTP"),
            QStringLiteral("lan"), QStringLiteral("transfer"), QStringLiteral("share")};
}

bool LanTransferPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence LanTransferPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+L"));
}

// ============================================================================
//  生命周期
// ============================================================================

bool LanTransferPlugin::initialize()
{
    WinEase::PluginServices *svc = services();
    if (svc != nullptr) {
        m_server.setShareDirectory(svc->configValue(id(), kKeyDirectory).toString());
        QString name = svc->configValue(id(), kKeyDeviceName).toString();
        if (name.trimmed().isEmpty()) {
            name = LanTransfer::defaultDeviceName();
        }
        m_server.setDeviceName(name);
    } else {
        m_server.setDeviceName(LanTransfer::defaultDeviceName());
    }
    return true;
}

void LanTransferPlugin::shutdown()
{
    stopServer();
    stopDiscovery();
}

bool LanTransferPlugin::canEnable(QString *reason) const
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        if (reason != nullptr) {
            *reason = QStringLiteral("宿主服务未注入");
        }
        return false;
    }
    const QString directory = svc->configValue(id(), kKeyDirectory).toString();
    if (directory.trimmed().isEmpty() || !QFileInfo(directory).isDir()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("还没有设置分享目录：请先在设置面板里选一个存在的目录");
        }
        return false;
    }
    return true;
}

bool LanTransferPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入"));
        return false;
    }

    m_server.setShareDirectory(svc->configValue(id(), kKeyDirectory).toString());
    QString name = svc->configValue(id(), kKeyDeviceName).toString();
    if (name.trimmed().isEmpty()) {
        name = LanTransfer::defaultDeviceName();
    }
    m_server.setDeviceName(name);

    startServer();
    if (!m_server.isRunning()) {
        setLastError(QStringLiteral("传输服务没能启动（端口可能被占用）"));
        return false;
    }
    startDiscovery();
    refreshAddressLabel();

    Q_EMIT statusMessage(QStringLiteral("已在 %1 端口待命，手机可用浏览器访问 %2")
                             .arg(m_server.port())
                             .arg(primaryLanAddress().isEmpty()
                                      ? QStringLiteral("（未找到局域网地址）")
                                      : QStringLiteral("http://%1:%2")
                                            .arg(primaryLanAddress())
                                            .arg(m_server.port())));
    return true;
}

void LanTransferPlugin::onDisable()
{
    // 停用必须立刻关端口：用户点掉开关之后，这台机器就不该再在局域网上开着门
    stopServer();
    stopDiscovery();
}

void LanTransferPlugin::onHotkey(const QString &hotkeyId)
{
    if (hotkeyId.endsWith(QLatin1String("::open"))) {
        Q_EMIT statusMessage(m_server.isRunning()
                                 ? QStringLiteral("传输服务运行中，端口 %1").arg(m_server.port())
                                 : QStringLiteral("传输服务未运行"));
    }
}

// ============================================================================
//  服务 / 发现
// ============================================================================

void LanTransferPlugin::startServer()
{
    QString error;
    if (!m_server.start(configuredPort(), &error)) {
        appendLog(error);
        return;
    }
    appendLog(QStringLiteral("服务已启动：http://%1:%2（分享目录 %3）")
                  .arg(primaryLanAddress().isEmpty() ? QStringLiteral("127.0.0.1")
                                                     : primaryLanAddress())
                  .arg(m_server.port())
                  .arg(QDir::toNativeSeparators(m_server.shareDirectory())));
}

void LanTransferPlugin::stopServer()
{
    m_server.stop();
}

void LanTransferPlugin::startDiscovery()
{
    if (m_discovery == nullptr) {
        m_discovery = new QUdpSocket(this);
        // ShareAddress：同一台机器上跑两个 WinEase（或另一个监听者）时也能共用端口
        if (!m_discovery->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            appendLog(QStringLiteral("局域网发现不可用（%1 端口绑定失败：%2）——"
                                     "仍可手动填对方地址发送")
                          .arg(kDiscoveryPort)
                          .arg(m_discovery->errorString()));
            m_discovery->deleteLater();
            m_discovery = nullptr;
            return;
        }
        connect(m_discovery, &QUdpSocket::readyRead, this,
                &LanTransferPlugin::onAnnounceReceived);
    }
    m_announceTimer.start();
    broadcastAnnounce();
    appendLog(QStringLiteral("已开始广播自己（UDP %1），等待其它 WinEase 出现").arg(kDiscoveryPort));
}

void LanTransferPlugin::stopDiscovery()
{
    m_announceTimer.stop();
    if (m_discovery != nullptr) {
        m_discovery->close();
        m_discovery->deleteLater();
        m_discovery = nullptr;
    }
    m_peers.clear();
    refreshPeerList();
}

void LanTransferPlugin::broadcastAnnounce()
{
    if (m_discovery == nullptr || !m_server.isRunning()) {
        return;
    }
    const QByteArray datagram =
        LanTransfer::buildAnnounce(LanTransfer::defaultDeviceName(),
                                   m_server.port(), m_server.deviceName());
    m_discovery->writeDatagram(datagram, QHostAddress::Broadcast, kDiscoveryPort);
}

void LanTransferPlugin::onAnnounceReceived()
{
    if (m_discovery == nullptr) {
        return;
    }
    while (m_discovery->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_discovery->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_discovery->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        QString hostName;
        quint16 httpPort = 0;
        QString deviceName;
        if (!LanTransfer::parseAnnounce(datagram, &hostName, &httpPort, &deviceName)) {
            continue;
        }
        // 自己的广播也会绕回来，丢掉
        if (httpPort == m_server.port() && deviceName == m_server.deviceName()) {
            continue;
        }

        LanPeer peer;
        peer.deviceName = deviceName;
        peer.hostName = hostName;
        peer.address = sender;
        peer.port = httpPort;

        bool found = false;
        for (LanPeer &existing : m_peers) {
            if (existing.key() == peer.key()) {
                existing = peer;
                found = true;
                break;
            }
        }
        if (!found) {
            m_peers.append(peer);
            appendLog(QStringLiteral("发现设备：%1（%2:%3）")
                          .arg(peer.deviceName, peer.address.toString())
                          .arg(peer.port));
        }
        refreshPeerList();
    }
}

// ============================================================================
//  界面辅助
// ============================================================================

void LanTransferPlugin::appendLog(const QString &line)
{
    if (line.isEmpty() || line == m_lastLogLine) {
        return;
    }
    m_lastLogLine = line;
    if (m_logView != nullptr) {
        m_logView->appendPlainText(line);
    }
    if (m_statusLabel != nullptr) {
        m_statusLabel->setText(line);
    }
    logMessage(WinEase::PluginLogLevel::Info, line);
}

QString LanTransferPlugin::primaryLanAddress() const
{
    // 挑一块"像局域网"的网卡地址（优先 192.168/10/172.16 这类私有网段）
    QString fallback;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const QString text = address.toString();
            if (text.startsWith(QLatin1String("192.168."))
                || text.startsWith(QLatin1String("10."))
                || text.startsWith(QLatin1String("172."))) {
                return text;
            }
            if (fallback.isEmpty()) {
                fallback = text;
            }
        }
    }
    return fallback;
}

quint16 LanTransferPlugin::configuredPort() const
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return kDefaultPort;
    }
    bool ok = false;
    const int port = svc->configValue(id(), kKeyPort, kDefaultPort).toInt(&ok);
    if (!ok || port < 1024 || port > 65535) {
        return kDefaultPort;
    }
    return static_cast<quint16>(port);
}

QString LanTransferPlugin::configuredDirectory() const
{
    WinEase::PluginServices *svc = services();
    return svc != nullptr ? svc->configValue(id(), kKeyDirectory).toString() : QString();
}

void LanTransferPlugin::refreshAddressLabel()
{
    if (m_addressLabel == nullptr) {
        return;
    }
    if (!m_server.isRunning()) {
        m_addressLabel->setText(QStringLiteral("未运行"));
        return;
    }
    const QString address = primaryLanAddress();
    m_addressLabel->setText(
        address.isEmpty()
            ? QStringLiteral("未找到局域网地址（请检查是否连着 Wi-Fi / 网线）")
            : QStringLiteral("%1:%2　（手机浏览器打开 http://%1:%2）")
                  .arg(address)
                  .arg(m_server.port()));
}

void LanTransferPlugin::refreshPeerList()
{
    if (m_peerList == nullptr) {
        return;
    }
    m_peerList->clear();
    for (const LanPeer &peer : std::as_const(m_peers)) {
        m_peerList->addItem(QStringLiteral("%1　%2:%3")
                                .arg(peer.deviceName, peer.address.toString())
                                .arg(peer.port));
    }
    if (m_sendButton != nullptr) {
        m_sendButton->setEnabled(!m_peers.isEmpty());
    }
}

void LanTransferPlugin::sendFileToSelectedPeer()
{
    if (m_peerList == nullptr || m_peerList->currentRow() < 0
        || m_peerList->currentRow() >= m_peers.size()) {
        appendLog(QStringLiteral("请先在列表里选中一台设备。"));
        return;
    }
    const LanPeer peer = m_peers.at(m_peerList->currentRow());
    const QString filePath = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择要发送到 %1 的文件").arg(peer.deviceName));
    if (filePath.isEmpty()) {
        return;
    }
    if (!m_sender.start(peer.address, peer.port, filePath)) {
        return;
    }
    appendLog(QStringLiteral("开始发送 %1 → %2：%3")
                  .arg(QFileInfo(filePath).fileName(), peer.deviceName,
                       formatBytes(static_cast<quint64>(QFileInfo(filePath).size()))));
}

// ============================================================================
//  设置面板
// ============================================================================

QWidget *LanTransferPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *configGroup = new QGroupBox(QStringLiteral("本机设置"), page);
    auto *form = new QFormLayout(configGroup);
    form->setContentsMargins(12, 12, 12, 12);
    form->setSpacing(8);

    m_directoryEdit = new QLineEdit(configuredDirectory(), page);
    m_directoryEdit->setObjectName(QStringLiteral("lanTransferDirectoryEdit"));
    m_directoryEdit->setToolTip(QStringLiteral("共享给手机的目录。只读地列出来；手机上传的文件也存到这里。"));
    auto *directoryRow = new QWidget(configGroup);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setSpacing(6);
    directoryLayout->addWidget(m_directoryEdit, 1);
    auto *browseButton = new QPushButton(QStringLiteral("浏览…"), directoryRow);
    browseButton->setObjectName(QStringLiteral("lanTransferBrowseButton"));
    directoryLayout->addWidget(browseButton);
    form->addRow(QStringLiteral("分享目录"), directoryRow);

    m_nameEdit = new QLineEdit(m_server.deviceName(), page);
    m_nameEdit->setObjectName(QStringLiteral("lanTransferNameEdit"));
    m_nameEdit->setToolTip(QStringLiteral("手机上会看到这个名字，方便确认连对了机器。"));
    form->addRow(QStringLiteral("设备名"), m_nameEdit);

    m_portSpin = new QSpinBox(page);
    m_portSpin->setObjectName(QStringLiteral("lanTransferPortSpin"));
    m_portSpin->setRange(1024, 65535);
    m_portSpin->setValue(configuredPort());
    m_portSpin->setToolTip(QStringLiteral("服务端口。被占用时换一个（手机地址里的端口要跟着改）。"));
    form->addRow(QStringLiteral("端口"), m_portSpin);

    m_allowUploadCheck = new QCheckBox(QStringLiteral("允许对方上传文件到本机"), page);
    m_allowUploadCheck->setObjectName(QStringLiteral("lanTransferAllowUploadCheck"));
    m_allowUploadCheck->setChecked(true);
    m_allowUploadCheck->setEnabled(false); // 说明：当前版本上传始终开放，复选框留作后续开关
    m_allowUploadCheck->setToolTip(
        QStringLiteral("当前版本上传能力始终开放（手机页面的上传表单会用到）。\n"
                       "如果你只想单向分享，请把分享目录设成一个专门的中转目录。"));
    form->addRow(QString(), m_allowUploadCheck);

    m_addressLabel = new QLabel(QStringLiteral("未运行"), configGroup);
    m_addressLabel->setObjectName(QStringLiteral("lanTransferAddressLabel"));
    m_addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_addressLabel->setWordWrap(true);
    form->addRow(QStringLiteral("访问地址"), m_addressLabel);

    layout->addWidget(configGroup);

    // ---------------- 操作 ----------------
    auto *actionRow = new QWidget(page);
    auto *actionLayout = new QHBoxLayout(actionRow);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);
    m_toggleButton = new QPushButton(
        m_server.isRunning() ? QStringLiteral("停止服务") : QStringLiteral("启用服务"), actionRow);
    m_toggleButton->setObjectName(QStringLiteral("lanTransferToggleButton"));
    auto *copyButton = new QPushButton(QStringLiteral("复制地址"), actionRow);
    copyButton->setObjectName(QStringLiteral("lanTransferCopyButton"));
    actionLayout->addWidget(m_toggleButton);
    actionLayout->addWidget(copyButton);
    actionLayout->addStretch(1);
    layout->addWidget(actionRow);

    // ---------------- 设备发现 ----------------
    auto *peerGroup = new QGroupBox(QStringLiteral("发现的设备（同一局域网内的 WinEase）"), page);
    auto *peerLayout = new QVBoxLayout(peerGroup);
    peerLayout->setContentsMargins(12, 12, 12, 12);
    m_peerList = new QListWidget(peerGroup);
    m_peerList->setObjectName(QStringLiteral("lanTransferPeerList"));
    peerLayout->addWidget(m_peerList);
    auto *sendRow = new QWidget(peerGroup);
    auto *sendLayout = new QHBoxLayout(sendRow);
    sendLayout->setContentsMargins(0, 0, 0, 0);
    m_sendButton = new QPushButton(QStringLiteral("向选中设备发送文件…"), sendRow);
    m_sendButton->setObjectName(QStringLiteral("lanTransferSendButton"));
    m_sendButton->setEnabled(false);
    sendLayout->addWidget(m_sendButton);
    sendLayout->addStretch(1);
    peerLayout->addWidget(sendRow);
    layout->addWidget(peerGroup);

    // ---------------- 记录 ----------------
    auto *logGroup = new QGroupBox(QStringLiteral("传输记录"), page);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(12, 12, 12, 12);
    m_logView = new QPlainTextEdit(logGroup);
    m_logView->setObjectName(QStringLiteral("lanTransferLogView"));
    m_logView->setReadOnly(true);
    logLayout->addWidget(m_logView);
    layout->addWidget(logGroup, 1);

    m_statusLabel = new QLabel(QStringLiteral("未运行"), page);
    m_statusLabel->setObjectName(QStringLiteral("lanTransferStatusLabel"));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    // ---------------- 信号 ----------------
    const auto persist = [this](const QString &key, const QVariant &value) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), key, value);
        }
    };

    connect(browseButton, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(
            nullptr, QStringLiteral("选择分享目录"), m_directoryEdit->text());
        if (chosen.isEmpty()) {
            return;
        }
        m_directoryEdit->setText(QDir::toNativeSeparators(chosen));
    });
    connect(m_directoryEdit, &QLineEdit::textChanged, this, [this, persist](const QString &text) {
        persist(kKeyDirectory, text);
        m_server.setShareDirectory(text);
    });
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this, persist](const QString &text) {
        persist(kKeyDeviceName, text);
        m_server.setDeviceName(text.trimmed().isEmpty() ? LanTransfer::defaultDeviceName() : text);
    });
    connect(m_portSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, persist](int value) {
                persist(kKeyPort, value);
                if (m_server.isRunning()) {
                    stopServer();
                    startServer();
                    refreshAddressLabel();
                }
            });
    connect(m_toggleButton, &QPushButton::clicked, this, [this] {
        if (m_server.isRunning()) {
            stopServer();
            m_toggleButton->setText(QStringLiteral("启用服务"));
            refreshAddressLabel();
            return;
        }
        QString reason;
        if (!canEnable(&reason)) {
            appendLog(QStringLiteral("启用失败：%1").arg(reason));
            return;
        }
        startServer();
        startDiscovery();
        m_toggleButton->setText(m_server.isRunning() ? QStringLiteral("停止服务")
                                                     : QStringLiteral("启用服务"));
        refreshAddressLabel();
    });
    connect(copyButton, &QPushButton::clicked, this, [this] {
        const QString address = primaryLanAddress();
        const QString url = address.isEmpty()
                                ? QString()
                                : QStringLiteral("http://%1:%2").arg(address).arg(m_server.port());
        if (url.isEmpty()) {
            appendLog(QStringLiteral("没有可复制的地址（未找到局域网地址）"));
            return;
        }
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(url);
        }
        appendLog(QStringLiteral("已复制地址：%1").arg(url));
    });
    connect(m_sendButton, &QPushButton::clicked, this, &LanTransferPlugin::sendFileToSelectedPeer);

    refreshAddressLabel();
    refreshPeerList();
    return page;
}

} // namespace WinEase::FeaturePlugins
