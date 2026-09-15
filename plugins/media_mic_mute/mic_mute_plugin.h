#pragma once

// ============================================================================
//  mic_mute_plugin.h —— P2-10 麦克风一键静音（media.mic_mute）
//
//  这个插件的全部难点只有一句话：**不要自己记状态**。
//
//  麦克风的静音状态会被太多人改：本插件、Windows 声音设置、耳机线上的麦键、
//  会议软件自己（进会议自动闭麦）……插件里只要存一个 bool，它迟早会和真实状态
//  分叉，而分叉的后果是"用户以为闭麦了、其实开着" —— 这恰恰是这个功能要防的事。
//  所以这里的每一次读写都问系统（`IAudioEndpointVolume`），并且启用期间**常驻轮询**：
//  外部改了状态，托盘徽标也要跟着变。
//
//  第二条纪律：**停用时不还原静音状态**。
//  项目常规是"停用即还原"（不留残留），但这里反过来：静音属于 fail-closed 的隐私
//  状态 —— 悄悄把用户的麦打开，比"留下一个静音"危险得多。停用只摘掉徽标，
//  麦克风保持原样（用户按一下麦键或再启用本功能即可）。
//
//  第三条：**写成功 ≠ 生效**。有些端点在独占模式下会接受 `SetMute` 却不变状态，
//  所以切换后必须回读，回读不一致就如实报错，绝不在界面上宣布一个没发生的结果。
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/CoreAudio.h"

#include <QPointer>
#include <QString>

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;
class QWidget;

class MicMutePlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "mic_mute_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit MicMutePlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 绑定默认采集（麦克风）端点；已绑定则直接成功
    bool ensureEndpoint(QString *errorOut);
    /// 读**系统真实状态**（失败返回 false，绝不用本地缓存兜底）
    bool readMuteState(bool *mutedOut, QString *errorOut) const;
    /// 取反并写回；写后回读确认，stateOut 返回"系统最终的真实状态"
    bool toggleMute(bool *stateOut, QString *errorOut);
    /// 把真实状态送到托盘徽标 + 卡片状态文本 + 面板
    /// @param force 状态没变时是否也重报（用户主动操作时为 true）
    void publishState(bool muted, bool force);
    /// 状态读不到时不要继续宣称旧状态：挂一个"未知"徽标
    void publishUnknownState(const QString &reason);
    /// 轮询：外部（耳机麦键 / 系统设置 / 会议软件）改了状态要跟上
    void syncExternalState();
    /// 读一次系统状态并刷新面板控件
    void refreshPanel();
    /// 面板上那个"静音/取消静音"按钮与快捷键走同一条路
    void toggleFromUser();

    WinEase::Win32::AudioEndpoint m_endpoint;
    QTimer *m_syncTimer = nullptr;

    /// 配置：切换时是否弹气泡提示（默认开，可在面板上关掉）
    bool m_notifyOnToggle = true;

    /// 上一次"报出去"的状态。轮询靠它判断"状态变了没有"——
    /// 注意它是**给轮询做参考**的，不是真相来源（真相永远是系统）
    bool m_publishedMuted = false;
    bool m_publishedValid = false;

    // 面板控件用 QPointer：面板归主窗口所有，可能比插件先销毁（见 ROADMAP 陷阱 21）
    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_stateLabel;
    QPointer<QLabel> m_deviceLabel;
    QPointer<QPushButton> m_toggleButton;
    QPointer<QCheckBox> m_notifyCheck;
};
