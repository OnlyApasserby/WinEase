#pragma once

// ============================================================================
//  FeatureGridView.h —— 功能卡片网格容器
//
//  自适应列数：根据可用宽度计算每行可容纳的卡片数，窗口缩放时自动重排。
// ============================================================================

#include <QList>
#include <QWidget>

class QGridLayout;

namespace WinEase {

class FeatureCard;

class FeatureGridView : public QWidget
{
    Q_OBJECT

public:
    explicit FeatureGridView(QWidget *parent = nullptr);
    ~FeatureGridView() override;

    /// 设置需要展示的卡片集合（未传入的卡片会被移出布局）
    void setCards(const QList<FeatureCard *> &cards);
    void clearCards();

    /// 重新计算列数并排布
    void relayout();

    int columnCount() const { return m_columnCount; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyLayout();

    QGridLayout *m_layout = nullptr;
    QList<FeatureCard *> m_cards;
    int m_columnCount = 0;
    int m_lastStretchRow = -1;
};

} // namespace WinEase
