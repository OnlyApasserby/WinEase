#pragma once

// ============================================================================
//  LanTransferPlugin —— 局域网跨平台文件传输
//
//  一方开服务、另一方用浏览器（手机）或 WinEase（另一台电脑）连过来：
//
//      ┌ 手机浏览器 ────────────────┐
//      │ http://192.168.x.x:8720    │  ← 目录页里直接点文件下载（电脑→手机）
//      └ 表单上传 ──────────────────┘  ← 页面上的「上传」把手机文件送进电脑
//
//      ┌ 另一台 WinEase ────────────┐
//      │ UDP 广播互相发现（27182）   │  ← "发现设备"列表里出现对方
//      └ PUT /api/put?name=… 直传 ──┘  ← 选中对端 + 选文件即可发送
//
//  ★ 纪律：
//      · 只监听 **IPv4 局域网**、只在用户**启用**时监听，停用立刻关端口
//      · 收到的路径一律经 LanTransfer::resolveSharePath（挡 `..` / 盘符 / 保留设备名）
//      · 传完/失败/被拒都要在界面上留下记录（失败绝不留白）
//      · UDP 发现只是"方便"，发现不到也能手输地址发（不把可用性押在广播上）
// ============================================================================

#include "lan_http_server.h"
#include "sdk/IFeaturePlugin.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTcpSocket;
class QUdpSocket;

namespace WinEase::FeaturePlugins {

/// 局域网里发现的另一台 WinEase
struct LanPeer {
    QString deviceName;
    QString hostName;
    QHostAddress address;
    quint16 port = 0;
    QString key() const { return address.toString() + QLatin1Char(':') + QString::number(port); }
};

/// 往一台对端（或任意 HTTP 接收端）推一个文件。
/// ⚠ 用 QTcpSocket 流式发送：不把整个文件读进内存（要能发几个 GB 的录像）。
class LanFileSender : public QObject
{
    Q_OBJECT

public:
    explicit LanFileSender(QObject *parent = nullptr);

    /// 开始发送；返回 false 表示"连起步都没成功"（本地文件打不开等）
    bool start(const QHostAddress &address, quint16 port, const QString &filePath);
    void abort();
    bool isRunning() const;

Q_SIGNALS:
    void progress(const QString &fileName, qint64 sent, qint64 total);
    void finished(const QString &fileName, bool ok, const QString &detail);

private Q_SLOTS:
    void onConnected();
    void onBytesWritten(qint64 bytes);
    void onErrorOccurred();

private:
    void fail(const QString &detail);

    QTcpSocket *m_socket = nullptr;
    QFile m_file;
    QString m_fileName;
    qint64 m_total = 0;
    qint64 m_sent = 0;
    bool m_headerSent = false;
    bool m_running = false;
};

class LanTransferPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "lan_transfer_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit LanTransferPlugin(QObject *parent = nullptr);
    ~LanTransferPlugin() override;

    QString id() const override;
    QString name() const override;
    QString description() const override;
    QString detailedDescription() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    void startServer();
    void stopServer();
    void startDiscovery();
    void stopDiscovery();

    void onAnnounceReceived();
    void broadcastAnnounce();

    void appendLog(const QString &line);
    void refreshAddressLabel();
    void refreshPeerList();
    void sendFileToSelectedPeer();

    QString primaryLanAddress() const;
    quint16 configuredPort() const;
    QString configuredDirectory() const;

private:
    LanHttpServer m_server;
    LanFileSender m_sender;
    QUdpSocket *m_discovery = nullptr;
    QTimer m_announceTimer;

    QList<LanPeer> m_peers;

    // 设置面板控件
    QPointer<QLineEdit> m_directoryEdit;
    QPointer<QSpinBox> m_portSpin;
    QPointer<QLineEdit> m_nameEdit;
    QPointer<QPushButton> m_toggleButton;
    QPointer<QLabel> m_addressLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QListWidget> m_peerList;
    QPointer<QPushButton> m_sendButton;
    QPointer<QPlainTextEdit> m_logView;
    QPointer<QCheckBox> m_allowUploadCheck;

    QString m_lastLogLine;
    int m_transferCount = 0;
};

} // namespace WinEase::FeaturePlugins
