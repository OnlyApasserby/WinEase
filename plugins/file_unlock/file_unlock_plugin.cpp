#include "file_unlock_plugin.h"

#include "ClipboardTools.h"
#include "UnlockPolicy.h"

#include "sdk/ElevationService.h"
#include "sdk/PluginServices.h"
#include "win32/ProcessUtils.h"
#include "win32/ShellUtils.h"
#include "win32/Win32Error.h"

#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Common::isDisruptive;
using WinEase::Common::lockerRoleText;
using WinEase::Common::needsElevation;
using WinEase::Common::protectedReason;
using WinEase::Common::systemSaysCannotShutDown;
using WinEase::Common::terminationWarning;
using WinEase::Win32::FileLocker;

/// 二次确认的文案（自检会断言"没勾选就点不动"）
const QString kConfirmText =
    QStringLiteral("我确认结束这些进程可能导致未保存的数据丢失");

/// 把用户给的路径归一化成绝对路径（Restart Manager 只接受绝对路径）
QString normalizePath(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"'))) {
        text = text.mid(1, text.size() - 2).trimmed(); // Explorer 复制出来的路径常带引号
    }
    if (text.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(QFileInfo(text).absoluteFilePath());
}

} // namespace

FileUnlockPlugin::FileUnlockPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString FileUnlockPlugin::id() const
{
    return QStringLiteral("file.unlock");
}

QString FileUnlockPlugin::name() const
{
    return QStringLiteral("文件锁定解除");
}

QString FileUnlockPlugin::description() const
{
    return QStringLiteral("查出是哪个进程占用了删不掉的文件，结束它之后再把文件移进回收站");
}

QIcon FileUnlockPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory FileUnlockPlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList FileUnlockPlugin::tags() const
{
    return { QStringLiteral("占用"), QStringLiteral("删不掉"), QStringLiteral("锁定"),
             QStringLiteral("解锁"), QStringLiteral("unlock"), QStringLiteral("Restart Manager") };
}

bool FileUnlockPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence FileUnlockPlugin::defaultHotkey() const
{
    // 「无感」入口：在资源管理器里复制一个文件，然后按它（和端口占用查看 P1-12 同一种形态）
    return QKeySequence(QStringLiteral("Ctrl+Alt+U"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool FileUnlockPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        // 自检把它设成 false 以免卡在没人点的模态框上（踩坑 #17：绕过确认的开关落在插件配置里）
        m_confirm = svc->configValue(id(), QStringLiteral("confirm"), true).toBool();
    }
    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("文件锁定解除已就绪（占用者查询走 Restart Manager，免提权只读）"));
    return true;
}

void FileUnlockPlugin::shutdown()
{
    m_lockers.clear();
    m_path.clear();
}

bool FileUnlockPlugin::canEnable(QString *reason) const
{
    Q_UNUSED(reason)
    // 查询只读、无硬前提：查不到占用者就是"查不到"，不需要禁用功能
    return true;
}

