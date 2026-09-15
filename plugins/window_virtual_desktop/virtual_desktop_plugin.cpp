#include "virtual_desktop_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/WindowUtils.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::DesktopWindowInfo;
using WinEase::Win32::VirtualDesktopInfo;

/// 只读探测的刷新周期（用户新建/删除桌面、开关窗口都要能跟上，探测本身很轻）
constexpr int kRefreshIntervalMs = 5000;

/// 列表项文字：**用"是不是当前桌面 + 有几个窗口 + 窗口标题样例"帮用户认桌面**
/// （公开接口拿不到桌面的名字与编号，别假装知道"这是第几个桌面"）
QString desktopItemText(const VirtualDesktopInfo &info)
{
    QString text = QStringLiteral("%1 · %2 个窗口").arg(info.label()).arg(info.windowCount);
    if (!info.sampleTitles.isEmpty()) {
        text += QStringLiteral(" —— %1").arg(info.sampleTitles.join(QStringLiteral(" / ")));
    }
    return text;
}

} // namespace

VirtualDesktopPlugin::VirtualDesktopPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString VirtualDesktopPlugin::id() const
{
    return QStringLiteral("window.virtual_desktop");
}

QString VirtualDesktopPlugin::name() const
{
    return QStringLiteral("虚拟桌面窗口一览");
}

QString VirtualDesktopPlugin::description() const
{
    return QStringLiteral("看看每个虚拟桌面上有哪些窗口 —— 那个找不到的窗口在哪个桌面，"
                          "一眼就能看到（只读，不动任何窗口）");
}

QString VirtualDesktopPlugin::detailedDescription() const
{
    // 帮助页「关于插件」里的完整说明。卡片上只显示两行 + Tooltip 全文，
    // 这些"为什么只做这一点"的话放在这里才说得清。
    return QStringLiteral(
        "**它做什么**：把每个虚拟桌面上的窗口列出来（含窗口标题与进程名），"
        "并标出哪一个是当前桌面。用于回答「我那个窗口跑到哪个桌面去了」。\n"
        "\n"
        "**它不做什么，以及为什么**：\n"
        "· 不搬窗口。ROADMAP 原计划的「把窗口搬到另一个虚拟桌面」用公开 API **做不到**："
        "`MoveWindowToDesktop` 只允许搬**本进程自己**的窗口（实测：自己的窗口成功、"
        "别的进程的窗口一律 `E_ACCESSDENIED`），而这条限制**官方文档一个字都没写**。"
        "唯一替代路子是系统快捷键 `Win+Ctrl+Shift+←/→`（把当前活动窗口移到相邻桌面），"
        "但要抢用户的前台焦点、依赖键位没被改过，代价与收益不匹配 —— 已砍掉。\n"
        "· 不切桌面（切换虚拟桌面同样没有公开 API），也不激活别的桌面上的窗口。\n"
        "\n"
        "**能力边界**：\n"
        "· 只看得到**有窗口的**桌面 —— 公开接口没有「枚举虚拟桌面」的方法，"
        "本功能靠枚举窗口反查桌面 GUID；刚建好、还没放过窗口的桌面不会出现，"
        "桌面被删掉后下一轮刷新（5 秒）会自动少一项；\n"
        "· 拿不到桌面的名字与编号（系统的「桌面 1 / 2」是外壳自己排的），"
        "所以只能靠「当前/其它 + 窗口数 + 窗口标题」来分辨；\n"
        "· 列表是**只读**的：这里不会激活窗口、不会切桌面、不会搬走任何东西。");
}

QIcon VirtualDesktopPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::WindowManagement);
}

WinEase::FeatureCategory VirtualDesktopPlugin::category() const
{
    return WinEase::FeatureCategory::WindowManagement;
}

QStringList VirtualDesktopPlugin::tags() const
{
    return { QStringLiteral("虚拟桌面"), QStringLiteral("桌面"), QStringLiteral("窗口一览"),
             QStringLiteral("desktop"), QStringLiteral("virtual"), QStringLiteral("xnzm") };
}

bool VirtualDesktopPlugin::supportsHotkey() const
{
    // 只读一览：没有需要快捷键触发的动作
    //（原来那条 `Ctrl+Alt+G` 是"把窗口搬到目标桌面"的，功能已砍 —— 快捷键随之注销）
    return false;
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool VirtualDesktopPlugin::initialize()
{
    if (m_refreshTimer == nullptr) {
        m_refreshTimer = new QTimer(this);
        m_refreshTimer->setInterval(kRefreshIntervalMs);
        connect(m_refreshTimer, &QTimer::timeout, this, [this] { refreshDesktops(); });
    }

    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("虚拟桌面窗口一览已就绪（只读探测；不搬移、不切桌面）"));
    return true;
}

void VirtualDesktopPlugin::shutdown()
{
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
    }
    m_desktops.clear();
}

