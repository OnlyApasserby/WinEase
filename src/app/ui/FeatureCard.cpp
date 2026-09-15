#include "ui/FeatureCard.h"

#include "core/AdminHelper.h"
#include "core/Logging.h"
#include "sdk/IFeaturePlugin.h"

#include <QCheckBox>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QStringList>
#include <QStyle>
#include <QTextLayout>
#include <QTextOption>
#include <QToolButton>
#include <QVBoxLayout>

namespace WinEase {

namespace {

constexpr int kCardWidth = 336;
constexpr int kCardHeight = 150;
constexpr int kIconSize = 34;
/// 卡片上留给描述的行数（**固定两行**；超出部分进 Tooltip，不再挤在卡片上）
constexpr int kDescriptionLines = 2;

/// 把文本压到**最多 maxLines 行**：按**实际宽度**断行，超出部分在末行末尾加省略号。
///
/// 为什么不用"按字符数截断"（原来是 `text.left(45) + '…'`）：
///   卡片宽度固定，但**字体和 DPI** 会变 —— 按字数截断在有的机器上会挤成三行、
///   在另一些机器上只显示半行；而"固定两行"这件事只有**按排版结果**切才准。
/// 为什么用 `QTextLayout` 而不是 `QFontMetrics::elidedText`：
///   `elidedText` 是**单行**省略，管不了换行后的第二行。
QString elideToLines(const QString &text, const QFont &font, int width, int maxLines)
{
    if (text.isEmpty() || width <= 0 || maxLines < 1) {
        return text;
    }

    // ⚠ `QTextLayout` 要的是 QFont（`QFontMetrics` 在 Qt 6 里没有 font()）
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    int visibleLines = 0;
    int overflowStart = -1;
    layout.beginLayout();
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break; // 文本到此结束 —— 没有超出
        }
        line.setLineWidth(width);
        if (visibleLines >= maxLines) {
            overflowStart = line.textStart(); // 第 maxLines+1 行的起点 = 该截断的位置
            break;
        }
        ++visibleLines;
    }
    layout.endLayout();

    if (overflowStart < 0) {
        return text; // 两行以内，原样返回（这时 Tooltip 与正文内容一致）
    }

    QString kept = text.left(overflowStart).trimmed();
    // 行尾别留下孤零零的标点/空白
    const QString trailing = QStringLiteral(" \t\r\n，。、；：,.!?;:");
    while (!kept.isEmpty() && trailing.contains(kept.at(kept.size() - 1))) {
        kept.chop(1);
    }
    return kept + QChar(0x2026);
}

} // namespace

FeatureCard::FeatureCard(IFeaturePlugin *plugin, QWidget *parent)
    : QFrame(parent)
    , m_plugin(plugin)
{
    // 构造期缓存 id：插件崩溃后被隔离时，界面仍然需要 id 做索引，
    // 而此刻已经不允许再调用插件的任何方法（见 markFailed 注释）
    m_pluginId = plugin ? plugin->id() : QString();

    setupUi();
    refresh();

    if (m_plugin) {
        connect(m_plugin, &IFeaturePlugin::enabledChanged, this, &FeatureCard::refresh);
        connect(m_plugin, &IFeaturePlugin::stateChanged, this, &FeatureCard::refresh);
        connect(m_plugin, &IFeaturePlugin::statusMessage, this, &FeatureCard::refresh);
        connect(m_plugin, &IFeaturePlugin::errorOccurred, this, &FeatureCard::refresh);
    }
}

FeatureCard::~FeatureCard() = default;

QSize FeatureCard::cardSize()
{
    return QSize(kCardWidth, kCardHeight);
}

