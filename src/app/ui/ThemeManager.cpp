#include "ui/ThemeManager.h"

#include "win32/WindowUtils.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTextStream>
#include <QWidget>

namespace WinEase::Ui {

namespace {

/// 让所有带标题栏的顶层窗口使用深色标题栏。
///
/// 标题栏由 DWM 绘制，**样式表与调色板都影响不到**；不处理的话，
/// 系统处于浅色模式时就会出现"纯黑界面 + 一条浅色标题栏"。
/// 这里用应用级事件过滤器覆盖所有顶层窗口（含后续弹出的对话框、消息框），
/// 避免"每新建一个窗口都要记得补一次调用"这种迟早会漏的做法。
class DarkTitleBarFilter : public QObject
{
public:
    explicit DarkTitleBarFilter(QObject *parent)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::WinIdChange: {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget != nullptr && widget->isWindow()) {
                applyTo(widget);
            }
            break;
        }
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    static void applyTo(QWidget *widget)
    {
        // 只处理真正有标题栏的窗口：弹出菜单与工具提示没有标题栏，无需设置
        const Qt::WindowType type = widget->windowType();
        if (type == Qt::Popup || type == Qt::ToolTip
            || widget->windowFlags().testFlag(Qt::FramelessWindowHint)) {
            return;
        }
        Win32::setDarkTitleBar(reinterpret_cast<Win32::WindowHandle>(widget->winId()), true);
    }
};

} // namespace

// ============================================================================

QPalette forcedDarkPalette()
{
    QPalette palette;

    // ---------------- 基础：黑底白字 ----------------
    palette.setColor(QPalette::Window, QColor(ThemeColor::kBase));
    palette.setColor(QPalette::WindowText, QColor(ThemeColor::kText));
    palette.setColor(QPalette::Base, QColor(0x0A, 0x0A, 0x0A)); // 输入框 / 列表底，比纯黑略亮一档
    palette.setColor(QPalette::AlternateBase, QColor(0x12, 0x12, 0x12));
    palette.setColor(QPalette::Text, QColor(ThemeColor::kText));
    palette.setColor(QPalette::Button, QColor(0x16, 0x16, 0x16));
    palette.setColor(QPalette::ButtonText, QColor(ThemeColor::kText));
    palette.setColor(QPalette::BrightText, QColor(ThemeColor::kError));
    palette.setColor(QPalette::Link, QColor(ThemeColor::kAccent));
    palette.setColor(QPalette::LinkVisited, QColor(ThemeColor::kAccentHover));
    palette.setColor(QPalette::Highlight, QColor(ThemeColor::kSelection));
    palette.setColor(QPalette::HighlightedText, QColor(ThemeColor::kText));
    palette.setColor(QPalette::PlaceholderText, QColor(ThemeColor::kTextTertiary));

    // ---------------- 浮层 ----------------
    palette.setColor(QPalette::ToolTipBase, QColor(ThemeColor::kSurfaceHover));
    palette.setColor(QPalette::ToolTipText, QColor(ThemeColor::kText));

    // ---------------- 3D 边框系 ----------------
    // 必须显式设置：默认值是浅色主题的（接近白色），
    // 不设置的话 Fusion 画出的凹凸边框会是刺眼的浅灰。
    palette.setColor(QPalette::Light, QColor(ThemeColor::kBorderStrong));
    palette.setColor(QPalette::Midlight, QColor(0x2E, 0x2E, 0x2E));
    palette.setColor(QPalette::Mid, QColor(0x28, 0x28, 0x28));
    palette.setColor(QPalette::Dark, QColor(0x1C, 0x1C, 0x1C));
    palette.setColor(QPalette::Shadow, QColor(ThemeColor::kBase));

    // ---------------- 禁用态 ----------------
    // 不显式设置的话，禁用项会继承上面的高对比纯白，看起来仍像"可用"
    const QColor disabledText(ThemeColor::kTextDisabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor(0x0A, 0x0A, 0x0A));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor(0x12, 0x12, 0x12));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x1C, 0x1C, 0x1C));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Light, QColor(0x24, 0x24, 0x24));
    palette.setColor(QPalette::Disabled, QPalette::Mid, QColor(0x1E, 0x1E, 0x1E));

    return palette;
}

QString themeStyleSheetPath()
{
    return QStringLiteral(":/winease/style.qss");
}

void applyForcedDarkTheme(QApplication &app)
{
    // ① 固定控件样式为 Fusion。
    //    Windows 11 下 Qt 默认是 windows11 样式，它会跟随系统"应用模式"偏好；
    //    Fusion 完全由调色板驱动，不受系统主题影响。
    //    注意：必须在创建任何窗口之前调用，否则已有窗口不会重绘为新样式。
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }

    // ② 显式声明深色方案：让平台插件（原生对话框、滚动条等）也走深色
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    // ③ 深色调色板兜底
    app.setPalette(forcedDarkPalette());

    // ④ 全局样式表：所有配色均为字面量，不引用 palette(...) 等随系统变化的动态值
    QFile styleFile(themeStyleSheetPath());
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&styleFile);
        app.setStyleSheet(stream.readAll());
    }

    // ⑤ 深色标题栏（DWM 绘制，样式表管不到）
    app.installEventFilter(new DarkTitleBarFilter(&app));

    // 自检用的执行标记：证明本函数已完整跑到最后（含标题栏接管），不参与任何业务逻辑
    app.setProperty("winease.forcedDarkTheme", true);
}

} // namespace WinEase::Ui
