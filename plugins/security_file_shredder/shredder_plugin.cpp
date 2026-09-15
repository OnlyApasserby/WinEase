#include "shredder_plugin.h"

#include "FileShredder.h"
#include "sdk/PluginServices.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>

#include <memory>
#include <thread>

namespace Shredder = WinEase::FeaturePlugins::FileShredder;
using WinEase::FeaturePlugins::Cancel;

namespace {

/// 字节数 → "1.2 MB"
///
/// 为什么不复用 DuplicateFinder/FilePreview 里的同名函数：那会把
/// DuplicateFinder.cpp（以及 FilePreview.cpp 的 WinRT 依赖）拖进本插件 ——
/// 为了一个数字格式化不值得，十几行的东西各写一份更划算。
QString formatBytes(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("-");
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    static const char *const units[] = { "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(QString::number(value, 'f', value < 10.0 ? 1 : 0), QString::fromLatin1(units[unit]));
}

/// 界面上必须原样出现的那段话（与 FileShredder.h 顶部的说明同源）。
/// 单独抽成函数是为了让"面板正文"和"设置页提示"用同一份文案，不会改一处漏一处。
QString disclosureText()
{
    return QStringLiteral(
        "粉碎能防的是「用恢复软件按常规手段扫出旧数据」，防不了：\n"
        "· SSD 的磨损均衡 / TRIM：写下去的覆写块可能被映射到别处，原物理块由固件回收 —— "
        "这是硬件层面的事，用户态软件无法保证；\n"
        "· 快照 / 卷影副本（VSS）、文件历史、云同步里可能还有别的副本；\n"
        "· 已经产生的碎片备份、其它机器上的副本。\n"
        "所以粉碎是「提高门槛」，不是「物理保证」。SSD 上更靠谱的做法是全盘加密 + 丢弃密钥。\n"
        "★ 已粉碎的文件无法恢复，本工具不提供撤销。系统目录与盘根会被直接拒绝。");
}

} // namespace

// ============================================================================
//  粉碎面板
// ============================================================================

class ShredderWindow : public QWidget
{
public:
    explicit ShredderWindow(ShredderPlugin *plugin)
        : QWidget(nullptr)
        , m_plugin(plugin)
    {
        setObjectName(QStringLiteral("winease_file_shredder"));
        setWindowTitle(QStringLiteral("敏感文件粉碎 - WinEase"));
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_DeleteOnClose, true);
        resize(880, 760);
        buildUi();
        applyDarkStyle();
    }

    ~ShredderWindow() override { stopWorker(); }

