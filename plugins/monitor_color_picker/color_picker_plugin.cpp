#include "color_picker_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/ScreenCapture.h"
#include "win32/WindowTarget.h"

#include "ClipboardTools.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// ClipboardTools / ColorFormats 是**命名空间**（不是类），
// 需要用命名空间别名而不是 using 声明（using X::Y 只对类/函数有效）
namespace ClipboardTools = WinEase::FeaturePlugins::ClipboardTools;
namespace ColorFormats = WinEase::FeaturePlugins::ColorFormats;

namespace {

constexpr int kMaxHistory = 12;
constexpr int kMagnifierRadius = 7; ///< 放大镜采样半径（像素）
constexpr int kMagnifierSize = 132; ///< 放大镜控件边长（逻辑像素）
constexpr int kMagnifierIntervalMs = 100;

/// 放大镜预览：定时抓取光标周围的小像素块并放大显示（最近邻，不插值）
class MagnifierWidget : public QWidget
{
public:
    explicit MagnifierWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(kMagnifierSize, kMagnifierSize);
        setAutoFillBackground(true);
    }

    void setProbe(const WinEase::Win32::PixelProbe &probe)
    {
        m_probe = probe;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());

        if (!m_probe.valid) {
            painter.setPen(palette().windowText().color());
            painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                             QStringLiteral("把鼠标移到要取色的位置\n（取色按物理像素，缩放不影响）"));
            return;
        }

        // 最近邻放大：放大镜的目的就是看清每个像素，插值会把像素糊掉
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(rect(), m_probe.patch, m_probe.patch.rect());

        // 正中那个像素描一圈，对应"当前鼠标下"的颜色
        const int cell = qMax(1, rect().width() / qMax(1, m_probe.patch.width()));
        const QRect centerCell(rect().center().x() - cell / 2, rect().center().y() - cell / 2, cell, cell);
        painter.setPen(QPen(palette().highlight().color(), 2));
        painter.drawRect(centerCell);
    }

private:
    WinEase::Win32::PixelProbe m_probe;
};

} // namespace

ColorPickerPlugin::ColorPickerPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString ColorPickerPlugin::id() const
{
    return QStringLiteral("monitor.color_picker");
}

QString ColorPickerPlugin::name() const
{
    return QStringLiteral("屏幕取色器");
}

QString ColorPickerPlugin::description() const
{
    return QStringLiteral("取光标处的屏幕颜色，一键复制成 HEX / RGB / HSL / CMYK");
}

QIcon ColorPickerPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::SystemMonitor);
}

WinEase::FeatureCategory ColorPickerPlugin::category() const
{
    return WinEase::FeatureCategory::SystemMonitor;
}

QStringList ColorPickerPlugin::tags() const
{
    return { QStringLiteral("取色"), QStringLiteral("颜色"), QStringLiteral("拾色"),
             QStringLiteral("color"), QStringLiteral("picker"), QStringLiteral("qr") };
}

bool ColorPickerPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence ColorPickerPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+C"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool ColorPickerPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_format = ColorFormats::formatFromKey(
            svc->configValue(id(), QStringLiteral("format"), QStringLiteral("hex")).toString());
        m_includePosition = svc->configValue(id(), QStringLiteral("includePosition"), false).toBool();
        m_history = svc->configValue(id(), QStringLiteral("history")).toStringList();
        while (m_history.size() > kMaxHistory) {
            m_history.removeLast();
        }
    }
    return true;
}

void ColorPickerPlugin::shutdown()
{
    persist();
}

bool ColorPickerPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    // 辅助快捷键：不管默认格式是什么，都想要 HEX（这是最常用的一种）
    if (!svc->registerHotkey(id(), QStringLiteral("hex"),
                             QKeySequence(QStringLiteral("Ctrl+Alt+Shift+C")),
                             QStringLiteral("WinEase：复制光标处颜色的 HEX 值"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("辅助快捷键注册失败（可能被占用），可在快捷键设置里改键"));
    }

    Q_EMIT statusMessage(QStringLiteral("取色格式：%1")
                             .arg(ColorFormats::formatName(m_format)));
    return true;
}

void ColorPickerPlugin::onDisable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("hex"));
    }
    persist();
    Q_EMIT statusMessage(QStringLiteral("已停用取色器"));
}

void ColorPickerPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return; // 停用期间 <id>::default 仍可能被分发到
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("hex")) {
        pickAndCopy(ColorFormats::Format::Hex, QStringLiteral("HEX"));
    } else if (action == QLatin1String("rgb")) {
        pickAndCopy(ColorFormats::Format::Rgb, QStringLiteral("RGB"));
    } else if (action == QLatin1String("hsl")) {
        pickAndCopy(ColorFormats::Format::Hsl, QStringLiteral("HSL"));
    } else if (action == QLatin1String("cmyk")) {
        pickAndCopy(ColorFormats::Format::Cmyk, QStringLiteral("CMYK"));
    } else {
        pickAndCopy(m_format, ColorFormats::formatName(m_format)); // "default"
    }
}

// ---------------------------------------------------------------------------
//  取色
// ---------------------------------------------------------------------------

