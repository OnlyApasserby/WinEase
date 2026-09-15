#include "convert_plugin.h"

#include "ClipboardTools.h"
#include "sdk/PluginServices.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

using WinEase::FeaturePlugins::Cancel;
// ⚠ 这三套引擎是**命名空间**（不是类）→ 只能用命名空间别名，`using X::Y;` 会报 C2873
namespace ClipboardTools = WinEase::FeaturePlugins::ClipboardTools;
namespace ImageConvertEngine = WinEase::FeaturePlugins::ImageConvertEngine;
namespace TextEncodingTools = WinEase::FeaturePlugins::TextEncodingTools;

namespace {

/// 把用户输入/剪贴板里的一行整理成绝对路径（去掉引号，容忍 file:// 前缀）
QString normalizePath(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"'))) {
        text = text.mid(1, text.size() - 2);
    }
    if (text.startsWith(QStringLiteral("file:///"))) {
        text = text.mid(8);
    }
    if (text.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(QFileInfo(text).absoluteFilePath());
}

} // namespace

// ============================================================================
//  元信息
// ============================================================================

FileConvertPlugin::FileConvertPlugin(QObject *parent) : WinEase::IFeaturePlugin(parent) {}

FileConvertPlugin::~FileConvertPlugin()
{
    stopWorker();
}

QString FileConvertPlugin::id() const
{
    return QStringLiteral("file.convert");
}

QString FileConvertPlugin::name() const
{
    return QStringLiteral("批量格式转换");
}

QString FileConvertPlugin::description() const
{
    return QStringLiteral("一批图片换成另一种格式（可缩放、加水印），一批文本换字符集（GBK↔UTF-8），"
                          "带进度且随时能取消");
}

QString FileConvertPlugin::detailedDescription() const
{
    return QStringLiteral(
               "面板拖进一批文件（或文件夹）→ 选目标格式 → 点「开始转换」。\n"
               "图片：PNG / JPEG / WebP / TIFF / BMP / ICO（能否输出取决于本机 Qt 的编码插件，"
               "缺插件的格式在列表里会注明）。文本：UTF-8（可带 BOM）/ UTF-16 大小端 / GBK / "
               "本机 ANSI，并可按需要改写换行风格。\n"
               "\n"
               "【不会原地覆盖源文件】\n"
               "目标文件与源文件重合（同目录、同名、同扩展名）时，该项会直接标成「有问题」"
               "并跳过 —— 转换的产物永远是另一个文件。想压小原图请自己删掉原件。\n"
               "\n"
               "【有损转换会提前告诉你】\n"
               "转成 JPEG / 有损 WebP 会丢掉透明像素（铺白底）与细节；转成 GBK 时 emoji、"
               "生僻字、日文假名没有对应字符，这种文件会**失败并说清原因**，"
               "不会被悄悄替换成问号。\n"
               "\n"
               "【取消的语义】\n"
               "「取消」在每张图**开工前**生效：已经开始的那一张一定会写完，"
               "所以磁盘上不会留下半张图；取消后已经完成的部分保持不变。\n"
               "\n"
               "【元数据边界】\n"
               "勾选「保留元数据」保留的是：图像自带的文本属性（注释 / 作者 / 软件等）、"
               "ICC 色彩配置，以及源文件的修改时间。**EXIF 拍摄信息（相机型号、拍摄参数、GPS）"
               "在 Qt 把 JPEG 解成像素时就已经丢失了** —— 本功能不假装能保住它，"
               "需要完整保留 EXIF 请用专门的看图/修图软件。\n"
               "\n"
               "【有意没做的两件事】\n"
               "一是资源管理器右键菜单集成（主程序目前没有「带参数打开某个插件面板」的入口）；"
               "二是不占用全局快捷键 —— 批量任务的对象是「一批文件」，快捷键按下的时候"
               "无从得知用户想转哪一批。");
}

QIcon FileConvertPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory FileConvertPlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList FileConvertPlugin::tags() const
{
    return { QStringLiteral("转换"), QStringLiteral("格式"), QStringLiteral("批量"),
             QStringLiteral("图片"), QStringLiteral("编码"), QStringLiteral("GBK"),
             QStringLiteral("UTF-8"), QStringLiteral("convert") };
}

bool FileConvertPlugin::supportsHotkey() const
{
    // 见 convert_plugin.h 的裁剪说明：批量任务没有合理的快捷键语义
    return false;
}

// ============================================================================
//  生命周期
// ============================================================================

bool FileConvertPlugin::initialize()
{
    loadSettings();
    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("批量格式转换已就绪（图片走 QImageWriter，文本走 Win32 代码页 API）"));
    return true;
}

void FileConvertPlugin::shutdown()
{
    // 先取消 + 收尸：工作线程还在跑就退出进程会踩到"线程持有已析构对象"
    stopWorker();
    saveSettings();
    m_lastSummary.clear();
}

void FileConvertPlugin::onDisable()
{
    // ★ 停用必须把工作线程收干净（踩坑：回调里读到的自身状态是旧值 ⇒ 这里不读 isEnabled()）
    stopWorker();
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (m_panel != nullptr) {
                m_panel->setEnabled(true);
            }
            setStatus(QStringLiteral("已停用"));
        },
        Qt::QueuedConnection);
}

bool FileConvertPlugin::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_panel && m_panel != nullptr) {
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            QStringList paths;
            const QList<QUrl> urls = drop->mimeData()->urls();
            for (const QUrl &url : urls) {
                if (url.isLocalFile()) {
                    paths << url.toLocalFile();
                }
            }
            if (!paths.isEmpty()) {
                drop->acceptProposedAction();
                addPaths(paths);
                return true;
            }
        }
    }
    return WinEase::IFeaturePlugin::eventFilter(watched, event);
}

// ============================================================================
//  清单
// ============================================================================

void FileConvertPlugin::addPaths(const QStringList &paths)
{
    const bool recursive = m_recursiveCheck != nullptr && m_recursiveCheck->isChecked();

    int added = 0;
    int folders = 0;
    QStringList missing;

    for (const QString &raw : paths) {
        const QString path = normalizePath(raw);
        if (path.isEmpty()) {
            continue;
        }

        const QFileInfo info(path);
        if (!info.exists()) {
            missing << path;
            continue;
        }

        // 文件夹按"图片 + 文本"一起展开：清单里只放文件，题目由"模式"在计划阶段判
        QStringList candidates;
        if (info.isDir()) {
            ++folders;
            candidates = ImageConvertEngine::expandInputs({ path }, recursive);
            QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot,
                            recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
            while (it.hasNext()) {
                const QString file = it.next();
                if (TextEncodingTools::isTextFile(file) && !candidates.contains(file)) {
                    candidates << file;
                }
            }
        } else {
            candidates << path;
        }

        for (const QString &candidate : candidates) {
            const QString clean = QDir::cleanPath(candidate);
            bool exists = false;
            for (const QString &known : m_paths) {
                if (QString::compare(known, clean, Qt::CaseInsensitive) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                m_paths << clean;
                ++added;
            }
        }
    }

    refreshFileList();
    refreshPlan();

    QStringList parts;
    parts << QStringLiteral("加入 %1 个文件").arg(added);
    if (folders > 0) {
        parts << QStringLiteral("（来自 %1 个文件夹）").arg(folders);
    }
    if (!missing.isEmpty()) {
        // 前置不成立要如实报（踩坑 #11/#12）
        parts << QStringLiteral("%1 条路径不存在，已忽略").arg(missing.size());
    }
    setStatus(parts.join(QString()));
}

void FileConvertPlugin::addFilesViaDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        m_panel, QStringLiteral("选择要转换的文件"),
        m_paths.isEmpty() ? QDir::homePath() : QFileInfo(m_paths.first()).absolutePath(),
        QStringLiteral("所有文件 (*);;图片 (*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff *.ico);;"
                       "文本 (*.txt *.csv *.json *.xml *.ini *.log *.md)"));
    if (!files.isEmpty()) {
        addPaths(files);
    }
}

