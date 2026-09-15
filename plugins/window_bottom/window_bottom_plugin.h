#pragma once

// ============================================================================
//  WindowBottomPlugin —— P1-04 窗口置底
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+B）把**目标窗口**钉到 z 序底部
//    * 同时给它加 WS_EX_NOACTIVATE —— 否则点一下它就被激活并弹回最前，
//      "置底"形同虚设（ROADMAP 验收要求"点击不激活到最前"）
//    * 用 SetWinEventHook(EVENT_SYSTEM_FOREGROUND) 维持：别的窗口被激活、
//      或该窗口被程序自己 ShowWindow 抬起来时，重新压回底部
//    * Ctrl+Shift+Alt+B 解除；停用/退出时还原扩展样式并卸掉钩子
//
//  注意与任务栏的关系：
//      HWND_BOTTOM 只是把窗口放到 z 序最底（桌面窗口之上），
//      不会盖住任务栏 —— 这正是想要的效果，不需要额外处理。
//      （如果哪天真的把窗口压到桌面窗口之下，它会彻底看不见，这里不做那种事）
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/WindowUtils.h"

#include "WindowFeatureState.h"

#include <QHash>

class WindowBottomPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "window_bottom_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit WindowBottomPlugin(QObject *parent = nullptr);
    ~WindowBottomPlugin() override;

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

    /// 把目标窗口钉到底部（同时压制其抢焦点）
    bool pinTargetToBottom();
    /// 释放全部被钉底的窗口（还原扩展样式 + 卸钩子）
    void releaseAll();

private:
    /// 安装 / 卸载前台窗口变化钩子（仅在有窗口被钉底时需要）
    void ensureHookInstalled();
    void removeHookIfIdle();
    /// 钩子回调：把仍处于钉底状态的窗口重新压到底部
    void pushPinnedWindowsDown();
    static void CALLBACK foregroundEventProc(HWINEVENTHOOK hook,
                                             DWORD event,
                                             HWND hwnd,
                                             LONG idObject,
                                             LONG idChild,
                                             DWORD eventThread,
                                             DWORD eventTime);

    struct BottomRecord {
        WinEase::Win32::ExStyleSnapshot exStyle; ///< 用于还原"不抢焦点"以外的原始样式
        quint32 processId = 0;
        QString className;
    };

    WinEase::FeaturePlugins::WindowTargetState m_target;
    QHash<quintptr, BottomRecord> m_pinned;
    HWINEVENTHOOK m_hook = nullptr;
    bool m_pushing = false; ///< 防重入：回调里也要改 z 序
};
