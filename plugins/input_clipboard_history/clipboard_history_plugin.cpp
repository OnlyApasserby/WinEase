#include "clipboard_history_plugin.h"

#include "ClipboardTools.h"
#include "sdk/PluginServices.h"
#include "win32/ClipboardPrivacy.h"
#include "win32/WindowUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace ClipboardTools = WinEase::FeaturePlugins::ClipboardTools;
namespace Clip = WinEase::FeaturePlugins::ClipboardHistoryStore;
using WinEase::Win32::ClipboardOriginInfo;

namespace {

/// 面板的窗口对象名（自检隔着 DLL 边界就是按它找面板的）
const QString kPanelObjectName = QStringLiteral("cliphistPanel");
/// 搜索防抖：连打键盘时不每次都扫正文（正文搜索要读文件）
constexpr int kSearchDebounceMs = 200;
/// 右栏正文最多显示多少字符
constexpr int kBodyMaxChars = 4000;

QString timeText(const QDateTime &stamp)
{
    return stamp.isValid() ? stamp.toString(QStringLiteral("MM-dd HH:mm")) : QStringLiteral("时间未知");
}

} // namespace

// ============================================================================
//  面板：搜索 → 选中 → 粘贴 / 仅复制 / 收藏 / 删除 / 清空
//
//  与批量重命名面板同一条纪律：面板不自己算，一律问插件要数据
//  （"界面上写的"与"磁盘上存的"必须是同一份来源）
// ============================================================================