void FileConvertPlugin::addFolderViaDialog()
{
    const QString dir = QFileDialog::getExistingDirectory(
        m_panel, QStringLiteral("选择要加入的文件夹"),
        m_paths.isEmpty() ? QDir::homePath() : QFileInfo(m_paths.first()).absolutePath());
    if (!dir.isEmpty()) {
        addPaths({ dir });
    }
}

void FileConvertPlugin::addPathsFromClipboard()
{
    const QString text = ClipboardTools::clipboardText();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("剪贴板里没有文本（先在资源管理器里复制文件或文件夹）"));
        return;
    }

    QStringList candidates;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString path = normalizePath(line);
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            candidates << path;
            break; // 只取第一条有效路径：多行文本里通常只有一行是路径
        }
    }

    if (candidates.isEmpty()) {
        setStatus(QStringLiteral("剪贴板里没有可用的文件路径"));
        return;
    }
    addPaths(candidates);
}

void FileConvertPlugin::clearPaths()
{
    m_paths.clear();
    refreshFileList();
    refreshPlan();
    setStatus(QStringLiteral("清单已清空"));
}

void FileConvertPlugin::removeSelectedPaths()
{
    if (m_fileList == nullptr) {
        return;
    }
    QStringList selected;
    const QList<QListWidgetItem *> items = m_fileList->selectedItems();
    for (QListWidgetItem *item : items) {
        selected << item->data(Qt::UserRole).toString();
    }
    if (selected.isEmpty()) {
        setStatus(QStringLiteral("先在上面选中要移除的文件"));
        return;
    }

    for (const QString &path : selected) {
        for (int i = m_paths.size() - 1; i >= 0; --i) {
            if (QString::compare(m_paths.at(i), path, Qt::CaseInsensitive) == 0) {
                m_paths.removeAt(i);
            }
        }
    }
    refreshFileList();
    refreshPlan();
    setStatus(QStringLiteral("移除了 %1 个文件").arg(selected.size()));
}

void FileConvertPlugin::refreshFileList()
{
    if (m_fileList == nullptr) {
        return;
    }

    // ⚠ 重填会改当前行 → 用 QSignalBlocker 包住（踩坑 #61：程序化改控件又被回调改回去）
    const QSignalBlocker blocker(m_fileList);
    m_fileList->clear();
    for (const QString &path : m_paths) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(path), m_fileList);
        item->setData(Qt::UserRole, path);
        item->setToolTip(QDir::toNativeSeparators(path));
    }
    if (m_startButton != nullptr) {
        m_startButton->setEnabled(!m_paths.isEmpty() && !m_running);
    }
}

// ============================================================================
//  计划
// ============================================================================

namespace {

/// 文本侧的计划项（名字不变，只在"换目录"这件事上有冲突）
struct TextPlanItem
{
    QString input;
    QString output;
    QString problem;
    bool skipped = false;
};

struct TextPlan
{
    QVector<TextPlanItem> items;
    QString error;
    QString outputDir;
    int convertible = 0;
    int problems = 0;
};

QString uniqueTextPath(const QString &desired, const QSet<QString> &taken)
{
    const QFileInfo info(desired);
    const QString dir = info.absolutePath();
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int counter = 1; counter < 10000; ++counter) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("%1-%2.%3")
                                                         .arg(stem)
                                                         .arg(counter)
                                                         .arg(suffix));
        if (!taken.contains(QDir::cleanPath(candidate).toLower())
            && !QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return desired;
}

TextPlan buildTextPlan(const QStringList &paths, const QString &outputDir,
                       WinEase::FeaturePlugins::ImageConvertEngine::ConflictPolicy conflictPolicy)
{
    TextPlan plan;

    // ★ 文本转换几乎总是"换一个目录"：原地改名会把源文件覆盖掉，所以这里直接要求指定目录
    if (outputDir.trimmed().isEmpty()) {
        plan.error = QStringLiteral("文本转换需要指定输出目录 —— 本功能不会原地覆盖源文件");
        return plan;
    }

    const QString dir = QDir::cleanPath(QFileInfo(outputDir.trimmed()).absoluteFilePath());
    if (!QDir().mkpath(dir)) {
        plan.error = QStringLiteral("输出目录建不出来：%1").arg(dir);
        return plan;
    }
    plan.outputDir = dir;

    QSet<QString> taken;
    for (const QString &path : paths) {
        TextPlanItem item;
        item.input = path;

        const QFileInfo info(path);
        if (!info.isFile()) {
            item.problem = QStringLiteral("不是文件");
            ++plan.problems;
            plan.items.append(item);
            continue;
        }
        if (!WinEase::FeaturePlugins::TextEncodingTools::isTextFile(path)) {
            item.problem = QStringLiteral("按扩展名看不是文本文件（.%1）—— 当前是「文本」模式")
                               .arg(info.suffix());
            ++plan.problems;
            plan.items.append(item);
            continue;
        }

        QString desired = QDir(dir).filePath(info.fileName());

        if (QDir::cleanPath(path).toLower() == QDir::cleanPath(desired).toLower()) {
            item.output = desired;
            item.problem = QStringLiteral("目标与源文件是同一个文件 —— 请把输出目录换到别处");
            ++plan.problems;
            plan.items.append(item);
            continue;
        }

        if (taken.contains(QDir::cleanPath(desired).toLower())) {
            desired = uniqueTextPath(desired, taken);
        }

        if (QFileInfo::exists(desired)) {
            switch (conflictPolicy) {
            case WinEase::FeaturePlugins::ImageConvertEngine::ConflictPolicy::Skip:
                item.problem = QStringLiteral("目标已存在（%1），按当前策略跳过")
                                   .arg(QFileInfo(desired).fileName());
                item.skipped = true;
                ++plan.problems;
                plan.items.append(item);
                continue;
            case WinEase::FeaturePlugins::ImageConvertEngine::ConflictPolicy::AutoRename:
                desired = uniqueTextPath(desired, taken);
                break;
            case WinEase::FeaturePlugins::ImageConvertEngine::ConflictPolicy::Overwrite:
            default:
                break;
            }
        }

        item.output = QDir::cleanPath(desired);
        taken.insert(QDir::cleanPath(desired).toLower());
        ++plan.convertible;
        plan.items.append(item);
    }

    return plan;
}

} // namespace

bool FileConvertPlugin::isTextMode() const
{
    return m_kindCombo != nullptr && m_kindCombo->currentIndex() == int(Kind::Text);
}

void FileConvertPlugin::updateOptionVisibility()
{
    const bool text = isTextMode();
    if (m_imageGroup != nullptr) {
        m_imageGroup->setVisible(!text);
    }
    if (m_textGroup != nullptr) {
        m_textGroup->setVisible(text);
    }
}

