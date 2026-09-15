#include "ui/SystemTrayManager.h"

#include "core/AdminHelper.h"
#include "core/Logging.h"
#include "core/PluginManager.h"
#include "sdk/IFeaturePlugin.h"

#include <QAction>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QWidget>

#include <functional>
#include <utility>

namespace WinEase {

namespace {

/// 显式创建带回调的 QAction，避免 addAction 重载歧义
QAction *makeAction(QObject *parent, const QString &text, std::function<void()> handler)
{
    auto *action = new QAction(text, parent);
    QObject::connect(action, &QAction::triggered, parent, [handler = std::move(handler)] { handler(); });
    return action;
}

const QString kTrayBaseTooltip = QStringLiteral("WinEase —— Windows 易用性增强工具集");
const QString kDefaultBadgeColor = QStringLiteral("#E5484D");
/// 图标绘制尺寸：系统会自己缩到 16/20/24 px，这里画大一点保证徽标圆点清晰
constexpr int kTrayIconSize = 64;

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

SystemTrayManager::SystemTrayManager(PluginManager *pluginManager,
                                     QWidget *mainWindow,
                                     QObject *parent)
    : QObject(parent)
    , m_pluginManager(pluginManager)
    , m_mainWindow(mainWindow)
{
    setupTray();
}

SystemTrayManager::~SystemTrayManager() = default;

void SystemTrayManager::setupTray()
{
    m_trayIcon = std::make_unique<QSystemTrayIcon>(QIcon(QStringLiteral(":/winease/icons/app.svg")), this);
    m_trayIcon->setToolTip(kTrayBaseTooltip);

    m_menu = std::make_unique<QMenu>();
    m_trayIcon->setContextMenu(m_menu.get());

    connect(m_trayIcon.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                switch (reason) {
                case QSystemTrayIcon::Trigger:      // 单击左键
                case QSystemTrayIcon::DoubleClick:  // 双击
                    toggleMainWindow();
                    break;
                case QSystemTrayIcon::MiddleClick:  // 中键 → 快捷打开快捷键设置
                    Q_EMIT hotkeySettingsRequested();
                    break;
                default:
                    break;
                }
            });

    // 插件增减 / 启停 / 崩溃隔离后同步刷新菜单勾选状态
    if (m_pluginManager) {
        connect(m_pluginManager, &PluginManager::pluginLoaded, this, &SystemTrayManager::rebuildMenu);

        connect(m_pluginManager, &PluginManager::pluginStateChanged, this,
                [this](IFeaturePlugin *plugin, bool enabled) {
                    // 插件停用后它不再有机会收拾界面 —— 徽标由宿主摘掉。
                    // （插件自己也会在 onDisable() 里清，这里是"宿主兜底"那一层：
                    //   onDisable 抛异常/超时、或插件被隔离时，仍不会留下假状态。）
                    if (!enabled && plugin != nullptr) {
                        clearPluginBadge(m_pluginManager->idOf(plugin));
                    }
                    rebuildMenu();
                });

        connect(m_pluginManager, &PluginManager::pluginCrashed, this,
                [this](const QString &pluginId, const QString &) {
                    // 崩溃隔离的插件**不允许再被调用**，所以它永远不会执行 onDisable：
                    // 界面痕迹（这里是徽标）只能由宿主收拾。
                    clearPluginBadge(pluginId);
                    rebuildMenu();
                });
    }