class ClipboardHistoryPanel : public QWidget
{
public:
    explicit ClipboardHistoryPanel(ClipboardHistoryPlugin *plugin)
        : QWidget(nullptr, Qt::Window)
        , m_plugin(plugin)
    {
        setObjectName(kPanelObjectName);
        setWindowTitle(QStringLiteral("剪贴板历史 —— WinEase"));
        resize(880, 560);

        auto *root = new QVBoxLayout(this);

        // ---- 顶部：搜索 + 分类 ----
        auto *topRow = new QHBoxLayout();
        m_searchEdit = new QLineEdit(this);
        m_searchEdit->setObjectName(QStringLiteral("cliphistSearchEdit"));
        m_searchEdit->setPlaceholderText(
            QStringLiteral("搜索：预览、来源应用、正文内容都能搜（正文只读头部 8 KB）"));
        m_searchEdit->setClearButtonEnabled(true);
        topRow->addWidget(m_searchEdit, 1);

        m_filterCombo = new QComboBox(this);
        m_filterCombo->setObjectName(QStringLiteral("cliphistFilterCombo"));
        m_filterCombo->addItem(QStringLiteral("全部"), QStringLiteral("all"));
        m_filterCombo->addItem(QStringLiteral("文本"), QStringLiteral("text"));
        m_filterCombo->addItem(QStringLiteral("图片"), QStringLiteral("image"));
        m_filterCombo->addItem(QStringLiteral("文件"), QStringLiteral("files"));
        m_filterCombo->addItem(QStringLiteral("收藏"), QStringLiteral("favorite"));
        topRow->addWidget(m_filterCombo);
        root->addLayout(topRow);

        // ---- 中部：列表 + 右栏正文 ----
        auto *middleRow = new QHBoxLayout();
        m_list = new QListWidget(this);
        m_list->setObjectName(QStringLiteral("cliphistList"));
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setAlternatingRowColors(true);
        m_list->setWordWrap(false);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        middleRow->addWidget(m_list, 3);

        m_bodyEdit = new QPlainTextEdit(this);
        m_bodyEdit->setObjectName(QStringLiteral("cliphistBodyEdit"));
        m_bodyEdit->setReadOnly(true);
        m_bodyEdit->setPlaceholderText(QStringLiteral("选中左边一条，这里显示它的正文"));
        middleRow->addWidget(m_bodyEdit, 2);
        root->addLayout(middleRow, 1);

        // ---- 按钮 ----
        auto *buttonRow = new QHBoxLayout();
        m_pasteButton = new QPushButton(QStringLiteral("粘贴到光标处"), this);
        m_pasteButton->setObjectName(QStringLiteral("cliphistPasteButton"));
        m_copyButton = new QPushButton(QStringLiteral("仅复制到剪贴板"), this);
        m_copyButton->setObjectName(QStringLiteral("cliphistCopyButton"));
        m_favoriteButton = new QPushButton(QStringLiteral("收藏 / 取消收藏"), this);
        m_favoriteButton->setObjectName(QStringLiteral("cliphistFavoriteButton"));
        m_deleteButton = new QPushButton(QStringLiteral("删除这条"), this);
        m_deleteButton->setObjectName(QStringLiteral("cliphistDeleteButton"));
        m_clearButton = new QPushButton(QStringLiteral("清空全部（含系统剪贴板）"), this);
        m_clearButton->setObjectName(QStringLiteral("cliphistClearButton"));
        m_openDirButton = new QPushButton(QStringLiteral("打开历史目录"), this);
        m_openDirButton->setObjectName(QStringLiteral("cliphistOpenDirButton"));
        buttonRow->addWidget(m_pasteButton);
        buttonRow->addWidget(m_copyButton);
        buttonRow->addWidget(m_favoriteButton);
        buttonRow->addWidget(m_deleteButton);
        buttonRow->addStretch(1);
        buttonRow->addWidget(m_openDirButton);
        buttonRow->addWidget(m_clearButton);
        root->addLayout(buttonRow);

        // ---- 底部：条数/上限、隐私说明、动作结果 ----
        m_summaryLabel = new QLabel(this);
        m_summaryLabel->setObjectName(QStringLiteral("cliphistSummaryLabel"));
        m_summaryLabel->setWordWrap(true);
        root->addWidget(m_summaryLabel);

        m_privacyLabel = new QLabel(this);
        m_privacyLabel->setObjectName(QStringLiteral("cliphistPrivacyLabel"));
        m_privacyLabel->setWordWrap(true);
        root->addWidget(m_privacyLabel);

        m_statusLabel = new QLabel(this);
        m_statusLabel->setObjectName(QStringLiteral("cliphistStatusLabel"));
        m_statusLabel->setWordWrap(true);
        root->addWidget(m_statusLabel);

        // ---- 连线 ----
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this] {
            m_searchTimer->start(kSearchDebounceMs);
        });
        connect(m_filterCombo, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
        connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
            updateBody();
            updateButtons();
        });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
            onActivate(true);
        });
        connect(m_pasteButton, &QPushButton::clicked, this, [this] { onActivate(true); });
        connect(m_copyButton, &QPushButton::clicked, this, [this] { onActivate(false); });
        connect(m_favoriteButton, &QPushButton::clicked, this, [this] { onToggleFavorite(); });
        connect(m_deleteButton, &QPushButton::clicked, this, [this] { onDelete(); });
        connect(m_clearButton, &QPushButton::clicked, this, [this] { onClear(); });
        connect(m_openDirButton, &QPushButton::clicked, this, [this] {
            const QString dir = m_plugin->storeDirectory();
            if (dir.isEmpty()) {
                setStatus(QStringLiteral("历史目录尚未确定"));
                return;
            }
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            setStatus(QStringLiteral("已打开 %1").arg(QDir::toNativeSeparators(dir)));
        });

        m_searchTimer = new QTimer(this);
        m_searchTimer->setSingleShot(true);
        connect(m_searchTimer, &QTimer::timeout, this, [this] { refresh(); });

        connect(m_plugin, &ClipboardHistoryPlugin::historyChanged, this,
                &ClipboardHistoryPanel::refresh);
        connect(m_plugin, &ClipboardHistoryPlugin::statisticsChanged, this,
                &ClipboardHistoryPanel::refresh);

        refresh();
    }

    /// 按当前搜索词/分类重建列表（保留原来选中的那一条）
    void refresh()
    {
        const QString keepId = selectedId();

        m_list->clear();
        const QString keyword = m_searchEdit->text().trimmed();
        const QString filter = m_filterCombo->currentData().toString();
        const QVector<Clip::Entry> items = m_plugin->search(keyword, filter);

        for (const Clip::Entry &entry : items) {
            QString detail = timeText(entry.createdAt);
            detail += entry.sourceApp.isEmpty()
                ? QStringLiteral(" · 来源未知")
                : QStringLiteral(" · 来自 %1").arg(entry.sourceApp);
            if (entry.payloadMissing) {
                detail += QStringLiteral(" · ⚠ 正文文件已丢失");
            }
            if (entry.thumbnailOnly) {
                detail += QStringLiteral(" · 仅缩略图");
            }

            auto *item = new QListWidgetItem(
                QStringLiteral("%1[%2] %3  ·  %4")
                    .arg(entry.favorite ? QStringLiteral("★ ") : QString(),
                         Clip::kindDisplayName(entry.kind),
                         entry.preview.isEmpty() ? QStringLiteral("（无预览）") : entry.preview,
                         detail));
            item->setData(Qt::UserRole, entry.id);
            m_list->addItem(item);
        }

        // 恢复选中：刷新不该把用户正在看的那条弄丢
        int restoreRow = -1;
        for (int row = 0; row < m_list->count(); ++row) {
            if (m_list->item(row)->data(Qt::UserRole).toString() == keepId) {
                restoreRow = row;
                break;
            }
        }
        if (restoreRow < 0 && m_list->count() > 0) {
            restoreRow = 0;
        }
        m_list->setCurrentRow(restoreRow);

        updateSummary(items.size());
        updateBody();
        updateButtons();
    }

    void setStatus(const QString &text) { m_statusLabel->setText(text); }
    QString statusText() const { return m_statusLabel->text(); }

