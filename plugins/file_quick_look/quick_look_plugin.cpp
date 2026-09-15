#include "quick_look_plugin.h"

#include "FilePreview.h"
#include "sdk/HookService.h"
#include "sdk/PluginServices.h"
#include "win32/WindowUtils.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace FilePreview = WinEase::FeaturePlugins::FilePreview;

namespace {

/// 本插件的快捷键。
///
/// 单独抽出来是为了让**静态**函数（fileFromClipboard）也能拿到它 ——
/// 提示语里要告诉用户"按哪个键预览"，这属于给用户看的"说明书"，
/// 不能让提示文案里的键和 defaultHotkey() 各写一份（改一处漏一处）。
QKeySequence previewHotkey()
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+Space"));
}

/// 资源管理器"文件窗口"的窗口类名。
///
/// ★ 为什么不直接判"进程是不是 explorer.exe"：桌面（Progman/WorkerW）和任务栏
///   （Shell_TrayWnd）也都是 explorer.exe，但它们不是"用户在看着一堆文件"的场景，
///   在桌面上按空格被我们吃掉会很莫名其妙。类名判断更准，而且比取进程名更快
///   （GetClassName 不用打开进程句柄）。
bool isFileExplorerWindow(WinEase::Win32::WindowHandle hwnd)
{
    if (hwnd == nullptr) {
        return false;
    }
    wchar_t className[64] = {};
    if (::GetClassNameW(hwnd, className, 63) == 0) {
        return false;
    }
    return ::lstrcmpW(className, L"CabinetWClass") == 0
        || ::lstrcmpW(className, L"ExploreWClass") == 0;
}

} // namespace

// ============================================================================
//  空格键订阅者
//
//  ⚠ 本类的方法运行在**钩子线程**上（Direct 模式），每一行都在 50ms 回调预算里。
//    这里只做三件事：键码比对、窗口类名比对、把活排队交给界面线程。
//    绝不在这里读剪贴板或渲染预览（读剪贴板可能阻塞在别的进程上）。
// ============================================================================

class SpaceKeyListener : public WinEase::HookListener
{
public:
    explicit SpaceKeyListener(QuickLookPlugin *plugin)
        : WinEase::HookListener(plugin) // parent = 插件：插件析构时自动退订
        , m_plugin(plugin)
    {
    }

    WinEase::HookDelivery delivery() const override { return WinEase::HookDelivery::Direct; }

protected:
    bool onHookEvent(const WinEase::HookEvent &event) override
    {
        if (event.type != WinEase::HookEventKey || !event.key.pressed || event.isInjected()) {
            return false;
        }
        if (event.key.virtualKey != kVkSpace) {
            return false;
        }
        if (m_plugin == nullptr || !m_plugin->isEnabled() || !m_plugin->spaceKeyEnabled()) {
            return false;
        }
        // 带修饰键的空格（Ctrl+空格、Alt+空格…）留给系统和别的功能
        if (!WinEase::HookService::modifiersMatch(event.key.modifiers, WinEase::HookModNone)) {
            return false;
        }
        if (!isFileExplorerWindow(WinEase::Win32::foregroundWindow())) {
            return false;
        }

        m_plugin->toggleFromSpaceKey();
        return true; // ★ 吞掉空格：否则资源管理器会同时把它当成"切换选中项"
    }

private:
    static constexpr quint32 kVkSpace = 0x20; // VK_SPACE
    QuickLookPlugin *m_plugin = nullptr;
};

// ============================================================================
//  预览窗口
//
//  ⚠ 加载一律在**界面线程**上做（见 ROADMAP 的实现说明）：
//    Shell 缩略图与 WinRT PDF 都要求调用线程已初始化 COM 套间，
//    而 Qt 线程池里的线程没有；在非 Qt 线程里补初始化 COM 会给后面的
//    "缩略图生成器崩了算谁的"埋雷。代价是大图解码会有几百毫秒的停顿 ——
//    对一个"按空格看一眼"的工具来说，这个取舍是划算的。
// ============================================================================

