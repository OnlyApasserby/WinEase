#include "usb_control_plugin.h"

#include "sdk/PluginServices.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::RemovalClass;
using WinEase::Win32::StorageDeviceInfo;

/// 列表刷新周期：设备插拔的即时反馈（枚举本身只读且很轻）
constexpr int kRefreshIntervalMs = 5000;

/// 二次确认的文案（自检会断言"没勾选就点不动"）
const QString kConfirmText = QStringLiteral("我确认该设备上没有正在进行的写入");

} // namespace

UsbControlPlugin::UsbControlPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString UsbControlPlugin::id() const
{
    return QStringLiteral("security.usb_control");
}

QString UsbControlPlugin::name() const
{
    return QStringLiteral("USB 设备管控");
}

QString UsbControlPlugin::description() const
{
    return QStringLiteral("列出可安全移除的存储设备并逐个弹出，占用时说明是谁在阻止");
}

QIcon UsbControlPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Security);
}

WinEase::FeatureCategory UsbControlPlugin::category() const
{
    return WinEase::FeatureCategory::Security;
}

QStringList UsbControlPlugin::tags() const
{
    return { QStringLiteral("USB"), QStringLiteral("U盘"), QStringLiteral("移动硬盘"),
             QStringLiteral("安全弹出"), QStringLiteral("usb") };
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool UsbControlPlugin::initialize()
{
    if (m_refreshTimer == nullptr) {
        m_refreshTimer = new QTimer(this);
        m_refreshTimer->setInterval(kRefreshIntervalMs);
        connect(m_refreshTimer, &QTimer::timeout, this, [this] { refreshDevices(); });
    }

    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("USB 设备管控已就绪"));
    return true;
}

void UsbControlPlugin::shutdown()
{
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
    }
}

bool UsbControlPlugin::canEnable(QString *reason) const
{
    // 没有硬前提：枚举是只读的，没有可移除设备时列表为空即可（如实显示）
    Q_UNUSED(reason)
    return true;
}

bool UsbControlPlugin::onEnable()
{
    refreshDevices();

    if (m_refreshTimer != nullptr) {
        m_refreshTimer->start();
    }

    Q_EMIT statusMessage(statusText());

    // ⚠ 时序（踩坑 #51 / #57）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void UsbControlPlugin::onDisable()
{
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
    }
    m_lastEvent = QStringLiteral("已停用（未弹出任何设备）");
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  枚举与弹出
// ---------------------------------------------------------------------------

void UsbControlPlugin::refreshDevices()
{
    QString error;
    m_devices = WinEase::Win32::removableStorageDevices(&error);

    if (!error.isEmpty()) {
        setLastError(error);
        m_lastEvent = QStringLiteral("枚举存储设备失败：%1").arg(error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
    } else {
        clearLastError();
        m_lastEvent = m_devices.isEmpty()
                          ? QStringLiteral("当前没有可安全移除的设备")
                          : QStringLiteral("检测到 %1 个可安全移除的设备").arg(m_devices.size());
    }

    refreshPanel();
}

bool UsbControlPlugin::selectedDevice(StorageDeviceInfo *out) const
{
    if (m_deviceList.isNull()) {
        return false;
    }
    const int row = m_deviceList->currentRow();
    if (row < 0 || row >= m_devices.size()) {
        return false;
    }
    if (out != nullptr) {
        *out = m_devices.at(row);
    }
    return true;
}

void UsbControlPlugin::ejectSelected()
{
    StorageDeviceInfo device;
    if (!selectedDevice(&device)) {
        m_lastEjectResult = QStringLiteral("请先在列表里选中一块设备");
        refreshPanel();
        return;
    }

    // ★ 二次确认：弹出会摘掉设备，正在写入时可能丢数据。
    //   本插件不提供"强制弹出"，也不替用户决定"数据可以丢"
    if (m_confirmCheck.isNull() || !m_confirmCheck->isChecked()) {
        m_lastEjectResult = QStringLiteral("未执行：请先勾选「%1」").arg(kConfirmText);
        Q_EMIT statusMessage(m_lastEjectResult);
        refreshPanel();
        return;
    }

    QString error;
    QString veto;
    const bool ok = WinEase::Win32::ejectStorageDevice(device, &error, &veto);

    if (ok) {
        clearLastError();
        m_lastEjectResult = QStringLiteral("已安全弹出：%1").arg(device.describe());
        Q_EMIT statusMessage(m_lastEjectResult);
    } else {
        setLastError(error);
        // 失败时**优先把 veto 原因说清楚**（"谁在用"比"失败了"有用得多）
        m_lastEjectResult = veto.isEmpty()
                                ? QStringLiteral("弹出失败：%1").arg(error)
                                : QStringLiteral("弹出失败：%1").arg(veto);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEjectResult);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), m_lastEjectResult);
        }
        Q_EMIT statusMessage(m_lastEjectResult);
    }

    if (m_confirmCheck != nullptr) {
        m_confirmCheck->setChecked(false); // 一次确认只对一次弹出有效
    }
    refreshDevices();
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QString UsbControlPlugin::statusText() const
{
    return m_lastEvent.isEmpty() ? QStringLiteral("就绪") : m_lastEvent;
}