ImageConvertEngine::Options FileConvertPlugin::currentImageOptions() const
{
    ImageConvertEngine::Options options;

    if (m_formatCombo != nullptr) {
        ImageConvertEngine::Format format = options.format;
        if (ImageConvertEngine::formatFromKey(m_formatCombo->currentData().toString(), &format)) {
            options.format = format;
        }
    }
    if (m_qualitySpin != nullptr) {
        options.quality = m_qualitySpin->value();
    }
    if (m_resizeCombo != nullptr) {
        ImageConvertEngine::ResizeMode mode = options.resizeMode;
        if (ImageConvertEngine::resizeModeFromKey(m_resizeCombo->currentData().toString(), &mode)) {
            options.resizeMode = mode;
        }
    }
    if (m_resizeSpin != nullptr) {
        options.resizeValue = m_resizeSpin->value();
    }
    if (m_watermarkEdit != nullptr) {
        options.watermarkText = m_watermarkEdit->text();
    }
    if (m_watermarkPosCombo != nullptr) {
        ImageConvertEngine::WatermarkPosition position = options.watermarkPosition;
        if (ImageConvertEngine::watermarkPositionFromKey(m_watermarkPosCombo->currentData().toString(),
                                                        &position)) {
            options.watermarkPosition = position;
        }
    }
    if (m_watermarkOpacitySpin != nullptr) {
        options.watermarkOpacityPercent = m_watermarkOpacitySpin->value();
    }
    if (m_watermarkSizeSpin != nullptr) {
        options.watermarkSizePercent = m_watermarkSizeSpin->value();
    }
    if (m_keepMetadataCheck != nullptr) {
        options.keepMetadata = m_keepMetadataCheck->isChecked();
    }
    if (m_customDirRadio != nullptr && m_customDirRadio->isChecked() && m_outputDirEdit != nullptr) {
        options.outputDir = m_outputDirEdit->text().trimmed();
    }
    if (m_templateEdit != nullptr) {
        options.nameTemplate = m_templateEdit->text().trimmed();
    }
    if (m_numberStartSpin != nullptr) {
        options.numberStart = m_numberStartSpin->value();
    }
    if (m_conflictCombo != nullptr) {
        ImageConvertEngine::ConflictPolicy policy = options.conflictPolicy;
        if (ImageConvertEngine::conflictPolicyFromKey(m_conflictCombo->currentData().toString(),
                                                     &policy)) {
            options.conflictPolicy = policy;
        }
    }
    if (m_recursiveCheck != nullptr) {
        options.recursive = m_recursiveCheck->isChecked();
    }

    return options;
}

void FileConvertPlugin::currentTextOptions(TextEncodingTools::Encoding *encodingOut,
                                           TextEncodingTools::LineEnding *lineEndingOut) const
{
    if (encodingOut != nullptr) {
        TextEncodingTools::Encoding encoding = TextEncodingTools::Encoding::Utf8;
        if (m_textEncodingCombo != nullptr) {
            TextEncodingTools::encodingFromKey(m_textEncodingCombo->currentData().toString(),
                                               &encoding);
        }
        *encodingOut = encoding;
    }
    if (lineEndingOut != nullptr) {
        TextEncodingTools::LineEnding lineEnding = TextEncodingTools::LineEnding::Keep;
        if (m_lineEndingCombo != nullptr) {
            TextEncodingTools::lineEndingFromKey(m_lineEndingCombo->currentData().toString(),
                                                 &lineEnding);
        }
        *lineEndingOut = lineEnding;
    }
}

void FileConvertPlugin::refreshPlan()
{
    if (m_planLabel == nullptr) {
        return;
    }

    if (m_paths.isEmpty()) {
        m_planLabel->setText(QStringLiteral("还没有加入文件：把文件或文件夹拖到面板上，"
                                            "或用上面的按钮添加"));
        m_planLabel->setToolTip(QString());
        return;
    }

    if (isTextMode()) {
        TextEncodingTools::Encoding encoding = TextEncodingTools::Encoding::Utf8;
        TextEncodingTools::LineEnding lineEnding = TextEncodingTools::LineEnding::Keep;
        currentTextOptions(&encoding, &lineEnding);

        const QString dir = (m_customDirRadio != nullptr && m_customDirRadio->isChecked()
                             && m_outputDirEdit != nullptr)
                                ? m_outputDirEdit->text().trimmed()
                                : QString();
        const auto options = currentImageOptions();
        const TextPlan plan = buildTextPlan(m_paths, dir, options.conflictPolicy);

        if (!plan.error.isEmpty()) {
            m_planLabel->setText(QStringLiteral("计划不成立：%1").arg(plan.error));
            m_planLabel->setToolTip(QStringLiteral("「指定目录」选好后这里会显示将要写出的文件名"));
            return;
        }

        QStringList parts;
        parts << QStringLiteral("将转换 %1 个文本文件").arg(plan.convertible);
        if (plan.problems > 0) {
            parts << QStringLiteral("%1 个有问题（不会动它）").arg(plan.problems);
        }
        parts << QStringLiteral("源字符集自动探测，写出为 %1；换行：%2；输出到：%3")
                     .arg(TextEncodingTools::encodingDisplayName(encoding),
                          TextEncodingTools::lineEndingDisplayName(lineEnding),
                          plan.outputDir);
        m_planLabel->setText(parts.join(QStringLiteral("；")));

        QStringList detail;
        for (const TextPlanItem &item : plan.items) {
            if (item.problem.isEmpty()) {
                detail << QStringLiteral("%1 → %2")
                              .arg(QFileInfo(item.input).fileName(),
                                   QDir::toNativeSeparators(item.output));
            } else {
                detail << QStringLiteral("%1 ✗ %2")
                              .arg(QFileInfo(item.input).fileName(), item.problem);
            }
        }
        m_planLabel->setToolTip(detail.join(QStringLiteral("\n")));
        return;
    }

    const auto options = currentImageOptions();
    ImageConvertEngine::Plan plan = ImageConvertEngine::plan(m_paths, options);

    if (plan.ok()) {
        // 图片模式下混进非图片文件要如实标出来（而不是让它排到最后才失败）
        for (ImageConvertEngine::Item &item : plan.items) {
            if (item.problem.isEmpty()
                && !ImageConvertEngine::isSupportedImageFile(item.inputPath)) {
                item.problem = QStringLiteral("按扩展名看不是图片文件（.%1）—— 当前是「图片」模式")
                                   .arg(QFileInfo(item.inputPath).suffix());
            }
        }
    }

    m_planLabel->setText(plan.summaryText());

    QStringList detail = plan.notes;
    for (const ImageConvertEngine::Item &item : plan.items) {
        if (!item.problem.isEmpty()) {
            detail << QStringLiteral("%1 ✗ %2")
                          .arg(QFileInfo(item.inputPath).fileName(), item.problem);
        } else if (!item.warning.isEmpty()) {
            detail << QStringLiteral("%1 ⚠ %2")
                          .arg(QFileInfo(item.inputPath).fileName(), item.warning);
        }
    }
    m_planLabel->setToolTip(detail.join(QStringLiteral("\n")));
}

// ============================================================================
//  面板
// ============================================================================