class QuickLookWindow : public QWidget
{
public:
    explicit QuickLookWindow(QuickLookPlugin *plugin)
        : QWidget(nullptr)
        , m_plugin(plugin)
    {
        setObjectName(QStringLiteral("winease_quick_look"));
        setWindowTitle(QStringLiteral("文件预览 - WinEase"));
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_DeleteOnClose, true);
        resize(980, 700);
        buildUi();
        applyDarkStyle();
    }

    /// 显示一个文件；读不出来时窗口不关，把原因写在界面里
    void showFile(const QString &path)
    {
        m_path = path;
        const QFileInfo fileInfo(path);
        if (!fileInfo.exists()) {
            showMessage(QStringLiteral("文件不存在：%1").arg(QDir::toNativeSeparators(path)));
            return;
        }

        const FilePreview::Info info = FilePreview::describe(path);
        m_infoLabel->setText(info.summary());

        if (info.isDirectory) {
            showDirectory(path, info);
            return;
        }

        // 解码上限（物理像素）= 内容区大小
        // ⚠ 不能直接量 m_stack->size()：窗口第一次显示前布局还没跑过，量到的是"未布局的
        //   默认尺寸"（40×30 那种），大图会被缩成一个指甲盖贴在大窗口中间 —— 先强制跑一遍布局
        if (QLayout *windowLayout = layout()) {
            windowLayout->activate();
        }
        QSize content = m_stack->size();
        if (content.width() < 320 || content.height() < 240) {
            // 布局尚未生效（例如窗口还没 show）：退回"按窗口尺寸估内容区"
            content = size() - QSize(0, m_infoLabel->sizeHint().height()
                                            + m_hintLabel->sizeHint().height());
        }
        const QSize contentPixels = content * m_stack->devicePixelRatioF();

        switch (info.kind) {
        case FilePreview::Kind::Text:
            showText(path);
            return;
        case FilePreview::Kind::Unsupported:
        case FilePreview::Kind::Other: {
            // 认不出来的类型：先问系统要缩略图（Office/压缩包这类系统能给图）；
            // 系统只给"类型图标"时，再试一次当文本读（很多配置文件的扩展名不在清单里）
            const FilePreview::BitmapResult bitmap =
                FilePreview::loadBitmap(path, info.kind, contentPixels);
            const bool usable = bitmap.ok && bitmap.source != QLatin1String("类型图标");
            if (usable) {
                showBitmap(bitmap);
                return;
            }
            const FilePreview::TextResult text = FilePreview::readText(path, m_plugin->maxBytes());
            if (text.ok && !text.binary) {
                showTextResult(text);
                return;
            }
            if (bitmap.ok) {
                showBitmap(bitmap);
                return;
            }
            showMessage(bitmap.error.isEmpty() ? QStringLiteral("这个文件没有可预览的内容")
                                               : bitmap.error);
            return;
        }
        default: {
            const FilePreview::BitmapResult bitmap =
                FilePreview::loadBitmap(path, info.kind, contentPixels);
            showBitmap(bitmap);
            return;
        }
        }
    }

    /// 没有任何可预览对象时的说明页面
    void showMessage(const QString &text)
    {
        m_path.clear();
        m_infoLabel->setText(QStringLiteral("WinEase 文件预览"));
        m_textView->setPlainText(text);
        m_stack->setCurrentWidget(m_textView);
        m_hintLabel->setText(QStringLiteral("Esc / 空格 关闭"));
        m_loaded = false;
    }

    /// 显示 = 放到光标所在显示器中央 + 置顶 + 拿焦点（键盘操作全靠它）
    void showPreviewWindow()
    {
        placeOnCursorMonitor();
        show();
        raise();
        activateWindow();
        setFocus(Qt::OtherFocusReason);
    }

    QString currentPath() const { return m_path; }
    /// 自检用：内容是否真的加载成功（而不是只显示了错误提示）
    bool loaded() const { return m_loaded; }
    /// 自检用：这一屏是文本还是位图
    bool showingImage() const { return m_stack->currentWidget() == m_imageLabel; }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Escape:
            close();
            return;
        case Qt::Key_Space:
            if (m_plugin->spaceKeyEnabled()) {
                close();
                return;
            }
            break;
        case Qt::Key_Left:
        case Qt::Key_Right: {
            const int offset = event->key() == Qt::Key_Left ? -1 : 1;
            const QString next = m_plugin->siblingPath(m_path, offset);
            if (!next.isEmpty()) {
                m_plugin->previewPath(next);
            }
            return;
        }
        default:
            break;
        }
        QWidget::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // 无边框窗口要能挪：按住顶部信息条拖动（走系统的 startSystemMove，
        // 自己算偏移量在高 DPI 多屏下必然出现"松手就跳"）
        if (event->button() == Qt::LeftButton && event->position().y() <= 32) {
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
        header->setObjectName(QStringLiteral("quickLookHeader"));
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(12, 6, 6, 6);
        m_infoLabel = new QLabel(header);
        m_infoLabel->setObjectName(QStringLiteral("quickLookInfo"));
        m_infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *closeButton = new QPushButton(QStringLiteral("✕"), header);
        closeButton->setObjectName(QStringLiteral("quickLookClose"));
        closeButton->setFixedSize(26, 26);
        closeButton->setToolTip(QStringLiteral("关闭（Esc）"));
        headerLayout->addWidget(m_infoLabel, 1);
        headerLayout->addWidget(closeButton);
        layout->addWidget(header);

        m_stack = new QStackedWidget(this);
        m_stack->setObjectName(QStringLiteral("quickLookStack"));

        m_textView = new QPlainTextEdit(m_stack);
        m_textView->setObjectName(QStringLiteral("quickLookText"));
        m_textView->setReadOnly(true);
        m_textView->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono(QStringLiteral("Consolas"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSize(10);
        m_textView->setFont(mono);

        m_imageLabel = new QLabel(m_stack);
        m_imageLabel->setObjectName(QStringLiteral("quickLookImage"));
        m_imageLabel->setAlignment(Qt::AlignCenter);
        m_imageLabel->setMinimumSize(200, 150);

        m_stack->addWidget(m_textView);
        m_stack->addWidget(m_imageLabel);
        layout->addWidget(m_stack, 1);

        m_hintLabel = new QLabel(this);
        m_hintLabel->setObjectName(QStringLiteral("quickLookHint"));
        m_hintLabel->setWordWrap(true);
        m_hintLabel->setContentsMargins(12, 4, 12, 6);
        layout->addWidget(m_hintLabel);

        QObject::connect(closeButton, &QPushButton::clicked, this, [this] { close(); });
    }

    void applyDarkStyle()
    {
        // 插件窗口不参与主程序的 QSS（那是主程序 exe 里的资源），
        // 所以这里自带一份深色样式，保持"WinEase 的窗口都是黑底白字"
        setStyleSheet(QStringLiteral(
            "QWidget#winease_quick_look { background:#1b1b1b; color:#e6e6e6; }"
            "QWidget#quickLookHeader { background:#262626; }"
            "QLabel#quickLookInfo { color:#e6e6e6; font-weight:600; }"
            "QLabel#quickLookHint { color:#9e9e9e; }"
            "QPlainTextEdit#quickLookText { background:#151515; color:#e0e0e0;"
            "  border:0; padding:8px; }"
            "QLabel#quickLookImage { background:#101010; }"
            "QPushButton { background:#333; color:#e6e6e6; border:0; border-radius:4px; }"
            "QPushButton:hover { background:#444; }"));
    }

    void showText(const QString &path)
    {
        const FilePreview::TextResult text = FilePreview::readText(path, m_plugin->maxBytes());
        if (!text.ok) {
            showMessage(text.error);
            return;
        }
        showTextResult(text);
    }

    void showTextResult(const FilePreview::TextResult &text)
    {
        m_textView->setPlainText(text.text);
        m_textView->moveCursor(QTextCursor::Start);
        m_stack->setCurrentWidget(m_textView);
        m_loaded = true;

        QStringList notes;
        notes.append(text.sizeText());
        notes.append(text.encoding);
        if (text.binary) {
            notes.append(QStringLiteral("二进制内容：按十六进制显示"));
        }
        m_hintLabel->setText(QStringLiteral("%1　|　%2").arg(notes.join(QStringLiteral(" · ")), navigationHint()));
    }

    void showBitmap(const FilePreview::BitmapResult &bitmap)
    {
        if (!bitmap.ok) {
            showMessage(bitmap.error);
            return;
        }
        m_imageLabel->setPixmap(QPixmap::fromImage(bitmap.image));
        m_stack->setCurrentWidget(m_imageLabel);
        m_loaded = true;
        m_hintLabel->setText(
            QStringLiteral("%1　|　%2").arg(bitmap.describe(), navigationHint()));
    }

    void showDirectory(const QString &path, const FilePreview::Info &info)
    {
        // 文件夹也值得看一眼：列出前若干项，比"不支持预览"有用得多
        const QDir dir(path);
        const QStringList names =
            dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        QStringList lines;
        lines.append(QStringLiteral("%1  （%2 项）").arg(info.summary(), QString::number(names.size())));
        lines.append(QString());
        const int limit = qMin(names.size(), 500);
        for (int i = 0; i < limit; ++i) {
            const QString name = names.at(i);
            lines.append(QFileInfo(dir.filePath(name)).isDir() ? QStringLiteral("[目录] %1").arg(name)
                                                               : name);
        }
        if (names.size() > limit) {
            lines.append(QStringLiteral("…… 还有 %1 项").arg(names.size() - limit));
        }
        m_textView->setPlainText(lines.join(QLatin1Char('\n')));
        m_textView->moveCursor(QTextCursor::Start);
        m_stack->setCurrentWidget(m_textView);
        m_loaded = true;
        m_hintLabel->setText(QStringLiteral("文件夹　|　Esc / 空格 关闭"));
    }

    QString navigationHint() const
    {
        const QString close = m_plugin->spaceKeyEnabled() ? QStringLiteral("空格 / Esc 关闭")
                                                          : QStringLiteral("Esc 关闭");
        return QStringLiteral("← → 换同目录文件　|　%1　|　上限 %2")
            .arg(close, FilePreview::formatBytes(m_plugin->maxBytes()));
    }

    void placeOnCursorMonitor()
    {
        // 多屏时"预览窗弹到另一块屏幕上"是很烦的事：以光标所在屏幕为准
        const QPoint cursor = QCursor::pos();
        QScreen *screen = QGuiApplication::screenAt(cursor);
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen == nullptr) {
            return;
        }
        const QRect available = screen->availableGeometry();
        const QSize windowSize = size();
        move(available.center().x() - windowSize.width() / 2,
             available.center().y() - windowSize.height() / 2);
    }

    QuickLookPlugin *m_plugin = nullptr;
    QLabel *m_infoLabel = nullptr;
    QStackedWidget *m_stack = nullptr;
    QPlainTextEdit *m_textView = nullptr;
    QLabel *m_imageLabel = nullptr;
    QLabel *m_hintLabel = nullptr;
    QString m_path;
    bool m_loaded = false;
};