void FeatureCard::setupUi()
{
    setObjectName(QStringLiteral("FeatureCard"));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumSize(kCardWidth, kCardHeight);
    setMaximumHeight(kCardHeight);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 14, 16, 12);
    rootLayout->setSpacing(10);

    // ---------------- 顶部：图标 + 名称 + 开关 ----------------
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(10);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(kIconSize, kIconSize);
    m_iconLabel->setScaledContents(true);

    auto *titleLayout = new QVBoxLayout();
    titleLayout->setSpacing(2);

    auto *nameRow = new QHBoxLayout();
    nameRow->setSpacing(6);

    m_nameLabel = new QLabel(this);
    m_nameLabel->setObjectName(QStringLiteral("CardName"));

    m_adminBadge = new QLabel(QStringLiteral("管理员"), this);
    m_adminBadge->setObjectName(QStringLiteral("AdminBadge"));
    m_adminBadge->setToolTip(QStringLiteral("该功能需要管理员权限"));
    m_adminBadge->setVisible(false);

    nameRow->addWidget(m_nameLabel);
    nameRow->addWidget(m_adminBadge);
    nameRow->addStretch(1);

    m_descriptionLabel = new QLabel(this);
    m_descriptionLabel->setObjectName(QStringLiteral("CardDescription"));
    // 允许换行：描述是**固定两行**（高度与省略都由 updateDescriptionElide() 负责）
    m_descriptionLabel->setWordWrap(true);

    titleLayout->addLayout(nameRow);
    titleLayout->addWidget(m_descriptionLabel);

    m_switch = new QCheckBox(this);
    m_switch->setObjectName(QStringLiteral("CardSwitch"));
    m_switch->setCursor(Qt::PointingHandCursor);
    m_switch->setToolTip(QStringLiteral("启用 / 停用该功能"));

    headerLayout->addWidget(m_iconLabel, 0, Qt::AlignTop);
    headerLayout->addLayout(titleLayout, 1);
    headerLayout->addWidget(m_switch, 0, Qt::AlignTop);

    rootLayout->addLayout(headerLayout);
    rootLayout->addStretch(1);

    // ---------------- 底部：状态指示 + 设置按钮 ----------------
    auto *footerLayout = new QHBoxLayout();
    footerLayout->setSpacing(8);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("CardStatus"));
    m_statusLabel->setText(QStringLiteral("已停用"));

    m_settingsButton = new QToolButton(this);
    m_settingsButton->setText(QStringLiteral("设置"));
    m_settingsButton->setObjectName(QStringLiteral("CardSettingsButton"));
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setAutoRaise(true);

    footerLayout->addWidget(m_statusLabel, 1);
    footerLayout->addWidget(m_settingsButton, 0, Qt::AlignRight);
    rootLayout->addLayout(footerLayout);

    connect(m_switch, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncing || !m_plugin) {
            return;
        }
        Q_EMIT toggleRequested(m_plugin, checked);
    });

    connect(m_settingsButton, &QToolButton::clicked, this, [this] {
        if (m_plugin) {
            Q_EMIT settingsRequested(m_plugin);
        }
    });
}

QString FeatureCard::pluginId() const
{
    return m_pluginId;
}

void FeatureCard::markFailed(const QString &reason)
{
    // 注意：这里不能清空 m_searchText —— 崩溃的功能仍应能被搜索到
    m_failureReason = reason;

    m_syncing = true;
    m_switch->setChecked(false);
    m_switch->setEnabled(false);
    m_syncing = false;
    m_settingsButton->setEnabled(false);

    updateStatusIndicator();
}

bool FeatureCard::matches(const QString &keyword) const
{
    const QString needle = keyword.trimmed();
    if (needle.isEmpty() || !m_plugin) {
        return true;
    }
    return m_searchText.contains(needle, Qt::CaseInsensitive);
}

