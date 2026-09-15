// ============================================================================
//  theme_smoke —— 强制深色主题（黑底白字）验收测试
//
//  用户要求（逐条对应，每条都要有可执行的证据）：
//
//    1. "全局样式表修改为强制性的黑底白字配色"
//       → 校验底色为纯黑 #000000、主文字为纯白 #FFFFFF（调色板 + 样式表 + 实际渲染像素）。
//
//    2. "确保该配色方案固定不变，不随操作系统的浅色或深色模式偏好自动切换"
//       → 这条是本次的重点，分四层验证：
//          a) 静态层：样式表里**不允许出现任何动态颜色源**
//             （palette(...)、color-scheme、@media），且自动扫描**所有** background
//             声明，逐一确认都是深底色 —— 从源头排除"某个控件偷偷跟随系统"。
//          b) 样式层：控件样式必须是 Fusion，而不是会跟随系统"应用模式"的 windows11 样式。
//             （只改样式表和调色板而不动样式，是最容易漏的一道。）
//          c) 运行时层：向本进程所有顶层窗口投递系统切换浅色/深色时真正会发的那几条消息
//             （WM_SETTINGCHANGE "ImmersiveColorSet" / WM_THEMECHANGED / WM_SYSCOLORCHANGE），
//             之后再断言样式、色彩方案、调色板、**实际渲染像素**全部没有变化。
//          d) 启动层（★ 最关键）：把系统临时切成浅色，然后**另起一个子进程**从零启动，
//             复现"系统是浅色时打开 WinEase"。之所以用子进程：QWindowsTheme 只在启动时
//             读一次色彩方案，本进程内改注册表是"摆环境"而不是"变成浅色系统"。
//             加 --with-os-light-mode 启用；没加参数时这一层会跳过并明确标注。
//
//    3. 附带验证"用户看得见的地方"确实变黑了：实际渲染取像素（窗口底色、按钮底色、
//       开关强调色）、白字确实被绘制、DWM 标题栏深色属性被系统接受。
//
//  ⚠ 测试期间会短暂显示窗口（父进程 360x260、子进程 240x160）用于真实渲染取像素。
//     窗口都设置了"显示但不激活"，不会抢走当前焦点；取完像素立即关闭。
//  ⚠ --with-os-light-mode 会临时改写 HKCU 下的 AppsUseLightTheme（只这一个值，
//     不碰任务栏），并在结束前恢复原值 + 回读校验；失败会打印手动恢复指引。
// ============================================================================

#include "ui/ThemeManager.h"
#include "win32/WindowUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include <windows.h>

namespace {

namespace ThemeColor = WinEase::Ui::ThemeColor;

// ---------------------------------------------------------------------------
//  输出
// ---------------------------------------------------------------------------

constexpr const char *kReset = "\x1b[0m";
constexpr const char *kGreen = "\x1b[32m";
constexpr const char *kRed = "\x1b[31m";
constexpr const char *kGray = "\x1b[90m";
constexpr const char *kCyan = "\x1b[36m";

void printLine(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
    std::fputc('\n', stdout);
}

class Reporter
{
public:
    void section(const QString &title)
    {
        printLine(QStringLiteral("\n%1== %2 ==%3")
                      .arg(QString::fromUtf8(kCyan), title, QString::fromUtf8(kReset)));
    }

    void check(bool ok, const QString &name, const QString &detail = QString())
    {
        if (ok) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3")
                          .arg(QString::fromUtf8(kGreen), QString::fromUtf8(kReset), name));
        } else {
            ++m_failed;
            printLine(QStringLiteral("  %1[失败]%2 %3")
                          .arg(QString::fromUtf8(kRed), QString::fromUtf8(kReset), name));
        }
        if (!detail.isEmpty()) {
            printLine(QStringLiteral("         %1%2%3")
                          .arg(QString::fromUtf8(kGray), detail, QString::fromUtf8(kReset)));
        }
    }

    void note(const QString &text)
    {
        printLine(QStringLiteral("  %1· %2%3")
                      .arg(QString::fromUtf8(kGray), text, QString::fromUtf8(kReset)));
    }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    int m_passed = 0;
    int m_failed = 0;
};

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QString hex(const QColor &color)
{
    return color.name(QColor::HexRgb).toUpper();
}

/// 感知亮度：0.0 全黑 ~ 1.0 全白
qreal brightness(const QColor &color)
{
    return (0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue()) / 255.0;
}

/// 深色主题判定阈值：感知亮度低于此值才算"深底"
constexpr qreal kDarkBackgroundMaxBrightness = 0.60;

bool isDarkBackground(const QColor &color)
{
    return brightness(color) < kDarkBackgroundMaxBrightness;
}

bool nearlyEqual(const QColor &lhs, const QColor &rhs, int tolerance = 4)
{
    return std::abs(lhs.red() - rhs.red()) <= tolerance
        && std::abs(lhs.green() - rhs.green()) <= tolerance
        && std::abs(lhs.blue() - rhs.blue()) <= tolerance;
}

/// 去掉 /* ... */ 注释。
/// 必须先去注释再做文本检查：色板的说明注释里本身就写了 "#000000" 与 "palette(...)"，
/// 不剥离的话检查会被自己的文档内容"喂"成通过 —— 那是最典型的假测试。
QString stripComments(const QString &text)
{
    static const QRegularExpression comment(QStringLiteral("/\\*.*?\\*/"),
                                            QRegularExpression::DotMatchesEverythingOption);
    QString result = text;
    result.remove(comment);
    return result;
}

// ---------------------------------------------------------------------------
//  样式表解析
// ---------------------------------------------------------------------------

struct QssRule
{
    QString selector;
    QString property;
    QString value;
};

