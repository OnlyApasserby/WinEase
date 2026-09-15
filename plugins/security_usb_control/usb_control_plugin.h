#pragma once

// ============================================================================
//  usb_control_plugin.h —— P3-14 USB 设备管控（security.usb_control）
//
//  列出"可安全移除的存储设备"（U 盘 / 移动硬盘 / 外置 SSD），并提供安全弹出。
//
//  ---------------------------------------------------------------------------
//  四条设计纪律（每条都对应预研实测出来的事实）：
//
//   1. **"可移动"必须按总线与 PnP 策略判断，不能看盘符类型**：
//      移动硬盘在 Windows 里几乎总被报成 `DRIVE_FIXED`，用 `DRIVE_REMOVABLE` 会静默漏掉
//      它 —— 而它恰恰最需要安全弹出。判据统一在平台层 `StorageDevices` 里
//      （预研探针与插件共用同一份实现，避免"探针说一套、插件做一套"）。
//   2. **弹出前必须二次确认**：弹出会摘掉设备，正在写入时可能丢数据。
//      本插件要求用户勾选"我确认该设备上没有正在进行的写入"才允许点弹出 ——
//      这不是形式主义：它同时也是自检能验证的那一半（自检**绝不真弹出**用户的硬盘）。
//   3. **不做"一键弹出全部"**（有意裁剪，与路线图的"一键"原文有偏差）：
//      用户插着的移动硬盘很可能正在备份/挂着虚拟机。正确形态是**逐个 + 明确显示是哪块盘**。
//   4. **失败要说清是谁在占用**：`CM_Request_Device_Eject` 的 veto 机制会带回
//      阻止者的名字（应用名 / 服务名 / "还有未关闭的句柄"），比只报错误码有用得多。
//
//  ⚠ 本插件**不提供强制弹出**（不绕过 veto）：宁可弹不出来，也不能替用户决定"数据可以丢"。
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/StorageDevices.h"

#include <QList>
#include <QPointer>
#include <QString>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class QWidget;

class UsbControlPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "usb_control_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit UsbControlPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;

private:
    /// 重新枚举设备（只读）
    void refreshDevices();
    /// 对列表里选中的设备执行安全弹出（**先过二次确认**）
    void ejectSelected();
    /// 当前选中的设备（未选中返回 false）
    bool selectedDevice(WinEase::Win32::StorageDeviceInfo *out) const;

    void refreshPanel();
    void updateDetail();
    QString statusText() const;

    QList<WinEase::Win32::StorageDeviceInfo> m_devices;
    QString m_lastEvent;
    QString m_lastEjectResult;
    QTimer *m_refreshTimer = nullptr;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_detailLabel;
    QPointer<QLabel> m_ejectResultLabel;
    QPointer<QListWidget> m_deviceList;
    QPointer<QPushButton> m_ejectButton;
    QPointer<QPushButton> m_refreshButton;
    QPointer<QCheckBox> m_confirmCheck;
};