bool VirtualDesktopPlugin::canEnable(QString *reason) const
{
    // ★ fail-closed：本机根本没有虚拟桌面管理器（老系统 / 被裁剪的系统）时**如实拒绝启用**，
    //   而不是"启用了但什么都干不了"。
    //   注意判据是"查询本身报错"，**不是**"桌面列表为空"—— 当前没有任何窗口时
    //   探测不到桌面是正常的，那种情况下功能仍然应该能启用（用户一开窗口就能看到）。
    QString error;
    WinEase::Win32::virtualDesktops(&error);
    if (!error.isEmpty()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("本机无法使用虚拟桌面管理器：%1").arg(error);
        }
        return false;
    }
    return true;
}

bool VirtualDesktopPlugin::onEnable()
{
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->start();
    }

    refreshDesktops(); // 内部会刷面板
    Q_EMIT statusMessage(statusText());

    // ⚠ 时序（踩坑 #51 / #57）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    return true;
}

void VirtualDesktopPlugin::onDisable()
{
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
    }
    m_desktops.clear();
    m_lastEvent.clear();

    // 只读功能：停用**不需要还原任何东西**（没有动过窗口、没有写过系统状态）
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  探测（只读）
// ---------------------------------------------------------------------------

void VirtualDesktopPlugin::refreshDesktops()
{
    QString error;
    m_desktops = WinEase::Win32::virtualDesktops(&error);

    if (!error.isEmpty()) {
        setLastError(error);
        m_lastEvent = QStringLiteral("探测虚拟桌面失败：%1").arg(error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
    } else {
        clearLastError();
        m_lastEvent.clear();
    }

    refreshPanel();
}

const WinEase::Win32::VirtualDesktopInfo *VirtualDesktopPlugin::selectedDesktop() const
{
    if (m_desktopList == nullptr) {
        return nullptr;
    }
    const int row = m_desktopList->currentRow();
    if (row < 0 || row >= m_desktops.size()) {
        return nullptr;
    }
    return &m_desktops.at(row);
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QString VirtualDesktopPlugin::statusText() const
{
    if (!m_lastEvent.isEmpty()) {
        return m_lastEvent;
    }
    if (m_desktops.isEmpty()) {
        return QStringLiteral("还没有探测到虚拟桌面（当前没有任何窗口时就会这样）");
    }

    const VirtualDesktopInfo *current = nullptr;
    for (const VirtualDesktopInfo &info : m_desktops) {
        if (info.isCurrent) {
            current = &info;
        }
    }

    return QStringLiteral("探测到 %1 个虚拟桌面%2（只能看到**有窗口的**桌面）")
        .arg(m_desktops.size())
        .arg(current != nullptr
                 ? QStringLiteral("；当前桌面有 %1 个窗口").arg(current->windowCount)
                 : QString());
}

QWidget *VirtualDesktopPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("vdeskPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("vdeskStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *desktopLabel = new QLabel(QStringLiteral("虚拟桌面（点一个看它上面有哪些窗口）："), widget);
    desktopLabel->setObjectName(QStringLiteral("vdeskDesktopLabel"));
    layout->addWidget(desktopLabel);

    auto *desktopList = new QListWidget(widget);
    desktopList->setObjectName(QStringLiteral("vdeskDesktopList"));
    desktopList->setMinimumHeight(96);
    layout->addWidget(desktopList);

    auto *detailLabel = new QLabel(widget);
    detailLabel->setObjectName(QStringLiteral("vdeskDetailLabel"));
    detailLabel->setWordWrap(true);
    detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailLabel);

    auto *windowLabel = new QLabel(QStringLiteral("这个桌面上的窗口（按 z 序，最上面的在前）："), widget);
    windowLabel->setObjectName(QStringLiteral("vdeskWindowLabel"));
    layout->addWidget(windowLabel);

    auto *windowList = new QListWidget(widget);
    windowList->setObjectName(QStringLiteral("vdeskWindowList"));
    windowList->setMinimumHeight(120);
    windowList->setSelectionMode(QAbstractItemView::NoSelection); // 只读一览，点选没有意义
    layout->addWidget(windowList);

    auto *buttonRow = new QHBoxLayout();
    auto *refreshButton = new QPushButton(QStringLiteral("重新探测"), widget);
    refreshButton->setObjectName(QStringLiteral("vdeskRefreshButton"));
    buttonRow->addWidget(refreshButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *hint = new QLabel(
        QStringLiteral(
            "· 这是**只读一览**：不会激活窗口、不会切桌面、更不会把窗口搬走；\n"
            "· **没有「枚举虚拟桌面」的公开接口** → 本功能靠枚举窗口反查桌面，"
            "所以**只看得到「有窗口的」桌面**（刚建好、还没放过窗口的桌面不会出现），"
            "删掉的桌面会在下一轮刷新（5 秒）自动消失；\n"
            "· 也**拿不到桌面的名字与编号**（系统里那个「桌面 1 / 2」是外壳自己排的），"
            "所以用「当前/其它 + 窗口数 + 窗口标题」来分辨；\n"
            "· 为什么不做「把窗口搬过去」：公开接口只允许搬 WinEase 自己的窗口，"
            "实测对别的程序一律被系统拒绝（详见帮助 →「关于插件」）。"),
        widget);
    hint->setObjectName(QStringLiteral("vdeskHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_desktopList = desktopList;
    m_detailLabel = detailLabel;
    m_windowList = windowList;
    m_refreshButton = refreshButton;

    connect(refreshButton, &QPushButton::clicked, widget, [this] { refreshDesktops(); });
    // 选中项变化**只刷新"这个桌面上有哪些窗口"**，绝不回手重填列表（踩坑 #61）
    connect(desktopList, &QListWidget::currentRowChanged, widget, [this](int) {
        refreshWindowList();
        refreshDetail();
        updateActionState();
    });

    refreshPanel();
    return widget;
}

void VirtualDesktopPlugin::refreshDesktopList()
{
    if (m_desktopList == nullptr) {
        return;
    }

    // 重填之前记下当前选的是哪个桌面（按 GUID，不按下标 —— 排序会变）
    const VirtualDesktopInfo *previous = selectedDesktop();
    const QString previousId = (previous != nullptr) ? previous->id : QString();

    // ⚠ 重填列表必须屏蔽信号（clear()/setCurrentRow() 都会发 currentRowChanged，踩坑 #61）
    const QSignalBlocker blocker(m_desktopList.data());

    m_desktopList->clear();
    int restoreRow = -1;
    for (int index = 0; index < m_desktops.size(); ++index) {
        const VirtualDesktopInfo &info = m_desktops.at(index);
        auto *item = new QListWidgetItem(desktopItemText(info), m_desktopList.data());
        item->setToolTip(QStringLiteral("桌面标识：%1\n（系统的桌面名字与编号拿不到，"
                                        "这是公开接口的边界）")
                             .arg(info.id));
        if (!previousId.isEmpty() && info.id == previousId) {
            restoreRow = index;
        }
    }
    if (restoreRow >= 0) {
        m_desktopList->setCurrentRow(restoreRow);
    } else if (m_desktopList->count() > 0) {
        // 首次进来（或原来选的那个桌面被删了）→ 选第一个（排序后当前桌面在最前）
        m_desktopList->setCurrentRow(0);
    }
}

void VirtualDesktopPlugin::refreshWindowList()
{
    if (m_windowList == nullptr) {
        return;
    }

    const QSignalBlocker blocker(m_windowList.data());
    m_windowList->clear();

    const VirtualDesktopInfo *desktop = selectedDesktop();
    if (desktop == nullptr) {
        m_windowList->addItem(QStringLiteral("（还没选中桌面）"));
        return;
    }
    if (desktop->windows.isEmpty()) {
        // 能探测到这个桌面说明它有窗口，但"用户心里算数的窗口"可能是 0 个
        // （只有外壳窗口那种）—— 如实说，而不是留个空列表让人以为坏了
        m_windowList->addItem(QStringLiteral("（这个桌面上没有可显示的顶层窗口）"));
        return;
    }

    for (const DesktopWindowInfo &window : desktop->windows) {
        auto *item = new QListWidgetItem(window.label(), m_windowList.data());
        item->setToolTip(QStringLiteral("进程：%1（PID %2）\n窗口句柄：%3")
                             .arg(window.processName.isEmpty() ? QStringLiteral("未知")
                                                               : window.processName)
                             .arg(window.processId)
                             .arg(reinterpret_cast<quintptr>(window.handle), 0, 16));
    }
}

void VirtualDesktopPlugin::refreshDetail()
{
    if (m_detailLabel == nullptr) {
        return;
    }

    const VirtualDesktopInfo *desktop = selectedDesktop();
    if (desktop == nullptr) {
        m_detailLabel->setText(QStringLiteral("（还没选中桌面）"));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("选中：%1").arg(desktop->label());
    lines << (desktop->isCurrent ? QStringLiteral("这是你**现在正在看**的桌面")
                                 : QStringLiteral("它在**别的**桌面上（切过去就能看到这些窗口）"));
    lines << QStringLiteral("（桌面标识 %1 —— 系统不给名字与编号）").arg(desktop->id.left(8));
    m_detailLabel->setText(lines.join(QStringLiteral("\n")));
}

void VirtualDesktopPlugin::updateActionState()
{
    const bool running = isEnabled();
    if (m_desktopList != nullptr) {
        m_desktopList->setEnabled(running);
    }
    if (m_windowList != nullptr) {
        m_windowList->setEnabled(running);
    }
    if (m_refreshButton != nullptr) {
        m_refreshButton->setEnabled(running);
    }
}

void VirtualDesktopPlugin::refreshPanel()
{
    if (m_panel == nullptr) {
        return;
    }

    if (m_statusLabel != nullptr) {
        m_statusLabel->setText(statusText());
    }

    refreshDesktopList();
    refreshWindowList();
    refreshDetail();
    updateActionState();
}