bool ColorPickerPlugin::pickAndCopy(ColorFormats::Format format, const QString &label)
{
    // 全程物理像素：平台层已经按物理像素取色，这里不做任何 DPI 换算
    const QPoint cursor = WinEase::Win32::cursorPosition();
    bool ok = false;
    const QColor color = WinEase::Win32::colorAt(cursor, &ok);
    if (!ok || !color.isValid()) {
        setLastError(QStringLiteral("无法读取屏幕像素（坐标 %1,%2）").arg(cursor.x()).arg(cursor.y()));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        Q_EMIT statusMessage(QStringLiteral("取色失败：读不到屏幕像素"));
        return false;
    }

    QString text = ColorFormats::format(color, format);
    if (m_includePosition) {
        text += QStringLiteral(" @ %1,%2").arg(cursor.x()).arg(cursor.y());
    }

    const bool copied = ClipboardTools::setText(text);
    rememberColor(ColorFormats::format(color, ColorFormats::Format::Hex));

    if (!copied) {
        setLastError(QStringLiteral("写入剪贴板失败（可能被其它程序占用）"));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        Q_EMIT statusMessage(QStringLiteral("取到 %1，但写入剪贴板失败").arg(text));
        return false;
    }

    Q_EMIT statusMessage(QStringLiteral("已复制 %1 %2").arg(label, text));
    return true;
}

void ColorPickerPlugin::rememberColor(const QString &hex)
{
    if (hex.isEmpty()) {
        return;
    }
    // 去重后置顶：历史里同一个颜色只留一条，且最近用的排最前
    m_history.removeAll(hex);
    m_history.prepend(hex);
    while (m_history.size() > kMaxHistory) {
        m_history.removeLast();
    }
    persist();
}

void ColorPickerPlugin::persist()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("history"), m_history);
        svc->setConfigValue(id(), QStringLiteral("format"), ColorFormats::formatKey(m_format));
        svc->setConfigValue(id(), QStringLiteral("includePosition"), m_includePosition);
    }
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *ColorPickerPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *previewGroup = new QGroupBox(QStringLiteral("实时预览"), widget);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    auto *magnifier = new MagnifierWidget(previewGroup);
    previewLayout->addWidget(magnifier, 0, Qt::AlignHCenter);
    auto *previewText = new QLabel(previewGroup);
    previewText->setWordWrap(true);
    previewLayout->addWidget(previewText);
    layout->addWidget(previewGroup);

    auto *formatRow = new QHBoxLayout();
    formatRow->addWidget(new QLabel(QStringLiteral("默认复制格式："), widget));
    auto *formatBox = new QComboBox(widget);
    for (const ColorFormats::Format format : ColorFormats::allFormats()) {
        formatBox->addItem(ColorFormats::formatName(format), static_cast<int>(format));
    }
    formatBox->setCurrentIndex(formatBox->findData(static_cast<int>(m_format)));
    formatRow->addWidget(formatBox);
    formatRow->addStretch(1);
    layout->addLayout(formatRow);

    auto *positionBox = new QCheckBox(QStringLiteral("复制时附带坐标（@ x,y）"), widget);
    positionBox->setChecked(m_includePosition);
    layout->addWidget(positionBox);

    layout->addWidget(new QLabel(QStringLiteral("取色历史（点一下就复制）："), widget));
    auto *historyList = new QListWidget(widget);
    historyList->setMaximumHeight(150);
    layout->addWidget(historyList);

    auto *pickButton = new QPushButton(QStringLiteral("立即取光标处的颜色"), widget);
    layout->addWidget(pickButton, 0, Qt::AlignLeft);

    auto *hint = new QLabel(QStringLiteral("快捷键：%1 取色 · Ctrl+Alt+Shift+C 固定复制 HEX\n"
                                          "取色按**物理像素**，与系统缩放无关。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    // ---- 放大镜刷新 ----
    auto *timer = new QTimer(widget);
    QObject::connect(timer, &QTimer::timeout, widget, [magnifier, previewText] {
        const QPoint cursor = WinEase::Win32::cursorPosition();
        const WinEase::Win32::PixelProbe probe = WinEase::Win32::probePixel(cursor, kMagnifierRadius);
        magnifier->setProbe(probe);
        if (probe.valid) {
            previewText->setText(QStringLiteral("%1\n坐标 %2,%3")
                                     .arg(ColorFormats::describe(probe.color))
                                     .arg(cursor.x())
                                     .arg(cursor.y()));
        } else {
            previewText->setText(QStringLiteral("读不到屏幕像素：%1").arg(probe.error));
        }
    });
    timer->start(kMagnifierIntervalMs);

    const auto refreshHistory = [this, historyList] {
        historyList->clear();
        for (const QString &hex : m_history) {
            const QColor color = ColorFormats::parse(hex);
            auto *item = new QListWidgetItem(QStringLiteral("%1    %2").arg(hex, ColorFormats::describe(color)));
            item->setData(Qt::UserRole, hex);
            item->setBackground(color);
            // 亮色背景配黑字，深色背景配白字，否则整行会被主题吃掉看不见
            item->setForeground(color.lightness() > 128 ? QColor(Qt::black) : QColor(Qt::white));
            historyList->addItem(item);
        }
        if (m_history.isEmpty()) {
            historyList->addItem(QStringLiteral("（还没有取过色）"));
        }
    };
    refreshHistory();

    QObject::connect(formatBox, &QComboBox::currentIndexChanged, widget, [this, formatBox](int) {
        m_format = static_cast<ColorFormats::Format>(formatBox->currentData().toInt());
        persist();
        Q_EMIT statusMessage(QStringLiteral("取色格式：%1").arg(ColorFormats::formatName(m_format)));
    });
    QObject::connect(positionBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_includePosition = checked;
        persist();
    });
    QObject::connect(pickButton, &QPushButton::clicked, widget, [this] {
        pickAndCopy(m_format, ColorFormats::formatName(m_format));
    });
    QObject::connect(historyList, &QListWidget::itemClicked, widget,
                     [historyList](QListWidgetItem *item) {
                         const QString hex = item->data(Qt::UserRole).toString();
                         if (!hex.isEmpty()) {
                             ClipboardTools::setText(hex);
                         }
                     });

    return widget;
}