    /// 自检用
    int modeIndex() const { return m_modeCombo->currentIndex(); }
    void setModeIndex(int index) { m_modeCombo->setCurrentIndex(index); }
    void setAcknowledge(bool checked) { m_acknowledgeCheck->setChecked(checked); }
    void clickStart() { m_startButton->click(); }
    bool shredding() const { return m_working; }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && event->position().y() <= 40) {
            if (QWindow *handle = windowHandle()) {
                handle->startSystemMove();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

private:
    void buildUi()
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto *header = new QWidget(this);
        header->setObjectName(QStringLiteral("shredHeader"));
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(12, 8, 8, 8);
        auto *title = new QLabel(QStringLiteral("敏感文件粉碎"), header);
        title->setObjectName(QStringLiteral("shredTitle"));
        auto *closeButton = new QPushButton(QStringLiteral("✕"), header);
        closeButton->setFixedSize(26, 26);
        headerLayout->addWidget(title, 1);
        headerLayout->addWidget(closeButton);
        layout->addWidget(header);

        auto *body = new QWidget(this);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(12, 8, 12, 10);

        auto *danger = new QLabel(disclosureText(), body);
        danger->setObjectName(QStringLiteral("shredDisclosure"));
        danger->setWordWrap(true);
        danger->setStyleSheet(QStringLiteral("color:#ffb4a2; background:#2a1c18;"
                                            " border:1px solid #6b3b2f; border-radius:6px;"
                                            " padding:8px;"));
        bodyLayout->addWidget(danger);

        auto *pathsBox = new QGroupBox(QStringLiteral("要粉碎的文件 / 目录（每行一个）"), body);
        auto *pathsLayout = new QVBoxLayout(pathsBox);
        m_pathsEdit = new QPlainTextEdit(pathsBox);
        m_pathsEdit->setObjectName(QStringLiteral("shredderPaths"));
        m_pathsEdit->setPlaceholderText(QStringLiteral("D:\\临时\\身份证扫描件.jpg"));
        m_pathsEdit->setFixedHeight(84);
        pathsLayout->addWidget(m_pathsEdit);
        auto *pathButtons = new QHBoxLayout();
        auto *addFilesButton = new QPushButton(QStringLiteral("添加文件…"), pathsBox);
        auto *addDirButton = new QPushButton(QStringLiteral("添加目录…"), pathsBox);
        auto *clearButton = new QPushButton(QStringLiteral("清空"), pathsBox);
        pathButtons->addWidget(addFilesButton);
        pathButtons->addWidget(addDirButton);
        pathButtons->addWidget(clearButton);
        pathButtons->addStretch(1);
        pathsLayout->addLayout(pathButtons);
        bodyLayout->addWidget(pathsBox);

        auto *form = new QFormLayout();
        m_modeCombo = new QComboBox(body);
        m_modeCombo->setObjectName(QStringLiteral("shredderMode"));
        const Shredder::PassMode modes[] = { Shredder::PassMode::ZeroSinglePass,
                                            Shredder::PassMode::RandomSinglePass,
                                            Shredder::PassMode::RandomThreePass,
                                            Shredder::PassMode::DodThreePass };
        for (Shredder::PassMode mode : modes) {
            m_modeCombo->addItem(QStringLiteral("%1（%2 遍）")
                                     .arg(Shredder::passModeDisplayName(mode))
                                     .arg(Shredder::passCount(mode)),
                                 Shredder::passModeKey(mode));
        }
        // 三个控件的初值都来自配置（面板与引擎必须同一份数，不能各写一份默认值）
        m_modeCombo->setCurrentIndex(qMax(0, m_modeCombo->findData(m_plugin->modeKey())));
        form->addRow(QStringLiteral("覆写方式"), m_modeCombo);

        m_renameCheck = new QCheckBox(QStringLiteral("覆写后先改名再删除（让按原名恢复目录项也失效）"),
                                      body);
        m_renameCheck->setObjectName(QStringLiteral("shredderRename"));
        m_renameCheck->setChecked(m_plugin->renameBeforeDelete());
        form->addRow(QString(), m_renameCheck);
        m_verifyCheck = new QCheckBox(QStringLiteral("每遍覆写后回读校验（更慢，但能确认确实写进去了）"),
                                      body);
        m_verifyCheck->setObjectName(QStringLiteral("shredderVerify"));
        m_verifyCheck->setChecked(m_plugin->verifyEachPass());
        form->addRow(QString(), m_verifyCheck);
        m_acknowledgeCheck = new QCheckBox(QStringLiteral("我知道这些文件无法恢复，且本工具不能撤销"), body);
        m_acknowledgeCheck->setObjectName(QStringLiteral("shredderAcknowledge"));
        form->addRow(QString(), m_acknowledgeCheck);
        bodyLayout->addLayout(form);

        auto *actionRow = new QHBoxLayout();
        m_startButton = new QPushButton(QStringLiteral("开始粉碎"), body);
        m_startButton->setObjectName(QStringLiteral("shredderStart"));
        m_cancelButton = new QPushButton(QStringLiteral("取消"), body);
        m_cancelButton->setObjectName(QStringLiteral("shredderCancel"));
        m_cancelButton->setEnabled(false);
        actionRow->addWidget(m_startButton);
        actionRow->addWidget(m_cancelButton);
        actionRow->addStretch(1);
        bodyLayout->addLayout(actionRow);

        m_progress = new QProgressBar(body);
        m_progress->setObjectName(QStringLiteral("shredderProgress"));
        m_progress->setRange(0, 100);
        bodyLayout->addWidget(m_progress);

        m_stageLabel = new QLabel(body);
        m_stageLabel->setObjectName(QStringLiteral("shredderStage"));
        m_stageLabel->setWordWrap(true);
        // 一打开就把"这几遍到底写的是什么"摊开给用户看
        m_stageLabel->setText(Shredder::passDescriptions(currentMode()).join(QStringLiteral(" → ")));
        bodyLayout->addWidget(m_stageLabel);

        m_statusLabel = new QLabel(body);
        m_statusLabel->setObjectName(QStringLiteral("shredderStatus"));
        m_statusLabel->setWordWrap(true);
        m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bodyLayout->addWidget(m_statusLabel, 1);

        layout->addWidget(body, 1);

        QObject::connect(closeButton, &QPushButton::clicked, this, [this] { close(); });
        QObject::connect(addFilesButton, &QPushButton::clicked, this, [this] { appendFiles(); });
        QObject::connect(addDirButton, &QPushButton::clicked, this, [this] { appendDirectory(); });
        QObject::connect(clearButton, &QPushButton::clicked, this, [this] { m_pathsEdit->clear(); });
        QObject::connect(m_startButton, &QPushButton::clicked, this, [this] { startShred(); });
        QObject::connect(m_cancelButton, &QPushButton::clicked, this, [this] { cancelShred(); });
        QObject::connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
            m_stageLabel->setText(Shredder::passDescriptions(currentMode()).join(QStringLiteral(" → ")));
            if (WinEase::PluginServices *svc = m_plugin->services()) {
                svc->setConfigValue(m_plugin->id(), QStringLiteral("mode"),
                                    m_modeCombo->itemData(index).toString());
            }
        });
    }

    void applyDarkStyle()
    {
        setStyleSheet(QStringLiteral(
            "QWidget#winease_file_shredder { background:#1b1b1b; color:#e6e6e6; }"
            "QWidget#shredHeader { background:#262626; }"
            "QLabel#shredTitle { font-weight:600; }"
            "QLabel { color:#e6e6e6; }"
            "QGroupBox { border:1px solid #3a3a3a; border-radius:6px; margin-top:10px;"
            "  padding-top:8px; }"
            "QGroupBox::title { subcontrol-origin:margin; left:10px; color:#bdbdbd; }"
            "QPlainTextEdit, QComboBox { background:#151515; color:#e0e0e0;"
            "  border:1px solid #3a3a3a; }"
            "QPushButton { background:#333; color:#e6e6e6; border:0; border-radius:4px;"
            "  padding:6px 12px; }"
            "QPushButton:hover { background:#444; }"
            "QPushButton:disabled { background:#2a2a2a; color:#777; }"
            "QProgressBar { border:1px solid #3a3a3a; border-radius:4px; background:#151515;"
            "  text-align:center; color:#e0e0e0; }"
            "QProgressBar::chunk { background:#b3543f; }"));
    }

    Shredder::PassMode currentMode() const
    {
        bool ok = false;
        const Shredder::PassMode mode =
            Shredder::passModeFromKey(m_modeCombo->currentData().toString(), &ok);
        return ok ? mode : Shredder::PassMode::ZeroSinglePass;
    }

    Shredder::Options currentOptions() const
    {
        Shredder::Options options;
        options.mode = currentMode();
        options.renameBeforeDelete = m_renameCheck->isChecked();
        options.verifyEachPass = m_verifyCheck->isChecked();
        return options;
    }

    QStringList currentPaths() const
    {
        QStringList paths;
        const QStringList lines = m_pathsEdit->toPlainText().split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                paths.append(QDir::toNativeSeparators(QDir::cleanPath(trimmed)));
            }
        }
        return paths;
    }

    void appendFiles()
    {
        const QStringList chosen = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择要粉碎的文件"), QDir::homePath());
        appendLines(chosen);
    }

    void appendDirectory()
    {
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择要粉碎的目录（会递归粉碎里面的所有文件）"), QDir::homePath());
        if (!chosen.isEmpty()) {
            appendLines({ chosen });
        }
    }

    void appendLines(const QStringList &paths)
    {
        QString text = m_pathsEdit->toPlainText().trimmed();
        for (const QString &path : paths) {
            if (!text.isEmpty()) {
                text += QLatin1Char('\n');
            }
            text += QDir::toNativeSeparators(path);
        }
        m_pathsEdit->setPlainText(text);
    }

    void setStatus(const QString &text) { m_statusLabel->setText(text); }

    // ------------------------------------------------------------------
    //  粉碎
    // ------------------------------------------------------------------

    void startShred()
    {
        if (m_working) {
            return;
        }
        const QStringList paths = currentPaths();
        if (paths.isEmpty()) {
            setStatus(QStringLiteral("先指定要粉碎的文件或目录。"));
            return;
        }
        if (!m_acknowledgeCheck->isChecked()) {
            // 不弹窗、不静默：把话说明白，让用户自己勾
            setStatus(QStringLiteral("请先勾选「我知道这些文件无法恢复」——"
                                     "粉碎是本工具里唯一不可撤销的操作。"));
            return;
        }

        // 执行前后各做一次安全阀检查，把"会被拒绝的路径"提前摊开给用户看
        QStringList protectedOnes;
        for (const QString &path : paths) {
            QString reason;
            if (Shredder::isProtectedPath(path, &reason)) {
                protectedOnes.append(QStringLiteral("%1（%2）").arg(path, reason));
            }
        }
        if (!protectedOnes.isEmpty()) {
            setStatus(QStringLiteral("这些路径被保护策略拒绝，已从本次任务中剔除：\n%1")
                          .arg(protectedOnes.join(QLatin1Char('\n'))));
        }

        const Shredder::Options options = currentOptions();
        if (m_plugin->confirmBeforeShred()) {
            const QMessageBox::StandardButton answer = QMessageBox::question(
                this, QStringLiteral("确认粉碎"),
                QStringLiteral("将按「%1」粉碎 %2 个路径，共 %3 遍覆写%4。\n\n"
                               "★ 无法恢复，本工具不提供撤销。确定继续？")
                    .arg(Shredder::passModeDisplayName(options.mode))
                    .arg(paths.size())
                    .arg(Shredder::passCount(options.mode))
                    .arg(options.verifyEachPass ? QStringLiteral("，每遍回读校验")
                                                : QString()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                setStatus(QStringLiteral("已取消（没有动任何文件）。"));
                return;
            }
        }

        m_working = true;
        m_cancel = std::make_shared<Cancel>();
        m_startButton->setEnabled(false);
        m_cancelButton->setEnabled(true);
        m_progress->setRange(0, paths.size());
        m_progress->setValue(0);
        m_stageLabel->setText(Shredder::passDescriptions(options.mode).join(QStringLiteral(" → ")));
        setStatus(QStringLiteral("粉碎中…（覆写是实打实的磁盘写入，大文件请耐心等）"));

        // ★ 必须离开界面线程：覆写 100 MB × 3 遍就是 300 MB 的磁盘写入，
        //   放在界面线程上会让窗口直接"未响应"
        const QStringList readonlyPaths = paths;
        auto cancel = m_cancel;
        m_worker = std::thread([this, readonlyPaths, options, cancel] {
            const WinEase::FeaturePlugins::Progress progress =
                [this](const QString &stage, int done, int total) {
                    // ⚠ 进度回调在工作线程上 → 排队回界面线程
                    QMetaObject::invokeMethod(
                        this,
                        [this, stage, done, total] {
                            m_stageLabel->setText(total > 0
                                                      ? QStringLiteral("%1（%2 / %3）")
                                                            .arg(stage)
                                                            .arg(done)
                                                            .arg(total)
                                                      : stage);
                            if (total > 0) {
                                m_progress->setValue(qBound(0, done, total));
                            }
                        },
                        Qt::QueuedConnection);
                };
            const Shredder::Result result =
                Shredder::shredPaths(readonlyPaths, options, cancel.get(), progress);
            QMetaObject::invokeMethod(
                this, [this, result] { finishShred(result); }, Qt::QueuedConnection);
        });
    }

    void cancelShred()
    {
        if (m_cancel != nullptr) {
            m_cancel->request();
            m_cancelButton->setEnabled(false);
            setStatus(QStringLiteral("正在取消…（当前文件覆写完成后退出）"));
        }
    }

    void finishShred(const Shredder::Result &result)
    {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_working = false;
        m_startButton->setEnabled(true);
        m_cancelButton->setEnabled(false);
        m_progress->setValue(m_progress->maximum());

        const Shredder::Stats &stats = result.stats;
        QStringList lines;
        if (!result.error.isEmpty()) {
            lines.append(QStringLiteral("粉碎失败：%1").arg(result.error));
        } else if (stats.canceled) {
            lines.append(QStringLiteral("已取消：粉碎完成 %1 个文件。").arg(stats.filesShredded));
        } else {
            lines.append(QStringLiteral("粉碎完成：成功 %1 个文件，删除空目录 %2 个；"
                                        "覆写 %3（%4 遍），回读校验通过 %5 遍。")
                             .arg(stats.filesShredded)
                             .arg(stats.directoriesRemoved)
                             .arg(formatBytes(stats.bytesOverwritten))
                             .arg(stats.passesPerformed)
                             .arg(stats.verifiedPasses));
        }
        if (!stats.skipped.isEmpty()) {
            lines.append(QStringLiteral("因保护策略跳过：\n%1").arg(stats.skipped.join(QLatin1Char('\n'))));
        }
        if (!stats.failures.isEmpty()) {
            lines.append(QStringLiteral("失败（文件可能正被别的程序占用）：\n%1")
                             .arg(stats.failures.join(QLatin1Char('\n'))));
        }
        setStatus(lines.join(QStringLiteral("\n")));
        // 面板不做 Q_OBJECT（.cpp 内的类要单独 moc，不值当）：直接回调插件上报卡片状态
        m_plugin->reportStatus(lines.first());
    }

    void stopWorker()
    {
        if (m_cancel != nullptr) {
            m_cancel->request();
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_working = false;
    }

    ShredderPlugin *m_plugin = nullptr;
    QPlainTextEdit *m_pathsEdit = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QCheckBox *m_renameCheck = nullptr;
    QCheckBox *m_verifyCheck = nullptr;
    QCheckBox *m_acknowledgeCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_stageLabel = nullptr;
    QLabel *m_statusLabel = nullptr;

    std::shared_ptr<Cancel> m_cancel;
    std::thread m_worker;
    bool m_working = false;
};

// ============================================================================
//  插件本体
// ============================================================================

ShredderPlugin::ShredderPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

QString ShredderPlugin::id() const
{
    return QStringLiteral("security.file_shredder");
}

QString ShredderPlugin::name() const
{
    return QStringLiteral("敏感文件粉碎");
}

QString ShredderPlugin::description() const
{
    return QStringLiteral("多遍覆写 + 改名 + 永久删除；只提高恢复门槛，不提供撤销");
}

QIcon ShredderPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Security);
}

