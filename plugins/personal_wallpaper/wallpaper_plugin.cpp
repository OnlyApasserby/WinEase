#include "wallpaper_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/DesktopWallpaper.h"
#include "win32/WindowTarget.h" // cursorPosition()
#include "win32/WindowUtils.h"  // monitorForPoint() / MonitorInfo

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace {

constexpr int kMaxHistory = 10;

QString normalizePath(const QString &path)
{
    // 跨存储比较路径前必须归一化（大小写 + 分隔符），否则同一个文件会判成两个
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

} // namespace

WallpaperPlugin::WallpaperPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, [this] { step(1); });
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString WallpaperPlugin::id() const
{
    return QStringLiteral("personal.wallpaper");
}

QString WallpaperPlugin::name() const
{
    return QStringLiteral("壁纸自动切换");
}

QString WallpaperPlugin::description() const
{
    return QStringLiteral("从指定文件夹按序或随机换壁纸，可定时自动换；停用即恢复原来的壁纸");
}

QIcon WallpaperPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Personalization);
}

WinEase::FeatureCategory WallpaperPlugin::category() const
{
    return WinEase::FeatureCategory::Personalization;
}

QStringList WallpaperPlugin::tags() const
{
    return { QStringLiteral("壁纸"), QStringLiteral("桌面"), QStringLiteral("wallpaper"),
             QStringLiteral("轮换"), QStringLiteral("随机") };
}

bool WallpaperPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence WallpaperPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+W"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool WallpaperPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_folder = svc->configValue(id(), QStringLiteral("folder")).toString();
        m_mode = svc->configValue(id(), QStringLiteral("mode"), QStringLiteral("sequential")).toString();
        m_intervalMinutes = svc->configValue(id(), QStringLiteral("intervalMinutes"), 0).toInt();
        m_dryRun = svc->configValue(id(), QStringLiteral("dryRun"), false).toBool();
        m_perMonitor = svc->configValue(id(), QStringLiteral("perMonitor"), false).toBool();
        m_original = svc->configValue(id(), QStringLiteral("originalWallpaper")).toString();
        m_hasOriginal = !m_original.isEmpty();
        m_lastApplied = svc->configValue(id(), QStringLiteral("lastApplied")).toString();
        m_history = svc->configValue(id(), QStringLiteral("history")).toStringList();
    }
    if (m_mode != QLatin1String("random")) {
        m_mode = QStringLiteral("sequential");
    }
    return true;
}

void WallpaperPlugin::shutdown()
{
    m_timer.stop();
    // 退出**不还原**（有意）：壁纸是用户看得见的结果，退出时翻回原来的图会很突兀；
    // 验收原文也是"重启后保持"。还原契约落在 onDisable（用户明确停用功能）。
}

bool WallpaperPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    if (!WinEase::Win32::isDesktopWallpaperAvailable()) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("IDesktopWallpaper 不可用，将退回 SystemParametersInfo（只能整屏一张）"));
    }

    // 记录"启用前"的壁纸作为还原目标（每次启用重新取，不跨会话沿用旧值）
    m_original = WinEase::Win32::currentWallpaper();
    m_hasOriginal = !m_original.isEmpty();
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("originalWallpaper"), m_original);
    }

    restartTimer();

    const QStringList files = imageFiles();
    Q_EMIT statusMessage(QStringLiteral("就绪：%1 换下一张（%2 · %3 张图%4）")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText),
                                  m_mode == QLatin1String("random") ? QStringLiteral("随机")
                                                                    : QStringLiteral("按序"))
                             .arg(files.size())
                             .arg(m_intervalMinutes > 0
                                      ? QStringLiteral(" · 每 %1 分钟自动换").arg(m_intervalMinutes)
                                      : QString()));
    return true;
}