private:
    QString selectedId() const
    {
        QListWidgetItem *item = m_list->currentItem();
        return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
    }

    void updateSummary(int shownCount)
    {
        const int total = m_plugin->entryCount();
        const qint64 maxKb = m_plugin->maxItemBytes() / 1024;
        m_summaryLabel->setText(
            QStringLiteral("共 %1 条（当前筛出 %2 条）｜条数上限 %3 条（收藏不计入）｜"
                           "单条上限 %4 KB｜图片策略：%5")
                .arg(total)
                .arg(shownCount)
                .arg(m_plugin->maxItems())
                .arg(maxKb)
                .arg(imageModeText()));

        m_privacyLabel->setText(
            QStringLiteral("隐私：剪贴板上标着「不记录」的内容（密码管理器、浏览器等）"
                           "一个字都不会写进历史——已跳过 %1 条；因超过单条上限跳过 %2 条。%3")
                .arg(m_plugin->skippedPrivacyCount())
                .arg(m_plugin->skippedOversizeCount())
                .arg(m_plugin->lastSkipReason().isEmpty()
                         ? QStringLiteral("历史文件在：%1").arg(QDir::toNativeSeparators(
                               m_plugin->storeDirectory()))
                         : QStringLiteral("最近一次跳过：%1").arg(m_plugin->lastSkipReason())));
    }

    QString imageModeText() const
    {
        const QString mode = m_plugin->imageModeKey();
        if (mode == QLatin1String("off")) {
            return QStringLiteral("不记录");
        }
        if (mode == QLatin1String("full")) {
            return QStringLiteral("存原图");
        }
        return QStringLiteral("只存缩略图（粘贴回去的也是缩略图）");
    }

    void updateButtons()
    {
        const bool hasSelection = m_list->currentItem() != nullptr;
        m_pasteButton->setEnabled(hasSelection);
        m_copyButton->setEnabled(hasSelection);
        m_favoriteButton->setEnabled(hasSelection);
        m_deleteButton->setEnabled(hasSelection);
        m_clearButton->setEnabled(m_plugin->entryCount() > 0);
    }

    void updateBody()
    {
        const QString id = selectedId();
        if (id.isEmpty()) {
            m_bodyEdit->setPlainText(QString());
            return;
        }
        QString error;
        QString body = m_plugin->entryBodyText(id, &error);
        if (!error.isEmpty()) {
            body = QStringLiteral("（读不到正文：%1）").arg(error);
        } else if (body.size() > kBodyMaxChars) {
            body = body.left(kBodyMaxChars)
                + QStringLiteral("\n…（右栏只显示前 %1 个字符）").arg(kBodyMaxChars);
        }
        m_bodyEdit->setPlainText(body);
    }

    void onActivate(bool paste)
    {
        const QString id = selectedId();
        if (id.isEmpty()) {
            setStatus(QStringLiteral("先选中一条历史"));
            return;
        }
        QString message;
        m_plugin->activate(id, paste, &message);
        // ⚠ 顺序就是语义：先刷新（可能把选中项挪走），**再**报本次动作的结果
        refresh();
        setStatus(message);
    }

    void onToggleFavorite()
    {
        const QString id = selectedId();
        if (id.isEmpty()) {
            setStatus(QStringLiteral("先选中一条历史"));
            return;
        }
        const bool nowFavorite = !isFavorite(id);
        m_plugin->setFavorite(id, nowFavorite);
        refresh();
        setStatus(nowFavorite ? QStringLiteral("已收藏（收藏的条目不会被条数上限淘汰）")
                              : QStringLiteral("已取消收藏"));
    }

    bool isFavorite(const QString &id) const
    {
        const QVector<Clip::Entry> all = m_plugin->search(QString(), QString());
        for (const Clip::Entry &entry : all) {
            if (entry.id == id) {
                return entry.favorite;
            }
        }
        return false;
    }

    void onDelete()
    {
        const QString id = selectedId();
        if (id.isEmpty()) {
            setStatus(QStringLiteral("先选中一条历史"));
            return;
        }
        QString message;
        const bool ok = m_plugin->removeEntry(id, &message);
        refresh();
        setStatus(ok ? QStringLiteral("已删除这条（正文文件也一起删了）") : message);
    }

    void onClear()
    {
        const int total = m_plugin->entryCount();
        if (total == 0) {
            setStatus(QStringLiteral("历史里没有内容"));
            return;
        }
        if (m_plugin->confirmBeforeClear()) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("清空剪贴板历史"),
                QStringLiteral("将删除全部 %1 条历史（包括收藏），并清空系统剪贴板。\n"
                               "此操作不可撤销。")
                    .arg(total),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                setStatus(QStringLiteral("已取消：什么都没删"));
                return;
            }
        }
        QString message;
        m_plugin->clearAll(true, &message);
        refresh();
        setStatus(message);
    }

    ClipboardHistoryPlugin *m_plugin = nullptr;

    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_filterCombo = nullptr;
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_bodyEdit = nullptr;
    QPushButton *m_pasteButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_favoriteButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_openDirButton = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_privacyLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_searchTimer = nullptr;
};

