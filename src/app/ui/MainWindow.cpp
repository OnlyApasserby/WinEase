#include "ui/MainWindow.h"

#include "core/AdminHelper.h"
#include "core/GlobalHotkeyManager.h"
#include "core/Logging.h"
#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "sdk/IFeaturePlugin.h"
#include "ui/CategoryNavWidget.h"
#include "ui/FeatureCard.h"
#include "ui/FeatureGridView.h"
#include "ui/HelpDialog.h"
#include "ui/HotkeySettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace WinEase {

namespace {

constexpr int kDefaultWindowWidth = 1180;
constexpr int kDefaultWindowHeight = 760;

/// 显式创建 QAction，避免 QWidget/QToolBar/QMenu 之间 addAction 重载歧义
QAction *makeAction(QObject *parent, const QString &text, std::function<void()> handler)
{
    auto *action = new QAction(text, parent);
    QObject::connect(action, &QAction::triggered, parent, [handler = std::move(handler)] { handler(); });
    return action;
}

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

MainWindow::MainWindow(PluginManager *pluginManager, GlobalHotkeyManager *hotkeyManager, QWidget *parent)
    : QMainWindow(parent)
    , m_pluginManager(pluginManager)
    , m_hotkeyManager(hotkeyManager)
{
    setObjectName(QStringLiteral("WinEaseMainWindow"));
    setWindowTitle(QStringLiteral("WinEase —— Windows 易用性增强工具集"));
    setWindowIcon(QIcon(QStringLiteral(":/winease/icons/app.svg")));
    resize(kDefaultWindowWidth, kDefaultWindowHeight);
    setMinimumSize(940, 620);

    setupUi();
    setupToolBar();
    setupMenuBar();
    setupStatusBar();
    connectSignals();

    rebuildCards();
    refreshStatistics();
}

MainWindow::~MainWindow() = default;

// ============================================================================
//  界面搭建
// ============================================================================

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("CentralWidget"));
    setCentralWidget(central);

    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ---------------- 左侧分类导航 ----------------
    m_nav = new CategoryNavWidget(central);
    rootLayout->addWidget(m_nav);

    // ---------------- 右侧内容区 ----------------
    auto *content = new QWidget(central);
    content->setObjectName(QStringLiteral("ContentArea"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(20, 18, 20, 12);
    contentLayout->setSpacing(14);

    // 顶部：标题 + 副标题 + 搜索框
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(12);

    auto *titleLayout = new QVBoxLayout();
    titleLayout->setSpacing(2);
    m_headerTitle = new QLabel(content);
    m_headerTitle->setObjectName(QStringLiteral("HeaderTitle"));
    m_headerSubtitle = new QLabel(content);
    m_headerSubtitle->setObjectName(QStringLiteral("HeaderSubtitle"));
    titleLayout->addWidget(m_headerTitle);
    titleLayout->addWidget(m_headerSubtitle);

    m_searchEdit = new QLineEdit(content);
    m_searchEdit->setObjectName(QStringLiteral("SearchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索功能（名称 / 描述 / 关键词）"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedWidth(300);
    m_searchEdit->setMinimumHeight(34);

    headerLayout->addLayout(titleLayout, 1);
    headerLayout->addWidget(m_searchEdit, 0, Qt::AlignVCenter);
    contentLayout->addLayout(headerLayout);

    // 中部：卡片网格
    m_grid = new FeatureGridView(content);
    m_emptyHint = new QLabel(QStringLiteral("没有匹配的功能，试试其它关键词或分类。"), content);
    m_emptyHint->setObjectName(QStringLiteral("EmptyHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setVisible(false);

    auto *gridContainer = new QWidget(content);
    auto *gridContainerLayout = new QVBoxLayout(gridContainer);
    gridContainerLayout->setContentsMargins(0, 0, 0, 0);
    gridContainerLayout->setSpacing(0);
    gridContainerLayout->addWidget(m_grid);
    gridContainerLayout->addWidget(m_emptyHint);
    gridContainerLayout->addStretch(1);

    m_scrollArea = new QScrollArea(content);
    m_scrollArea->setObjectName(QStringLiteral("CardScrollArea"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidget(gridContainer);
    contentLayout->addWidget(m_scrollArea, 1);

    rootLayout->addWidget(content, 1);
}

void MainWindow::setupToolBar()
{
    auto *toolBar = addToolBar(QStringLiteral("主工具栏"));
    toolBar->setObjectName(QStringLiteral("MainToolBar"));
    toolBar->setMovable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    toolBar->addAction(makeAction(this, QStringLiteral("全部启用"), [this] { setAllPluginsEnabled(true); }));
    toolBar->addAction(makeAction(this, QStringLiteral("全部停用"), [this] { setAllPluginsEnabled(false); }));
    toolBar->addSeparator();
    toolBar->addAction(makeAction(this, QStringLiteral("全局快捷键"), [this] { showHotkeySettings(); }));
    toolBar->addAction(makeAction(this, QStringLiteral("配置目录"), [this] {
        openPathInExplorer(SettingsManager::instance().dataDirectory());
    }));
    toolBar->addAction(makeAction(this, QStringLiteral("日志目录"), [this] {
        openPathInExplorer(SettingsManager::instance().logDirectory());
    }));
}

void MainWindow::setupMenuBar()
{
    // ---------------- 文件 ----------------
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(makeAction(this, QStringLiteral("最小化到托盘"), [this] {
        hide();
        Q_EMIT minimizeToTrayRequested();
    }));
    fileMenu->addAction(makeAction(this, QStringLiteral("打开配置目录"), [this] {
        openPathInExplorer(SettingsManager::instance().dataDirectory());
    }));
    fileMenu->addAction(makeAction(this, QStringLiteral("打开日志目录"), [this] {
        openPathInExplorer(SettingsManager::instance().logDirectory());
    }));
    fileMenu->addSeparator();
    fileMenu->addAction(makeAction(this, QStringLiteral("退出"), [this] {
        m_quitRequested = true;
        Q_EMIT quitRequested();
    }));

    // ---------------- 功能 ----------------
    QMenu *featureMenu = menuBar()->addMenu(QStringLiteral("功能(&E)"));
    featureMenu->addAction(makeAction(this, QStringLiteral("全部启用"), [this] { setAllPluginsEnabled(true); }));
    featureMenu->addAction(makeAction(this, QStringLiteral("全部停用"), [this] { setAllPluginsEnabled(false); }));
    featureMenu->addSeparator();
    featureMenu->addAction(makeAction(this, QStringLiteral("重新扫描插件"), [this] {
        const QString directory = QApplication::applicationDirPath() + QStringLiteral("/plugins");
        const int count = m_pluginManager ? m_pluginManager->loadPlugins(directory) : 0;
        rebuildCards();
        refreshStatistics();
        statusBar()->showMessage(QStringLiteral("已重新扫描插件目录，新增 %1 个。").arg(count), 4000);
    }));

    // ---------------- 设置 ----------------
    QMenu *settingsMenu = menuBar()->addMenu(QStringLiteral("设置(&S)"));
    settingsMenu->addAction(makeAction(this, QStringLiteral("全局快捷键…"), [this] { showHotkeySettings(); }));
    settingsMenu->addAction(makeAction(this, QStringLiteral("以管理员身份重启"), [this] {
        if (Admin::isProcessElevated()) {
            QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("当前已具备管理员权限。"));
            return;
        }
        if (Admin::restartAsElevated()) {
            m_quitRequested = true;
            Q_EMIT quitRequested();
        }
    }));

    // ---------------- 帮助 ----------------
    // 两个入口共用**同一个栏目式帮助对话框**（HelpDialog）：
    //   卡片上只留两行介绍，标识 / 版本号 / 完整说明都归到「关于插件」那一栏。
    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(makeAction(this, QStringLiteral("关于 WinEase"), [this] {
        showHelp(HelpDialog::AboutApp);
    }));
    helpMenu->addAction(makeAction(this, QStringLiteral("关于插件"), [this] {
        showHelp(HelpDialog::AboutPlugins);
    }));
}

void MainWindow::setupStatusBar()
{
    m_statusPluginCount = new QLabel(this);
    m_statusRunningCount = new QLabel(this);
    m_statusPrivilege = new QLabel(this);

    statusBar()->addPermanentWidget(m_statusPluginCount);
    statusBar()->addPermanentWidget(m_statusRunningCount);
    statusBar()->addPermanentWidget(m_statusPrivilege);
    statusBar()->showMessage(QStringLiteral("就绪"), 2000);
}

void MainWindow::connectSignals()
{
    connect(m_nav, &CategoryNavWidget::categoryActivated, this, &MainWindow::onCategoryChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);

    if (m_pluginManager) {
        connect(m_pluginManager, &PluginManager::pluginLoaded, this, &MainWindow::onPluginLoaded);
        connect(m_pluginManager, &PluginManager::pluginStateChanged, this, &MainWindow::onPluginStateChanged);
        connect(m_pluginManager, &PluginManager::pluginError, this,
                [this](const QString &pluginId, const QString &message) {
                    statusBar()->showMessage(QStringLiteral("[%1] %2").arg(pluginId, message), 6000);
                });
        // 插件崩溃被隔离：卡片立刻标记为失败并显示原因（P0-5）
        connect(m_pluginManager, &PluginManager::pluginCrashed, this,
                [this](const QString &pluginId, const QString &reason) {
                    if (FeatureCard *card = m_cards.value(pluginId, nullptr)) {
                        card->markFailed(reason);
                    }
                    refreshStatistics();
                    statusBar()->showMessage(
                        QStringLiteral("功能 %1 已崩溃并被隔离，其他功能不受影响。").arg(pluginId),
                        10000);
                });
    }
}

// ============================================================================
//  卡片构建与过滤
// ============================================================================

void MainWindow::rebuildCards()
{
    if (!m_pluginManager) {
        return;
    }

    const QList<IFeaturePlugin *> plugins = m_pluginManager->plugins();

    // ⚠ 一律使用管理器的"缓存读"（idOf/nameOf），而不是 plugin->id()：
    //   插件崩溃被隔离后不允许再调用插件对象
    QStringList aliveIds;
    for (IFeaturePlugin *plugin : plugins) {
        aliveIds.append(m_pluginManager->idOf(plugin));
    }
    for (auto it = m_cards.begin(); it != m_cards.end();) {
        if (!aliveIds.contains(it.key())) {
            it.value()->deleteLater();
            it = m_cards.erase(it);
        } else {
            ++it;
        }
    }

    // 为新插件创建卡片
    for (IFeaturePlugin *plugin : plugins) {
        const QString pluginId = m_pluginManager->idOf(plugin);
        if (m_cards.contains(pluginId)) {
            continue;
        }

        auto *card = new FeatureCard(plugin, this);
        connect(card, &FeatureCard::toggleRequested, this,
                [this](IFeaturePlugin *target, bool enabled) {
                    const QString targetId = m_pluginManager->idOf(target);
                    if (!m_pluginManager->setPluginEnabled(targetId, enabled)) {
                        // 启用失败（例如权限不足、功能已崩溃隔离）：回滚界面状态
                        refreshCard(target);
                    } else {
                        statusBar()->showMessage(
                            QStringLiteral("%1 已%2")
                                .arg(m_pluginManager->nameOf(target),
                                     enabled ? QStringLiteral("启用") : QStringLiteral("停用")),
                            3000);
                    }
                });
        connect(card, &FeatureCard::settingsRequested, this, &MainWindow::showPluginSettings);

        m_cards.insert(pluginId, card);
    }

    m_nav->setCategoryCounts(m_pluginManager->categoryCounts());
    applyFilter();
}

void MainWindow::applyFilter()
{
    QList<FeatureCard *> visible;
    visible.reserve(m_cards.size());

    // 按插件在管理器中的顺序展示，保证界面顺序稳定
    const QList<IFeaturePlugin *> plugins = m_pluginManager ? m_pluginManager->plugins()
                                                            : QList<IFeaturePlugin *>();

    for (IFeaturePlugin *plugin : plugins) {
        FeatureCard *card = m_cards.value(m_pluginManager->idOf(plugin), nullptr);
        if (!card) {
            continue;
        }

        const bool categoryOk = (m_currentCategory == FeatureCategory::Unknown)
                                || (m_pluginManager->categoryOf(plugin) == m_currentCategory);
        const bool keywordOk = m_searchKeyword.isEmpty() || card->matches(m_searchKeyword);

        if (categoryOk && keywordOk) {
            visible.append(card);
        } else {
            card->setVisible(false);
        }
    }

    m_grid->setCards(visible);

    const bool empty = visible.isEmpty();
    m_emptyHint->setVisible(empty);
    m_grid->setVisible(!empty);

    updateHeader();
}

void MainWindow::updateHeader()
{
    const QString title = Category::displayName(m_currentCategory);
    m_headerTitle->setText(title);

    const int total = m_cards.size();

    int visibleCount = 0;
    for (FeatureCard *card : std::as_const(m_cards)) {
        if (card && card->isVisible()) {
            ++visibleCount;
        }
    }

    if (m_searchKeyword.isEmpty()) {
        m_headerSubtitle->setText(Category::description(m_currentCategory) + QStringLiteral(" · 共 %1 项")
                                      .arg(m_currentCategory == FeatureCategory::Unknown
                                               ? total
                                               : m_pluginManager->pluginCount(m_currentCategory)));
    } else {
        m_headerSubtitle->setText(QStringLiteral("搜索“%1” · 命中 %2 项")
                                      .arg(m_searchKeyword)
                                      .arg(visibleCount));
    }
}

void MainWindow::refreshCard(IFeaturePlugin *plugin)
{
    if (!plugin || !m_pluginManager) {
        return;
    }
    if (FeatureCard *card = m_cards.value(m_pluginManager->idOf(plugin), nullptr)) {
        card->refresh();
    }
}

void MainWindow::refreshAllCards()
{
    for (FeatureCard *card : std::as_const(m_cards)) {
        if (card) {
            card->refresh();
        }
    }
}

// ============================================================================
//  槽
// ============================================================================

void MainWindow::setSearchText(const QString &text)
{
    m_searchEdit->setText(text);
}

void MainWindow::onSearchTextChanged(const QString &text)
{
    m_searchKeyword = text.trimmed();
    applyFilter();
}

void MainWindow::onCategoryChanged(FeatureCategory category)
{
    m_currentCategory = category;
    applyFilter();
}

void MainWindow::onPluginLoaded(IFeaturePlugin *plugin)
{
    Q_UNUSED(plugin)
    rebuildCards();
}

void MainWindow::onPluginStateChanged(IFeaturePlugin *plugin, bool enabled)
{
    Q_UNUSED(enabled)
    refreshCard(plugin);
    refreshStatistics();
}

void MainWindow::setAllPluginsEnabled(bool enabled)
{
    if (!m_pluginManager) {
        return;
    }

    const int changed = m_pluginManager->setAllEnabled(enabled);
    refreshAllCards();
    refreshStatistics();

    statusBar()->showMessage(
        QStringLiteral("%1 %2 个功能。")
            .arg(enabled ? QStringLiteral("已启用") : QStringLiteral("已停用"))
            .arg(changed),
        4000);
}

void MainWindow::showHotkeySettings()
{
    if (!m_hotkeyDialog) {
        m_hotkeyDialog = std::make_unique<HotkeySettingsDialog>(m_hotkeyManager, m_pluginManager, this);
    }
    m_hotkeyDialog->reload();
    m_hotkeyDialog->show();
    m_hotkeyDialog->raise();
    m_hotkeyDialog->activateWindow();
}

void MainWindow::showHelp(int topic)
{
    if (!m_helpDialog) {
        m_helpDialog = std::make_unique<HelpDialog>(m_pluginManager, this);
    }
    m_helpDialog->showTopic(static_cast<HelpDialog::Topic>(topic));
    m_helpDialog->show();
    m_helpDialog->raise();
    m_helpDialog->activateWindow();
}

void MainWindow::showPluginSettings(IFeaturePlugin *plugin)
{
    if (!plugin || !m_pluginManager) {
        return;
    }

    // 名称与设置面板一律经由管理器获取：
    //   * nameOf() 读缓存，插件崩溃后仍可安全使用
    //   * createSettingsWidget() 带 SEH/C++ 异常边界，插件崩在设置面板构造里也不会带走主程序
    const QString pluginName = m_pluginManager->nameOf(plugin);
    const QString pluginId = m_pluginManager->idOf(plugin);

    QWidget *panel = m_pluginManager->createSettingsWidget(pluginId, this);
    if (!panel) {
        // 崩溃隔离时管理器会通过 pluginError/pluginCrashed 给出原因，这里只做兜底提示
        QMessageBox::information(this, pluginName,
                                 m_pluginManager->isPluginFailed(pluginId)
                                     ? m_pluginManager->pluginFailureReason(pluginId)
                                     : QStringLiteral("该功能暂无可配置项。"));
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("%1 - 设置").arg(pluginName));
    dialog->resize(520, 420);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->addWidget(panel, 1);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this](int) { refreshAllCards(); });

    dialog->show();
}

void MainWindow::refreshStatistics()
{
    if (!m_pluginManager) {
        return;
    }

    int running = 0;
    for (IFeaturePlugin *plugin : m_pluginManager->plugins()) {
        // ⚠ 走管理器的安全查询，而不是 plugin->isEnabled()：
        //   已崩溃隔离的插件对象可能已损坏，任何直接调用都属于二次崩溃风险（P0-5）
        if (m_pluginManager->isPluginEnabled(m_pluginManager->idOf(plugin))) {
            ++running;
        }
    }

    m_statusPluginCount->setText(QStringLiteral("插件 %1 个").arg(m_pluginManager->pluginCount()));
    m_statusRunningCount->setText(QStringLiteral("运行中 %1 个").arg(running));
    m_statusPrivilege->setText(Admin::elevationDescription());

    if (!Admin::isProcessElevated() && m_pluginManager->hasElevationBlockedPlugins()) {
        m_statusPrivilege->setToolTip(
            QStringLiteral("以下功能需要管理员权限：%1")
                .arg(m_pluginManager->elevationBlockedPluginNames().join(QStringLiteral("、"))));
    } else {
        m_statusPrivilege->setToolTip(QString());
    }
}

// ============================================================================
//  窗口行为
// ============================================================================

void MainWindow::showAndActivate()
{
    if (isMinimized()) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
    }
    show();
    raise();
    activateWindow();
}

bool MainWindow::hasOpenDialog() const
{
    return (m_hotkeyDialog && m_hotkeyDialog->isVisible())
           || (m_helpDialog && m_helpDialog->isVisible());
}

void MainWindow::openPathInExplorer(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_quitRequested) {
        event->accept();
        return;
    }

    // 默认行为：关闭窗口 = 最小化到托盘（可在设置中改为直接退出）
    const bool minimizeToTray = SettingsManager::instance()
                                    .value(QStringLiteral("General/minimizeToTrayOnClose"), true);
    if (minimizeToTray) {
        event->ignore();
        hide();
        Q_EMIT minimizeToTrayRequested();
        return;
    }

    event->accept();
    m_quitRequested = true;
    Q_EMIT quitRequested();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);

    if (event->type() != QEvent::WindowStateChange) {
        return;
    }

    const bool minimizeToTray = SettingsManager::instance()
                                    .value(QStringLiteral("General/minimizeToTrayOnMinimize"), true);
    if (minimizeToTray && isMinimized()) {
        hide();
        Q_EMIT minimizeToTrayRequested();
    }
}

} // namespace WinEase