    rebuildMenu();
}

// ============================================================================
//  主窗口显隐
// ============================================================================

void SystemTrayManager::setMainWindow(QWidget *mainWindow)
{
    m_mainWindow = mainWindow;
}

QWidget *SystemTrayManager::mainWindow() const
{
    return m_mainWindow;
}

bool SystemTrayManager::isMainWindowActive() const
{
    return m_mainWindow && m_mainWindow->isVisible() && !m_mainWindow->isMinimized();
}

void SystemTrayManager::toggleMainWindow()
{
    Q_EMIT toggleRequested();

    if (isMainWindowActive()) {
        hideMainWindow();
    } else {
        showMainWindow();
    }
}

void SystemTrayManager::showMainWindow()
{
    if (!m_mainWindow) {
        qCWarning(lcTray) << "未设置主窗口，无法显示";
        return;
    }

    if (m_mainWindow->isMinimized()) {
        m_mainWindow->setWindowState(m_mainWindow->windowState() & ~Qt::WindowMinimized);
        m_mainWindow->showNormal();
    } else {
        m_mainWindow->show();
    }
    m_mainWindow->raise();
    m_mainWindow->activateWindow();

    qCDebug(lcTray) << "主窗口已显示";
    Q_EMIT mainWindowShown();
}

void SystemTrayManager::hideMainWindow()
{
    if (!m_mainWindow) {
        return;
    }
    m_mainWindow->hide();

    // 首次隐藏时给一次气泡提示，避免用户以为程序已退出
    static bool hintShown = false;
    if (!hintShown) {
        hintShown = true;
        showMessage(QStringLiteral("WinEase 仍在后台运行"),
                    QStringLiteral("单击托盘图标可以重新显示主界面，右键可快速启停功能。"));
    }

    qCDebug(lcTray) << "主窗口已隐藏到托盘";
    Q_EMIT mainWindowHidden();
}

// ============================================================================
//  托盘图标基础操作
// ============================================================================

bool SystemTrayManager::show()
{
    if (!m_trayIcon) {
        return false;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qCWarning(lcTray) << "当前系统环境不支持系统托盘";
        return false;
    }
    m_trayIcon->show();
    return true;
}

void SystemTrayManager::hide()
{
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
}

bool SystemTrayManager::isVisible() const
{
    return m_trayIcon && m_trayIcon->isVisible();
}

bool SystemTrayManager::isSystemTrayAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void SystemTrayManager::showMessage(const QString &title, const QString &body, int milliseconds)
{
    if (!m_trayIcon || !m_trayIcon->isVisible()) {
        qCDebug(lcTray) << "托盘不可用，跳过气泡通知:" << title << body;
        return;
    }
    m_trayIcon->showMessage(title, body, QSystemTrayIcon::Information, milliseconds);
}

// ============================================================================
//  插件状态徽标
// ============================================================================

void SystemTrayManager::setPluginBadge(const QString &pluginId,
                                       const QString &text,
                                       const QString &colorHex,
                                       const QString &tooltip)
{
    if (pluginId.isEmpty()) {
        return;
    }
    if (text.isEmpty()) { // 空文字 = 清除（约定见 PluginServices::setTrayBadge）
        clearPluginBadge(pluginId);
        return;
    }

    m_badges.insert(pluginId, Badge{text, colorHex, tooltip});
    updateTrayIcon();

    qCDebug(lcTray) << "托盘徽标已挂：" << pluginId << text;
}

void SystemTrayManager::clearPluginBadge(const QString &pluginId)
{
    if (m_badges.remove(pluginId) == 0) {
        return; // 本来就没挂：不重画（避免无谓的图标闪烁）
    }
    updateTrayIcon();
    qCDebug(lcTray) << "托盘徽标已摘：" << pluginId;
}

QString SystemTrayManager::pluginBadgeText(const QString &pluginId) const
{
    const auto it = m_badges.constFind(pluginId);
    return it == m_badges.constEnd() ? QString() : it.value().text;
}

void SystemTrayManager::updateTrayIcon()
{
    if (!m_trayIcon) {
        return;
    }

    QPixmap canvas = QIcon(QStringLiteral(":/winease/icons/app.svg"))
                         .pixmap(QSize(kTrayIconSize, kTrayIconSize));
    if (canvas.isNull()) {
        canvas = QPixmap(kTrayIconSize, kTrayIconSize);
        canvas.fill(Qt::transparent);
    }

    QString tooltip = kTrayBaseTooltip;
    if (!m_badges.isEmpty()) {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int dot = kTrayIconSize / 2; // 徽标圆点直径 = 图标一半，够醒目又不糊住主图标
        int slot = 0;
        for (auto it = m_badges.constBegin(); it != m_badges.constEnd(); ++it, ++slot) {
            const QColor color(it.value().colorHex.isEmpty() ? kDefaultBadgeColor
                                                             : QColor(it.value().colorHex));
            const QPoint center(canvas.width() - dot / 2,
                                canvas.height() - dot / 2 - slot * (dot + 2));
            painter.setPen(Qt::NoPen);
            painter.setBrush(color.isValid() ? color : QColor(kDefaultBadgeColor));
            painter.drawEllipse(center, dot / 2, dot / 2);

            // 圆点里放得下 1~2 个字；约定徽标文字要短（长了靠 left(2) 截断，不报错）
            QFont font = painter.font();
            font.setBold(true);
            font.setPixelSize(dot * 3 / 5);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(QRect(center.x() - dot / 2, center.y() - dot / 2, dot, dot),
                             Qt::AlignCenter, it.value().text.left(2));

            tooltip += QStringLiteral("\n%1").arg(
                it.value().tooltip.isEmpty()
                    ? it.value().text
                    : QStringLiteral("%1（%2）").arg(it.value().text, it.value().tooltip));
        }
        painter.end();
    }

    m_trayIcon->setIcon(QIcon(canvas));
    m_trayIcon->setToolTip(tooltip);
}

// ============================================================================
//  右键菜单
// ============================================================================

void SystemTrayManager::rebuildMenu()
{
    m_menu->clear();

    m_menu->addAction(makeAction(this, QStringLiteral("显示 / 隐藏主界面"), [this] {
        toggleMainWindow();
    }));
    m_menu->addSeparator();

    buildPluginSection(m_menu.get());

    m_menu->addSeparator();
    m_menu->addAction(makeAction(this, QStringLiteral("全部启用"), [this] {
        if (m_pluginManager) {
            m_pluginManager->setAllEnabled(true);
        }
    }));
    m_menu->addAction(makeAction(this, QStringLiteral("全部停用"), [this] {
        if (m_pluginManager) {
            m_pluginManager->setAllEnabled(false);
        }
    }));

    m_menu->addSeparator();
    m_menu->addAction(makeAction(this, QStringLiteral("全局快捷键…"), [this] {
        Q_EMIT hotkeySettingsRequested();
    }));
    m_menu->addSeparator();
    m_menu->addAction(makeAction(this, QStringLiteral("退出 WinEase"), [this] {
        Q_EMIT quitRequested();
    }));
}

void SystemTrayManager::buildPluginSection(QMenu *parentMenu)
{
    if (!m_pluginManager || m_pluginManager->pluginCount() == 0) {
        QAction *empty = parentMenu->addAction(QStringLiteral("（未发现可用功能）"));
        empty->setEnabled(false);
        return;
    }

    const bool elevated = Admin::isProcessElevated();

    // 按分类分组：分类 → 功能勾选项
    for (FeatureCategory category : Category::all()) {
        const QList<IFeaturePlugin *> plugins = m_pluginManager->plugins(category);
        if (plugins.isEmpty()) {
            continue;
        }

        QMenu *categoryMenu = parentMenu->addMenu(Category::icon(category), Category::displayName(category));
        for (IFeaturePlugin *plugin : plugins) {
            // ⚠ 崩溃隔离的插件不允许再被调用（对象可能已损坏，属二次崩溃风险）：
            //   只按管理器的缓存元信息展示一条不可用项，原因取自 pluginFailureReason()（P0-5）
            const QString cachedPluginId = m_pluginManager->idOf(plugin);
            if (m_pluginManager->isPluginFailed(cachedPluginId)) {
                QAction *failed = categoryMenu->addAction(Category::icon(category),
                                                          m_pluginManager->nameOf(plugin));
                failed->setEnabled(false);
                failed->setToolTip(m_pluginManager->pluginFailureReason(cachedPluginId));
                continue;
            }

            const QString pluginId = plugin->id();

            QAction *action = categoryMenu->addAction(plugin->icon(), plugin->name());
            action->setCheckable(true);
            action->setChecked(plugin->isEnabled());
            action->setToolTip(plugin->description());

            // 需要管理员权限且当前未提权：置灰并说明原因
            if (plugin->requiresAdmin() && !elevated) {
                action->setEnabled(false);
                action->setToolTip(QStringLiteral("%1\n（需要管理员权限，请以管理员身份重启 WinEase）")
                                       .arg(plugin->description()));
                continue;
            }

            connect(action, &QAction::triggered, this, [this, pluginId](bool checked) {
                if (!m_pluginManager->setPluginEnabled(pluginId, checked)) {
                    rebuildMenu(); // 启用失败：回滚菜单勾选状态
                }
            });
        }
    }
}

} // namespace WinEase