// ============================================================================
//  插件本体
// ============================================================================

ClipboardHistoryPlugin::ClipboardHistoryPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString ClipboardHistoryPlugin::id() const
{
    return QStringLiteral("input.clipboard_history");
}

QString ClipboardHistoryPlugin::name() const
{
    return QStringLiteral("剪贴板历史");
}

QString ClipboardHistoryPlugin::description() const
{
    return QStringLiteral("记录剪贴板历史（搜索/收藏/分类），并识别「不记录」标记保护隐私");
}

QIcon ClipboardHistoryPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::InputEfficiency);
}

WinEase::FeatureCategory ClipboardHistoryPlugin::category() const
{
    return WinEase::FeatureCategory::InputEfficiency;
}

QStringList ClipboardHistoryPlugin::tags() const
{
    return { QStringLiteral("剪贴板"), QStringLiteral("历史"), QStringLiteral("粘贴"),
             QStringLiteral("clipboard"), QStringLiteral("隐私"), QStringLiteral("清理") };
}

bool ClipboardHistoryPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence ClipboardHistoryPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+V"));
}

bool ClipboardHistoryPlugin::hasSettings() const
{
    return true;
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

void ClipboardHistoryPlugin::loadFromConfig()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    m_maxItems = qMax(1, svc->configValue(id(), QStringLiteral("maxItems"), 200).toInt());
    const int maxKb = qMax(0, svc->configValue(id(), QStringLiteral("maxItemKb"), 256).toInt());
    m_maxItemBytes = static_cast<qint64>(maxKb) * 1024;

    const QString mode = svc->configValue(id(), QStringLiteral("imageMode"),
                                          QStringLiteral("thumbnail")).toString().toLower();
    m_imageMode = (mode == QLatin1String("off") || mode == QLatin1String("full"))
        ? mode
        : QStringLiteral("thumbnail");

    m_confirm = svc->configValue(id(), QStringLiteral("confirm"), true).toBool();
    m_pasteBack = svc->configValue(id(), QStringLiteral("pasteBack"), false).toBool();

    // 历史目录：显式配置优先；否则放在配置目录旁边（%APPDATA%/WinEase/clipboard_history）
    m_storageDir = svc->configValue(id(), QStringLiteral("storageDir")).toString().trimmed();
    if (m_storageDir.isEmpty()) {
        const QString configPath = svc->configFilePath();
        m_storageDir = configPath.isEmpty()
            ? QDir(QDir::homePath()).filePath(QStringLiteral(".winease/clipboard_history"))
            : QDir(QFileInfo(configPath).absolutePath())
                  .filePath(QStringLiteral("clipboard_history"));
    }
    m_storageDir = QDir::cleanPath(m_storageDir);

    m_store.setDirectory(m_storageDir);
    m_store.setMaxItems(m_maxItems);
    m_store.setMaxItemBytes(m_maxItemBytes);

    QString error;
    if (!m_store.load(&error)) {
        // 读不出来不是死局：把原因说清楚，然后从空历史继续（绝不静默覆盖）
        setLastError(error);
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("历史索引读取失败：%1（将从空历史继续，原文件不动）").arg(error));
    }
    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("剪贴板历史已加载 %1 条，目录：%2")
                   .arg(m_store.count())
                   .arg(QDir::toNativeSeparators(m_storageDir)));
}

bool ClipboardHistoryPlugin::initialize()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法初始化"));
        return false;
    }
    loadFromConfig();
    return true;
}

void ClipboardHistoryPlugin::shutdown()
{
    closePanel();
    // ⚠ 停用/关闭**不删历史**：那是用户自己的数据（见头文件纪律 ②）
    QString error;
    if (!m_store.save(&error)) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("退出时保存历史索引失败：%1").arg(error));
    }
}

bool ClipboardHistoryPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        setLastError(QStringLiteral("取不到剪贴板（图形界面未就绪）"));
        return false;
    }
    connect(clipboard, &QClipboard::dataChanged, this,
            &ClipboardHistoryPlugin::onClipboardChanged, Qt::UniqueConnection);

    // 第二个动作：只清空系统剪贴板（复制完密码立刻抹掉），不碰历史
    if (!services()->registerHotkey(id(), QStringLiteral("privacy_clear"),
                                    QKeySequence(QStringLiteral("Ctrl+Alt+X")),
                                    QStringLiteral("WinEase：清空系统剪贴板（历史不受影响）"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「清空系统剪贴板」快捷键注册失败（可能被占用）——面板按钮仍然可用"));
    }

    m_captureEnabled = true;
    m_selfWrite = false;
    const QKeySequence panelKey = defaultHotkey();
    Q_EMIT statusMessage(
        QStringLiteral("正在记录（%1 条，上限 %2 条）｜按 %3 打开面板")
            .arg(m_store.count())
            .arg(m_maxItems)
            .arg(panelKey.toString(QKeySequence::NativeText)));
    return true;
}

void ClipboardHistoryPlugin::onDisable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("privacy_clear"));
    }
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        disconnect(clipboard, &QClipboard::dataChanged, this,
                   &ClipboardHistoryPlugin::onClipboardChanged);
    }
    closePanel();
    Q_EMIT statusMessage(QStringLiteral("已停用（历史保留在磁盘上，共 %1 条）").arg(m_store.count()));
}

void ClipboardHistoryPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("privacy_clear")) {
        QString message;
        clearSystemClipboard(&message);
        Q_EMIT statusMessage(message);
        return;
    }
    openPanel();
}

// ---------------------------------------------------------------------------
//  采集
// ---------------------------------------------------------------------------

QString ClipboardHistoryPlugin::foregroundAppName() const
{
    const WinEase::Win32::WindowHandle hwnd = WinEase::Win32::foregroundWindow();
    if (hwnd == nullptr) {
        return QString();
    }
    return WinEase::Win32::windowProcessName(hwnd);
}

void ClipboardHistoryPlugin::onClipboardChanged()
{
    if (m_selfWrite) {
        // 我们自己刚写进去的（面板粘贴/清空）→ 不算"新复制的内容"
        m_selfWrite = false;
        return;
    }
    if (!m_captureEnabled || !isEnabled()) {
        return;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }
    const QMimeData *mime = clipboard->mimeData(QClipboard::Clipboard);
    if (mime == nullptr) {
        return;
    }

    // ---- ① 隐私闸门（本功能的安全红线，先判它，再谈记什么）----
    const ClipboardOriginInfo origin = WinEase::Win32::inspectClipboardOrigin();
    if (!origin.inspected) {
        ++m_skippedPrivacyCount;
        m_lastSkipReason = QStringLiteral("%1 —— 无法判定来源，按「不记录」处理").arg(origin.error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastSkipReason);
        Q_EMIT statusMessage(QStringLiteral("本条未记录：%1").arg(m_lastSkipReason));
        Q_EMIT statisticsChanged(); // 面板页脚上的"已跳过 N 条"要跟着动
        return;
    }
    if (origin.shouldSkipHistory()) {
        ++m_skippedPrivacyCount;
        m_lastSkipReason = QStringLiteral("%1 —— 本条未写入历史").arg(origin.reasonText());
        Q_EMIT statusMessage(QStringLiteral("已按来源要求跳过：%1").arg(origin.reasonText()));
        Q_EMIT statisticsChanged();
        return;
    }

    // ---- ② 取内容：文件 > 图片 > 文本 ----
    //    （复制的是一批文件时，用户心里的"那件事"是这些文件，而不是它们的缩略图）
    const QString app = foregroundAppName();
    const QString previousSkip = m_lastSkipReason;

    if (mime->hasUrls()) {
        QStringList paths;
        const QList<QUrl> urls = mime->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile()) {
                paths.append(QDir::toNativeSeparators(url.toLocalFile()));
            }
        }
        if (!paths.isEmpty()) {
            QString error;
            const Clip::AddResult result = m_store.addFiles(paths, app, nullptr, &error);
            handleAddResult(result, m_store.entries().isEmpty() ? QString()
                                                               : m_store.entries().first().preview,
                            error);
            return;
        }
    }

    if (mime->hasImage()) {
        if (m_imageMode == QLatin1String("off")) {
            m_lastSkipReason = QStringLiteral("配置里图片策略是「不记录」，本条未记录");
            Q_EMIT statusMessage(m_lastSkipReason);
            Q_EMIT statisticsChanged();
            return;
        }
        const QImage image = clipboard->image();
        QString error;
        const bool thumbnailOnly = (m_imageMode != QLatin1String("full"));
        const Clip::AddResult result =
            m_store.addImage(image, thumbnailOnly, app, nullptr, &error);
        handleAddResult(result, m_store.entries().isEmpty() ? QString()
                                                          : m_store.entries().first().preview,
                        error);
        return;
    }

    if (mime->hasText()) {
        const QString text = mime->text();
        QString error;
        const Clip::AddResult result = m_store.addText(text, app, nullptr, &error);
        handleAddResult(result, Clip::Store::previewFor(text), error);
        return;
    }

    // 其它格式（自定义二进制）：记了也还原不出可用内容，明确不动
    m_lastSkipReason = previousSkip;
    logMessage(WinEase::PluginLogLevel::Debug,
               QStringLiteral("剪贴板里只有非文本/图片/文件格式，未记录：%1")
                   .arg(origin.formats.join(QStringLiteral(", "))));
}

