#include "ui/CategoryNavWidget.h"

#include "core/Logging.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace WinEase {

namespace {
/// 导航条目的固定高度
constexpr int kItemHeight = 42;
} // namespace

CategoryNavWidget::CategoryNavWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

CategoryNavWidget::~CategoryNavWidget() = default;

void CategoryNavWidget::setupUi()
{
    setObjectName(QStringLiteral("CategoryNav"));
    setFixedWidth(212);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 16, 12, 12);
    layout->setSpacing(6);

    m_titleLabel = new QLabel(QStringLiteral("WinEase"), this);
    m_titleLabel->setObjectName(QStringLiteral("NavTitle"));
    m_subtitleLabel = new QLabel(QStringLiteral("Windows 易用性增强工具集"), this);
    m_subtitleLabel->setObjectName(QStringLiteral("NavSubtitle"));

    layout->addWidget(m_titleLabel);
    layout->addWidget(m_subtitleLabel);
    layout->addSpacing(10);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("CategoryList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);

    // "全部功能" 置于列表首位
    appendCategory(FeatureCategory::Unknown);
    for (FeatureCategory category : Category::all()) {
        appendCategory(category);
    }

    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                if (m_updating || !current) {
                    return;
                }
                const auto category = static_cast<FeatureCategory>(current->data(Qt::UserRole).toInt());
                qCDebug(lcUi) << "切换分类:" << Category::displayName(category);
                Q_EMIT categoryActivated(category);
            });

    m_list->setCurrentRow(0);
}

void CategoryNavWidget::appendCategory(FeatureCategory category)
{
    auto *item = new QListWidgetItem(Category::icon(category), QString(), m_list);
    item->setData(Qt::UserRole, static_cast<int>(category));
    item->setToolTip(Category::description(category));
    item->setSizeHint(QSize(0, kItemHeight));

    m_items.insert(static_cast<int>(category), item);
    m_counts.insert(static_cast<int>(category), 0);
    updateItemText(category);
}

void CategoryNavWidget::updateItemText(FeatureCategory category)
{
    QListWidgetItem *item = itemFor(category);
    if (!item) {
        return;
    }

    const int count = m_counts.value(static_cast<int>(category), 0);
    const QString name = Category::displayName(category);
    item->setText(count > 0 ? QStringLiteral("%1  (%2)").arg(name).arg(count) : name);
}

QListWidgetItem *CategoryNavWidget::itemFor(FeatureCategory category) const
{
    return m_items.value(static_cast<int>(category), nullptr);
}

FeatureCategory CategoryNavWidget::currentCategory() const
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        return FeatureCategory::Unknown;
    }
    return static_cast<FeatureCategory>(item->data(Qt::UserRole).toInt());
}

void CategoryNavWidget::setCurrentCategory(FeatureCategory category)
{
    QListWidgetItem *item = itemFor(category);
    if (!item || item == m_list->currentItem()) {
        return;
    }
    m_list->setCurrentItem(item);
}

void CategoryNavWidget::setCategoryCounts(const QHash<int, int> &counts)
{
    m_counts = counts;

    int total = 0;
    for (FeatureCategory category : Category::all()) {
        const int key = static_cast<int>(category);
        const int count = m_counts.value(key, 0);
        total += count;
        updateItemText(category);
    }
    m_counts.insert(static_cast<int>(FeatureCategory::Unknown), total);
    updateItemText(FeatureCategory::Unknown);
}

} // namespace WinEase