QWidget *FileConvertPlugin::createSettingsWidget(QWidget *parent)
{
    auto *panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("convertPanel"));
    panel->setAcceptDrops(true); // 拖入文件 / 文件夹
    panel->installEventFilter(this);
    m_panel = panel;

    auto *root = new QVBoxLayout(panel);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *hint = new QLabel(
        QStringLiteral("把文件或文件夹拖进这个面板，或用右边的按钮添加。转换**不会覆盖源文件**："
                       "目标与源重合的项会被标出来并跳过。"),
        panel);
    hint->setObjectName(QStringLiteral("convertHintLabel"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    // ---- 转换类型 ----
    auto *kindRow = new QHBoxLayout();
    kindRow->addWidget(new QLabel(QStringLiteral("转换类型："), panel));
    m_kindCombo = new QComboBox(panel);
    m_kindCombo->setObjectName(QStringLiteral("convertKindCombo"));
    m_kindCombo->addItem(QStringLiteral("图片格式（PNG / JPEG / WebP / TIFF / BMP / ICO）"),
                         int(Kind::Image));
    m_kindCombo->addItem(QStringLiteral("文本字符集（UTF-8 / UTF-16 / GBK / 本机 ANSI）"),
                         int(Kind::Text));
    m_kindCombo->setToolTip(QStringLiteral("两套选项共用同一份文件清单与输出目录设置"));
    kindRow->addWidget(m_kindCombo, 1);
    root->addLayout(kindRow);

    // ---- 文件清单 ----
    auto *listRow = new QHBoxLayout();
    m_fileList = new QListWidget(panel);
    m_fileList->setObjectName(QStringLiteral("convertFileList"));
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileList->setMinimumHeight(120);
    listRow->addWidget(m_fileList, 1);

    auto *buttonColumn = new QVBoxLayout();
    auto *addFilesButton = new QPushButton(QStringLiteral("添加文件…"), panel);
    addFilesButton->setObjectName(QStringLiteral("convertAddFilesButton"));
    auto *addFolderButton = new QPushButton(QStringLiteral("添加文件夹…"), panel);
    addFolderButton->setObjectName(QStringLiteral("convertAddFolderButton"));
    auto *pasteButton = new QPushButton(QStringLiteral("从剪贴板取路径"), panel);
    pasteButton->setObjectName(QStringLiteral("convertPasteButton"));
    auto *removeButton = new QPushButton(QStringLiteral("移除选中"), panel);
    removeButton->setObjectName(QStringLiteral("convertRemoveButton"));
    auto *clearButton = new QPushButton(QStringLiteral("清空清单"), panel);
    clearButton->setObjectName(QStringLiteral("convertClearButton"));
    buttonColumn->addWidget(addFilesButton);
    buttonColumn->addWidget(addFolderButton);
    buttonColumn->addWidget(pasteButton);
    buttonColumn->addWidget(removeButton);
    buttonColumn->addWidget(clearButton);
    buttonColumn->addStretch(1);

    m_recursiveCheck = new QCheckBox(QStringLiteral("文件夹递归"), panel);
    m_recursiveCheck->setObjectName(QStringLiteral("convertRecursiveCheck"));
    m_recursiveCheck->setToolTip(QStringLiteral("加入文件夹时是否连子目录一起收进来。"
                                                "⚠ 它只影响**之后**加入的文件夹，"
                                                "已经加进来的文件不会重新扫描"));
    buttonColumn->addWidget(m_recursiveCheck);
    listRow->addLayout(buttonColumn);
    root->addLayout(listRow);

    // ---- 图片选项 ----
    m_imageGroup = new QGroupBox(QStringLiteral("图片选项"), panel);
    m_imageGroup->setObjectName(QStringLiteral("imageOptionsGroup"));
    auto *imageForm = new QGridLayout(m_imageGroup);
    imageForm->setColumnStretch(1, 1);

    imageForm->addWidget(new QLabel(QStringLiteral("目标格式："), m_imageGroup), 0, 0);
    m_formatCombo = new QComboBox(m_imageGroup);
    m_formatCombo->setObjectName(QStringLiteral("imageFormatCombo"));
    const QVector<ImageConvertEngine::Format> allFormats = ImageConvertEngine::allFormats();
    for (ImageConvertEngine::Format format : allFormats) {
        const bool ok = ImageConvertEngine::isFormatAvailable(format);
        m_formatCombo->addItem(ImageConvertEngine::formatDisplayName(format),
                               ImageConvertEngine::formatKey(format));
        // 缺插件的格式**列出来但置灰**：用户才知道"我这里为什么没有 webp"
        if (!ok) {
            const int row = m_formatCombo->count() - 1;
            m_formatCombo->setItemData(
                row, ImageConvertEngine::formatUnavailableReason(format), Qt::ToolTipRole);
            if (auto *model = qobject_cast<QStandardItemModel *>(m_formatCombo->model())) {
                if (QStandardItem *entry = model->item(row)) {
                    entry->setEnabled(false);
                }
            }
        }
    }
    m_formatCombo->setToolTip(QStringLiteral("JPEG / 有损 WebP 会丢透明与细节；"
                                             "ICO 只能存 256×256 以内"));
    imageForm->addWidget(m_formatCombo, 0, 1);

    imageForm->addWidget(new QLabel(QStringLiteral("质量（1~100）："), m_imageGroup), 1, 0);
    m_qualitySpin = new QSpinBox(m_imageGroup);
    m_qualitySpin->setObjectName(QStringLiteral("imageQualitySpin"));
    m_qualitySpin->setRange(1, 100);
    m_qualitySpin->setValue(90);
    m_qualitySpin->setToolTip(QStringLiteral("只对 JPEG / 有损 WebP 有意义；"
                                             "无损格式不看这个值"));
    imageForm->addWidget(m_qualitySpin, 1, 1);

    imageForm->addWidget(new QLabel(QStringLiteral("缩放："), m_imageGroup), 2, 0);
    auto *resizeRow = new QHBoxLayout();
    m_resizeCombo = new QComboBox(m_imageGroup);
    m_resizeCombo->setObjectName(QStringLiteral("imageResizeCombo"));
    m_resizeCombo->addItem(ImageConvertEngine::resizeModeDisplayName(
                               ImageConvertEngine::ResizeMode::Keep),
                           ImageConvertEngine::resizeModeKey(ImageConvertEngine::ResizeMode::Keep));
    for (ImageConvertEngine::ResizeMode mode :
         { ImageConvertEngine::ResizeMode::Width, ImageConvertEngine::ResizeMode::Height,
           ImageConvertEngine::ResizeMode::MaxEdge, ImageConvertEngine::ResizeMode::Percent }) {
        m_resizeCombo->addItem(ImageConvertEngine::resizeModeDisplayName(mode),
                               ImageConvertEngine::resizeModeKey(mode));
    }
    m_resizeSpin = new QSpinBox(m_imageGroup);
    m_resizeSpin->setObjectName(QStringLiteral("imageResizeSpin"));
    m_resizeSpin->setRange(1, 20000);
    m_resizeSpin->setValue(1920);
    m_resizeSpin->setEnabled(false);
    m_resizeSpin->setToolTip(QStringLiteral("固定宽/高、长边上限按像素算；百分比模式按 1~1000 算"));
    resizeRow->addWidget(m_resizeCombo, 1);
    resizeRow->addWidget(m_resizeSpin);
    imageForm->addLayout(resizeRow, 2, 1);

    imageForm->addWidget(new QLabel(QStringLiteral("水印文字："), m_imageGroup), 3, 0);
    m_watermarkEdit = new QLineEdit(m_imageGroup);
    m_watermarkEdit->setObjectName(QStringLiteral("imageWatermarkEdit"));
    m_watermarkEdit->setPlaceholderText(QStringLiteral("留空则不加；水印会烧进像素里，不可撤销"));
    imageForm->addWidget(m_watermarkEdit, 3, 1);

    imageForm->addWidget(new QLabel(QStringLiteral("水印位置："), m_imageGroup), 4, 0);
    auto *watermarkRow = new QHBoxLayout();
    m_watermarkPosCombo = new QComboBox(m_imageGroup);
    m_watermarkPosCombo->setObjectName(QStringLiteral("imageWatermarkPosCombo"));
    for (ImageConvertEngine::WatermarkPosition position :
         { ImageConvertEngine::WatermarkPosition::Disabled,
           ImageConvertEngine::WatermarkPosition::TopLeft,
           ImageConvertEngine::WatermarkPosition::TopRight,
           ImageConvertEngine::WatermarkPosition::BottomLeft,
           ImageConvertEngine::WatermarkPosition::BottomRight,
           ImageConvertEngine::WatermarkPosition::Center,
           ImageConvertEngine::WatermarkPosition::Tile }) {
        m_watermarkPosCombo->addItem(
            ImageConvertEngine::watermarkPositionDisplayName(position),
            ImageConvertEngine::watermarkPositionKey(position));
    }
    m_watermarkOpacitySpin = new QSpinBox(m_imageGroup);
    m_watermarkOpacitySpin->setObjectName(QStringLiteral("imageWatermarkOpacitySpin"));
    m_watermarkOpacitySpin->setRange(1, 100);
    m_watermarkOpacitySpin->setValue(40);
    m_watermarkOpacitySpin->setSuffix(QStringLiteral("% 不透明"));
    m_watermarkSizeSpin = new QSpinBox(m_imageGroup);
    m_watermarkSizeSpin->setObjectName(QStringLiteral("imageWatermarkSizeSpin"));
    m_watermarkSizeSpin->setRange(1, 30);
    m_watermarkSizeSpin->setValue(4);
    m_watermarkSizeSpin->setSuffix(QStringLiteral("% 字号"));
    watermarkRow->addWidget(m_watermarkPosCombo, 1);
    watermarkRow->addWidget(m_watermarkOpacitySpin);
    watermarkRow->addWidget(m_watermarkSizeSpin);
    imageForm->addLayout(watermarkRow, 4, 1);

    m_keepMetadataCheck = new QCheckBox(QStringLiteral("保留元数据与拍摄时间"), m_imageGroup);
    m_keepMetadataCheck->setObjectName(QStringLiteral("imageKeepMetadataCheck"));
    m_keepMetadataCheck->setChecked(true);
    m_keepMetadataCheck->setToolTip(QStringLiteral(
        "保留：图像文本属性（注释 / 作者 / 软件）、ICC 色彩配置、源文件修改时间。\n"
        "⚠ EXIF 拍摄信息（相机型号、参数、GPS）在读取像素时就已经丢失，本功能保不住它。"));
    imageForm->addWidget(m_keepMetadataCheck, 5, 0, 1, 2);

    imageForm->addWidget(new QLabel(QStringLiteral("命名模板："), m_imageGroup), 6, 0);
    auto *templateRow = new QHBoxLayout();
    m_templateEdit = new QLineEdit(m_imageGroup);
    m_templateEdit->setObjectName(QStringLiteral("imageTemplateEdit"));
    m_templateEdit->setPlaceholderText(QStringLiteral("留空 = 沿用原文件名。可用 {name} {n} {date}"));
    m_numberStartSpin = new QSpinBox(m_imageGroup);
    m_numberStartSpin->setObjectName(QStringLiteral("imageNumberStartSpin"));
    m_numberStartSpin->setRange(0, 99999);
    m_numberStartSpin->setToolTip(QStringLiteral("{n} 的起始序号（补零到 3 位）"));
    templateRow->addWidget(m_templateEdit, 1);
    templateRow->addWidget(m_numberStartSpin);
    imageForm->addLayout(templateRow, 6, 1);
    root->addWidget(m_imageGroup);

    // ---- 文本选项 ----
    m_textGroup = new QGroupBox(QStringLiteral("文本选项"), panel);
    m_textGroup->setObjectName(QStringLiteral("textOptionsGroup"));
    auto *textForm = new QGridLayout(m_textGroup);
    textForm->setColumnStretch(1, 1);

    textForm->addWidget(new QLabel(QStringLiteral("目标字符集："), m_textGroup), 0, 0);
    m_textEncodingCombo = new QComboBox(m_textGroup);
    m_textEncodingCombo->setObjectName(QStringLiteral("textEncodingCombo"));
    for (TextEncodingTools::Encoding encoding :
         { TextEncodingTools::Encoding::Utf8, TextEncodingTools::Encoding::Utf8Bom,
           TextEncodingTools::Encoding::Utf16Le, TextEncodingTools::Encoding::Utf16Be,
           TextEncodingTools::Encoding::Gbk, TextEncodingTools::Encoding::Ansi }) {
        m_textEncodingCombo->addItem(TextEncodingTools::encodingDisplayName(encoding),
                                     TextEncodingTools::encodingKey(encoding));
    }
    textForm->addWidget(m_textEncodingCombo, 0, 1);

    textForm->addWidget(new QLabel(QStringLiteral("换行风格："), m_textGroup), 1, 0);
    m_lineEndingCombo = new QComboBox(m_textGroup);
    m_lineEndingCombo->setObjectName(QStringLiteral("textLineEndingCombo"));
    for (TextEncodingTools::LineEnding lineEnding :
         { TextEncodingTools::LineEnding::Keep, TextEncodingTools::LineEnding::Lf,
           TextEncodingTools::LineEnding::Crlf }) {
        m_lineEndingCombo->addItem(TextEncodingTools::lineEndingDisplayName(lineEnding),
                                   TextEncodingTools::lineEndingKey(lineEnding));
    }
    textForm->addWidget(m_lineEndingCombo, 1, 1);

    auto *textNote = new QLabel(
        QStringLiteral("源字符集**自动探测**（BOM → 是否合法 UTF-8 → 是否合法 GBK → 本机 ANSI），"
                       "结论与依据显示在下面的计划行里。目标字符集装不下的字符（emoji、生僻字）"
                       "会让**这一个文件失败并说明原因**，不会被替换成问号。"),
        m_textGroup);
    textNote->setObjectName(QStringLiteral("convertTextNoteLabel"));
    textNote->setWordWrap(true);
    textForm->addWidget(textNote, 2, 0, 1, 2);
    root->addWidget(m_textGroup);

    // ---- 输出与冲突（两种模式共用） ----
    auto *outputGroup = new QGroupBox(QStringLiteral("输出"), panel);
    outputGroup->setObjectName(QStringLiteral("convertOutputGroup"));
    auto *outputForm = new QGridLayout(outputGroup);
    outputForm->setColumnStretch(1, 1);

    auto *dirRow = new QHBoxLayout();
    m_sameDirRadio = new QRadioButton(QStringLiteral("与源文件同目录"), outputGroup);
    m_sameDirRadio->setObjectName(QStringLiteral("outputSameDirRadio"));
    m_customDirRadio = new QRadioButton(QStringLiteral("指定目录："), outputGroup);
    m_customDirRadio->setObjectName(QStringLiteral("outputCustomDirRadio"));
    m_outputDirEdit = new QLineEdit(outputGroup);
    m_outputDirEdit->setObjectName(QStringLiteral("outputDirEdit"));
    m_outputDirEdit->setPlaceholderText(QStringLiteral("C:\\Users\\…\\Pictures\\converted"));
    m_outputDirEdit->setEnabled(false);
    m_outputBrowseButton = new QPushButton(QStringLiteral("浏览…"), outputGroup);
    m_outputBrowseButton->setObjectName(QStringLiteral("outputBrowseButton"));
    m_outputBrowseButton->setEnabled(false);
    dirRow->addWidget(m_sameDirRadio);
    dirRow->addWidget(m_customDirRadio);
    dirRow->addWidget(m_outputDirEdit, 1);
    dirRow->addWidget(m_outputBrowseButton);
    outputForm->addLayout(dirRow, 0, 0, 1, 2);

    outputForm->addWidget(new QLabel(QStringLiteral("目标已存在时："), outputGroup), 1, 0);
    m_conflictCombo = new QComboBox(outputGroup);
    m_conflictCombo->setObjectName(QStringLiteral("convertConflictCombo"));
    for (ImageConvertEngine::ConflictPolicy policy :
         { ImageConvertEngine::ConflictPolicy::Skip, ImageConvertEngine::ConflictPolicy::Overwrite,
           ImageConvertEngine::ConflictPolicy::AutoRename }) {
        m_conflictCombo->addItem(ImageConvertEngine::conflictPolicyDisplayName(policy),
                                 ImageConvertEngine::conflictPolicyKey(policy));
    }
    outputForm->addWidget(m_conflictCombo, 1, 1);
    root->addWidget(outputGroup);

    // ---- 计划 / 执行 ----
    m_planLabel = new QLabel(panel);
    m_planLabel->setObjectName(QStringLiteral("convertPlanLabel"));
    m_planLabel->setWordWrap(true);
    root->addWidget(m_planLabel);

    m_progress = new QProgressBar(panel);
    m_progress->setObjectName(QStringLiteral("convertProgress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto *actionRow = new QHBoxLayout();
    m_startButton = new QPushButton(QStringLiteral("开始转换"), panel);
    m_startButton->setObjectName(QStringLiteral("convertStartButton"));
    m_startButton->setEnabled(false);
    m_cancelButton = new QPushButton(QStringLiteral("取消"), panel);
    m_cancelButton->setObjectName(QStringLiteral("convertCancelButton"));
    m_cancelButton->setEnabled(false);
    m_stageLabel = new QLabel(panel);
    m_stageLabel->setObjectName(QStringLiteral("convertStageLabel"));
    actionRow->addWidget(m_startButton);
    actionRow->addWidget(m_cancelButton);
    actionRow->addWidget(m_stageLabel, 1);
    root->addLayout(actionRow);

    m_statusLabel = new QLabel(panel);
    m_statusLabel->setObjectName(QStringLiteral("convertStatusLabel"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_resultList = new QListWidget(panel);
    m_resultList->setObjectName(QStringLiteral("convertResultList"));
    m_resultList->setMinimumHeight(90);
    m_resultList->setToolTip(QStringLiteral("逐条结果：成功、失败（含原因）、按策略跳过"));
    root->addWidget(m_resultList);

    // ---- 连接 ----
    connect(m_kindCombo, &QComboBox::currentIndexChanged, this, [this] {
        updateOptionVisibility();
        refreshPlan();
    });
    connect(addFilesButton, &QPushButton::clicked, this, &FileConvertPlugin::addFilesViaDialog);
    connect(addFolderButton, &QPushButton::clicked, this, &FileConvertPlugin::addFolderViaDialog);
    connect(pasteButton, &QPushButton::clicked, this,
            &FileConvertPlugin::addPathsFromClipboard);
    connect(removeButton, &QPushButton::clicked, this, &FileConvertPlugin::removeSelectedPaths);
    connect(clearButton, &QPushButton::clicked, this, &FileConvertPlugin::clearPaths);
    connect(m_recursiveCheck, &QCheckBox::toggled, this, [this] { refreshPlan(); });
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, [this] { refreshPlan(); });
    connect(m_qualitySpin, &QSpinBox::valueChanged, this, [this] { refreshPlan(); });
    connect(m_resizeCombo, &QComboBox::currentIndexChanged, this, [this] {
        m_resizeSpin->setEnabled(m_resizeCombo->currentData().toString()
                                 != ImageConvertEngine::resizeModeKey(
                                        ImageConvertEngine::ResizeMode::Keep));
        refreshPlan();
    });
    connect(m_resizeSpin, &QSpinBox::valueChanged, this, [this] { refreshPlan(); });
    connect(m_watermarkEdit, &QLineEdit::textChanged, this, [this] { refreshPlan(); });
    connect(m_watermarkPosCombo, &QComboBox::currentIndexChanged, this, [this] { refreshPlan(); });
    connect(m_watermarkOpacitySpin, &QSpinBox::valueChanged, this, [this] { refreshPlan(); });
    connect(m_watermarkSizeSpin, &QSpinBox::valueChanged, this, [this] { refreshPlan(); });
    connect(m_keepMetadataCheck, &QCheckBox::toggled, this, [this] { refreshPlan(); });
    connect(m_templateEdit, &QLineEdit::textChanged, this, [this] { refreshPlan(); });
    connect(m_numberStartSpin, &QSpinBox::valueChanged, this, [this] { refreshPlan(); });
    connect(m_textEncodingCombo, &QComboBox::currentIndexChanged, this, [this] { refreshPlan(); });
    connect(m_lineEndingCombo, &QComboBox::currentIndexChanged, this, [this] { refreshPlan(); });
    connect(m_conflictCombo, &QComboBox::currentIndexChanged, this, [this] { refreshPlan(); });
    connect(m_sameDirRadio, &QRadioButton::toggled, this, [this](bool checked) {
        m_outputDirEdit->setEnabled(!checked);
        m_outputBrowseButton->setEnabled(!checked);
        refreshPlan();
    });
    connect(m_outputDirEdit, &QLineEdit::textChanged, this, [this] { refreshPlan(); });
    connect(m_outputBrowseButton, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(m_panel, QStringLiteral("选择输出目录"),
                                                              m_outputDirEdit->text());
        if (!dir.isEmpty()) {
            m_outputDirEdit->setText(QDir::toNativeSeparators(dir));
        }
    });
    connect(m_startButton, &QPushButton::clicked, this, &FileConvertPlugin::startConversion);
    connect(m_cancelButton, &QPushButton::clicked, this, &FileConvertPlugin::requestCancel);

    // 面板建好之后再把配置灌进去（loadSettings 对空指针全程免疫）
    loadSettings();
    updateOptionVisibility();
    refreshFileList();
    refreshPlan();
    setStatus(QStringLiteral("就绪"));
    return panel;
}

// ============================================================================
//  执行
// ============================================================================

void FileConvertPlugin::startConversion()
{
    if (m_running) {
        setStatus(QStringLiteral("正在转换中…… 要停下来请点「取消」"));
        return;
    }
    if (m_paths.isEmpty()) {
        setStatus(QStringLiteral("先加入要转换的文件"));
        return;
    }

    m_lastSummary.clear();
    m_lastResultLines.clear();
    m_lastConverted = 0;
    m_lastFailed = 0;
    if (m_resultList != nullptr) {
        m_resultList->clear();
    }

    const auto note = [this](const QString &stage, int done, int total) {
        // ⚠ 进度回调来自工作线程：必须排队回界面线程再碰 QWidget
        QMetaObject::invokeMethod(
            this, [this, stage, done, total] { updateProgress(stage, done, total); },
            Qt::QueuedConnection);
    };

    if (isTextMode()) {
        TextEncodingTools::Encoding encoding = TextEncodingTools::Encoding::Utf8;
        TextEncodingTools::LineEnding lineEnding = TextEncodingTools::LineEnding::Keep;
        currentTextOptions(&encoding, &lineEnding);

        const auto options = currentImageOptions();
        const QString dir = (m_customDirRadio != nullptr && m_customDirRadio->isChecked())
                                ? m_outputDirEdit->text().trimmed()
                                : QString();
        const TextPlan plan = buildTextPlan(m_paths, dir, options.conflictPolicy);

        if (!plan.error.isEmpty()) {
            setStatus(QStringLiteral("没能开始：%1").arg(plan.error));
            refreshPlan();
            return;
        }

        QStringList work;
        QStringList lines;
        for (const TextPlanItem &item : plan.items) {
            if (item.problem.isEmpty()) {
                work << item.input;
                work << item.output; // 成对存放：源、目标
            } else {
                lines << (item.skipped
                              ? QStringLiteral("跳过：%1（%2）")
                                    .arg(QFileInfo(item.input).fileName(), item.problem)
                              : QStringLiteral("有问题：%1（%2）")
                                    .arg(QFileInfo(item.input).fileName(), item.problem));
            }
        }

        if (work.isEmpty()) {
            setStatus(QStringLiteral("没有可转换的文件：每一项都有问题或被跳过（原因见上面的计划行）"));
            if (m_resultList != nullptr) {
                for (const QString &line : lines) {
                    m_resultList->addItem(line);
                }
            }
            return;
        }

        m_cancel = std::make_shared<Cancel>();
        const std::shared_ptr<Cancel> cancel = m_cancel;
        const int total = work.size() / 2;

        m_running = true;
        m_startButton->setEnabled(false);
        m_cancelButton->setEnabled(true);
        setStatus(QStringLiteral("开始转换 %1 个文本文件……").arg(total));
        note(QStringLiteral("准备"), 0, total);

        reapFinishedWorker();
        m_worker = std::thread([this, work, cancel, encoding, lineEnding, lines, total, note]() {
            QStringList resultLines = lines;
            QStringList failed;
            int converted = 0;
            int handled = 0;
            bool cancelled = false;

            for (int i = 0; i < work.size(); i += 2) {
                if (cancel->isRequested()) {
                    cancelled = true;
                    break;
                }

                const QString &source = work.at(i);
                const QString &target = work.at(i + 1);

                TextEncodingTools::Encoding detected = TextEncodingTools::Encoding::Utf8;
                QString reason;
                if (TextEncodingTools::convertFile(source, target, encoding, lineEnding,
                                                  &detected, &reason)) {
                    resultLines << QStringLiteral("完成：%1 → %2　（按 %3 读取）")
                                       .arg(QFileInfo(source).fileName(),
                                            QFileInfo(target).fileName(),
                                            TextEncodingTools::encodingDisplayName(detected));
                    ++converted;
                } else {
                    const QString text = QStringLiteral("%1：%2")
                                             .arg(QFileInfo(source).fileName(), reason);
                    resultLines << QStringLiteral("失败：%1").arg(text);
                    failed << text;
                }

                ++handled;
                note(QStringLiteral("转换"), handled, total);
            }

            QMetaObject::invokeMethod(
                this,
                [this, cancelled, resultLines, converted, failed, handled, total] {
                    const QString summary =
                        cancelled
                            ? QStringLiteral("已取消：完成 %1 个（还剩 %2 个没处理）")
                                  .arg(converted)
                                  .arg(total - handled)
                            : QStringLiteral("完成 %1 个；失败 %2 个%3")
                                  .arg(converted)
                                  .arg(failed.size())
                                  .arg(failed.isEmpty()
                                           ? QString()
                                           : QStringLiteral("（原因逐条列在下面）"));
                    finishConversion(cancelled, resultLines, summary, QString());
                },
                Qt::QueuedConnection);
        });

        return;
    }

    // ---- 图片模式 ----
    const auto options = currentImageOptions();
    ImageConvertEngine::Plan plan = ImageConvertEngine::plan(m_paths, options);
    if (plan.ok()) {
        for (ImageConvertEngine::Item &item : plan.items) {
            if (item.problem.isEmpty()
                && !ImageConvertEngine::isSupportedImageFile(item.inputPath)) {
                item.problem = QStringLiteral("按扩展名看不是图片文件（.%1）—— 当前是「图片」模式")
                                   .arg(QFileInfo(item.inputPath).suffix());
            }
        }
    }

    if (!plan.ok()) {
        setStatus(QStringLiteral("没能开始：%1").arg(plan.error));
        refreshPlan();
        return;
    }

    QStringList lines;
    for (const ImageConvertEngine::Item &item : plan.items) {
        if (!item.problem.isEmpty()) {
            lines << (item.skippedByConflict
                          ? QStringLiteral("跳过：%1（%2）")
                                .arg(QFileInfo(item.inputPath).fileName(), item.problem)
                          : QStringLiteral("有问题：%1（%2）")
                                .arg(QFileInfo(item.inputPath).fileName(), item.problem));
        }
    }

    if (plan.convertibleCount() == 0) {
        setStatus(QStringLiteral("没有可转换的图片：每一项都有问题或被跳过（原因见上面的计划行）"));
        if (m_resultList != nullptr) {
            for (const QString &line : lines) {
                m_resultList->addItem(line);
            }
        }
        return;
    }

    m_cancel = std::make_shared<Cancel>();
    const std::shared_ptr<Cancel> cancel = m_cancel;

    m_running = true;
    m_startButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    setStatus(QStringLiteral("开始转换……（输出：%1）").arg(plan.outputDir));

    const WinEase::FeaturePlugins::Progress progress = note;
    reapFinishedWorker();
    m_worker = std::thread([this, plan, options, cancel, lines, progress]() {
        const ImageConvertEngine::ApplyResult result =
            ImageConvertEngine::apply(plan, options, cancel.get(), progress);

        QStringList resultLines = lines;
        for (const auto &pair : result.converted) {
            resultLines << QStringLiteral("完成：%1 → %2")
                               .arg(QFileInfo(pair.first).fileName(),
                                    QDir::toNativeSeparators(pair.second));
        }
        for (const QString &skipped : result.skipped) {
            resultLines << QStringLiteral("跳过：%1").arg(skipped);
        }
        for (const QString &failure : result.failed) {
            resultLines << QStringLiteral("失败：%1").arg(failure);
        }

        QMetaObject::invokeMethod(
            this,
            [this, result, resultLines] {
                finishConversion(result.cancelled, resultLines, result.summaryText(),
                                 result.error);
            },
            Qt::QueuedConnection);
    });
}

void FileConvertPlugin::requestCancel()
{
    if (m_cancel != nullptr) {
        m_cancel->request();
        setStatus(QStringLiteral("已请求取消：正在写的那一张会写完，之后停下来"));
    }
}

void FileConvertPlugin::updateProgress(const QString &stage, int done, int total)
{
    if (m_progress != nullptr) {
        const int percent = total > 0 ? qBound(0, done * 100 / total, 100) : 0;
        m_progress->setValue(percent);
    }
    if (m_stageLabel != nullptr) {
        m_stageLabel->setText(QStringLiteral("%1：%2 / %3").arg(stage).arg(done).arg(total));
    }
}

void FileConvertPlugin::finishConversion(bool cancelled, const QStringList &lines,
                                         const QString &summary, const QString &error)
{
    m_running = false;
    if (m_startButton != nullptr) {
        m_startButton->setEnabled(!m_paths.isEmpty());
    }
    if (m_cancelButton != nullptr) {
        m_cancelButton->setEnabled(false);
    }
    if (m_progress != nullptr && !cancelled) {
        m_progress->setValue(error.isEmpty() ? 100 : 0);
    }

    m_lastResultLines = lines;
    m_lastSummary = error.isEmpty() ? summary : QStringLiteral("没能开始：%1").arg(error);

    int failed = 0;
    int converted = 0;
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("失败："))) {
            ++failed;
        } else if (line.startsWith(QStringLiteral("完成："))) {
            ++converted;
        }
    }
    m_lastFailed = failed;
    m_lastConverted = converted;

    if (m_resultList != nullptr) {
        m_resultList->clear();
        for (const QString &line : lines) {
            m_resultList->addItem(QDir::toNativeSeparators(line));
        }
    }
    setStatus(m_lastSummary);
    logMessage(WinEase::PluginLogLevel::Info, m_lastSummary);
}