// ============================================================================
//  插件本体
// ============================================================================

QuickLookPlugin::QuickLookPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString QuickLookPlugin::id() const
{
    return QStringLiteral("file.quick_look");
}

QString QuickLookPlugin::name() const
{
    return QStringLiteral("快速文件预览");
}

QString QuickLookPlugin::description() const
{
    return QStringLiteral("在资源管理器里按空格，看一眼剪贴板文件的文本 / 图片 / PDF");
}

QIcon QuickLookPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory QuickLookPlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList QuickLookPlugin::tags() const
{
    return { QStringLiteral("预览"), QStringLiteral("quicklook"), QStringLiteral("空格"),
             QStringLiteral("图片"), QStringLiteral("pdf"), QStringLiteral("文本"),
             QStringLiteral("看一眼") };
}

bool QuickLookPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence QuickLookPlugin::defaultHotkey() const
{
    return previewHotkey();
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool QuickLookPlugin::initialize()
{
    loadFromConfig();
    if (WinEase::PluginServices *svc = services()) {
        svc->log(id(), WinEase::PluginLogLevel::Info,
                 QStringLiteral("快速文件预览已加载（文本上限 %1，空格键 %2）")
                     .arg(FilePreview::formatBytes(m_maxBytes),
                          m_spaceKey ? QStringLiteral("开") : QStringLiteral("关")));
    }
    return true;
}

void QuickLookPlugin::shutdown()
{
    closePreview();
}

bool QuickLookPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    if (m_spaceKey) {
        WinEase::HookService *hooks = svc->hookService();
        if (hooks == nullptr) {
            // 钩子不可用不影响快捷键入口，如实告知即可
            Q_EMIT statusMessage(QStringLiteral("输入钩子不可用：空格键预览已跳过，"
                                                "仍可用 %1 预览")
                                     .arg(defaultHotkey().toString(QKeySequence::NativeText)));
        } else {
            if (m_listener == nullptr) {
                m_listener = new SpaceKeyListener(this);
            }
            if (!hooks->subscribe(m_listener, WinEase::HookEventKey)) {
                Q_EMIT statusMessage(QStringLiteral("空格键订阅失败（钩子未运行），"
                                                    "仍可用 %1 预览")
                                         .arg(defaultHotkey().toString(QKeySequence::NativeText)));
            } else {
                Q_EMIT statusMessage(QStringLiteral("就绪：在资源管理器里按空格预览剪贴板文件，"
                                                    "或按 %1")
                                         .arg(defaultHotkey().toString(QKeySequence::NativeText)));
            }
        }
    } else {
        Q_EMIT statusMessage(QStringLiteral("就绪：按 %1 预览剪贴板文件")
                                 .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    }
    return true;
}

void QuickLookPlugin::onDisable()
{
    if (m_listener != nullptr) {
        // HookListener 析构会自动退订；但 deleteLater 是排队执行的，
        // 这里先显式退订，避免"停用后又被回调一次"
        if (WinEase::PluginServices *svc = services()) {
            if (WinEase::HookService *hooks = svc->hookService()) {
                hooks->unsubscribe(m_listener);
            }
        }
        m_listener->deleteLater();
        m_listener = nullptr;
    }
    closePreview();
    Q_EMIT statusMessage(QStringLiteral("已停用快速文件预览"));
}

void QuickLookPlugin::onHotkey(const QString &hotkeyId)
{
    Q_UNUSED(hotkeyId) // 本插件只有一个动作
    if (!isEnabled()) {
        return;
    }
    // 再按一次 = 收起（和空格键的手感保持一致）
    if (isPreviewVisible()) {
        closePreview();
        return;
    }
    QString message;
    if (!previewCurrentTarget(&message)) {
        Q_EMIT statusMessage(message);
    }
}

void QuickLookPlugin::toggleFromSpaceKey()
{
    // ⚠ 从钩子线程进入：一律排队到界面线程再碰窗口/剪贴板。
    //    不用自定义事件类型跨模块传（见 ROADMAP 踩坑 #1），lambda 排队即可。
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (isPreviewVisible()) {
                closePreview();
                return;
            }
            QString message;
            if (!previewCurrentTarget(&message)) {
                Q_EMIT statusMessage(message);
            }
        },
        Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
//  取预览对象
// ---------------------------------------------------------------------------

QString QuickLookPlugin::fileFromClipboard(QString *messageOut)
{
    const auto fail = [messageOut](const QString &reason) {
        if (messageOut != nullptr) {
            *messageOut = reason;
        }
        return QString();
    };

    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return fail(QStringLiteral("取不到剪贴板"));
    }

    // ① 剪贴板里直接就是文件（资源管理器里 Ctrl+C 复制的）
    const QMimeData *mime = clipboard->mimeData();
    if (mime != nullptr && mime->hasUrls()) {
        QStringList existing;
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QString path = QDir::toNativeSeparators(url.toLocalFile());
            if (QFileInfo::exists(path)) {
                existing.append(path);
            }
        }
        if (!existing.isEmpty()) {
            if (messageOut != nullptr) {
                *messageOut = existing.size() > 1
                    ? QStringLiteral("剪贴板里有 %1 个文件，预览第 1 个").arg(existing.size())
                    : QStringLiteral("预览：%1").arg(QFileInfo(existing.first()).fileName());
            }
            return existing.first();
        }
    }

    // ② 剪贴板里是一段文本，而这段文本正好是一个存在的路径
    const QString text = clipboard->text().trimmed();
    if (!text.isEmpty()) {
        QString candidate = text;
        if (candidate.size() > 1 && candidate.startsWith(QLatin1Char('"'))
            && candidate.endsWith(QLatin1Char('"'))) {
            candidate = candidate.mid(1, candidate.size() - 2); // 复制的路径常带引号
        }
        candidate = QDir::fromNativeSeparators(candidate.trimmed());
        if (!candidate.contains(QLatin1Char('\n')) && QFileInfo::exists(candidate)) {
            const QString path = QDir::toNativeSeparators(candidate);
            if (messageOut != nullptr) {
                *messageOut = QStringLiteral("预览路径：%1").arg(path);
            }
            return path;
        }
    }

    return fail(QStringLiteral("剪贴板里没有文件。请在资源管理器里选中文件后按 Ctrl+C"
                               "（复制文件），或复制一个文件路径，再按 %1")
                    .arg(previewHotkey().toString(QKeySequence::NativeText)));
}