bool FileUnlockPlugin::onEnable()
{
    m_lastEvent = m_path.isEmpty() ? QStringLiteral("把文件拖进来，或选择/粘贴一个路径后点「查找占用进程」")
                                   : QStringLiteral("上次查询：%1").arg(m_path);
    Q_EMIT statusMessage(m_lastEvent);

    // ⚠ 时序（踩坑 #51 / #57）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void FileUnlockPlugin::onDisable()
{
    m_lockers.clear();
    m_lastEvent = QStringLiteral("已停用（没有结束任何进程，也没有删除任何文件）");
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

void FileUnlockPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    Q_UNUSED(hotkeyId)

    const QString text = WinEase::FeaturePlugins::ClipboardTools::clipboardText();
    const QString candidate = normalizePath(text.section(QLatin1Char('\n'), 0, 0));
    if (candidate.isEmpty()) {
        m_lastResult = QStringLiteral("快捷键入口：剪贴板里没有路径（先在资源管理器里复制一个文件）");
        Q_EMIT statusMessage(m_lastResult);
        refreshPanel();
        return;
    }

    if (m_pathEdit != nullptr) {
        m_pathEdit->setText(QDir::toNativeSeparators(candidate));
    }
    scanPath(candidate);
}

bool FileUnlockPlugin::eventFilter(QObject *watched, QEvent *event)
{
    if (m_panel != nullptr && watched == m_panel.data()) {
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (drag->mimeData() != nullptr && drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            const QList<QUrl> urls = drop->mimeData()->urls();
            if (!urls.isEmpty()) {
                const QString local = normalizePath(urls.first().toLocalFile());
                if (!local.isEmpty()) {
                    if (m_pathEdit != nullptr) {
                        m_pathEdit->setText(QDir::toNativeSeparators(local));
                    }
                    drop->acceptProposedAction();
                    scanPath(local);
                    return true;
                }
            }
        }
    }
    return WinEase::IFeaturePlugin::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

void FileUnlockPlugin::scanFromEdit()
{
    scanPath(m_pathEdit != nullptr ? m_pathEdit->text() : QString());
}

void FileUnlockPlugin::scanPath(const QString &path)
{
    const QString target = normalizePath(path);
    if (target.isEmpty()) {
        m_path.clear();
        m_lockers.clear();
        m_lastEvent = QStringLiteral("请先给出一个文件或文件夹路径");
        refreshPanel();
        return;
    }

    m_path = target;

    const QFileInfo info(m_path);
    if (!info.exists()) {
        m_lockers.clear();
        m_lastEvent = QStringLiteral("路径不存在：%1").arg(m_path);
        refreshPanel();
        return;
    }

    refreshLockers();

    if (m_lastEvent.startsWith(QStringLiteral("查询失败"))) {
        return; // refreshLockers 已经写好原因
    }

    const QString what = info.isDir() ? QStringLiteral("该文件夹（或其下的文件）") : QStringLiteral("该文件");
    if (m_lockers.isEmpty()) {
        m_lastEvent = QStringLiteral("没有**进程**在占用%1 —— 但驱动级占用（杀软实时扫描、索引服务）"
                                     "查不出来，如果仍然删不掉，请先关掉相关安全软件再试")
                          .arg(what);
    } else {
        m_lastEvent = QStringLiteral("检测到 %1 个进程占用%2%3")
                          .arg(m_lockers.size())
                          .arg(what)
                          .arg(m_protectedCount > 0
                                   ? QStringLiteral("（其中 %1 个受保护、不允许结束）")
                                         .arg(m_protectedCount)
                                   : QString());
    }
    refreshPanel();
}

void FileUnlockPlugin::refreshLockers()
{
    m_lockers.clear();
    m_protectedCount = 0;

    if (m_path.isEmpty()) {
        return;
    }

    QString error;
    m_lockers = WinEase::Win32::findFileLockers(m_path, &error);

    if (!error.isEmpty()) {
        // "查不到" ≠ "没人占用"：这句话必须原样给用户（踩坑 #12 的精神）
        setLastError(error);
        m_lastEvent = QStringLiteral("查询失败：%1").arg(error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
        return;
    }

    clearLastError();

    const quint32 selfPid = WinEase::Win32::currentProcessId();
    for (const FileLocker &locker : m_lockers) {
        if (!protectedReason(locker, selfPid).isEmpty()) {
            ++m_protectedCount;
        }
    }
}

// ---------------------------------------------------------------------------
//  结束进程
// ---------------------------------------------------------------------------

bool FileUnlockPlugin::confirmed(const QString &title, const QString &text)
{
    if (!m_confirm) {
        return true; // 配置里关掉了（自检用；用户也可以在配置里关）
    }
    return QMessageBox::warning(m_panel != nullptr ? m_panel.data() : nullptr, title, text,
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
           == QMessageBox::Yes;
}

bool FileUnlockPlugin::terminateLocker(const FileLocker &locker, QString *messageOut)
{
    const quint32 selfPid = WinEase::Win32::currentProcessId();

    // ① 保护规则优先于一切：这不是"权限不够"，是**不该做**
    const QString reason = protectedReason(locker, selfPid);
    if (!reason.isEmpty()) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("拒绝结束 %1：%2").arg(locker.describe(), reason);
        }
        return false;
    }

    // ② 需要提权的（管理员进程 / 其它登录会话）直接走助手，别去撞"拒绝访问"
    if (needsElevation(locker)) {
        WinEase::PluginServices *svc = services();
        WinEase::ElevationService *elevation = (svc != nullptr) ? svc->elevationService() : nullptr;
        if (elevation == nullptr) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("%1 需要管理员权限才能结束，但提权通道不可用"
                                             "（安全模式下不提供）")
                                  .arg(locker.describe());
            }
            return false;
        }
        if (!elevation->isHelperAvailable()) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("%1 需要管理员权限才能结束，但提权助手当前不可用 —— "
                                             "请先启动助手（或关闭该程序）后重试")
                                  .arg(locker.describe());
            }
            return false;
        }

        QVariantMap arguments;
        arguments.insert(QStringLiteral("pid"), static_cast<uint>(locker.pid));
        const WinEase::ElevationResult result =
            elevation->execute(QStringLiteral("killProcess"), arguments);
        if (!result.ok) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("经提权助手结束 %1 失败：%2")
                                  .arg(locker.describe(), result.error);
            }
            return false;
        }
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("已通过提权助手结束 %1").arg(locker.describe());
        }
        return true;
    }

    // ③ 普通进程：本地结束（能本地做的绝不惊动提权助手）
    QString error;
    if (WinEase::Win32::terminateProcess(locker.pid, 1, &error)) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("已结束 %1").arg(locker.describe());
        }
        return true;
    }

    // ④ 本地失败（个别进程受保护或权限不足）→ 再试提权助手，并**如实说明走了哪条路**
    WinEase::PluginServices *svc = services();
    WinEase::ElevationService *elevation = (svc != nullptr) ? svc->elevationService() : nullptr;
    if (elevation != nullptr && elevation->isHelperAvailable()) {
        QVariantMap arguments;
        arguments.insert(QStringLiteral("pid"), static_cast<uint>(locker.pid));
        const WinEase::ElevationResult result =
            elevation->execute(QStringLiteral("killProcess"), arguments);
        if (result.ok) {
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("本地结束失败（%1），已改用提权助手结束 %2")
                                  .arg(error, locker.describe());
            }
            return true;
        }
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("结束 %1 失败：%2；改用提权助手也失败：%3")
                              .arg(locker.describe(), error, result.error);
        }
        return false;
    }

    if (messageOut != nullptr) {
        *messageOut = QStringLiteral("结束 %1 失败：%2（提权助手不可用，无法以管理员身份重试）")
                          .arg(locker.describe(), error);
    }
    return false;
}