WinEase::FeatureCategory ShredderPlugin::category() const
{
    return WinEase::FeatureCategory::Security;
}

QStringList ShredderPlugin::tags() const
{
    return { QStringLiteral("粉碎"), QStringLiteral("shred"), QStringLiteral("擦除"),
             QStringLiteral("覆写"), QStringLiteral("隐私"), QStringLiteral("不可恢复") };
}

bool ShredderPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence ShredderPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+X"));
}

bool ShredderPlugin::initialize()
{
    loadFromConfig();
    if (WinEase::PluginServices *svc = services()) {
        svc->log(id(), WinEase::PluginLogLevel::Info,
                 QStringLiteral("敏感文件粉碎已加载（默认方式 %1，执行前确认 %2）")
                     .arg(Shredder::passModeDisplayName(
                         Shredder::passModeFromKey(m_modeKey, nullptr)))
                     .arg(m_confirm ? QStringLiteral("开") : QStringLiteral("关")));
    }
    return true;
}

void ShredderPlugin::shutdown()
{
    closePanel();
}

bool ShredderPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral("就绪：按 %1 打开粉碎面板（不可撤销，请谨慎）")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void ShredderPlugin::onDisable()
{
    closePanel();
    Q_EMIT statusMessage(QStringLiteral("已停用敏感文件粉碎"));
}