void FeatureCard::refresh()
{
    if (!m_plugin) {
        return;
    }

    // 已崩溃隔离：插件对象可能已损坏，不再读取它的任何字段
    if (!m_failureReason.isEmpty()) {
        updateStatusIndicator();
        return;
    }

    // 搜索匹配用的文本一并缓存，隔离后 matches() 仍可正常工作
    m_searchText = QStringLiteral("%1 %2 %3 %4")
                       .arg(m_plugin->name(), m_plugin->description(), m_pluginId,
                            m_plugin->tags().join(QLatin1Char(' ')));

    // 名称与描述：卡片上只显示**两行**，完整文本进 Tooltip
    m_nameLabel->setText(m_plugin->name());
    m_descriptionText = m_plugin->description();
    m_descriptionLabel->setToolTip(m_descriptionText);
    updateDescriptionElide();

    // 图标（插件未提供时回退到分类图标）
    const QIcon pluginIcon = m_plugin->icon();
    const QIcon icon = pluginIcon.isNull() ? Category::icon(m_plugin->category()) : pluginIcon;
    m_iconLabel->setPixmap(icon.pixmap(kIconSize, kIconSize));

    // 卡片整体的 Tooltip = 完整描述。
    // ⚠ 标识（id）与版本号**不在这里了**：它们已迁到「帮助 → 关于插件」
    //   （卡片本来就小，把 id/版本堆在 tooltip 里既挤又没有使用场景）。
    setToolTip(m_descriptionText);

    // 管理员标记
    const bool needAdmin = m_plugin->requiresAdmin();
    m_adminBadge->setVisible(needAdmin);
    if (needAdmin && !Admin::isProcessElevated()) {
        m_adminBadge->setText(QStringLiteral("需提权"));
        m_adminBadge->setProperty("state", QStringLiteral("danger"));
    } else if (needAdmin) {
        m_adminBadge->setText(QStringLiteral("管理员"));
        m_adminBadge->setProperty("state", QStringLiteral("ok"));
    } else {
        m_adminBadge->setProperty("state", QString());
    }
    m_adminBadge->style()->unpolish(m_adminBadge);
    m_adminBadge->style()->polish(m_adminBadge);

    // 设置按钮：插件未提供设置面板时禁用
    m_settingsButton->setEnabled(m_plugin->hasSettings());

    // 开关（避免触发 toggled 造成递归）
    m_syncing = true;
    m_switch->setChecked(m_plugin->isEnabled());
    m_syncing = false;

    updateStatusIndicator();
}

void FeatureCard::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    updateDescriptionElide();
}

void FeatureCard::updateDescriptionElide()
{
    if (m_descriptionLabel == nullptr) {
        return;
    }

    const QFontMetrics metrics = m_descriptionLabel->fontMetrics();
    const int width = m_descriptionLabel->width();

    // 首帧（还没被 QSS polish / 还没布局）时宽度是 Qt 的默认值、字体也可能不是样式表里的 ——
    // 这一帧先按整段放着，resizeEvent 会立刻用真实宽度与真实字体重算一次。
    if (width <= 40) {
        m_descriptionLabel->setText(m_descriptionText);
        return;
    }

    // 高度固定成两行：描述短的卡片也不会比别的卡片矮，整排看起来是齐的
    const int wantedHeight = metrics.lineSpacing() * kDescriptionLines;
    if (m_descriptionLabel->maximumHeight() != wantedHeight) {
        m_descriptionLabel->setFixedHeight(wantedHeight);
    }
    m_descriptionLabel->setText(
        elideToLines(m_descriptionText, m_descriptionLabel->font(), width, kDescriptionLines));
}

void FeatureCard::updateStatusIndicator()
{
    QString text;
    QString state;

    if (!m_plugin) {
        state = QStringLiteral("idle");
        text = QStringLiteral("不可用");
    } else if (!m_failureReason.isEmpty()) {
        // 崩溃隔离：原因由 PluginManager 提供（插件已不可调用）
        state = QStringLiteral("error");
        text = m_failureReason;
    } else {
        switch (m_plugin->state()) {
        case PluginState::Unloaded:
            state = QStringLiteral("idle");
            text = QStringLiteral("未加载");
            break;
        case PluginState::Loaded:
            state = QStringLiteral("idle");
            text = QStringLiteral("已停用");
            break;
        case PluginState::Running:
            state = QStringLiteral("running");
            text = QStringLiteral("运行中");
            break;
        case PluginState::Failed:
            state = QStringLiteral("error");
            text = m_plugin->lastError().isEmpty() ? QStringLiteral("加载失败") : m_plugin->lastError();
            break;
        }
    }

    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
    m_statusLabel->setProperty("state", state);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
}

} // namespace WinEase
