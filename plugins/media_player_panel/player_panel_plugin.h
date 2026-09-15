#pragma once

// ============================================================================
//  P3-11 媒体控制面板（media.player_panel）
//
//  面板上所有能点的东西分成**两组**，界面上必须分得清（这是本功能最容易说假话的地方）：
//      * 「控制选中的会话」：走 SMTC 精确控制（`TryPauseAsync` 这一族），
//        打的就是列表里选中的那一行；
//      * 「发送媒体键」：走 `SendInput(VK_MEDIA_*)`，等价于按键盘上那颗键 ——
//        由**系统**决定交给哪个会话（当前会话），**不是**列表里选中的那个。
//
//  三条纪律：
//      1. **停用不改变任何播放状态**（谁也没在放歌，面板停掉不该动它）；
//      2. **读不到就写原因**（位置/时长读不到时绝不显示 0:00 或 0%，
//         模型层 `PlayerModel` 负责这件事）；
//      3. **只有用户点按钮才发控制命令** —— 轮询只读，不写。
//
//  自检用的固定 objectName（插件不导出符号，自检按名定位）：
//      playerStatusLabel / playerSessionList / playerArtworkLabel /
//      playerTitleLabel / playerArtistLabel / playerProgressSlider /
//      playerProgressLabel / playerPreviousButton / playerToggleButton /
//      playerNextButton / playerStopButton / playerSendPlayPauseButton /
//      playerSendPrevButton / playerSendNextButton / playerRefreshButton /
//      playerAutoRefreshCheck / playerPlayingOnlyCheck / playerDetailLabel /
//      playerHintLabel
//  列表项的数据槽：UserRole = 会话定位串（sessionId）
// ============================================================================

#include "PlayerModel.h"
#include "sdk/IFeaturePlugin.h"
#include "win32/MediaSessions.h"

#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>

class QCheckBox;
class QImage;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QWidget;

class MediaPlayerPanelPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "player_panel_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit MediaPlayerPanelPlugin(QObject *parent = nullptr);
    ~MediaPlayerPanelPlugin() override;

    // ---- 元信息 ----
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QString version() const override;
    QString author() const override;
    QString detailedDescription() const override;
    QStringList tags() const override;

    // ---- 能力标记 ----
    bool requiresAdmin() const override;
    bool supportsHotkey() const override;

    // ---- 生命周期 ----
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    bool canEnable(QString *reason) const override;

private:
    // ---- 轮询与刷新 ----
    void pollOnce();
    void refreshSessions(bool fromUser);
    void rebuildList();
    void restoreSelection();
    void refreshPanel();
    void updateDetail();
    void updateProgressView();
    void updateStatusLabel();

    // ---- 动作（只有用户操作会走到这里）----
    void applyCommand(WinEase::Win32::MediaCommand command, const QString &actionName);
    void applyMediaKey(WinEase::Win32::MediaKey key);
    void applySeek(int percent);
    void loadArtwork(const QString &sessionKey);

    // ---- 小工具 ----
    WinEase::FeaturePlugins::Player::PlayerRow currentRow() const;
    QString selectedKey() const;
    void publishStatus(const QString &text);
    void reportFailure(const QString &text);
    void scheduleFollowUpRefresh();

    QTimer *m_pollTimer = nullptr;
    QTimer *m_tickTimer = nullptr;

    bool m_autoRefresh = true;
    bool m_playingOnly = false;
    int m_pollSeconds = 2;

    WinEase::FeaturePlugins::Player::PlayerSnapshot m_snapshot;
    QString m_statusLine;
    QString m_enumError;
    QString m_selectedKey;
    QString m_artworkKey;
    QString m_lastAction;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QListWidget> m_sessionList;
    QPointer<QLabel> m_artworkLabel;
    QPointer<QLabel> m_titleLabel;
    QPointer<QLabel> m_artistLabel;
    QPointer<QSlider> m_progressSlider;
    QPointer<QLabel> m_progressLabel;
    QPointer<QPushButton> m_previousButton;
    QPointer<QPushButton> m_toggleButton;
    QPointer<QPushButton> m_nextButton;
    QPointer<QPushButton> m_stopButton;
    QPointer<QPushButton> m_sendPlayPauseButton;
    QPointer<QPushButton> m_sendPrevButton;
    QPointer<QPushButton> m_sendNextButton;
    QPointer<QPushButton> m_refreshButton;
    QPointer<QCheckBox> m_autoRefreshCheck;
    QPointer<QCheckBox> m_playingOnlyCheck;
    QPointer<QLabel> m_detailLabel;

    bool m_syncingUi = false;
    bool m_seeking = false;
};