QWidget *UsbControlPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("usbPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("usbStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *deviceList = new QListWidget(widget);
    deviceList->setObjectName(QStringLiteral("usbDeviceList"));
    deviceList->setMinimumHeight(140);
    layout->addWidget(deviceList);

    auto *detailLabel = new QLabel(widget);
    detailLabel->setObjectName(QStringLiteral("usbDetailLabel"));
    detailLabel->setWordWrap(true);
    detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailLabel);

    auto *confirmCheck = new QCheckBox(kConfirmText, widget);
    confirmCheck->setObjectName(QStringLiteral("usbConfirmCheck"));
    layout->addWidget(confirmCheck);

    auto *buttonRow = new QHBoxLayout();
    auto *ejectButton = new QPushButton(QStringLiteral("安全弹出所选设备"), widget);
    ejectButton->setObjectName(QStringLiteral("usbEjectButton"));
    buttonRow->addWidget(ejectButton);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新列表"), widget);
    refreshButton->setObjectName(QStringLiteral("usbRefreshButton"));
    buttonRow->addWidget(refreshButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *ejectResultLabel = new QLabel(widget);
    ejectResultLabel->setObjectName(QStringLiteral("usbEjectResultLabel"));
    ejectResultLabel->setWordWrap(true);
    layout->addWidget(ejectResultLabel);

    auto *hint = new QLabel(
        QStringLiteral("「可安全移除」是按**总线与 PnP 的可移除策略**判断的（不是盘符类型）——"
                       "移动硬盘在 Windows 里通常被当成固定盘，"
                       "按盘符类型筛会把它漏掉，而它恰恰最需要安全弹出。\n"
                       "弹出前请先勾选确认：设备上可能有正在写入的数据，"
                       "本功能**不提供强制弹出**、也**不做「一键全弹」**。\n"
                       "弹出失败时会尽量说清**是谁在占用**（应用名 / 服务名 / 还有未关闭的句柄）。"),
        widget);
    hint->setObjectName(QStringLiteral("usbHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_detailLabel = detailLabel;
    m_ejectResultLabel = ejectResultLabel;
    m_deviceList = deviceList;
    m_ejectButton = ejectButton;
    m_refreshButton = refreshButton;
    m_confirmCheck = confirmCheck;

    connect(ejectButton, &QPushButton::clicked, widget, [this] { ejectSelected(); });
    connect(refreshButton, &QPushButton::clicked, widget, [this] { refreshDevices(); });
    connect(confirmCheck, &QCheckBox::toggled, widget, [this](bool) { refreshPanel(); });
    // ⚠ 这里**只刷新详情**，绝不回调 `refreshPanel()`：
    //    refreshPanel() 会重填设备列表，而重填会改变当前行 → 再次发出 currentRowChanged
    //    → 再进这个槽 → 无限递归（实测直接把自检打成 0xC00000FD 栈溢出）。
    //    选中项变化不影响任何按钮的启用状态（弹出按钮只看"有设备 + 已确认"），
    //    所以根本不需要刷新整个面板。
    connect(deviceList, &QListWidget::currentRowChanged, widget, [this](int) { updateDetail(); });

    refreshPanel();
    return widget;
}

void UsbControlPlugin::updateDetail()
{
    if (m_detailLabel.isNull()) {
        return;
    }

    StorageDeviceInfo device;
    if (!selectedDevice(&device)) {
        m_detailLabel->setText(QStringLiteral("（选中一块设备查看详情）"));
        return;
    }

    const QString freeText = device.isReady
                                 ? WinEase::Win32::humanizeBytes(device.freeBytes)
                                       + QStringLiteral(" / ")
                                       + WinEase::Win32::humanizeBytes(device.totalBytes)
                                       + QStringLiteral(" 可用")
                                 : QStringLiteral("无介质或未就绪");
    m_detailLabel->setText(QStringLiteral("%1\n文件系统：%2 · 总线：%3 · 可移除性：%4\n"
                                          "容量：%5\n%6")
                               .arg(device.describe(),
                                    device.fileSystem.isEmpty() ? QStringLiteral("未知")
                                                                : device.fileSystem,
                                    device.busText,
                                    WinEase::Win32::removalClassText(device.removal),
                                    freeText,
                                    device.isUsbStorage
                                        ? QStringLiteral("（USB 总线上的存储设备）")
                                        : QStringLiteral("（非 USB 总线，但系统标记为可移除）")));
}

void UsbControlPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    const bool running = isEnabled();

    if (!m_statusLabel.isNull()) {
        m_statusLabel->setText(statusText());
    }
    if (!m_ejectResultLabel.isNull()) {
        m_ejectResultLabel->setText(m_lastEjectResult);
    }

    if (!m_deviceList.isNull()) {
        // ⚠ 重填列表期间**必须屏蔽信号**：clear()/setCurrentRow() 都会发 currentRowChanged，
        //    不挡就会和"选中项变化 → 刷新详情"的回调互相触发（经典递归陷阱）
        QSignalBlocker blocker(m_deviceList.data());

        // 保持选中项：按"设备路径 + 盘符"匹配，而不是按下标（插拔会改变顺序）
        QString keepKey;
        const int previous = m_deviceList->currentRow();
        if (previous >= 0 && previous < m_devices.size()) {
            keepKey = m_devices.at(previous).devicePath + m_devices.at(previous).driveLetter;
        }

        m_deviceList->clear();
        int restoreRow = -1;
        for (int index = 0; index < m_devices.size(); ++index) {
            const StorageDeviceInfo &device = m_devices.at(index);
            auto *item = new QListWidgetItem(device.describe(), m_deviceList.data());
            item->setToolTip(device.volumePath.isEmpty()
                                 ? QStringLiteral("（无盘符）")
                                 : device.volumePath);
            if (!keepKey.isEmpty() && restoreRow < 0
                && device.devicePath + device.driveLetter == keepKey) {
                restoreRow = index;
            }
        }
        if (restoreRow >= 0) {
            m_deviceList->setCurrentRow(restoreRow);
        } else if (m_deviceList->count() > 0 && m_deviceList->currentRow() < 0) {
            m_deviceList->setCurrentRow(0);
        }
        m_deviceList->setEnabled(running);
    }

    if (!m_confirmCheck.isNull()) {
        m_confirmCheck->setEnabled(running && !m_devices.isEmpty());
        if (!running && m_confirmCheck->isChecked()) {
            QSignalBlocker blocker(m_confirmCheck.data()); // 同理：别让这次清除再触发刷新
            m_confirmCheck->setChecked(false);
        }
    }
    if (!m_ejectButton.isNull()) {
        // 三重门槛：功能启用 + 有设备 + 已勾选确认
        m_ejectButton->setEnabled(running && !m_devices.isEmpty()
                                  && m_confirmCheck != nullptr && m_confirmCheck->isChecked());
    }
    if (!m_refreshButton.isNull()) {
        m_refreshButton->setEnabled(running);
    }
    if (!m_deviceList.isNull() && m_deviceList->count() == 0) {
        m_deviceList->clear();
    }

    updateDetail();
}
