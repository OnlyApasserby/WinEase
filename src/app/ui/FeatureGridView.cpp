#include "ui/FeatureGridView.h"

#include "ui/FeatureCard.h"

#include <QGridLayout>
#include <QResizeEvent>

#include <utility>

namespace WinEase {

namespace {
constexpr int kSpacing = 16;
} // namespace

FeatureGridView::FeatureGridView(QWidget *parent)
    : QWidget(parent)
{
    m_layout = new QGridLayout(this);
    m_layout->setContentsMargins(4, 4, 4, 4);
    m_layout->setHorizontalSpacing(kSpacing);
    m_layout->setVerticalSpacing(kSpacing);
    m_layout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
}

FeatureGridView::~FeatureGridView() = default;

void FeatureGridView::setCards(const QList<FeatureCard *> &cards)
{
    if (m_cards == cards) {
        // 集合未变时仍需保证布局正确（例如窗口尺寸变化）
        relayout();
        return;
    }

    m_cards = cards;
    relayout();
}

void FeatureGridView::clearCards()
{
    m_cards.clear();
    applyLayout();
}

void FeatureGridView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void FeatureGridView::relayout()
{
    const QSize cardSize = FeatureCard::cardSize();
    const int available = qMax(width() - m_layout->contentsMargins().left()
                                   - m_layout->contentsMargins().right(),
                               cardSize.width());
    const int columns = qMax(1, (available + kSpacing) / (cardSize.width() + kSpacing));

    if (columns == m_columnCount && m_layout->count() == m_cards.size()) {
        return;
    }

    m_columnCount = columns;
    applyLayout();
}

void FeatureGridView::applyLayout()
{
    // 清空布局（仅移除布局项，不销毁控件）
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        delete item;
    }

    if (m_lastStretchRow >= 0) {
        m_layout->setRowStretch(m_lastStretchRow, 0);
        m_lastStretchRow = -1;
    }

    const int columns = qMax(1, m_columnCount);
    int row = 0;
    int column = 0;

    for (FeatureCard *card : std::as_const(m_cards)) {
        if (!card) {
            continue;
        }
        card->setParent(this);
        card->setVisible(true);
        m_layout->addWidget(card, row, column, Qt::AlignTop | Qt::AlignLeft);

        if (++column >= columns) {
            column = 0;
            ++row;
        }
    }

    // 末尾留一个弹性行，使卡片整体靠上对齐
    const int stretchRow = (column == 0) ? row : row + 1;
    m_layout->setRowStretch(stretchRow, 1);
    m_lastStretchRow = stretchRow;

    // 重置可能的旧列拉伸，再为实际使用的列开放拉伸
    constexpr int kMaxTrackedColumns = 16;
    for (int c = 0; c < kMaxTrackedColumns; ++c) {
        m_layout->setColumnStretch(c, c < columns ? 1 : 0);
    }

    updateGeometry();
}

} // namespace WinEase