void WallpaperPlugin::onDisable()
{
    m_timer.stop();
    const bool restored = restoreOriginal();
    m_hasOriginal = false;
    Q_EMIT statusMessage(restored ? QStringLiteral("已停用，壁纸已恢复原样")
                                  : QStringLiteral("已停用（原壁纸路径未知，未做恢复）"));
}

void WallpaperPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("prev")) {
        step(-1);
    } else if (action == QLatin1String("restore")) {
        if (restoreOriginal()) {
            Q_EMIT statusMessage(QStringLiteral("已恢复启用前的壁纸"));
        } else {
            Q_EMIT statusMessage(QStringLiteral("恢复失败：没有记录到原壁纸路径"));
        }
    } else {
        step(1); // "default"
    }
}

// ---------------------------------------------------------------------------
//  功能
// ---------------------------------------------------------------------------

QStringList WallpaperPlugin::imageFiles() const
{
    QStringList result;
    if (m_folder.isEmpty()) {
        return result;
    }
    const QDir dir(m_folder);
    if (!dir.exists()) {
        return result;
    }

    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &info : entries) {
        if (WinEase::Win32::isSupportedWallpaperFile(info.fileName())) {
            result.append(info.absoluteFilePath());
        }
    }
    return result;
}

int WallpaperPlugin::currentImageIndex(const QStringList &files) const
{
    const QString current = normalizePath(WinEase::Win32::currentWallpaper());
    if (current.isEmpty()) {
        return -1;
    }
    for (int index = 0; index < files.size(); ++index) {
        if (normalizePath(files.at(index)) == current) {
            return index;
        }
    }
    return -1;
}

bool WallpaperPlugin::applyImage(int index, QString *appliedOut)
{
    const QStringList files = imageFiles();
    if (files.isEmpty()) {
        setLastError(QStringLiteral("文件夹里没有可用图片：%1")
                         .arg(m_folder.isEmpty() ? QStringLiteral("（还没设置文件夹）") : m_folder));
        Q_EMIT statusMessage(QStringLiteral("换壁纸失败：%1").arg(lastError()));
        return false;
    }

    const int count = files.size();
    int target = ((index % count) + count) % count; // 支持负数与回绕
    const QString path = files.at(target);
    m_cursor = target;

    if (m_dryRun) {
        if (appliedOut != nullptr) {
            *appliedOut = path;
        }
        Q_EMIT statusMessage(QStringLiteral("（演练）下一张会是：%1（第 %2 / %3 张）")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(target + 1)
                                 .arg(count));
        return true;
    }

    QString error;
    bool ok = false;
    if (m_perMonitor) {
        // 多屏：把当前这张图分给"光标所在显示器"，其余保持不动。
        // 这样一句话就能解释它：换的是"我正在看的那块屏"
        const WinEase::Win32::MonitorInfo monitor =
            WinEase::Win32::monitorForPoint(WinEase::Win32::cursorPosition());
        const QList<WinEase::Win32::WallpaperInfo> all = WinEase::Win32::wallpapers();
        int monitorIndex = 0;
        for (int i = 0; i < all.size(); ++i) {
            if (all.at(i).monitorRect == monitor.geometry) {
                monitorIndex = i;
                break;
            }
        }
        ok = WinEase::Win32::setWallpaperForMonitor(monitorIndex, path, &error);
    } else {
        ok = WinEase::Win32::setWallpaper(path, &error);
    }

    if (!ok) {
        setLastError(error);
        logMessage(WinEase::PluginLogLevel::Warning, error);
        Q_EMIT statusMessage(QStringLiteral("换壁纸失败：%1").arg(error));
        return false;
    }

    m_lastApplied = path;
    m_history.prepend(path);
    while (m_history.size() > kMaxHistory) {
        m_history.removeLast();
    }
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("lastApplied"), m_lastApplied);
        svc->setConfigValue(id(), QStringLiteral("history"), m_history);
    }
    if (appliedOut != nullptr) {
        *appliedOut = path;
    }
    return true;
}