void ClipboardHistoryPlugin::handleAddResult(Clip::AddResult result,
                                             const QString &preview,
                                             const QString &error)
{
    switch (result) {
    case Clip::AddResult::Added:
        ++m_capturedCount;
        m_lastSkipReason.clear();
        Q_EMIT statusMessage(QStringLiteral("已记录：%1（共 %2 条）")
                                 .arg(preview.left(40))
                                 .arg(m_store.count()));
        Q_EMIT historyChanged();
        break;
    case Clip::AddResult::Replaced:
        m_lastSkipReason.clear();
        Q_EMIT statusMessage(QStringLiteral("同样内容已在历史里，已提到最前（共 %1 条）")
                                 .arg(m_store.count()));
        Q_EMIT historyChanged();
        break;
    case Clip::AddResult::SkippedEmpty:
        m_lastSkipReason = QStringLiteral("内容为空，未记录");
        Q_EMIT statisticsChanged();
        break;
    case Clip::AddResult::SkippedTooLarge:
        ++m_skippedOversizeCount;
        m_lastSkipReason = QStringLiteral("超过单条上限 %1 KB，未记录")
                               .arg(m_maxItemBytes / 1024);
        Q_EMIT statusMessage(m_lastSkipReason);
        Q_EMIT statisticsChanged();
        break;
    case Clip::AddResult::Failed:
        setLastError(error.isEmpty() ? QStringLiteral("写入历史失败") : error);
        m_lastSkipReason = QStringLiteral("写入历史失败：%1").arg(error);
        Q_EMIT errorOccurred(m_lastSkipReason);
        Q_EMIT statisticsChanged();
        break;
    }
}

// ---------------------------------------------------------------------------
//  面板
// ---------------------------------------------------------------------------

void ClipboardHistoryPlugin::openPanel()
{
    if (m_panel == nullptr) {
        auto *panel = new ClipboardHistoryPanel(this);
        panel->setAttribute(Qt::WA_DeleteOnClose, true);
        m_panel = panel;
    }
    m_panel->show();
    m_panel->raise();
    m_panel->activateWindow();
}

bool ClipboardHistoryPlugin::isPanelOpen() const
{
    return m_panel != nullptr && m_panel->isVisible();
}

void ClipboardHistoryPlugin::closePanel()
{
    if (m_panel != nullptr) {
        m_panel->close();
    }
}

// ---------------------------------------------------------------------------
//  查询与操作（面板与自检共用）
// ---------------------------------------------------------------------------

int ClipboardHistoryPlugin::entryCount() const
{
    return m_store.count();
}

QVector<Clip::Entry> ClipboardHistoryPlugin::search(const QString &keyword,
                                                    const QString &filterKey) const
{
    QVector<Clip::Entry> result = m_store.search(keyword, filterKey);
    for (Clip::Entry &entry : result) {
        // 每次刷新都重新核对"正文文件还在不在"，界面上如实标出（不装作还有）
        if (!entry.payloadFile.isEmpty()) {
            entry.payloadMissing = !QFile::exists(m_store.payloadPath(entry));
        }
    }
    return result;
}

bool ClipboardHistoryPlugin::setFavorite(const QString &id, bool favorite)
{
    const bool ok = m_store.setFavorite(id, favorite);
    if (ok) {
        Q_EMIT historyChanged();
    }
    return ok;
}

bool ClipboardHistoryPlugin::removeEntry(const QString &id, QString *messageOut)
{
    QString error;
    const bool ok = m_store.remove(id, &error);
    if (ok) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("已删除这条");
        }
        Q_EMIT historyChanged();
        return true;
    }
    if (messageOut != nullptr) {
        *messageOut = error;
    }
    return false;
}

int ClipboardHistoryPlugin::clearAll(bool alsoClearClipboard, QString *messageOut)
{
    QString error;
    const int removed = m_store.clear(&error);

    bool clipboardCleared = true;
    QString clipboardMessage;
    if (alsoClearClipboard) {
        clipboardCleared = clearSystemClipboard(&clipboardMessage);
    }

    Q_EMIT historyChanged();

    if (messageOut != nullptr) {
        if (!error.isEmpty()) {
            *messageOut = QStringLiteral("清了 %1 条，但索引落盘有问题：%2").arg(removed).arg(error);
        } else if (!clipboardCleared) {
            *messageOut = QStringLiteral("已清空 %1 条历史；系统剪贴板没清掉（%2）")
                              .arg(removed)
                              .arg(clipboardMessage);
        } else if (alsoClearClipboard) {
            *messageOut = QStringLiteral("已清空 %1 条历史，并清空了系统剪贴板").arg(removed);
        } else {
            *messageOut = QStringLiteral("已清空 %1 条历史").arg(removed);
        }
    }
    return removed;
}