void FileUnlockPlugin::killSelected()
{
    const int row = (m_lockerList != nullptr) ? m_lockerList->currentRow() : -1;
    if (row < 0 || row >= m_lockers.size()) {
        m_lastResult = QStringLiteral("请先在上面的列表里选中一个进程");
        refreshPanel();
        return;
    }

    const FileLocker locker = m_lockers.at(row);

    if (!protectedReason(locker, WinEase::Win32::currentProcessId()).isEmpty()) {
        m_lastResult = QStringLiteral("未执行：%1")
                           .arg(protectedReason(locker, WinEase::Win32::currentProcessId()));
        Q_EMIT statusMessage(m_lastResult);
        refreshPanel();
        return;
    }

    // ★ 二次确认：结束一个正在编辑文件的程序，用户会丢还没保存的内容
    if (m_confirmCheck == nullptr || !m_confirmCheck->isChecked()) {
        m_lastResult = QStringLiteral("未执行：请先勾选「%1」").arg(kConfirmText);
        Q_EMIT statusMessage(m_lastResult);
        refreshPanel();
        return;
    }

    if (!confirmed(QStringLiteral("结束进程"), terminationWarning({ locker }))) {
        m_lastResult = QStringLiteral("已取消（进程没有被结束）");
        refreshPanel();
        return;
    }

    QString message;
    const bool ok = terminateLocker(locker, &message);
    m_lastResult = message;

    if (ok) {
        clearLastError();
        logMessage(WinEase::PluginLogLevel::Info, message);
        Q_EMIT statusMessage(message);
        if (m_confirmCheck != nullptr) {
            m_confirmCheck->setChecked(false); // 一次确认只对一次动作有效
        }
        refreshLockers();
        // 进程刚结束，句柄由内核回收 —— 稍后再查一遍，列表才不会停在旧结果上
        QTimer::singleShot(300, this, [this] { refreshLockers(); refreshPanel(); });
    } else {
        setLastError(message);
        logMessage(WinEase::PluginLogLevel::Warning, message);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), message);
        }
        Q_EMIT statusMessage(message);
        refreshLockers();
    }

    refreshPanel();
}

