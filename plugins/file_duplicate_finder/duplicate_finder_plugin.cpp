#include "duplicate_finder_plugin.h"

#include "DuplicateFinder.h"
#include "sdk/PluginServices.h"
#include "win32/ShellUtils.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <memory>
#include <thread>

namespace DuplicateFinder = WinEase::FeaturePlugins::DuplicateFinder;
using WinEase::FeaturePlugins::Cancel;

// ============================================================================
//  扫描结果面板
//
//  ⚠ 为什么面板要自己扛扫描线程：扫描是"点一下按钮、盯着进度"的用例，
//    把线程放在插件里再往面板推信号，多一层中转没有收益。
//    插件只提供配置（确认开关、抽样字节数）。
// ============================================================================

class DuplicateFinderWindow : public QWidget
{
public:
    explicit DuplicateFinderWindow(DuplicateFinderPlugin *plugin)
        : QWidget(nullptr)
        , m_plugin(plugin)
    {
        setObjectName(QStringLiteral("winease_duplicate_finder"));
        setWindowTitle(QStringLiteral("重复文件查找 - WinEase"));
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_DeleteOnClose, true);
        resize(1040, 720);
        buildUi();
        applyDarkStyle();
        const QStringList roots = m_plugin->defaultRoots();
        if (!roots.isEmpty()) {
            m_rootsEdit->setPlainText(roots.join(QLatin1Char('\n'))); // 上次用过的目录
        }
        setStatus(QStringLiteral("选好目录后点「开始扫描」。默认只比对 4 KB 以上的文件，"
                                 "先按大小分组、再抽样比对、最后才算全量哈希。"));
    }

    ~DuplicateFinderWindow() override
    {
        // ⚠ 线程必须在窗口析构前收干净：取消是协作式的（引擎在自己的检查点退出）
        stopWorker();
    }

    /// 自检用：扫描是否还在进行
    bool scanning() const { return m_scanning; }
    /// 自检用：当前结果行数
    int resultRowCount() const { return m_table->rowCount(); }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        // 无边框窗口：按住顶部标题条拖动
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
        header->setObjectName(QStringLiteral("dupHeader"));
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(12, 8, 8, 8);
        auto *title = new QLabel(QStringLiteral("重复文件查找"), header);
        title->setObjectName(QStringLiteral("dupTitle"));
        auto *closeButton = new QPushButton(QStringLiteral("✕"), header);
        closeButton->setObjectName(QStringLiteral("dupClose"));
        closeButton->setFixedSize(26, 26);
        headerLayout->addWidget(title, 1);
        headerLayout->addWidget(closeButton);
        layout->addWidget(header);

        auto *body = new QWidget(this);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(12, 8, 12, 10);

        auto *rootsBox = new QGroupBox(QStringLiteral("要扫描的目录（每行一个）"), body);
        auto *rootsLayout = new QVBoxLayout(rootsBox);
        m_rootsEdit = new QPlainTextEdit(rootsBox);
        m_rootsEdit->setObjectName(QStringLiteral("dupFinderRoots"));
        m_rootsEdit->setPlaceholderText(QStringLiteral("D:\\下载\nD:\\照片"));
        m_rootsEdit->setFixedHeight(72);
        rootsLayout->addWidget(m_rootsEdit);
        auto *rootsButtons = new QHBoxLayout();
        auto *addButton = new QPushButton(QStringLiteral("添加目录…"), rootsBox);
        auto *addButton2 = new QPushButton(QStringLiteral("添加子目录（含隐藏）…"), rootsBox);
        auto *clearButton = new QPushButton(QStringLiteral("清空"), rootsBox);
        rootsButtons->addWidget(addButton);
        rootsButtons->addWidget(addButton2);
        rootsButtons->addWidget(clearButton);
        rootsButtons->addStretch(1);
        rootsLayout->addLayout(rootsButtons);
        bodyLayout->addWidget(rootsBox);

        auto *optionsRow = new QHBoxLayout();
        m_minSizeSpin = new QSpinBox(body);
        m_minSizeSpin->setObjectName(QStringLiteral("dupFinderMinSize"));
        m_minSizeSpin->setRange(0, 1024 * 1024);
        m_minSizeSpin->setSuffix(QStringLiteral(" KB"));
        m_minSizeSpin->setValue(4);
        m_minSizeSpin->setToolTip(QStringLiteral("小于这个大小的文件不参与比对（0 字节文件默认被排除）"));
        m_sampleSpin = new QSpinBox(body);
        m_sampleSpin->setObjectName(QStringLiteral("dupFinderSample"));
        m_sampleSpin->setRange(1, 1024);
        m_sampleSpin->setSuffix(QStringLiteral(" KB"));
        // 初值来自配置（不是写死 4）：面板和引擎必须是同一份数，否则"改了没反应"
        m_sampleSpin->setValue(qBound(1, m_plugin->sampleBytes() / 1024, 1024));
        m_sampleSpin->setToolTip(QStringLiteral("抽样阶段只读每个文件的头部这么多字节"));
        m_hiddenCheck = new QCheckBox(QStringLiteral("包含隐藏文件"), body);
        m_hiddenCheck->setObjectName(QStringLiteral("dupFinderHidden"));
        optionsRow->addWidget(new QLabel(QStringLiteral("最小文件大小"), body));
        optionsRow->addWidget(m_minSizeSpin);
        optionsRow->addSpacing(12);
        optionsRow->addWidget(new QLabel(QStringLiteral("抽样"), body));
        optionsRow->addWidget(m_sampleSpin);
        optionsRow->addSpacing(12);
        optionsRow->addWidget(m_hiddenCheck);
        optionsRow->addStretch(1);
        bodyLayout->addLayout(optionsRow);

        auto *actionRow = new QHBoxLayout();
        m_startButton = new QPushButton(QStringLiteral("开始扫描"), body);
        m_startButton->setObjectName(QStringLiteral("dupFinderStart"));
        m_cancelButton = new QPushButton(QStringLiteral("取消"), body);
        m_cancelButton->setObjectName(QStringLiteral("dupFinderCancel"));
        m_cancelButton->setEnabled(false);
        m_recycleButton = new QPushButton(QStringLiteral("把建议删除的项移到回收站"), body);
        m_recycleButton->setObjectName(QStringLiteral("dupFinderRecycle"));
        m_recycleButton->setEnabled(false);
        actionRow->addWidget(m_startButton);
        actionRow->addWidget(m_cancelButton);
        actionRow->addStretch(1);
        actionRow->addWidget(m_recycleButton);
        bodyLayout->addLayout(actionRow);

        m_progress = new QProgressBar(body);
        m_progress->setObjectName(QStringLiteral("dupFinderProgress"));
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_progress->setTextVisible(true);
        bodyLayout->addWidget(m_progress);

        m_stageLabel = new QLabel(body);
        m_stageLabel->setObjectName(QStringLiteral("dupFinderStage"));
        m_stageLabel->setWordWrap(true);
        bodyLayout->addWidget(m_stageLabel);

        m_table = new QTableWidget(body);
        m_table->setObjectName(QStringLiteral("dupFinderTable"));
        m_table->setColumnCount(5);
        m_table->setHorizontalHeaderLabels({ QStringLiteral("处置"), QStringLiteral("大小"),
                                             QStringLiteral("路径"), QStringLiteral("哈希前 12 位"),
                                             QStringLiteral("组") });
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setAlternatingRowColors(true);
        bodyLayout->addWidget(m_table, 1);

        m_statusLabel = new QLabel(body);
        m_statusLabel->setObjectName(QStringLiteral("dupFinderStatus"));
        m_statusLabel->setWordWrap(true);
        bodyLayout->addWidget(m_statusLabel);

        auto *hint = new QLabel(
            QStringLiteral("每组只保留字典序最小的一份，其余为「建议删除」。"
                           "「移到回收站」不会永久删除任何东西 —— 后悔了去回收站还原即可。"
                           "双击一行可以在资源管理器里定位它。"),
            body);
        hint->setWordWrap(true);
        bodyLayout->addWidget(hint);

        layout->addWidget(body, 1);

        QObject::connect(closeButton, &QPushButton::clicked, this, [this] { close(); });
        QObject::connect(addButton, &QPushButton::clicked, this, [this] { appendDirectory(); });
        QObject::connect(addButton2, &QPushButton::clicked, this, [this] { appendDirectory(); });
        QObject::connect(clearButton, &QPushButton::clicked, this, [this] {
            m_rootsEdit->clear();
        });
        QObject::connect(m_startButton, &QPushButton::clicked, this, [this] { startScan(); });
        QObject::connect(m_cancelButton, &QPushButton::clicked, this, [this] { cancelScan(); });
        QObject::connect(m_recycleButton, &QPushButton::clicked, this, [this] { recycleSuggested(); });
        QObject::connect(m_table, &QTableWidget::itemDoubleClicked, this, [](QTableWidgetItem *item) {
            if (item == nullptr) {
                return;
            }
            const QTableWidgetItem *pathItem = item->tableWidget()->item(item->row(), 2);
            if (pathItem != nullptr) {
                WinEase::Win32::revealInExplorer(pathItem->text());
            }
        });
    }

    void applyDarkStyle()
    {
        setStyleSheet(QStringLiteral(
            "QWidget#winease_duplicate_finder { background:#1b1b1b; color:#e6e6e6; }"
            "QWidget#dupHeader { background:#262626; }"
            "QLabel#dupTitle { font-weight:600; }"
            "QLabel { color:#e6e6e6; }"
            "QGroupBox { border:1px solid #3a3a3a; border-radius:6px; margin-top:10px;"
            "  padding-top:8px; }"
            "QGroupBox::title { subcontrol-origin:margin; left:10px; color:#bdbdbd; }"
            "QPlainTextEdit, QTableWidget, QSpinBox { background:#151515; color:#e0e0e0;"
            "  border:1px solid #3a3a3a; }"
            "QHeaderView::section { background:#262626; color:#d0d0d0; border:0; padding:4px; }"
            "QPushButton { background:#333; color:#e6e6e6; border:0; border-radius:4px; padding:6px 12px; }"
            "QPushButton:hover { background:#444; }"
            "QPushButton:disabled { background:#2a2a2a; color:#777; }"
            "QProgressBar { border:1px solid #3a3a3a; border-radius:4px; background:#151515;"
            "  text-align:center; color:#e0e0e0; }"
            "QProgressBar::chunk { background:#4a7ebb; }"));
    }

    // ------------------------------------------------------------------
    //  参数
    // ------------------------------------------------------------------

    DuplicateFinder::ScanOptions currentOptions() const
    {
        DuplicateFinder::ScanOptions options;
        const QStringList lines = m_rootsEdit->toPlainText().split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            options.roots.append(QDir::toNativeSeparators(QDir::cleanPath(trimmed)));
        }
        options.minSizeBytes = static_cast<qint64>(m_minSizeSpin->value()) * 1024;
        options.includeHidden = m_hiddenCheck->isChecked();
        return options;
    }

    void appendDirectory()
    {
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择要扫描的目录"), QDir::homePath());
        if (chosen.isEmpty()) {
            return;
        }
        QString text = m_rootsEdit->toPlainText().trimmed();
        if (!text.isEmpty()) {
            text += QLatin1Char('\n');
        }
        m_rootsEdit->setPlainText(text + QDir::toNativeSeparators(chosen));
    }

    void setStatus(const QString &text)
    {
        m_statusLabel->setText(text);
    }

    // ------------------------------------------------------------------
    //  扫描
    // ------------------------------------------------------------------

    void startScan()
    {
        if (m_scanning) {
            return;
        }
        const DuplicateFinder::ScanOptions options = currentOptions();
        if (options.roots.isEmpty()) {
            setStatus(QStringLiteral("先指定要扫描的目录（每行一个）。"));
            return;
        }
        for (const QString &root : options.roots) {
            if (!QFileInfo(root).isDir()) {
                setStatus(QStringLiteral("目录不存在：%1").arg(root));
                return;
            }
        }

        m_table->setRowCount(0);
        m_groups.clear();
        m_stats = DuplicateFinder::Stats();
        m_scanning = true;
        m_cancel = std::make_shared<Cancel>();
        m_startButton->setEnabled(false);
        m_cancelButton->setEnabled(true);
        m_recycleButton->setEnabled(false);
        m_progress->setRange(0, 0); // 总数未知 → 忙碌态
        m_stageLabel->setText(QStringLiteral("正在枚举文件…"));
        setStatus(QStringLiteral("扫描中…"));

        // ★ 扫描必须离开界面线程：几万个文件的枚举 + 采样哈希会把界面彻底卡死
        // 抽样大小取面板里的值并写回插件（含配置）：控件上的数字必须真的起作用
        m_plugin->setSampleBytes(m_sampleSpin->value() * 1024);
        const int sampleBytes = m_plugin->sampleBytes();
        auto cancel = m_cancel;
        m_worker = std::thread([this, options, cancel, sampleBytes] {
            DuplicateFinder::Stats stats;
            QString error;
            const WinEase::FeaturePlugins::Progress progress =
                [this](const QString &stage, int done, int total) {
                    // ⚠ 进度回调可能在引擎内部的工作线程上被调用 → 排队回界面线程
                    QMetaObject::invokeMethod(
                        this,
                        [this, stage, done, total] { updateProgress(stage, done, total); },
                        Qt::QueuedConnection);
                };
            const QVector<DuplicateFinder::Group> groups = DuplicateFinder::findDuplicates(
                options, sampleBytes, cancel.get(), progress, &stats, &error);
            QMetaObject::invokeMethod(
                this,
                [this, groups, stats, error] { finishScan(groups, stats, error); },
                Qt::QueuedConnection);
        });
    }

    void cancelScan()
    {
        if (m_cancel != nullptr) {
            m_cancel->request();
            m_cancelButton->setEnabled(false);
            setStatus(QStringLiteral("正在取消…（引擎会在检查点退出）"));
        }
    }

    void updateProgress(const QString &stage, int done, int total)
    {
        m_stageLabel->setText(total > 0 ? QStringLiteral("%1：%2 / %3").arg(stage).arg(done).arg(total)
                                        : QStringLiteral("%1：%2").arg(stage).arg(done));
        if (total > 0) {
            m_progress->setRange(0, total);
            m_progress->setValue(qBound(0, done, total));
        }
    }

    void finishScan(const QVector<DuplicateFinder::Group> &groups,
                    const DuplicateFinder::Stats &stats,
                    const QString &error)
    {
        if (m_worker.joinable()) {
            m_worker.join(); // 线程已经把活干完、只是排队回来报信，这里收尸
        }
        m_scanning = false;
        m_startButton->setEnabled(true);
        m_cancelButton->setEnabled(false);
        m_progress->setRange(0, 100);
        m_progress->setValue(stats.canceled ? 0 : 100);
        m_groups = groups;
        m_stats = stats;

        if (!error.isEmpty()) {
            setStatus(QStringLiteral("扫描失败：%1").arg(error));
            m_stageLabel->clear();
            return;
        }

        fillTable(groups);
        const int suggested = DuplicateFinder::suggestDeletions(groups).size();
        m_recycleButton->setEnabled(!groups.isEmpty());
        m_recycleButton->setText(QStringLiteral("把建议删除的 %1 项移到回收站").arg(suggested));
        m_stageLabel->setText(stats.stageText());
        if (stats.canceled) {
            setStatus(QStringLiteral("已取消。%1").arg(stats.stageText()));
        } else {
            setStatus(QStringLiteral("%1　→　建议删除 %2 项，可回收 %3")
                          .arg(stats.stageText())
                          .arg(suggested)
                          .arg(DuplicateFinder::formatBytes(stats.duplicateBytes)));
        }
    }

    void stopWorker()
    {
        if (m_cancel != nullptr) {
            m_cancel->request();
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_scanning = false;
    }

    void fillTable(const QVector<DuplicateFinder::Group> &groups)
    {
        const QVector<DuplicateFinder::Row> rows = DuplicateFinder::flatten(groups);
        m_table->setRowCount(rows.size());
        int groupIndex = 0;
        QString lastHash;
        for (int i = 0; i < rows.size(); ++i) {
            const DuplicateFinder::Row &row = rows.at(i);
            if (row.hash != lastHash) {
                ++groupIndex;
                lastHash = row.hash;
            }
            auto *keepItem = new QTableWidgetItem(row.keep ? QStringLiteral("保留")
                                                           : QStringLiteral("建议删除"));
            keepItem->setForeground(row.keep ? QBrush(QColor(QStringLiteral("#8bd45f")))
                                             : QBrush(QColor(QStringLiteral("#ff8a65"))));
            m_table->setItem(i, 0, keepItem);
            m_table->setItem(i, 1, new QTableWidgetItem(DuplicateFinder::formatBytes(row.size)));
            m_table->setItem(i, 2, new QTableWidgetItem(row.path));
            m_table->setItem(i, 3, new QTableWidgetItem(row.hash.left(12)));
            m_table->setItem(i, 4, new QTableWidgetItem(QString::number(groupIndex)));
        }
    }

    // ------------------------------------------------------------------
    //  移到回收站（**绝不硬删**）
    // ------------------------------------------------------------------

    void recycleSuggested()
    {
        const QStringList victims = DuplicateFinder::suggestDeletions(m_groups);
        if (victims.isEmpty()) {
            setStatus(QStringLiteral("没有被建议删除的文件。"));
            m_recycleButton->setEnabled(false);
            return;
        }

        // 二次确认：默认开（这是"真的会动用户文件"的按钮）。
        // 自检把 [Plugins/file.duplicate_finder] confirm 设为 false。
        if (m_plugin->confirmBeforeRecycle()) {
            const QMessageBox::StandardButton answer = QMessageBox::question(
                this, QStringLiteral("移到回收站"),
                QStringLiteral("将把 %1 个重复文件移到回收站（可以还原）。\n"
                               "每组保留字典序最小的那一个。\n\n确定继续？")
                    .arg(victims.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }

        QString error;
        if (!WinEase::Win32::moveToRecycleBin(victims, &error)) {
            setStatus(QStringLiteral("移到回收站失败：%1（已经移走的那些不会重复操作，"
                                     "重新扫描一次可以刷新列表）")
                          .arg(error));
            return;
        }

        markRecycled(victims);
        m_groups.clear(); // 防止再点一次去处理已经不存在的文件
        m_recycleButton->setEnabled(false);
        setStatus(QStringLiteral("已把 %1 个重复文件移到回收站（可还原），之前建议回收 %2。")
                      .arg(victims.size())
                      .arg(DuplicateFinder::formatBytes(m_stats.duplicateBytes)));
    }

    void markRecycled(const QStringList &victims)
    {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            QTableWidgetItem *pathItem = m_table->item(row, 2);
            if (pathItem == nullptr || !victims.contains(pathItem->text())) {
                continue;
            }
            if (QTableWidgetItem *stateItem = m_table->item(row, 0)) {
                stateItem->setText(QStringLiteral("已移入回收站"));
            }
            pathItem->setForeground(QBrush(QColor(QStringLiteral("#9e9e9e"))));
        }
    }

    DuplicateFinderPlugin *m_plugin = nullptr;
    QPlainTextEdit *m_rootsEdit = nullptr;
    QSpinBox *m_minSizeSpin = nullptr;
    QSpinBox *m_sampleSpin = nullptr;
    QCheckBox *m_hiddenCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_recycleButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_stageLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTableWidget *m_table = nullptr;

    std::shared_ptr<Cancel> m_cancel;
    std::thread m_worker;
    bool m_scanning = false;
    QVector<DuplicateFinder::Group> m_groups;
    DuplicateFinder::Stats m_stats;
};

// ============================================================================
//  插件本体
// ============================================================================

DuplicateFinderPlugin::DuplicateFinderPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

QString DuplicateFinderPlugin::id() const
{
    return QStringLiteral("file.duplicate_finder");
}

QString DuplicateFinderPlugin::name() const
{
    return QStringLiteral("重复文件查找");
}

QString DuplicateFinderPlugin::description() const
{
    return QStringLiteral("四步筛出内容相同的文件：按大小分组、抽样比对、全量哈希，只移回收站");
}

QIcon DuplicateFinderPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory DuplicateFinderPlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList DuplicateFinderPlugin::tags() const
{
    return { QStringLiteral("重复"), QStringLiteral("去重"), QStringLiteral("查重"),
             QStringLiteral("duplicate"), QStringLiteral("哈希"), QStringLiteral("空间") };
}

bool DuplicateFinderPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence DuplicateFinderPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+D"));
}