bool QuickLookPlugin::previewCurrentTarget(QString *messageOut)
{
    QString message;
    const QString path = fileFromClipboard(&message);
    if (path.isEmpty()) {
        if (messageOut != nullptr) {
            *messageOut = message;
        }
        // 窗口照样打开：把"怎么用"讲清楚，比按了没反应强
        if (m_window == nullptr) {
            m_window = new QuickLookWindow(this);
        }
        m_window->showMessage(message);
        m_window->showPreviewWindow();
        return false;
    }
    return previewPath(path, messageOut);
}

bool QuickLookPlugin::previewPath(const QString &path, QString *messageOut)
{
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        const QString message = QStringLiteral("文件不存在：%1").arg(path);
        if (messageOut != nullptr) {
            *messageOut = message;
        }
        Q_EMIT statusMessage(message);
        return false;
    }

    const QString native = QDir::toNativeSeparators(fileInfo.absoluteFilePath());
    if (m_window == nullptr) {
        m_window = new QuickLookWindow(this);
    }
    m_window->showFile(native);
    m_window->showPreviewWindow();
    m_lastPath = native;

    const QString message = QStringLiteral("预览：%1").arg(fileInfo.fileName());
    if (messageOut != nullptr) {
        *messageOut = message;
    }
    Q_EMIT statusMessage(message);
    return true;
}