// ---------------------------------------------------------------------------
//  重试删除（回收站）
// ---------------------------------------------------------------------------

void FileUnlockPlugin::deleteCurrent()
{
    if (m_path.isEmpty()) {
        m_lastResult = QStringLiteral("请先给出一个文件或文件夹路径");
        refreshPanel();
        return;
    }

    const QFileInfo info(m_path);
    if (!info.exists()) {
        m_lockers.clear();
        m_lastResult = QStringLiteral("它已经不在了（可能刚才已经被删掉）");
        refreshPanel();
        return;
    }

    if (!confirmed(QStringLiteral("移到回收站"),
                   QStringLiteral("把「%1」移到回收站？\n\n它会被放进回收站而不是永久删除，"
                                  "随时可以还原。")
                       .arg(m_path))) {
        m_lastResult = QStringLiteral("已取消（文件没有被移动）");
        refreshPanel();
        return;
    }

    QString error;
    if (WinEase::Win32::moveToRecycleBin({ m_path }, &error)) {
        clearLastError();
        m_lockers.clear();
        m_protectedCount = 0;
        m_lastResult = QStringLiteral("已移到回收站（可随时还原）：%1").arg(m_path);
        logMessage(WinEase::PluginLogLevel::Info, m_lastResult);
        Q_EMIT statusMessage(m_lastResult);
        refreshPanel();
        return;
    }

    // ⚠ 删除失败时**立刻重新查一遍**，把"还剩谁在占用"直接说出来 ——
    //    只说"删除失败"等于把用户丢回原点
    refreshLockers();
    QStringList who;
    for (const FileLocker &locker : m_lockers) {
        who << locker.describe();
    }
    const QString tail = who.isEmpty()
                             ? QStringLiteral("（这次没有查到进程占用 —— 可能是驱动级占用、"
                                              "权限不足，或它自己就是只读的）")
                             : QStringLiteral("（仍被占用：%1）").arg(who.join(QStringLiteral("、")));
    m_lastResult = QStringLiteral("移到回收站失败：%1%2").arg(error, tail);
    setLastError(m_lastResult);
    logMessage(WinEase::PluginLogLevel::Warning, m_lastResult);
    if (WinEase::PluginServices *svc = services()) {
        svc->notify(name(), m_lastResult);
    }
    Q_EMIT statusMessage(m_lastResult);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QWidget *FileUnlockPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("unlockPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("unlockStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *pathRow = new QHBoxLayout();
    auto *pathEdit = new QLineEdit(widget);
    pathEdit->setObjectName(QStringLiteral("unlockPathEdit"));
    pathEdit->setPlaceholderText(QStringLiteral("删不掉的那个文件 / 文件夹的完整路径（也可以直接拖进来）"));
    pathRow->addWidget(pathEdit, 1);
    auto *browseButton = new QPushButton(QStringLiteral("选择文件…"), widget);
    browseButton->setObjectName(QStringLiteral("unlockBrowseButton"));
    pathRow->addWidget(browseButton);
    auto *pasteButton = new QPushButton(QStringLiteral("从剪贴板取路径"), widget);
    pasteButton->setObjectName(QStringLiteral("unlockPasteButton"));
    pathRow->addWidget(pasteButton);
    layout->addLayout(pathRow);

    auto *scanButton = new QPushButton(QStringLiteral("查找占用进程"), widget);
    scanButton->setObjectName(QStringLiteral("unlockScanButton"));
    layout->addWidget(scanButton, 0, Qt::AlignLeft);

    auto *lockerList = new QListWidget(widget);
    lockerList->setObjectName(QStringLiteral("unlockLockerList"));
    lockerList->setMinimumHeight(140);
    layout->addWidget(lockerList);

    auto *detailLabel = new QLabel(widget);
    detailLabel->setObjectName(QStringLiteral("unlockDetailLabel"));
    detailLabel->setWordWrap(true);
    detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailLabel);

    auto *confirmCheck = new QCheckBox(kConfirmText, widget);
    confirmCheck->setObjectName(QStringLiteral("unlockConfirmCheck"));
    layout->addWidget(confirmCheck);

    auto *buttonRow = new QHBoxLayout();
    auto *killButton = new QPushButton(QStringLiteral("结束选中的进程"), widget);
    killButton->setObjectName(QStringLiteral("unlockKillButton"));
    buttonRow->addWidget(killButton);
    auto *deleteButton = new QPushButton(QStringLiteral("重试删除（移到回收站）"), widget);
    deleteButton->setObjectName(QStringLiteral("unlockDeleteButton"));
    buttonRow->addWidget(deleteButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *resultLabel = new QLabel(widget);
    resultLabel->setObjectName(QStringLiteral("unlockResultLabel"));
    resultLabel->setWordWrap(true);
    layout->addWidget(resultLabel);

    auto *hint = new QLabel(
        QStringLiteral("占用者来自 Windows 自带的 **Restart Manager**（安装程序判断"
                       "「要重启才能替换哪些文件」用的就是它）：**免提权、只读**。\n"
                       "⚠ 它只看**进程**：杀软实时扫描、索引服务这类驱动级占用查不出来 —— "
                       "所以「列表为空」的意思是「没有**进程**在占用」，不是「没人用它」。\n"
                       "· 系统关键进程与 WinEase 自身**不允许结束**（按钮会禁用并写明理由）；\n"
                       "· 结束会导致未保存的内容丢失，所以必须勾选确认；"
                       "管理员进程走提权助手（会请求一次 UAC）；\n"
                       "· 「重试删除」走**回收站**（可还原）。想永久删，请用「敏感文件粉碎」(P2-12)。"),
        widget);
    hint->setObjectName(QStringLiteral("unlockHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_pathEdit = pathEdit;
    m_browseButton = browseButton;
    m_pasteButton = pasteButton;
    m_scanButton = scanButton;
    m_lockerList = lockerList;
    m_detailLabel = detailLabel;
    m_confirmCheck = confirmCheck;
    m_killButton = killButton;
    m_deleteButton = deleteButton;
    m_resultLabel = resultLabel;

    // 拖入文件 / 文件夹
    widget->setAcceptDrops(true);
    widget->installEventFilter(this);

    connect(scanButton, &QPushButton::clicked, widget, [this] { scanFromEdit(); });
    connect(browseButton, &QPushButton::clicked, widget, [this] {
        const QString picked = QFileDialog::getOpenFileName(
            m_panel.data(), QStringLiteral("选择被占用的文件"), m_path);
        if (!picked.isEmpty()) {
            if (m_pathEdit != nullptr) {
                m_pathEdit->setText(QDir::toNativeSeparators(picked));
            }
            scanPath(picked);
        }
    });
    connect(pasteButton, &QPushButton::clicked, widget, [this] {
        const QString text =
            WinEase::FeaturePlugins::ClipboardTools::clipboardText();
        const QString candidate = normalizePath(text.section(QLatin1Char('\n'), 0, 0));
        if (candidate.isEmpty()) {
            m_lastResult = QStringLiteral("剪贴板里没有可用的路径");
            refreshPanel();
            return;
        }
        if (m_pathEdit != nullptr) {
            m_pathEdit->setText(QDir::toNativeSeparators(candidate));
        }
        scanPath(candidate);
    });
    connect(killButton, &QPushButton::clicked, widget, [this] { killSelected(); });
    connect(deleteButton, &QPushButton::clicked, widget, [this] { deleteCurrent(); });
    connect(confirmCheck, &QCheckBox::toggled, widget, [this](bool) { updateActionState(); });
    connect(pathEdit, &QLineEdit::returnPressed, widget, [this] { scanFromEdit(); });
    // ⚠ 选中项变化**只刷新详情与按钮状态**：绝不回手重填列表 ——
    //    重填会改当前行 → 再发 currentRowChanged → 再进来 → 无限递归
    //    （踩坑 #61：P3-14 正是这么把进程打成 0xC00000FD 栈溢出的）
    connect(lockerList, &QListWidget::currentRowChanged, widget, [this](int) {
        updateDetail();
        updateActionState();
    });

    refreshPanel();
    return widget;
}

void FileUnlockPlugin::updateDetail()
{
    if (m_detailLabel == nullptr) {
        return;
    }

    const int row = (m_lockerList != nullptr) ? m_lockerList->currentRow() : -1;
    if (row < 0 || row >= m_lockers.size()) {
        m_detailLabel->setText(QStringLiteral("（选中一行查看进程详情）"));
        return;
    }

    const FileLocker &locker = m_lockers.at(row);
    const quint32 selfPid = WinEase::Win32::currentProcessId();
    const QString protectedWhy = protectedReason(locker, selfPid);

    QStringList lines;
    lines << QStringLiteral("%1 · %2").arg(locker.describe(),
                                           lockerRoleText(locker, selfPid));
    lines << (locker.path.isEmpty()
                  ? QStringLiteral("路径：读不到（系统进程或其它用户会话）")
                  : QStringLiteral("路径：%1").arg(locker.path));
    lines << QStringLiteral("类型：%1").arg(WinEase::Win32::appTypeText(locker.applicationType));
    if (locker.isService && !locker.serviceName.isEmpty()) {
        lines << QStringLiteral("服务名：%1").arg(locker.serviceName);
    }
    lines << QStringLiteral("会话：%1%2")
                 .arg(locker.sameSession ? QStringLiteral("当前登录会话")
                                         : QStringLiteral("其它登录会话"))
                 .arg(locker.isElevated ? QStringLiteral(" · 以管理员身份运行") : QString());
    if (!protectedWhy.isEmpty()) {
        lines << QStringLiteral("⚠ 不允许结束：%1").arg(protectedWhy);
    } else if (needsElevation(locker)) {
        lines << QStringLiteral("结束它需要管理员权限（会经提权助手请求一次 UAC）");
    }
    if (isDisruptive(locker)) {
        lines << QStringLiteral("⚠ 结束资源管理器会让任务栏与桌面短暂消失（系统通常会自动重启它）");
    }
    if (systemSaysCannotShutDown(locker)) {
        // ⚠ 这一句必须写成"系统是这么说的"，而不是"它是关键进程"：
        //    `RmCritical` 的成因里包含"我们没权限"和"它就是查询方自己"
        lines << QStringLiteral("系统认为它「关不掉、要重启才能释放」—— 这可能只是权限不足，"
                                "也可能它不接受被关闭（**不等于**它是系统关键进程）");
    }
    if (locker.restartable) {
        lines << QStringLiteral("系统认为它可以在恢复后自动回到原状态");
    }

    m_detailLabel->setText(lines.join(QStringLiteral("\n")));
}

void FileUnlockPlugin::refreshLockerList()
{
    if (m_lockerList == nullptr) {
        return;
    }

    // ⚠ 重填列表必须屏蔽信号（clear()/setCurrentRow() 都会发 currentRowChanged，
    //    不挡就会和"选中项变化 → 刷新详情"的回调互相触发，踩坑 #61）
    const QSignalBlocker blocker(m_lockerList.data());

    // 保持选中项：按 PID 匹配（进程刚结束时列表会变短、下标会变）
    quint32 keepPid = 0;
    const int previousRow = m_lockerList->currentRow();
    if (previousRow >= 0 && previousRow < m_lockers.size()) {
        keepPid = m_lockers.at(previousRow).pid;
    }

    m_lockerList->clear();
    const quint32 selfPid = WinEase::Win32::currentProcessId();
    int restoreRow = -1;
    for (int index = 0; index < m_lockers.size(); ++index) {
        const FileLocker &locker = m_lockers.at(index);
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 —— %2").arg(locker.describe(), lockerRoleText(locker, selfPid)),
            m_lockerList.data());
        item->setToolTip(locker.path.isEmpty() ? QStringLiteral("路径读不到") : locker.path);
        if (keepPid != 0 && restoreRow < 0 && locker.pid == keepPid) {
            restoreRow = index;
        }
    }
    if (restoreRow >= 0) {
        m_lockerList->setCurrentRow(restoreRow);
    } else if (m_lockerList->count() > 0 && m_lockerList->currentRow() < 0) {
        m_lockerList->setCurrentRow(0);
    }

    updateDetail();
}

void FileUnlockPlugin::updateActionState()
{
    const bool running = isEnabled();

    if (m_lockerList != nullptr) {
        m_lockerList->setEnabled(running);
    }
    if (m_pathEdit != nullptr) {
        m_pathEdit->setEnabled(running);
    }
    if (m_scanButton != nullptr) {
        m_scanButton->setEnabled(running);
    }
    if (m_browseButton != nullptr) {
        m_browseButton->setEnabled(running);
    }
    if (m_pasteButton != nullptr) {
        m_pasteButton->setEnabled(running);
    }
    if (m_confirmCheck != nullptr) {
        m_confirmCheck->setEnabled(running && !m_lockers.isEmpty());
        if (!running && m_confirmCheck->isChecked()) {
            const QSignalBlocker blocker(m_confirmCheck.data());
            m_confirmCheck->setChecked(false);
        }
    }
    if (m_deleteButton != nullptr) {
        // 「重试删除」不要求先选中进程：解除占用之后用户想做的就是删掉它；
        // 仍然被占用时它会**如实失败并把还剩谁在占用说出来**
        m_deleteButton->setEnabled(running && !m_path.isEmpty());
    }

    // 结束按钮：功能启用 + 选中行不受保护 + 已勾选确认，三重门槛
    bool killable = false;
    if (running && m_lockerList != nullptr && m_confirmCheck != nullptr
        && m_confirmCheck->isChecked()) {
        const int row = m_lockerList->currentRow();
        if (row >= 0 && row < m_lockers.size()) {
            killable =
                protectedReason(m_lockers.at(row), WinEase::Win32::currentProcessId()).isEmpty();
        }
    }
    if (m_killButton != nullptr) {
        m_killButton->setEnabled(killable);
    }
}

void FileUnlockPlugin::refreshPanel()
{
    if (m_panel == nullptr) {
        return;
    }

    if (m_statusLabel != nullptr) {
        m_statusLabel->setText(m_lastEvent.isEmpty() ? QStringLiteral("就绪") : m_lastEvent);
    }
    if (m_resultLabel != nullptr) {
        m_resultLabel->setText(m_lastResult);
    }

    refreshLockerList();
    updateActionState();
}