bool DuplicateFinderPlugin::initialize()
{
    loadFromConfig();
    if (WinEase::PluginServices *svc = services()) {
        svc->log(id(), WinEase::PluginLogLevel::Info,
                 QStringLiteral("重复文件查找已加载（抽样 %1 KB，默认目录 %2 个）")
                     .arg(m_sampleBytes / 1024)
                     .arg(m_defaultRoots.size()));
    }
    return true;
}

void DuplicateFinderPlugin::shutdown()
{
    closePanel();
}

bool DuplicateFinderPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral("就绪：按 %1 打开查重面板（删除一律走回收站）")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void DuplicateFinderPlugin::onDisable()
{
    closePanel();
    Q_EMIT statusMessage(QStringLiteral("已停用重复文件查找"));
}

void DuplicateFinderPlugin::onHotkey(const QString &hotkeyId)
{
    Q_UNUSED(hotkeyId) // 只有一个动作
    if (!isEnabled()) {
        return;
    }
    openPanel();
}

void DuplicateFinderPlugin::openPanel()
{
    if (m_window == nullptr) {
        m_window = new DuplicateFinderWindow(this);
    }
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
}

void DuplicateFinderPlugin::closePanel()
{
    if (m_window != nullptr) {
        m_window->close(); // WA_DeleteOnClose → 自己析构（析构里会收线程）
        m_window = nullptr;
    }
}