void ShredderPlugin::onHotkey(const QString &hotkeyId)
{
    Q_UNUSED(hotkeyId)
    if (!isEnabled()) {
        return;
    }
    openPanel();
}

void ShredderPlugin::openPanel()
{
    if (m_window == nullptr) {
        m_window = new ShredderWindow(this);
    }
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
}

void ShredderPlugin::closePanel()
{
    if (m_window != nullptr) {
        m_window->close();
        m_window = nullptr;
    }
}

QWidget *ShredderPlugin::panelWindow() const
{
    return m_window.data();
}

void ShredderPlugin::loadFromConfig()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    m_confirm = svc->configValue(id(), QStringLiteral("confirm"), true).toBool();
    m_renameBeforeDelete =
        svc->configValue(id(), QStringLiteral("renameBeforeDelete"), true).toBool();
    m_verifyEachPass = svc->configValue(id(), QStringLiteral("verifyEachPass"), true).toBool();
    m_modeKey = svc->configValue(id(), QStringLiteral("mode"),
                                 QStringLiteral("zero")).toString();
}

QWidget *ShredderPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *danger = new QLabel(disclosureText(), widget);
    danger->setWordWrap(true);
    danger->setStyleSheet(QStringLiteral("color:#ffb4a2; background:#2a1c18;"
                                        " border:1px solid #6b3b2f; border-radius:6px;"
                                        " padding:8px;"));
    layout->addWidget(danger);

    auto *form = new QFormLayout();
    auto *modeCombo = new QComboBox(widget);
    const Shredder::PassMode modes[] = { Shredder::PassMode::ZeroSinglePass,
                                        Shredder::PassMode::RandomSinglePass,
                                        Shredder::PassMode::RandomThreePass,
                                        Shredder::PassMode::DodThreePass };
    for (Shredder::PassMode mode : modes) {
        modeCombo->addItem(QStringLiteral("%1（%2 遍）")
                               .arg(Shredder::passModeDisplayName(mode))
                               .arg(Shredder::passCount(mode)),
                           Shredder::passModeKey(mode));
    }
    modeCombo->setCurrentIndex(modeCombo->findData(m_modeKey));
    form->addRow(QStringLiteral("默认覆写方式"), modeCombo);

    auto *renameCheck = new QCheckBox(QStringLiteral("覆写后先改名再删除"), widget);
    renameCheck->setChecked(m_renameBeforeDelete);
    form->addRow(QString(), renameCheck);

    auto *verifyCheck = new QCheckBox(QStringLiteral("每遍覆写后回读校验"), widget);
    verifyCheck->setChecked(m_verifyEachPass);
    form->addRow(QString(), verifyCheck);

    auto *confirmCheck = new QCheckBox(QStringLiteral("执行前弹二次确认"), widget);
    confirmCheck->setChecked(m_confirm);
    form->addRow(QString(), confirmCheck);
    layout->addLayout(form);

    auto *openButton = new QPushButton(QStringLiteral("打开粉碎面板"), widget);
    layout->addWidget(openButton);

    auto *hint = new QLabel(
        QStringLiteral("快捷键 %1 打开面板。\n"
                       "回读校验会多读一遍磁盘：慢一倍，但能证明「确实写进去了」"
                       "（自检正是靠它做断言）。"),
        widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    QObject::connect(modeCombo, &QComboBox::currentIndexChanged, widget, [this, modeCombo](int index) {
        m_modeKey = modeCombo->itemData(index).toString();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("mode"), m_modeKey);
        }
    });
    QObject::connect(renameCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_renameBeforeDelete = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("renameBeforeDelete"), checked);
        }
    });
    QObject::connect(verifyCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_verifyEachPass = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("verifyEachPass"), checked);
        }
    });
    QObject::connect(confirmCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_confirm = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("confirm"), checked);
        }
    });
    QObject::connect(openButton, &QPushButton::clicked, widget, [this] { openPanel(); });

    return widget;
}