/// 极简 QSS 解析：本项目样式表不含 @media / @规则 / 嵌套块，
/// 因此"选择器 { 属性: 值; ... }"的平铺解析已经足够，
/// 不需要为此引入一个完整 CSS 解析器。
QVector<QssRule> parseRules(const QString &qssWithoutComments)
{
    QVector<QssRule> rules;

    int pos = 0;
    while (true) {
        const int open = qssWithoutComments.indexOf(QLatin1Char('{'), pos);
        if (open < 0) {
            break;
        }
        const int close = qssWithoutComments.indexOf(QLatin1Char('}'), open);
        if (close < 0) {
            break;
        }

        const QString selector = qssWithoutComments.mid(pos, open - pos).trimmed();
        const QString body = qssWithoutComments.mid(open + 1, close - open - 1);

        const QStringList declarations = body.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString &declaration : declarations) {
            const int colon = declaration.indexOf(QLatin1Char(':'));
            if (colon < 0) {
                continue;
            }
            QssRule rule;
            rule.selector = selector;
            rule.property = declaration.left(colon).trimmed().toLower();
            rule.value = declaration.mid(colon + 1).trimmed();
            rules.append(rule);
        }

        pos = close + 1;
    }

    return rules;
}

/// 读取系统当前的主题偏好（1 = 浅色，0 = 深色，读不到则为 -1）
struct SystemThemePreference
{
    int appsUseLight = -1;   ///< 应用模式：决定"应用/窗口"是浅色还是深色
    int systemUseLight = -1; ///< 系统模式：决定任务栏/开始菜单
};

/// 主题偏好所在的注册表键（其中 AppsUseLightTheme 决定"应用窗口"是浅色还是深色）
QString personalizeKey()
{
    return QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
}

SystemThemePreference readSystemThemePreference()
{
    QSettings personalize(personalizeKey(), QSettings::NativeFormat);

    SystemThemePreference preference;
    preference.appsUseLight = personalize.value(QStringLiteral("AppsUseLightTheme"), -1).toInt();
    preference.systemUseLight = personalize.value(QStringLiteral("SystemUsesLightTheme"), -1).toInt();
    return preference;
}

QString describePreference(int value)
{
    if (value == 1) {
        return QStringLiteral("浅色");
    }
    if (value == 0) {
        return QStringLiteral("深色");
    }
    return QStringLiteral("未知");
}

/// 模拟"用户在系统设置里切换了浅色/深色"时 Windows 真正发给各窗口的消息。
/// 只发给自己进程的顶层窗口：既走通了平台插件的真实处理路径，
/// 又不会阻塞在别的进程上（HWND_BROADCAST + SendMessage 可能被别的程序拖住）。
void sendSystemThemeChangeMessages()
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    int sent = 0;
    for (QWidget *widget : tops) {
        if (widget == nullptr || !widget->isWindow()) {
            continue;
        }
        const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
        if (hwnd == nullptr) {
            continue;
        }
        ::SendMessageW(hwnd, WM_SETTINGCHANGE, 0,
                       reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
        ::SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
        ::SendMessageW(hwnd, WM_SYSCOLORCHANGE, 0, 0);
        ++sent;
    }
    printLine(QStringLiteral("  %1· 已向 %2 个顶层窗口投递系统主题变更消息%3")
                  .arg(QString::fromUtf8(kGray))
                  .arg(sent)
                  .arg(QString::fromUtf8(kReset)));
}

/// 取一个 QStyle 实例的类名（nullptr 安全）
QString classNameOf(QStyle *style)
{
    return style != nullptr ? QString::fromLatin1(style->metaObject()->className())
                            : QStringLiteral("<null>");
}

/// Qt 在设置了全局样式表后，包在真实样式外面的代理类名
constexpr const char *kStyleSheetProxyClassName = "QStyleSheetStyle";

/// QApplication::style() 的类名（**外层**，通常是上面的样式表代理）
QString rawStyleClassName()
{
    return classNameOf(QApplication::style());
}

/// **底层**真实样式的类名（本次验收要判定的就是它）。
///
/// 三个坑，前两个都会让断言退化成"自己等于自己"的假通过：
///   1. 不能用 objectName() 判断。QStyleFactory::create("Fusion") 手工创建的样式实例
///      objectName 是**空字符串**（只有平台样式与样式插件才会设置它），
///      于是"切换前 == 切换后"会退化成"空串 == 空串"，永远通过。
///   2. 不能用 QApplication::style() 的类名判断。设置全局样式表后，
///      它返回的是包装用的 QStyleSheetStyle，而不是 Fusion —— 那样同样永远"没变"。
///   3. 也**不能**指望用 QProxyStyle::baseStyle() 剥开代理。实测本机 Qt 6.8.4 的代理是
///      `class QStyleSheetStyle : public QWindowsStyle`（见 QtWidgets/private/
///      qstylesheetstyle_p.h 第 39 行），它根本不是 QProxyStyle，
///      qobject_cast 与 dynamic_cast 都拿不到底层样式。
///
/// 所以只能走唯一一条公开路径：**把应用样式表临时清空**。
/// Qt 收到空样式表时会拆掉代理，并把底层样式重新装回 QApplication::style()
/// （qapplication.cpp 中即 `QApplicationPrivate::setStyle(proxy->base); proxy->deref();`），
/// 此时读到的类名就是 ThemeManager 真正装进去的那个样式。读完立刻原样装回。
QString styleClassName()
{
    QStyle *const installed = QApplication::style();
    const QString raw = classNameOf(installed);
    if (raw != QLatin1String(kStyleSheetProxyClassName)) {
        // 没有样式表代理：这个对象本身就是决定外观的真实样式
        return raw;
    }

    const QString savedSheet = qApp->styleSheet();
    qApp->setStyleSheet(QString());   // 拆掉代理，底层样式回到 QApplication::style()
    const QString base = classNameOf(QApplication::style());
    qApp->setStyleSheet(savedSheet);  // 原样装回（探测本身不能留下副作用）
    return base;
}