QWidget *DuplicateFinderPlugin::panelWindow() const
{
    return m_window.data();
}

void DuplicateFinderPlugin::setSampleBytes(int bytes)
{
    const int clamped = qBound(1, bytes / 1024, 1024) * 1024;
    if (clamped == m_sampleBytes) {
        return;
    }
    m_sampleBytes = clamped;
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("sampleKb"), clamped / 1024);
    }
}

void DuplicateFinderPlugin::loadFromConfig()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    m_confirm = svc->configValue(id(), QStringLiteral("confirm"), true).toBool();
    const int sampleKb = svc->configValue(id(), QStringLiteral("sampleKb"), 4).toInt();
    m_sampleBytes = qBound(1, sampleKb, 1024) * 1024;

    m_defaultRoots.clear();
    const QString raw = svc->configValue(id(), QStringLiteral("roots")).toString();
    if (!raw.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
        if (document.isArray()) {
            for (const QJsonValue &value : document.array()) {
                const QString path = value.toString();
                if (!path.isEmpty()) {
                    m_defaultRoots.append(path);
                }
            }
        }
    }
}

QWidget *DuplicateFinderPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *box = new QGroupBox(QStringLiteral("默认扫描目录（每行一个）"), widget);
    auto *boxLayout = new QVBoxLayout(box);
    auto *rootsEdit = new QPlainTextEdit(m_defaultRoots.join(QLatin1Char('\n')), box);
    rootsEdit->setFixedHeight(76);
    boxLayout->addWidget(rootsEdit);
    layout->addWidget(box);

    auto *form = new QFormLayout();
    auto *sampleSpin = new QSpinBox(widget);
    sampleSpin->setRange(1, 1024);
    sampleSpin->setSuffix(QStringLiteral(" KB"));
    sampleSpin->setValue(m_sampleBytes / 1024);
    sampleSpin->setToolTip(QStringLiteral("抽样阶段只读文件头部这么多字节（越小越快）"));
    form->addRow(QStringLiteral("抽样大小"), sampleSpin);
    auto *confirmCheck = new QCheckBox(QStringLiteral("移到回收站前弹确认"), widget);
    confirmCheck->setChecked(m_confirm);
    form->addRow(QString(), confirmCheck);
    layout->addLayout(form);

    auto *openButton = new QPushButton(QStringLiteral("打开查重面板"), widget);
    layout->addWidget(openButton);

    auto *hint = new QLabel(
        QStringLiteral("快捷键 %1 打开面板。\n"
                       "四级流水线：① 递归枚举（只取路径和大小）→ ② 按大小分组（大小不同的必然不是重复）"
                       "→ ③ 只读文件头部 %2 KB 抽样比对 → ④ 对剩下的少数文件算全量 SHA-256。\n"
                       "删除一律走「回收站」，本功能没有任何「永久删除」的入口。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText),
                 QString::number(m_sampleBytes / 1024)),
        widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    QObject::connect(rootsEdit, &QPlainTextEdit::textChanged, widget, [this, rootsEdit] {
        m_defaultRoots.clear();
        const QStringList lines = rootsEdit->toPlainText().split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                m_defaultRoots.append(QDir::toNativeSeparators(QDir::cleanPath(trimmed)));
            }
        }
        QJsonArray array;
        for (const QString &root : m_defaultRoots) {
            array.append(root);
        }
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(
                id(), QStringLiteral("roots"),
                QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
        }
    });
    QObject::connect(sampleSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_sampleBytes = value * 1024;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("sampleKb"), value);
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