bool ClipboardHistoryPlugin::clearSystemClipboard(QString *messageOut)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("清不了系统剪贴板：取不到剪贴板对象");
        }
        return false;
    }
    // 这是我们自己写剪贴板 → 别把"清空"当成一条新内容记下来
    m_selfWrite = true;
    clipboard->clear(QClipboard::Clipboard);
    m_selfWrite = false;

    const QMimeData *remaining = clipboard->mimeData(QClipboard::Clipboard);
    const bool stillHasContent = !clipboard->text(QClipboard::Clipboard).isEmpty()
        || (remaining != nullptr && !remaining->formats().isEmpty());
    if (stillHasContent) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("系统剪贴板没清干净（可能被其它程序抢占了）");
        }
        return false;
    }
    if (messageOut != nullptr) {
        *messageOut = QStringLiteral("已清空系统剪贴板");
    }
    return true;
}

QString ClipboardHistoryPlugin::entryBodyText(const QString &id, QString *errorOut) const
{
    bool found = false;
    const Clip::Entry entry = m_store.entry(id, &found);
    if (!found) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("历史里没有这条记录");
        }
        return QString();
    }

    if (entry.kind == Clip::Kind::Text) {
        QString text;
        QString error;
        if (!m_store.readText(id, &text, &error)) {
            if (errorOut != nullptr) {
                *errorOut = error;
            }
            return QString();
        }
        return text;
    }

    if (entry.payloadFile.isEmpty()) {
        return QStringLiteral("（这条没有正文文件）");
    }
    const QString path = m_store.payloadPath(entry);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("读不了正文文件 %1：%2").arg(path, file.errorString());
        }
        return QString();
    }
    const QByteArray raw = file.read(kBodyMaxChars * 4);

    if (entry.kind == Clip::Kind::Files) {
        return QString::fromUtf8(raw);
    }

    // 图片：说明"存的是哪一档"，并把尺寸写清楚（用户不会以为缩略图就是原图）
    const QImage image = QImage::fromData(raw);
    return QStringLiteral("图片（%1）\n存储尺寸：%2×%3\n原图尺寸：%4×%5")
        .arg(entry.thumbnailOnly ? QStringLiteral("只存了缩略图，粘贴回去的也是缩略图")
                                 : QStringLiteral("存了原图"))
        .arg(image.width())
        .arg(image.height())
        .arg(entry.originalWidth)
        .arg(entry.originalHeight);
}

QString ClipboardHistoryPlugin::storeDirectory() const
{
    return m_storageDir;
}

