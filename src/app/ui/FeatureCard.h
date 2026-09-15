#pragma once

// ============================================================================
//  FeatureCard.h —— 单个功能卡片
//
//  卡片包含：图标 | 功能名 | 描述 | 运行状态指示 | 开关 | 设置按钮
// ============================================================================

#include <QFrame>
#include <QString>

class QCheckBox;
class QLabel;
class QToolButton;

namespace WinEase {

class IFeaturePlugin;

class FeatureCard : public QFrame
{
    Q_OBJECT

public:
    explicit FeatureCard(IFeaturePlugin *plugin, QWidget *parent = nullptr);
    ~FeatureCard() override;

    IFeaturePlugin *plugin() const { return m_plugin; }
    QString pluginId() const;

    /// 关键词匹配：功能名 / 描述 / id / 标签
    bool matches(const QString &keyword) const;

    /// 卡片的固定尺寸（供网格布局计算列数）
    static QSize cardSize();

    /// 把卡片标记为"功能已崩溃/被隔离"（P0-5）。
    /// ⚠ 本函数**不会调用插件的任何方法**（崩溃后插件对象可能已损坏），
    ///   之后 refresh() 也会跳过插件调用，只更新状态区
    void markFailed(const QString &reason);

public Q_SLOTS:
    /// 从插件同步开关状态、状态文本与错误信息
    void refresh();

Q_SIGNALS:
    void toggleRequested(WinEase::IFeaturePlugin *plugin, bool enabled);
    void settingsRequested(WinEase::IFeaturePlugin *plugin);

protected:
    /// 宽度变了要按新的可用宽度重新"压到两行"（卡片宽度固定，但字体/DPI 会变）
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupUi();
    void updateStatusIndicator();
    /// 把描述压到**固定两行**（超出省略），完整文本放在 Tooltip 里
    void updateDescriptionElide();

    IFeaturePlugin *m_plugin = nullptr;
    /// 构造期缓存的插件 id（崩溃隔离后不再调用插件的 id()）
    QString m_pluginId;
    /// 搜索匹配文本（refresh() 时刷新，隔离后继续可用）
    QString m_searchText;
    /// 描述全文（卡片上只显示两行，全文进 Tooltip；宽度变化时要按它重算）
    QString m_descriptionText;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLabel *m_descriptionLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_adminBadge = nullptr;
    QCheckBox *m_switch = nullptr;
    QToolButton *m_settingsButton = nullptr;
    bool m_syncing = false;
    /// 非空表示该功能已崩溃并被隔离：卡片只显示原因，不再读写插件
    QString m_failureReason;
};

} // namespace WinEase
