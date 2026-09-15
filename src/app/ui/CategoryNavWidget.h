#pragma once

// ============================================================================
//  CategoryNavWidget.h —— 左侧分类导航
//
//  以 QListWidget 实现，条目 = 分类（含"全部功能"），
//  每个条目右侧显示该分类下的功能数量。
// ============================================================================

#include "sdk/FeatureCategory.h"

#include <QHash>
#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;

namespace WinEase {

class CategoryNavWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CategoryNavWidget(QWidget *parent = nullptr);
    ~CategoryNavWidget() override;

    FeatureCategory currentCategory() const;

public Q_SLOTS:
    void setCurrentCategory(FeatureCategory category);
    /// 刷新各分类的功能数量（FeatureCategory::Unknown 为总数）
    void setCategoryCounts(const QHash<int, int> &counts);

Q_SIGNALS:
    void categoryActivated(WinEase::FeatureCategory category);

private:
    void setupUi();
    void appendCategory(FeatureCategory category);
    void updateItemText(FeatureCategory category);
    QListWidgetItem *itemFor(FeatureCategory category) const;

    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QListWidget *m_list = nullptr;
    QHash<int, QListWidgetItem *> m_items;
    QHash<int, int> m_counts;
    bool m_updating = false;
};

} // namespace WinEase