QString QuickLookPlugin::siblingPath(const QString &path, int offset) const
{
    const QFileInfo fileInfo(path);
    const QDir dir = fileInfo.absoluteDir();
    // 与资源管理器默认排序一致：只列文件、按名称（Windows 下大小写不敏感）
    const QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    if (names.isEmpty()) {
        return QString();
    }
    int index = names.indexOf(fileInfo.fileName());
    if (index < 0) {
        index = 0;
    }
    const int count = names.size();
    const int next = ((index + offset) % count + count) % count; // 循环，到头绕回去
    return QDir::toNativeSeparators(dir.absoluteFilePath(names.at(next)));
}

bool QuickLookPlugin::closePreview()
{
    if (m_window == nullptr) {
        return false;
    }
    m_window->close(); // WA_DeleteOnClose → 自己析构，QPointer 随后归零
    m_window = nullptr;
    return true;
}

bool QuickLookPlugin::isPreviewVisible() const
{
    return m_window != nullptr && m_window->isVisible();
}

QWidget *QuickLookPlugin::previewWindow() const
{
    return m_window.data();
}

// ---------------------------------------------------------------------------
//  配置
// ---------------------------------------------------------------------------

void QuickLookPlugin::loadFromConfig()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    const int maxKb = svc->configValue(id(), QStringLiteral("maxPreviewKb"), 512).toInt();
    m_maxBytes = qBound(4, maxKb, 8192) * 1024LL; // 4 KB ~ 8 MB：再大就不是"看一眼"了
    m_spaceKey = svc->configValue(id(), QStringLiteral("spaceKey"), true).toBool();
}