/// 临时改写系统主题偏好，析构时自动恢复。
///
/// 为什么需要它：本机系统当前处于**深色**模式，而用户的要求恰恰是
/// "系统处于浅色时也不能变成浅色"。只验证"深色模式下 + 抗切换消息"
/// 并不足以证明这一点 —— 必须真的把系统切到浅色，再看应用是否依然黑底白字。
///
/// 风险与兜底：
///   · 只改 HKCU 下 AppsUseLightTheme 这一个值（不碰 SystemUsesLightTheme，不动任务栏），
///     且**只在显式加 --with-os-light-mode 参数时**才启用；
///   · 析构时恢复原值并回读校验，校验失败会打印手动恢复方法；
///   · 副作用：期间系统会短暂切到浅色（几百毫秒），恢复后原样。
class OsThemeOverride
{
public:
    OsThemeOverride() = default;
    ~OsThemeOverride() { restore(); }

    /// 读取并记住原始值（读不到就不启用，避免把系统改坏）
    void capture()
    {
        QSettings personalize(personalizeKey(), QSettings::NativeFormat);
        m_original = personalize.value(QStringLiteral("AppsUseLightTheme"), -1).toInt();
        m_active = m_original >= 0;
    }

    bool usable() const { return m_active; }
    int original() const { return m_original; }

    bool switchAppsToLight() { return write(1); }
    bool switchAppsToDark() { return write(0); }

    /// 恢复原值并回读校验
    bool restore()
    {
        if (!m_active) {
            return true;
        }
        if (!write(m_original)) {
            return false;
        }
        m_active = false;

        QSettings personalize(personalizeKey(), QSettings::NativeFormat);
        return personalize.value(QStringLiteral("AppsUseLightTheme"), -1).toInt() == m_original;
    }

private:
    bool write(int value)
    {
        QSettings personalize(personalizeKey(), QSettings::NativeFormat);
        personalize.setValue(QStringLiteral("AppsUseLightTheme"), value);
        personalize.sync();
        if (personalize.status() != QSettings::NoError) {
            return false;
        }

        // 只写注册表不会立刻生效，必须广播这条消息让系统与各程序重新读取偏好
        DWORD_PTR result = 0;
        ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                              reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                              SMTO_ABORTIFHUNG, 3000, &result);
        m_active = true;
        return true;
    }

    int m_original = -1;
    bool m_active = false;
};

// ---------------------------------------------------------------------------
//  像素统计
// ---------------------------------------------------------------------------