bool ClipboardHistoryPlugin::activate(const QString &id, bool pasteBack, QString *messageOut)
{
    bool found = false;
    const Clip::Entry entry = m_store.entry(id, &found);
    if (!found) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("历史里没有这条记录");
        }
        return false;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("取不到剪贴板对象");
        }
        return false;
    }

    QString detail;
    if (entry.kind == Clip::Kind::Text) {
        QString text;
        QString error;
        if (!m_store.readText(id, &text, &error)) {
            if (messageOut != nullptr) {
                *messageOut = error;
            }
            return false;
        }
        // 走共用工具：写入 + 读回校验（剪贴板可能被别的进程短暂占用）
        m_selfWrite = true;
        const bool ok = ClipboardTools::setText(text);
        m_selfWrite = false;
        if (!ok) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("写入剪贴板失败（可能被其它程序占用）");
            }
            return false;
        }
        detail = Clip::Store::previewFor(text, 30);
    } else if (entry.kind == Clip::Kind::Image) {
        QFile file(m_store.payloadPath(entry));
        if (!file.open(QIODevice::ReadOnly)) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("读不了图片正文：%1").arg(file.errorString());
            }
            return false;
        }
        const QImage image = QImage::fromData(file.readAll());
        if (image.isNull()) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("图片正文已损坏，解不出来");
            }
            return false;
        }
        m_selfWrite = true;
        clipboard->setImage(image, QClipboard::Clipboard);
        m_selfWrite = false;
        detail = entry.thumbnailOnly
            ? QStringLiteral("图片 %1×%2（注意：历史里只存了缩略图）")
                  .arg(image.width())
                  .arg(image.height())
            : QStringLiteral("图片 %1×%2").arg(image.width()).arg(image.height());
    } else {
        QFile file(m_store.payloadPath(entry));
        if (!file.open(QIODevice::ReadOnly)) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("读不了文件清单：%1").arg(file.errorString());
            }
            return false;
        }
        const QStringList paths = QString::fromUtf8(file.readAll())
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QList<QUrl> urls;
        for (const QString &path : paths) {
            urls.append(QUrl::fromLocalFile(path));
        }
        auto *mime = new QMimeData();
        mime->setUrls(urls);
        // ⚠ setMimeData 接管 mime 的所有权（剪贴板自己会 delete）
        m_selfWrite = true;
        clipboard->setMimeData(mime, QClipboard::Clipboard);
        m_selfWrite = false;
        detail = QStringLiteral("%1 项文件").arg(paths.size());
    }

    if (pasteBack && m_pasteBack) {
        // 向"当前前台窗口"注入 Ctrl+V：前台若是管理员程序会被 UIPI 丢弃，
        // 所以失败只如实说明，不当作错误（内容已经在剪贴板里了）
        if (!ClipboardTools::pasteBack()) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("已复制 %1，但粘贴失败（前台窗口可能不接受注入）").arg(detail);
            }
            return true;
        }
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("已粘贴 %1").arg(detail);
        }
        return true;
    }

    if (messageOut != nullptr) {
        *messageOut = pasteBack
            ? QStringLiteral("已复制 %1（配置里没开「粘贴回前台窗口」，只放到剪贴板）").arg(detail)
            : QStringLiteral("已复制 %1 到剪贴板").arg(detail);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *ClipboardHistoryPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *box = new QGroupBox(QStringLiteral("记录规则"), widget);
    auto *form = new QFormLayout(box);

    auto *maxItemsSpin = new QSpinBox(box);
    maxItemsSpin->setObjectName(QStringLiteral("cliphistMaxItemsSpin"));
    maxItemsSpin->setRange(1, 10000);
    maxItemsSpin->setValue(m_maxItems);
    maxItemsSpin->setSuffix(QStringLiteral(" 条"));
    form->addRow(QStringLiteral("条数上限（收藏不计入）"), maxItemsSpin);

    auto *maxKbSpin = new QSpinBox(box);
    maxKbSpin->setObjectName(QStringLiteral("cliphistMaxKbSpin"));
    maxKbSpin->setRange(0, 1024 * 1024);
    maxKbSpin->setValue(static_cast<int>(m_maxItemBytes / 1024));
    maxKbSpin->setSuffix(QStringLiteral(" KB"));
    maxKbSpin->setSpecialValueText(QStringLiteral("不限"));
    form->addRow(QStringLiteral("单条大小上限（超过就不记录）"), maxKbSpin);

    auto *imageCombo = new QComboBox(box);
    imageCombo->setObjectName(QStringLiteral("cliphistImageModeCombo"));
    imageCombo->addItem(QStringLiteral("不记录图片"), QStringLiteral("off"));
    imageCombo->addItem(QStringLiteral("只存缩略图（粘贴回去的也是缩略图）"),
                        QStringLiteral("thumbnail"));
    imageCombo->addItem(QStringLiteral("存原图"), QStringLiteral("full"));
    const int imageIndex = imageCombo->findData(m_imageMode);
    imageCombo->setCurrentIndex(imageIndex >= 0 ? imageIndex : 1);
    form->addRow(QStringLiteral("图片"), imageCombo);

    auto *confirmBox = new QCheckBox(QStringLiteral("清空前弹二次确认"), box);
    confirmBox->setObjectName(QStringLiteral("cliphistConfirmCheck"));
    confirmBox->setChecked(m_confirm);
    form->addRow(QString(), confirmBox);

    auto *pasteBackBox =
        new QCheckBox(QStringLiteral("面板「粘贴到光标处」真的注入 Ctrl+V（关掉则只写剪贴板）"), box);
    pasteBackBox->setObjectName(QStringLiteral("cliphistPasteBackCheck"));
    pasteBackBox->setChecked(m_pasteBack);
    form->addRow(QString(), pasteBackBox);

    layout->addWidget(box);

    const QString dir = m_storageDir;
    auto *hint = new QLabel(
        QStringLiteral("历史文件位置：%1\n"
                       "隐私：剪贴板上标着「不记录」的内容一律不写入历史；"
                       "剪贴板打不开、无法判定来源时也按「不记录」处理（宁可漏记，不可误记）。\n"
                       "停用功能不会删除历史——那是你自己的数据。")
            .arg(QDir::toNativeSeparators(dir)),
        widget);
    hint->setObjectName(QStringLiteral("cliphistStorageHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    connect(maxItemsSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_maxItems = value;
        m_store.setMaxItems(value);
        m_store.trimToLimit();
        m_store.save();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("maxItems"), value);
            svc->syncConfig();
        }
        Q_EMIT historyChanged();
    });

    connect(maxKbSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_maxItemBytes = static_cast<qint64>(value) * 1024;
        m_store.setMaxItemBytes(m_maxItemBytes);
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("maxItemKb"), value);
            svc->syncConfig();
        }
        Q_EMIT historyChanged();
    });

    connect(imageCombo, &QComboBox::currentIndexChanged, widget, [this, imageCombo](int) {
        m_imageMode = imageCombo->currentData().toString();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("imageMode"), m_imageMode);
            svc->syncConfig();
        }
        Q_EMIT historyChanged();
    });

    connect(confirmBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_confirm = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("confirm"), checked);
            svc->syncConfig();
        }
    });

    connect(pasteBackBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_pasteBack = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("pasteBack"), checked);
            svc->syncConfig();
        }
    });

    return widget;
}