QWidget *QuickLookPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *box = new QGroupBox(QStringLiteral("预览"), widget);
    auto *form = new QFormLayout(box);

    auto *maxKbSpin = new QSpinBox(box);
    maxKbSpin->setRange(4, 8192);
    maxKbSpin->setSuffix(QStringLiteral(" KB"));
    maxKbSpin->setValue(static_cast<int>(m_maxBytes / 1024));
    maxKbSpin->setToolTip(QStringLiteral("文本文件只读文件头部的这么多内容（默认 512 KB）"));
    form->addRow(QStringLiteral("文本预览上限"), maxKbSpin);

    auto *spaceCheck = new QCheckBox(QStringLiteral("在资源管理器里按空格预览（会吞掉这次空格）"),
                                     box);
    spaceCheck->setChecked(m_spaceKey);
    form->addRow(QString(), spaceCheck);
    layout->addWidget(box);

    auto *previewButton = new QPushButton(QStringLiteral("立即预览剪贴板里的文件"), widget);
    layout->addWidget(previewButton);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *hint = new QLabel(
        QStringLiteral("用法：在资源管理器里选中文件按 Ctrl+C（是「复制文件」，不是复制路径），"
                       "然后在资源管理器里按空格；或者随时按 %1。\n"
                       "窗口里 ← → 换同目录文件，Esc / 空格 关闭，按住顶部信息条可以拖动。\n"
                       "为什么不去抓资源管理器的选中项？那要模拟 Ctrl+C，会覆盖你的剪贴板，"
                       "而文件列表型剪贴板内容无法完整还原 —— 这条红线不碰。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText)),
        widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    QObject::connect(maxKbSpin, qOverload<int>(&QSpinBox::valueChanged), widget, [this](int value) {
        m_maxBytes = static_cast<qint64>(value) * 1024;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("maxPreviewKb"), value);
        }
    });
    QObject::connect(spaceCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_spaceKey = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("spaceKey"), checked);
        }
        // 钩子订阅随开关走：关掉就不再吃空格
        if (WinEase::HookService *hooks = (services() != nullptr) ? services()->hookService()
                                                                 : nullptr) {
            if (checked && m_listener == nullptr && isEnabled()) {
                m_listener = new SpaceKeyListener(this);
                hooks->subscribe(m_listener, WinEase::HookEventKey);
            } else if (!checked && m_listener != nullptr) {
                hooks->unsubscribe(m_listener);
                m_listener->deleteLater();
                m_listener = nullptr;
            }
        }
    });
    QObject::connect(previewButton, &QPushButton::clicked, widget, [this, statusLabel] {
        QString message;
        const bool ok = previewCurrentTarget(&message);
        statusLabel->setText(ok ? QStringLiteral("已打开预览：%1").arg(message) : message);
    });

    return widget;
}
