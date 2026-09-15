#pragma once

// ============================================================================
//  ClipboardHistoryPlugin —— P2-04 剪贴板历史管理（含剪贴板隐私清理，D4 合并功能）
//
//  行为：
//    * `default`（Ctrl+Alt+V）：打开历史面板（搜索 / 收藏 / 分类 / 粘贴 / 删除 / 清空）
//    * `privacy_clear`（Ctrl+Alt+X）：**只清空系统剪贴板**，不动历史
//      —— 复制完密码后立刻把剪贴板抹掉，是"隐私清理"最常用的那一半
//    * 后台：监听 `QClipboard::dataChanged`，把每条内容写进分片文件历史
//
//  ★ 三条纪律：
//    ① **隐私闸门优先于一切**：来源挂着「不记录」标记（密码管理器、浏览器、
//       Windows 自己的 CanIncludeInClipboardHistory=0）→ 一个字都不写进磁盘；
//       连"剪贴板打不开、无法判定"也按"不记录"处理（宁可漏记，不可误记）；
//    ② 停用功能**不删用户的历史**：历史是用户自己的数据，不是系统状态。
//       停用只做两件事 —— 停止监听、关掉面板；历史留在磁盘上，重新启用即在；
//    ③ 界面上说清三件事：单条上限、条数上限（收藏不计入）、
//       图片只存了缩略图时**粘回去的也是缩略图**（不能让用户以为拿回了原图）。
//
//  ⚠ 有意不做：不监听键盘去"抓"复制（那样会拿到密码框里的东西）；
//     不记录自定义二进制格式（无法还原成可用内容，记了也没用）。
// ============================================================================

#include "ClipboardHistoryStore.h"
#include "sdk/IFeaturePlugin.h"

#include <QPointer>
#include <QStringList>

namespace ClipStore = WinEase::FeaturePlugins::ClipboardHistoryStore;

class ClipboardHistoryPanel;

class ClipboardHistoryPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "clipboard_history_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit ClipboardHistoryPlugin(QObject *parent = nullptr);

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
    bool hasSettings() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

    // ---------------- 面板与自检共用的接口 ----------------
    /// 打开（或前置）历史面板
    void openPanel();
    bool isPanelOpen() const;

    int entryCount() const;
    QVector<ClipStore::Entry> search(const QString &keyword, const QString &filterKey) const;
    bool setFavorite(const QString &id, bool favorite);
    bool removeEntry(const QString &id, QString *messageOut = nullptr);
    /// 一键清空；@param alsoClearClipboard 同时清空系统剪贴板（面板按钮就是这条路）
    /// @return 删除的条数
    int clearAll(bool alsoClearClipboard, QString *messageOut);
    /// 把某条写回剪贴板；@param pasteBack true 时再向当前前台窗口发一次 Ctrl+V
    bool activate(const QString &id, bool pasteBack, QString *messageOut);

    /// 界面右栏要显示的正文：文本取正文（头部）、文件取路径清单、图片给尺寸说明。
    /// 正文文件丢了/读不了 → 如实返回失败原因（不装作还有内容）
    QString entryBodyText(const QString &id, QString *errorOut = nullptr) const;

    QString storeDirectory() const;
    /// 本次启用以来记录了多少条（"已记录"累计值）
    int capturedCount() const { return m_capturedCount; }
    /// 因隐私标记被跳过多少条（用户能在界面上看到这个数，这是本功能的安全感来源）
    int skippedPrivacyCount() const { return m_skippedPrivacyCount; }
    /// 因超过单条上限被跳过多少条
    int skippedOversizeCount() const { return m_skippedOversizeCount; }
    /// 最近一次跳过的中文原因（空 = 没跳过）
    QString lastSkipReason() const { return m_lastSkipReason; }

    int maxItems() const { return m_maxItems; }
    qint64 maxItemBytes() const { return m_maxItemBytes; }
    QString imageModeKey() const { return m_imageMode; }
    bool confirmBeforeClear() const { return m_confirm; }
    bool pasteBackOnActivate() const { return m_pasteBack; }

Q_SIGNALS:
    /// 历史发生变化（面板据此刷新）
    void historyChanged();

    /// 跳过计数/最近跳过原因等**统计信息**变了，但历史内容没变
    /// （隐私跳过、超上限跳过都属这一类）。
    /// 面板页脚上写的就是这些数字 —— 它们不刷新，用户就看不到
    /// "刚刚那条为什么没进来"，而这恰恰是本功能最需要说清的一件事。
    void statisticsChanged();

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    void loadFromConfig();
    void closePanel();
    /// 剪贴板变化 → 采集（含隐私闸门）
    void onClipboardChanged();
    void handleAddResult(ClipStore::AddResult result, const QString &preview, const QString &error);
    /// 当前前台进程名（复制动作多半来自它）；取不到返回空串
    QString foregroundAppName() const;
    /// 把系统剪贴板清空（算"我们自己的写入"，不会反过来记一条历史）
    bool clearSystemClipboard(QString *messageOut);

    ClipStore::Store m_store;
    QString m_storageDir;      ///< 落配置的目录（空 = 从 configFilePath 推导）
    int m_maxItems = 200;
    qint64 m_maxItemBytes = 256 * 1024;
    QString m_imageMode = QStringLiteral("thumbnail"); ///< off / thumbnail / full
    bool m_confirm = true;     ///< 清空前是否二次确认
    bool m_pasteBack = false;  ///< 「粘贴」是否真的向前台窗口注入 Ctrl+V

    bool m_captureEnabled = true;
    /// 我们自己在写剪贴板（回填/清空）→ 下一次 dataChanged 不算新内容
    bool m_selfWrite = false;

    int m_capturedCount = 0;
    int m_skippedPrivacyCount = 0;
    int m_skippedOversizeCount = 0;
    QString m_lastSkipReason;

    QPointer<ClipboardHistoryPanel> m_panel;
};