void FileConvertPlugin::stopWorker()
{
    if (m_cancel != nullptr) {
        m_cancel->request();
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_cancel.reset();
    m_running = false;
}

void FileConvertPlugin::reapFinishedWorker()
{
    // ★ 开新线程之前必须先收掉上一轮的线程对象（踩坑 #71）。
    //   `std::thread` 的赋值运算符撞上自己还 joinable 时**直接 std::terminate**（整个进程消失，
    //   连个错误框都没有）—— 而"跑完一批再点第二批"是最普通的用法。自检抓出来的真 bug。
    //   能走到这里的批次都已经收尾（开工期间「开始」是灰的），所以 join 最多等几毫秒。
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

// ============================================================================
//  配置
// ============================================================================

void FileConvertPlugin::loadSettings()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    // 按 itemData 里的键还原下拉框；⚠ QComboBox 没有 isItemEnabled()，要问模型
    const auto setComboByKey = [](QComboBox *combo, const QString &key) {
        if (combo == nullptr) {
            return;
        }
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        const auto itemEnabledAt = [model](int index) {
            if (model == nullptr) {
                return true;
            }
            QStandardItem *entry = model->item(index);
            return entry != nullptr && entry->isEnabled();
        };
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemData(i).toString() == key && itemEnabledAt(i)) {
                combo->setCurrentIndex(i);
                return;
            }
        }
        // 存下来的键本机不可用（比如换了台没有 webp 插件的机器）→ 退到第一个可用项，
        // 别让用户一进面板就面对一个灰着的"当前格式"
        if (!key.isEmpty()) {
            for (int i = 0; i < combo->count(); ++i) {
                if (itemEnabledAt(i)) {
                    combo->setCurrentIndex(i);
                    return;
                }
            }
        }
    };

    if (m_kindCombo != nullptr) {
        const QString kind = svc->configValue(id(), QStringLiteral("kind"),
                                             QStringLiteral("image")).toString();
        m_kindCombo->setCurrentIndex(kind == QLatin1String("text") ? int(Kind::Text)
                                                                   : int(Kind::Image));
    }

    setComboByKey(m_formatCombo,
                  svc->configValue(id(), QStringLiteral("format"), QStringLiteral("png")).toString());
    if (m_qualitySpin != nullptr) {
        m_qualitySpin->setValue(svc->configValue(id(), QStringLiteral("quality"), 90).toInt());
    }
    setComboByKey(m_resizeCombo,
                  svc->configValue(id(), QStringLiteral("resizeMode"), QStringLiteral("keep"))
                      .toString());
    if (m_resizeSpin != nullptr) {
        m_resizeSpin->setValue(svc->configValue(id(), QStringLiteral("resizeValue"), 1920).toInt());
        m_resizeSpin->setEnabled(m_resizeCombo != nullptr
                                 && m_resizeCombo->currentData().toString()
                                        != ImageConvertEngine::resizeModeKey(
                                               ImageConvertEngine::ResizeMode::Keep));
    }
    if (m_watermarkEdit != nullptr) {
        m_watermarkEdit->setText(
            svc->configValue(id(), QStringLiteral("watermarkText"), QString()).toString());
    }
    setComboByKey(m_watermarkPosCombo,
                  svc->configValue(id(), QStringLiteral("watermarkPosition"), QStringLiteral("off"))
                      .toString());
    if (m_watermarkOpacitySpin != nullptr) {
        m_watermarkOpacitySpin->setValue(
            svc->configValue(id(), QStringLiteral("watermarkOpacity"), 40).toInt());
    }
    if (m_watermarkSizeSpin != nullptr) {
        m_watermarkSizeSpin->setValue(
            svc->configValue(id(), QStringLiteral("watermarkSize"), 4).toInt());
    }
    if (m_keepMetadataCheck != nullptr) {
        m_keepMetadataCheck->setChecked(
            svc->configValue(id(), QStringLiteral("keepMetadata"), true).toBool());
    }
    if (m_templateEdit != nullptr) {
        m_templateEdit->setText(
            svc->configValue(id(), QStringLiteral("nameTemplate"), QString()).toString());
    }
    if (m_numberStartSpin != nullptr) {
        m_numberStartSpin->setValue(
            svc->configValue(id(), QStringLiteral("numberStart"), 1).toInt());
    }
    setComboByKey(m_textEncodingCombo,
                  svc->configValue(id(), QStringLiteral("textEncoding"), QStringLiteral("utf8"))
                      .toString());
    setComboByKey(m_lineEndingCombo,
                  svc->configValue(id(), QStringLiteral("lineEnding"), QStringLiteral("keep"))
                      .toString());
    setComboByKey(m_conflictCombo,
                  svc->configValue(id(), QStringLiteral("conflict"), QStringLiteral("skip"))
                      .toString());
    if (m_recursiveCheck != nullptr) {
        m_recursiveCheck->setChecked(
            svc->configValue(id(), QStringLiteral("recursive"), false).toBool());
    }

    const QString outputDir = svc->configValue(id(), QStringLiteral("outputDir"), QString()).toString();
    if (m_outputDirEdit != nullptr) {
        m_outputDirEdit->setText(QDir::toNativeSeparators(outputDir));
    }
    const bool custom = !outputDir.isEmpty();
    if (custom && m_customDirRadio != nullptr) {
        m_customDirRadio->setChecked(true);
    } else if (m_sameDirRadio != nullptr) {
        m_sameDirRadio->setChecked(true);
    }
    if (m_outputDirEdit != nullptr) {
        m_outputDirEdit->setEnabled(custom);
    }
    if (m_outputBrowseButton != nullptr) {
        m_outputBrowseButton->setEnabled(custom);
    }
}