bool WallpaperPlugin::step(int delta)
{
    const QStringList files = imageFiles();
    if (files.isEmpty()) {
        return applyImage(0, nullptr); // 统一走同一处报错
    }

    if (m_mode == QLatin1String("random") && delta > 0 && files.size() > 1) {
        int target = m_cursor;
        // 随机但**不连着重复**：否则用户会觉得"没反应"
        for (int attempt = 0; attempt < 8 && target == m_cursor; ++attempt) {
            target = static_cast<int>(QRandomGenerator::global()->bounded(files.size()));
        }
        return applyImage(target, nullptr);
    }

    const int current = m_cursor >= 0 ? m_cursor : currentImageIndex(files);
    const int next = (current < 0) ? 0 : current + delta;
    return applyImage(next, nullptr);
}

bool WallpaperPlugin::restoreOriginal()
{
    if (m_original.isEmpty()) {
        return false;
    }
    if (!QFileInfo::exists(m_original)) {
        // 原壁纸文件可能已被删除 —— 说明原因，不要静默失败
        setLastError(QStringLiteral("原壁纸文件已不存在：%1").arg(m_original));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        return false;
    }
    QString error;
    if (!WinEase::Win32::setWallpaper(m_original, &error)) {
        setLastError(error);
        return false;
    }
    m_lastApplied.clear();
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("lastApplied"), QString());
    }
    return true;
}

void WallpaperPlugin::restartTimer()
{
    m_timer.stop();
    if (m_intervalMinutes > 0) {
        m_timer.setInterval(m_intervalMinutes * 60 * 1000);
        m_timer.start();
    }
}