/// 在指定区域内统计满足条件的像素数
int countPixels(const QImage &image,
                const QRect &region,
                const std::function<bool(const QColor &)> &predicate)
{
    int count = 0;
    const QRect bounded = region.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (predicate(color)) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

// ============================================================================

/// 子进程模式（`--child-light-probe`）：在**系统处于浅色**的环境里从零启动一次应用，
/// 验证它依然是黑底白字。
///
/// 为什么非要另起一个进程，而不能在本进程里"模拟"：
/// QWindowsTheme 在**启动时**就把色彩方案读进缓存了，运行期改写注册表 + 广播
/// WM_SETTINGCHANGE 并不保证它会重新读取（实测本机 Qt 6.8.4 就不重读，
/// 那条"模拟生效"的断言会直接失败）。而用户真正会遇到、也最该验证的场景恰恰是
/// "系统是浅色，然后打开 WinEase" —— 那正是子进程从零启动的情形。
///
/// 退出码：0 = 全部通过；2 = 前置条件不成立（没能构造出浅色环境）；1 = 存在失败项。
int runSystemLightChildProbe()
{
    Reporter reporter;
    printLine(QStringLiteral("  ── 子进程：系统浅色下从零启动 ──"));

    const SystemThemePreference preference = readSystemThemePreference();
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    const bool systemIsLight = preference.appsUseLight == 1 && scheme == Qt::ColorScheme::Light;
    reporter.check(systemIsLight,
                   QStringLiteral("子进程启动时，系统确实处于浅色模式（前置条件）"),
                   QStringLiteral("注册表 AppsUseLightTheme = %1（%2），"
                                  "平台色彩方案 = %3")
                       .arg(preference.appsUseLight)
                       .arg(describePreference(preference.appsUseLight))
                       .arg(scheme == Qt::ColorScheme::Light
                                ? QStringLiteral("Light")
                                : (scheme == Qt::ColorScheme::Dark ? QStringLiteral("Dark")
                                                                  : QStringLiteral("Unknown"))));
    if (!systemIsLight) {
        return 2; // 前置条件不成立：本次结论无效，不能当成"通过"
    }

    const QString styleBefore = styleClassName();
    reporter.note(QStringLiteral("浅色系统下，应用主题前的控件样式 = %1").arg(styleBefore));

    WinEase::Ui::applyForcedDarkTheme(*qApp);
    pump(50);

    const QString styleAfter = styleClassName();
    reporter.check(styleAfter == QStringLiteral("QFusionStyle"),
                   QStringLiteral("系统浅色下启动：底层样式仍被固定为 Fusion"),
                   QStringLiteral("%1 → %2（外层 %3）")
                       .arg(styleBefore, styleAfter, rawStyleClassName()));

    const QColor baseBlack(ThemeColor::kBase);
    const QColor pureWhite(ThemeColor::kText);
    reporter.check(qApp->palette().color(QPalette::Window) == baseBlack
                       && qApp->palette().color(QPalette::WindowText) == pureWhite,
                   QStringLiteral("系统浅色下启动：调色板仍是黑底白字"),
                   QStringLiteral("Window = %1，WindowText = %2")
                       .arg(hex(qApp->palette().color(QPalette::Window)),
                            hex(qApp->palette().color(QPalette::WindowText))));

    QWidget window;
    window.setAttribute(Qt::WA_ShowWithoutActivating, true); // 不抢焦点
    window.resize(240, 160);
    auto *layout = new QVBoxLayout(&window);
    auto *label = new QLabel(QStringLiteral("黑底白字"), &window);
    label->setObjectName(QStringLiteral("HeaderTitle"));
    layout->addWidget(label);
    window.show();
    pump(250);

    const QImage frame = window.grab().toImage();
    reporter.check(nearlyEqual(frame.pixelColor(4, 4), baseBlack),
                   QStringLiteral("系统浅色下启动：窗口底色实际渲染仍是纯黑"),
                   QStringLiteral("(4,4) = %1").arg(hex(frame.pixelColor(4, 4))));

    const int whitePixels = countPixels(frame, frame.rect(), [](const QColor &c) {
        return c.alpha() > 0 && brightness(c) > 0.85;
    });
    reporter.check(whitePixels >= 20,
                   QStringLiteral("系统浅色下启动：白字依然被绘制"),
                   QStringLiteral("检出 %1 个高亮像素").arg(whitePixels));

    window.hide();
    pump(60);

    printLine(QStringLiteral("  %1子进程小计：通过 %2 / %3%4")
                  .arg(QString::fromUtf8(kGray))
                  .arg(reporter.passed())
                  .arg(reporter.passed() + reporter.failed())
                  .arg(QString::fromUtf8(kReset)));

    return reporter.failed() == 0 ? 0 : 1;
}

// ============================================================================

int main(int argc, char *argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Reporter reporter;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("theme_smoke"));

    // 子进程模式：只做"系统浅色下从零启动"这一件事，由父进程拉起（见上方函数注释）
    if (QCoreApplication::arguments().contains(QStringLiteral("--child-light-probe"))) {
        return runSystemLightChildProbe();
    }

    // 可选参数：把系统应用模式临时切到浅色，用来复现"系统浅色下启动 WinEase"这一
    // 最关键、也最难验证的场景（测试结束会自动恢复）
    const bool simulateLightMode =
        QCoreApplication::arguments().contains(QStringLiteral("--with-os-light-mode"));

    printLine(QStringLiteral("\n──────────── 强制深色主题验收（黑底白字 · 固定不变）────────────"));

    // ========================================================================
    reporter.section(QStringLiteral("〇、环境基线（报告用，不参与判定）"));
    // ========================================================================

    const SystemThemePreference preference = readSystemThemePreference();
    reporter.note(QStringLiteral("注册表 AppsUseLightTheme   = %1（%2）")
                      .arg(preference.appsUseLight)
                      .arg(describePreference(preference.appsUseLight)));
    reporter.note(QStringLiteral("注册表 SystemUsesLightTheme = %1（%2）")
                      .arg(preference.systemUseLight)
                      .arg(describePreference(preference.systemUseLight)));
    reporter.note(QStringLiteral("应用主题前，平台报告的色彩方案 = %1")
                      .arg(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                               ? QStringLiteral("Dark")
                               : (QGuiApplication::styleHints()->colorScheme()
                                          == Qt::ColorScheme::Light
                                      ? QStringLiteral("Light")
                                      : QStringLiteral("Unknown"))));
    const QString styleClassBefore = styleClassName();
    reporter.note(QStringLiteral("应用主题前，控件样式 = %1").arg(styleClassBefore));
    if (preference.appsUseLight == 1) {
        reporter.note(QStringLiteral("当前系统处于**浅色模式** —— "
                                     "这正是最关键的验证场景：应用必须依然是黑底白字"));
    } else if (preference.appsUseLight == 0) {
        reporter.note(QStringLiteral("当前系统处于深色模式"));
    }

    // ---- 可选：把系统临时切到浅色 ----
    // ⚠ 注意：这只是把环境"摆成"浅色，本进程并不会因此变成浅色 ——
    //    QWindowsTheme 在启动时就把色彩方案读进缓存了，运行期改注册表 + 广播
    //    WM_SETTINGCHANGE 它并不重新读取（实测本机 Qt 6.8.4 如此）。
    //    所以"系统浅色下启动 WinEase"这个场景不靠本进程模拟，而是由第七节
    //    **另起一个子进程**从零启动来验证 —— 那也正是用户真正的使用方式。
    OsThemeOverride osThemeOverride;
    if (simulateLightMode) {
        osThemeOverride.capture();
        if (osThemeOverride.usable() && osThemeOverride.switchAppsToLight()) {
            reporter.note(QStringLiteral("已把系统应用模式临时切到**浅色**"
                                         "（原值 %1）→ 第七节会在该环境里重跑一次启动")
                              .arg(osThemeOverride.original()));
            pump(1200);
        } else {
            reporter.note(QStringLiteral("无法临时切换系统主题，本次只做深色模式验证"));
        }
    } else {
        reporter.note(QStringLiteral("未加 --with-os-light-mode："
                                     "本次只验证深色模式；用该参数可覆盖浅色场景"));
    }

    // ========================================================================
    reporter.section(QStringLiteral("一、应用强制深色主题（真实入口）"));
    // ========================================================================

    // 调用主程序真正使用的那一个函数，而不是在测试里另抄一份实现
    WinEase::Ui::applyForcedDarkTheme(app);
    pump(50);

    // styleClassName() 内部会临时清空样式表来探测底层样式，这里只取一次，
    // 断言与详情都复用这个结果（否则每调用一次就会多一轮"拆代理 + 装回"）
    const QString styleClassAfter = styleClassName();

    reporter.check(styleClassAfter == QStringLiteral("QFusionStyle"),
                   QStringLiteral("底层控件样式已固定为 Fusion（不随系统主题变化）"),
                   QStringLiteral("实际底层 = %1（外层包裹 = %2），应用主题前 = %3；"
                                  "若底层仍是 QWindows11Style，系统切浅色时就会漏出浅色")
                       .arg(styleClassAfter, rawStyleClassName(), styleClassBefore));

    // 上一条断言靠"临时清空样式表"探测底层样式，这里反过来确认探针把样式表原样装回了。
    // 若装回失败，后面所有基于样式表的断言（尤其是第五节取像素）都会在错误的样式上做，
    // 那种情况下即使全部"通过"也没有意义。
    reporter.check(rawStyleClassName() == QLatin1String(kStyleSheetProxyClassName)
                       && !app.styleSheet().isEmpty(),
                   QStringLiteral("探测底层样式的探针已把样式表原样装回（无副作用）"),
                   QStringLiteral("外层 = %1，样式表 %2 字节")
                       .arg(rawStyleClassName())
                       .arg(app.styleSheet().size()));

    reporter.check(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark,
                   QStringLiteral("已显式声明色彩方案为 Dark"));

    reporter.check(app.property("winease.forcedDarkTheme").toBool(),
                   QStringLiteral("主题入口完整执行（含标题栏接管）"));

    // ========================================================================
    reporter.section(QStringLiteral("二、调色板：黑底白字"));
    // ========================================================================

    const QPalette palette = app.palette();
    const QColor baseBlack(ThemeColor::kBase);
    const QColor pureWhite(ThemeColor::kText);

    reporter.check(palette.color(QPalette::Window) == baseBlack,
                   QStringLiteral("Window（窗口底色）= 纯黑"),
                   QStringLiteral("实际 = %1").arg(hex(palette.color(QPalette::Window))));
    reporter.check(palette.color(QPalette::WindowText) == pureWhite,
                   QStringLiteral("WindowText（窗口文字）= 纯白"),
                   QStringLiteral("实际 = %1").arg(hex(palette.color(QPalette::WindowText))));
    reporter.check(palette.color(QPalette::Text) == pureWhite,
                   QStringLiteral("Text（输入类文字）= 纯白"));
    reporter.check(isDarkBackground(palette.color(QPalette::Base)),
                   QStringLiteral("Base（输入框/列表底）= 深色"),
                   QStringLiteral("实际 = %1，亮度 %2")
                       .arg(hex(palette.color(QPalette::Base)))
                       .arg(brightness(palette.color(QPalette::Base)), 0, 'f', 2));
    reporter.check(isDarkBackground(palette.color(QPalette::Button))
                       && palette.color(QPalette::ButtonText) == pureWhite,
                   QStringLiteral("Button 深色底 + 纯白字"));

    reporter.check(isDarkBackground(palette.color(QPalette::Highlight))
                       && palette.color(QPalette::HighlightedText) == pureWhite,
                   QStringLiteral("选中态（Highlight）= 深色底 + 纯白字（可读）"),
                   QStringLiteral("Highlight = %1")
                       .arg(hex(palette.color(QPalette::Highlight))));

    reporter.check(isDarkBackground(palette.color(QPalette::ToolTipBase))
                       && palette.color(QPalette::ToolTipText) == pureWhite,
                   QStringLiteral("工具提示 = 深色底 + 纯白字"));

    // 3D 边框系：默认值是浅色主题的（接近白色），漏设的话 Fusion 画出的
    // 凹凸边框会是刺眼的浅灰 —— 这是深色主题最常见的"浅色残留"来源
    const QVector<QPair<QString, QPalette::ColorRole>> threeDeeRoles{
        {QStringLiteral("Light"), QPalette::Light},
        {QStringLiteral("Midlight"), QPalette::Midlight},
        {QStringLiteral("Mid"), QPalette::Mid},
        {QStringLiteral("Dark"), QPalette::Dark},
        {QStringLiteral("Shadow"), QPalette::Shadow},
    };
    int lightLeaks = 0;
    QStringList leakDetail;
    for (const auto &entry : threeDeeRoles) {
        const QColor color = palette.color(entry.second);
        if (!isDarkBackground(color)) {
            ++lightLeaks;
            leakDetail.append(QStringLiteral("%1=%2").arg(entry.first, hex(color)));
        }
    }
    reporter.check(lightLeaks == 0,
                   QStringLiteral("3D 边框系 5 个颜色全部为深色（无浅色残留）"),
                   lightLeaks == 0 ? QStringLiteral("Light/Midlight/Mid/Dark/Shadow 均已设为深色")
                                   : QStringLiteral("漏设为浅色：%1").arg(leakDetail.join(u',')));

    reporter.check(palette.color(QPalette::Disabled, QPalette::ButtonText) == QColor(0x56, 0x56, 0x56),
                   QStringLiteral("禁用态文字为低对比灰（不与可用态混淆）"),
                   QStringLiteral("实际 = %1")
                       .arg(hex(palette.color(QPalette::Disabled, QPalette::ButtonText))));

    // ========================================================================
    reporter.section(QStringLiteral("三、样式表：强制黑底白字，且不含任何动态颜色源"));
    // ========================================================================

    QFile styleFile(WinEase::Ui::themeStyleSheetPath());
    const bool styleOpened = styleFile.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString rawQss = styleOpened
                               ? QString::fromUtf8(styleFile.readAll())
                               : QString();
    const QString qss = stripComments(rawQss);

    reporter.check(styleOpened && !rawQss.isEmpty(),
                   QStringLiteral("全局样式表已加载且非空"),
                   QStringLiteral("%1 字节").arg(rawQss.size()));

    // 动态颜色源 = 跟随系统的入口
    reporter.check(!qss.contains(QStringLiteral("palette(")),
                   QStringLiteral("样式表不使用 palette(...) 动态取色"));
    reporter.check(!qss.contains(QStringLiteral("color-scheme"), Qt::CaseInsensitive)
                       && !qss.contains(QStringLiteral("prefers-color-scheme"), Qt::CaseInsensitive),
                   QStringLiteral("样式表不含 color-scheme / prefers-color-scheme"));
    reporter.check(!qss.contains(QStringLiteral("@media"), Qt::CaseInsensitive)
                       && !qss.contains(QLatin1Char('@')),
                   QStringLiteral("样式表不含 @ 规则（无系统主题媒体查询）"));

    reporter.check(qss.contains(baseBlack.name().toUpper())
                       || qss.contains(baseBlack.name()),
                   QStringLiteral("样式表明确写入了纯黑 #000000"));
    reporter.check(qss.contains(pureWhite.name().toUpper())
                       || qss.contains(pureWhite.name()),
                   QStringLiteral("样式表明确写入了纯白 #FFFFFF"));

    // ---- 自动扫描所有 background 声明，确认没有浅底 ----
    // 唯一允许的例外：滑块/开关的把手（handle），那本来就是刻意做成浅色的
    const QVector<QssRule> rules = parseRules(qss);
    reporter.check(rules.size() > 50,
                   QStringLiteral("样式表规则数量合理（解析未出错）"),
                   QStringLiteral("解析到 %1 条声明").arg(rules.size()));

    int backgroundCount = 0;
    int lightBackgroundCount = 0;
    QStringList lightBackgrounds;
    for (const QssRule &rule : rules) {
        if (rule.property != QStringLiteral("background")
            && rule.property != QStringLiteral("background-color")) {
            continue;
        }
        if (rule.value.compare(QStringLiteral("transparent"), Qt::CaseInsensitive) == 0
            || rule.value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        const QColor color(rule.value);
        if (!color.isValid()) {
            continue;
        }
        ++backgroundCount;
        if (rule.selector.contains(QStringLiteral("handle"), Qt::CaseInsensitive)) {
            continue; // 把手是刻意的浅色元素
        }
        if (!isDarkBackground(color)) {
            ++lightBackgroundCount;
            lightBackgrounds.append(
                QStringLiteral("%1 → %2").arg(rule.selector, hex(color)));
        }
    }
    reporter.check(backgroundCount >= 30,
                   QStringLiteral("background 声明被完整扫描（覆盖度足够）"),
                   QStringLiteral("共扫描 %1 条 background 声明").arg(backgroundCount));
    reporter.check(lightBackgroundCount == 0,
                   QStringLiteral("所有背景色均为深色（零浅色残留）"),
                   lightBackgroundCount == 0
                       ? QStringLiteral("无任何 background 的感知亮度 ≥ %1")
                             .arg(kDarkBackgroundMaxBrightness, 0, 'f', 2)
                       : QStringLiteral("发现浅色背景：%1").arg(lightBackgrounds.join(u"; ")));

    // ========================================================================
    reporter.section(QStringLiteral("四、图标在纯黑底上必须可见"));
    // ========================================================================

    const QStringList iconNames{
        QStringLiteral("app"),      QStringLiteral("window"),   QStringLiteral("file"),
        QStringLiteral("input"),    QStringLiteral("monitor"),  QStringLiteral("display"),
        QStringLiteral("media"),    QStringLiteral("launcher"), QStringLiteral("security"),
        QStringLiteral("dev"),      QStringLiteral("personal"),
    };
    static const QRegularExpression colorPattern(QStringLiteral("#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})"));

    int invisibleIconCount = 0;
    int iconColorCount = 0;
    QStringList invisibleIcons;
    for (const QString &name : iconNames) {
        QFile iconFile(QStringLiteral(":/winease/icons/%1.svg").arg(name));
        if (!iconFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            ++invisibleIconCount;
            invisibleIcons.append(QStringLiteral("%1(读取失败)").arg(name));
            continue;
        }
        const QString svg = QString::fromUtf8(iconFile.readAll());

        // 归一化为 6 位后用亮度判定
        bool hasInvisible = false;
        auto it = colorPattern.globalMatch(svg);
        while (it.hasNext()) {
            const QColor color(it.next().captured(0));
            if (!color.isValid()) {
                continue;
            }
            ++iconColorCount;
            // 纯黑描边画在纯黑底上等于不可见
            if (brightness(color) < 0.30) {
                hasInvisible = true;
            }
        }
        if (hasInvisible) {
            ++invisibleIconCount;
            invisibleIcons.append(name);
        }
    }
    reporter.check(iconColorCount > 0,
                   QStringLiteral("图标配色被完整扫描"),
                   QStringLiteral("共扫描 %1 处颜色").arg(iconColorCount));
    reporter.check(invisibleIconCount == 0,
                   QStringLiteral("所有图标都不含黑底上不可见的深色描边"),
                   invisibleIconCount == 0
                       ? QStringLiteral("11 个图标全部只使用亮蓝/白色")
                       : QStringLiteral("可疑图标：%1").arg(invisibleIcons.join(u',')));

    // ========================================================================
    reporter.section(QStringLiteral("五、实际渲染像素（用户真正看到的东西）"));
    // ========================================================================

    QWidget window;
    window.setWindowTitle(QStringLiteral("theme_smoke —— 取像素用窗口"));
    window.setAttribute(Qt::WA_ShowWithoutActivating, true); // 显示但不激活，避免抢走焦点
    window.resize(360, 260);

    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("黑底白字"), &window);
    title->setObjectName(QStringLiteral("HeaderTitle"));
    auto *button = new QPushButton(QStringLiteral("确定"), &window);
    button->setObjectName(QStringLiteral("CardSettingsButton"));
    auto *toggle = new QCheckBox(&window);
    toggle->setObjectName(QStringLiteral("CardSwitch"));
    toggle->setChecked(true);

    layout->addWidget(title);
    layout->addWidget(button);
    layout->addWidget(toggle);
    layout->addStretch(1);

    window.show();
    pump(250); // 等窗口真正完成首次绘制

    const QImage frame = window.grab().toImage();
    reporter.check(!frame.isNull() && frame.size().isValid(),
                   QStringLiteral("成功抓取窗口渲染结果"),
                   QStringLiteral("%1 x %2 像素").arg(frame.width()).arg(frame.height()));

    // grab() 出来的是**设备像素**（本机 150% → 1.5 倍），而控件几何是逻辑像素。
    // 取像素前必须换算，否则采样位置会整体偏移 —— 那会变成假通过或假失败。
    const qreal dpr = frame.devicePixelRatio() > 0.0 ? frame.devicePixelRatio() : 1.0;
    const auto toDevice = [dpr](const QRect &logical) {
        return QRect(qRound(logical.x() * dpr),
                     qRound(logical.y() * dpr),
                     qRound(logical.width() * dpr),
                     qRound(logical.height() * dpr));
    };
    reporter.note(QStringLiteral("窗口 360x260 逻辑像素 → 抓取 %1x%2 设备像素（DPR %3）")
                      .arg(frame.width())
                      .arg(frame.height())
                      .arg(dpr));

    // ---- 5.1 窗口底色 = 纯黑 ----
    const QColor cornerColor = frame.pixelColor(4, 4);
    reporter.check(nearlyEqual(cornerColor, baseBlack),
                   QStringLiteral("窗口底色实际渲染为纯黑"),
                   QStringLiteral("(4,4) = %1").arg(hex(cornerColor)));

    // ---- 5.2 按钮底色 = 深色（证明样式表真的生效，而不是只改了调色板） ----
    const QRect buttonRegion = toDevice(button->geometry());
    const int buttonDarkPixels = countPixels(frame, buttonRegion,
                                             [](const QColor &c) { return isDarkBackground(c); });
    reporter.check(buttonDarkPixels > 100,
                   QStringLiteral("按钮实际渲染为深色（样式表已生效）"),
                   QStringLiteral("按钮区域 %1x%2 内深色像素 %3 个")
                       .arg(buttonRegion.width())
                       .arg(buttonRegion.height())
                       .arg(buttonDarkPixels));

    // ---- 5.3 白字确实被绘制 ----
    const int whiteTextPixels = countPixels(frame, toDevice(title->geometry()),
                                            [](const QColor &c) {
                                                return c.alpha() > 0 && brightness(c) > 0.85;
                                            });
    reporter.check(whiteTextPixels >= 20,
                   QStringLiteral("标题确实以白字绘制在纯黑底上"),
                   QStringLiteral("标题区域检出 %1 个高亮像素").arg(whiteTextPixels));

    // ---- 5.4 开关（本次唯一改动的交互控件配色） ----
    const QColor accent(ThemeColor::kAccent);
    const QRect toggleRegion = toDevice(toggle->geometry());
    const int accentPixels = countPixels(frame, toggleRegion, [&accent](const QColor &c) {
        return nearlyEqual(c, accent, 12);
    });
    reporter.check(accentPixels >= 50,
                   QStringLiteral("开关打开状态渲染为强调色（纯黑底上可辨识）"),
                   QStringLiteral("开关区域检出 %1 个强调色像素（期望色 %2）")
                       .arg(accentPixels)
                       .arg(hex(accent)));

    // ---- 5.5 DWM 标题栏深色属性被系统接受 ----
    const HWND windowHandle = reinterpret_cast<HWND>(window.winId());
    reporter.check(WinEase::Win32::setDarkTitleBar(windowHandle, true),
                   QStringLiteral("DWM 接受标题栏深色属性（系统浅色模式下标题栏也不会变白）"),
                   QStringLiteral("hwnd = 0x%1").arg(reinterpret_cast<quintptr>(windowHandle), 0, 16));

    // ========================================================================
    reporter.section(QStringLiteral("六、★ 抗系统主题切换（本次要求的核心）"));
    // ========================================================================

    const QString styleBefore = styleClassName();
    pump(50); // 探测底层样式会拆一次代理、再装回并重新 polish，先让它落定再取"变更前"基准值
    const QColor windowBefore = app.palette().color(QPalette::Window);
    const QColor windowTextBefore = app.palette().color(QPalette::WindowText);
    const QColor lightBefore = app.palette().color(QPalette::Light);
    const QColor widgetPaletteBefore = window.palette().color(QPalette::Window);
    const QColor windowPixelBefore = frame.pixelColor(4, 4);

    reporter.note(QStringLiteral("投递消息前的状态：样式=%1，色彩方案=%2，Window=%3，WindowText=%4")
                      .arg(styleBefore)
                      .arg(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                               ? QStringLiteral("Dark")
                               : QStringLiteral("其它"))
                      .arg(hex(windowBefore))
                      .arg(hex(windowTextBefore)));

    sendSystemThemeChangeMessages();
    pump(300); // 给平台插件足够时间处理并（若它会的话）重新套用系统调色板

    const QImage frameAfter = window.grab().toImage();

    reporter.check(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark,
                   QStringLiteral("系统主题变更后：色彩方案仍为 Dark"));
    reporter.check(app.palette().color(QPalette::Window) == windowBefore
                       && app.palette().color(QPalette::WindowText) == windowTextBefore,
                   QStringLiteral("系统主题变更后：调色板未被系统覆盖"),
                   QStringLiteral("Window=%1（变更前 %2），WindowText=%3（变更前 %4）")
                       .arg(hex(app.palette().color(QPalette::Window)),
                            hex(windowBefore),
                            hex(app.palette().color(QPalette::WindowText)),
                            hex(windowTextBefore)));
    reporter.check(app.palette().color(QPalette::Light) == lightBefore,
                   QStringLiteral("系统主题变更后：3D 边框色未被系统覆盖"));
    reporter.check(window.palette().color(QPalette::Window) == widgetPaletteBefore,
                   QStringLiteral("系统主题变更后：已存在窗口的调色板未变"));
    reporter.check(nearlyEqual(frameAfter.pixelColor(4, 4), windowPixelBefore),
                   QStringLiteral("系统主题变更后：实际渲染像素仍是纯黑"),
                   QStringLiteral("(4,4) %1 → %2")
                       .arg(hex(windowPixelBefore), hex(frameAfter.pixelColor(4, 4))));

    // 样式断言放在最后：探测底层样式会拆一次代理再装回并重新 polish，
    // 放到最后就不会影响上面那些读调色板 / 像素的断言的取值时机。
    const QString styleAfter = styleClassName();
    reporter.check(styleAfter == styleBefore && styleAfter == QStringLiteral("QFusionStyle"),
                   QStringLiteral("系统主题变更后：控件样式未变（仍是 Fusion）"),
                   QStringLiteral("%1 → %2（外层始终是 %3，未被系统换成平台样式）")
                       .arg(styleBefore, styleAfter, rawStyleClassName()));

    // ---- 反向方向（仅在模拟了浅色模式时才需要） ----
    if (osThemeOverride.usable()) {
        reporter.check(osThemeOverride.switchAppsToDark(),
                       QStringLiteral("已把系统应用模式临时切回深色"));
        pump(1000);
        const QString styleAfterDark = styleClassName();
        reporter.check(styleAfterDark == QStringLiteral("QFusionStyle")
                           && app.palette().color(QPalette::Window) == baseBlack
                           && app.palette().color(QPalette::WindowText) == pureWhite,
                       QStringLiteral("系统切回深色后：样式与调色板仍与之前完全一致"),
                       QStringLiteral("样式 = %1，Window = %2，WindowText = %3")
                           .arg(styleAfterDark,
                                hex(app.palette().color(QPalette::Window)),
                                hex(app.palette().color(QPalette::WindowText))));
    }

    // ========================================================================
    reporter.section(QStringLiteral("七、★ 系统浅色下从零启动（另起子进程重跑一次真实启动路径）"));
    // ========================================================================

    if (simulateLightMode && osThemeOverride.usable()) {
        // 子进程必须在"系统是浅色"的环境里启动，所以这里再切回浅色
        osThemeOverride.switchAppsToLight();
        pump(600);

        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments(QStringList{QStringLiteral("--child-light-probe")});
        child.start();

        const bool finished = child.waitForFinished(30000); // 超时也当失败，不能让测试挂死
        const QByteArray childOut = child.readAllStandardOutput();
        if (!childOut.isEmpty()) {
            printLine(QString::fromUtf8(childOut).trimmed());
        }
        const int childCode = finished ? child.exitCode() : -1;

        QString detail;
        if (!finished) {
            detail = QStringLiteral("子进程 30 秒内没有退出（已强制结束）");
            child.kill();
        } else if (childCode == 2) {
            detail = QStringLiteral("子进程报告前置条件不成立：它启动时并没有被识别为浅色系统");
        } else if (childCode == 0) {
            detail = QStringLiteral("子进程在每个环节都是黑底白字（见上方子进程输出）");
        } else {
            detail = QStringLiteral("子进程退出码 = %1，见上方子进程输出").arg(childCode);
        }
        reporter.check(finished && childCode == 0,
                       QStringLiteral("系统浅色下从零启动：应用依然是黑底白字"),
                       detail);
    } else {
        reporter.note(QStringLiteral("未模拟系统浅色，跳过（加 --with-os-light-mode 可覆盖）"));
    }

    // ---- 收尾：恢复系统主题偏好并校验（必须放在子进程验证之后） ----
    if (osThemeOverride.usable()) {
        const bool restored = osThemeOverride.restore();
        const SystemThemePreference afterRestore = readSystemThemePreference();
        reporter.check(restored && afterRestore.appsUseLight == osThemeOverride.original(),
                       QStringLiteral("系统主题偏好已恢复为测试前的值（不留副作用）"),
                       QStringLiteral("AppsUseLightTheme 现值 = %1，原值 = %2")
                           .arg(afterRestore.appsUseLight)
                           .arg(osThemeOverride.original()));
        if (!restored) {
            printLine(QStringLiteral("  %1!! 自动恢复失败，请手动把 %2 下的 "
                                     "AppsUseLightTheme 改回 %3%4")
                          .arg(QString::fromUtf8(kRed), personalizeKey())
                          .arg(osThemeOverride.original())
                          .arg(QString::fromUtf8(kReset)));
        }
    }

    // ========================================================================
    reporter.section(QStringLiteral("八、新创建的控件同样继承固定配色"));
    // ========================================================================

    // 主题是在创建窗口之前应用的；这里确认"之后新建的控件"拿到的也是深色调色板
    // （而不是平台在某个时刻又把系统调色板塞了回来）
    QWidget lateWidget;
    lateWidget.setObjectName(QStringLiteral("ContentArea"));
    reporter.check(lateWidget.palette().color(QPalette::Window) == baseBlack
                       && lateWidget.palette().color(QPalette::WindowText) == pureWhite,
                   QStringLiteral("新建控件的调色板 = 纯黑底 + 纯白字"),
                   QStringLiteral("Window=%1，WindowText=%2")
                       .arg(hex(lateWidget.palette().color(QPalette::Window)),
                            hex(lateWidget.palette().color(QPalette::WindowText))));

    window.hide();
    window.close();
    pump(60);

    // ========================================================================
    const int total = reporter.passed() + reporter.failed();
    printLine(QString());
    printLine(QStringLiteral("──────────── 汇总 ────────────"));
    printLine(QStringLiteral("  通过 %1 / %2").arg(reporter.passed()).arg(total));
    if (reporter.failed() > 0) {
        printLine(QStringLiteral("  %1失败 %2%3")
                      .arg(QString::fromUtf8(kRed))
                      .arg(reporter.failed())
                      .arg(QString::fromUtf8(kReset)));
    }
    printLine(QStringLiteral("  结果：%1")
                  .arg(reporter.failed() == 0 ? QStringLiteral("全部通过")
                                              : QStringLiteral("存在失败项")));
    printLine(QStringLiteral("  已覆盖场景：%1")
                  .arg(simulateLightMode
                           ? QStringLiteral("系统深色下启动 → 抗切换消息 → "
                                            "系统切浅色 → 子进程从零启动复验 → 恢复原偏好")
                           : QStringLiteral("系统深色下启动 → 抗切换消息"
                                            "（加 --with-os-light-mode 可覆盖浅色场景）")));
    printLine(QStringLiteral("  %1子进程场景可用 --child-light-probe 单独运行%2")
                  .arg(QString::fromUtf8(kGray), QString::fromUtf8(kReset)));
    printLine(QStringLiteral("  %1提示：标题栏深色无法用 API 读回，"
                             "请肉眼确认标题栏为深色（尤其系统处于浅色模式时）。%2")
                  .arg(QString::fromUtf8(kGray), QString::fromUtf8(kReset)));

    return reporter.failed() == 0 ? 0 : 1;
}