void FileConvertPlugin::saveSettings()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    if (m_kindCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("kind"),
                            m_kindCombo->currentIndex() == int(Kind::Text) ? QStringLiteral("text")
                                                                          : QStringLiteral("image"));
    }
    if (m_formatCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("format"),
                            m_formatCombo->currentData().toString());
    }
    if (m_qualitySpin != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("quality"), m_qualitySpin->value());
    }
    if (m_resizeCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("resizeMode"),
                            m_resizeCombo->currentData().toString());
    }
    if (m_resizeSpin != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("resizeValue"), m_resizeSpin->value());
    }
    if (m_watermarkEdit != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("watermarkText"), m_watermarkEdit->text());
    }
    if (m_watermarkPosCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("watermarkPosition"),
                            m_watermarkPosCombo->currentData().toString());
    }
    if (m_watermarkOpacitySpin != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("watermarkOpacity"),
                            m_watermarkOpacitySpin->value());
    }
    if (m_watermarkSizeSpin != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("watermarkSize"), m_watermarkSizeSpin->value());
    }
    if (m_keepMetadataCheck != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("keepMetadata"), m_keepMetadataCheck->isChecked());
    }
    if (m_templateEdit != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("nameTemplate"), m_templateEdit->text());
    }
    if (m_numberStartSpin != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("numberStart"), m_numberStartSpin->value());
    }
    if (m_textEncodingCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("textEncoding"),
                            m_textEncodingCombo->currentData().toString());
    }
    if (m_lineEndingCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("lineEnding"),
                            m_lineEndingCombo->currentData().toString());
    }
    if (m_conflictCombo != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("conflict"),
                            m_conflictCombo->currentData().toString());
    }
    if (m_recursiveCheck != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("recursive"), m_recursiveCheck->isChecked());
    }
    if (m_outputDirEdit != nullptr && m_customDirRadio != nullptr) {
        svc->setConfigValue(id(), QStringLiteral("outputDir"),
                            m_customDirRadio->isChecked() ? m_outputDirEdit->text() : QString());
    }
}

void FileConvertPlugin::setStatus(const QString &text)
{
    if (m_statusLabel != nullptr) {
        m_statusLabel->setText(text);
    }
}