QString WallpaperPlugin::describeCurrent() const
{
    const QString current = WinEase::Win32::currentWallpaper();
    if (current.isEmpty()) {
        return QStringLiteral("（读不到当前壁纸，可能是幻灯片或纯色）");
    }
    const QStringList files = imageFiles();
    const int index = currentImageIndex(files);
    if (index >= 0) {
        return QStringLiteral("%1（第 %2 / %3 张）")
            .arg(QFileInfo(current).fileName())
            .arg(index + 1)
            .arg(files.size());
    }
    return QFileInfo(current).fileName();
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *WallpaperPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *folderRow = new QHBoxLayout();
    folderRow->addWidget(new QLabel(QStringLiteral("图片文件夹："), widget));
    auto *folderEdit = new QLineEdit(m_folder, widget);
    folderEdit->setPlaceholderText(QStringLiteral("选一个放壁纸的文件夹"));
    folderRow->addWidget(folderEdit);
    auto *browseButton = new QPushButton(QStringLiteral("浏览…"), widget);
    folderRow->addWidget(browseButton);
    layout->addLayout(folderRow);

    auto *modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel(QStringLiteral("顺序："), widget));
    auto *sequentialBox = new QCheckBox(QStringLiteral("按文件名顺序（不勾＝随机）"), widget);
    sequentialBox->setChecked(m_mode != QLatin1String("random"));
    modeRow->addWidget(sequentialBox);
    modeRow->addStretch(1);
    layout->addLayout(modeRow);

    auto *intervalRow = new QHBoxLayout();
    intervalRow->addWidget(new QLabel(QStringLiteral("自动轮换间隔："), widget));
    auto *intervalBox = new QSpinBox(widget);
    intervalBox->setRange(0, 24 * 60);
    intervalBox->setSuffix(QStringLiteral(" 分钟（0 = 只手动）"));
    intervalBox->setValue(m_intervalMinutes);
    intervalRow->addWidget(intervalBox);
    intervalRow->addStretch(1);
    layout->addLayout(intervalRow);

    auto *dryRunBox = new QCheckBox(QStringLiteral("演练模式（只提示下一张，不真的换）"), widget);
    dryRunBox->setChecked(m_dryRun);
    layout->addWidget(dryRunBox);
    auto *perMonitorBox = new QCheckBox(QStringLiteral("只换「光标所在的那块屏」（多显示器）"), widget);
    perMonitorBox->setChecked(m_perMonitor);
    layout->addWidget(perMonitorBox);

    auto *buttonRow = new QHBoxLayout();
    auto *nextButton = new QPushButton(QStringLiteral("下一张"), widget);
    auto *prevButton = new QPushButton(QStringLiteral("上一张"), widget);
    auto *restoreButton = new QPushButton(QStringLiteral("恢复原壁纸"), widget);
    buttonRow->addWidget(nextButton);
    buttonRow->addWidget(prevButton);
    buttonRow->addWidget(restoreButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *infoLabel = new QLabel(widget);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto *hint = new QLabel(QStringLiteral("快捷键：%1 下一张 · Ctrl+Shift+Alt+W 上一张\n"
                                          "· 支持 bmp / jpg / png / webp；优先用 IDesktopWallpaper（多屏可分屏设置）\n"
                                          "· **停用本功能会恢复成启用前的壁纸**；退出 WinEase 不影响已换的壁纸")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refreshInfo = [this, infoLabel] {
        const QStringList files = imageFiles();
        infoLabel->setText(QStringLiteral("文件夹里找到 %1 张可用图片\n当前壁纸：%2")
                               .arg(files.size())
                               .arg(describeCurrent()));
    };
    refreshInfo();

    const auto persist = [this] {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("folder"), m_folder);
            svc->setConfigValue(id(), QStringLiteral("mode"), m_mode);
            svc->setConfigValue(id(), QStringLiteral("intervalMinutes"), m_intervalMinutes);
            svc->setConfigValue(id(), QStringLiteral("dryRun"), m_dryRun);
            svc->setConfigValue(id(), QStringLiteral("perMonitor"), m_perMonitor);
        }
    };

    QObject::connect(browseButton, &QPushButton::clicked, widget, [this, folderEdit, refreshInfo, persist] {
        const QString picked = QFileDialog::getExistingDirectory(nullptr,
                                                                 QStringLiteral("选择壁纸文件夹"),
                                                                 m_folder);
        if (picked.isEmpty()) {
            return;
        }
        m_folder = picked;
        folderEdit->setText(picked);
        persist();
        refreshInfo();
    });
    QObject::connect(folderEdit, &QLineEdit::editingFinished, widget,
                     [this, folderEdit, refreshInfo, persist] {
                         m_folder = folderEdit->text().trimmed();
                         persist();
                         refreshInfo();
                     });
    QObject::connect(sequentialBox, &QCheckBox::toggled, widget, [this, refreshInfo, persist](bool checked) {
        m_mode = checked ? QStringLiteral("sequential") : QStringLiteral("random");
        persist();
        refreshInfo();
    });
    QObject::connect(intervalBox, &QSpinBox::valueChanged, widget, [this, persist](int value) {
        m_intervalMinutes = value;
        persist();
        restartTimer();
    });
    QObject::connect(dryRunBox, &QCheckBox::toggled, widget, [this, persist](bool checked) {
        m_dryRun = checked;
        persist();
    });
    QObject::connect(perMonitorBox, &QCheckBox::toggled, widget, [this, persist](bool checked) {
        m_perMonitor = checked;
        persist();
    });
    QObject::connect(nextButton, &QPushButton::clicked, widget, [this, refreshInfo] {
        step(1);
        refreshInfo();
    });
    QObject::connect(prevButton, &QPushButton::clicked, widget, [this, refreshInfo] {
        step(-1);
        refreshInfo();
    });
    QObject::connect(restoreButton, &QPushButton::clicked, widget, [this, refreshInfo] {
        if (!restoreOriginal()) {
            Q_EMIT statusMessage(QStringLiteral("恢复失败：%1").arg(lastError()));
        }
        refreshInfo();
    });

    return widget;
}
