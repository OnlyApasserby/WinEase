// ============================================================================
//  feature_smoke / tool_group.cpp —— P1-B 工具组端到端用例
//                （P1-06 取色 / P1-10 文本格式化 / P1-11 编码转换 / P1-12 端口占用）
//
//  三条纪律（与窗口组一致，但工具组的"真实状态"换成了剪贴板 / 屏幕像素 / 系统网络表）：
//      ① 每个用例先做前置断言（剪贴板读到的是什么、端口是不是真的被自己占着）；
//      ② 只经公开入口驱动（PluginManager::dispatchHotkey），断言外部真实状态；
//      ③ 用例结束必须还原外部状态 —— 工具组写的是**用户的剪贴板**，
//         所以统一用 ClipboardGuard 兜住，退出作用域自动复原。
//
//  另外：文本转换类断言直接调 plugins/common/TextTools.h 的**同一份实现**。
//        这不是"自己测自己"：格式化/编码的规则（键序保持、错误行列号、
//        Base64 字母表、哈希向量）都是与实现无关的客观规格，
//        用固定向量把它们钉死；插件那一侧只负责"取文本 → 调用 → 写回"的接线，
//        接线是否正确由上面第 ② 条的端到端断言负责。
// ============================================================================

#include "tool_group.h"

#include "ColorFormats.h"
#include "TextTools.h"

#include "win32/NetUtils.h"
#include "win32/ProcessUtils.h"
#include "win32/ScreenCapture.h"
#include "win32/WindowUtils.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStringList>
#include <QThread>

namespace FeatureSmoke {

// ColorFormats / TextTools 是命名空间 → 用命名空间别名（using X::Y 只对类/函数有效）
namespace ColorFormats = WinEase::FeaturePlugins::ColorFormats;
namespace TextTools = WinEase::FeaturePlugins::TextTools;

namespace {

const QString kPickerId = QStringLiteral("monitor.color_picker");
const QString kFormatId = QStringLiteral("dev.text_format");
const QString kEncoderId = QStringLiteral("dev.encoder");
const QString kPortId = QStringLiteral("dev.port_viewer");

/// 界面上看不见的空白差异（换行/空格）直接打印出来会误导，转成可读形式
QString visible(const QString &text)
{
    QString shown = text;
    shown.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    shown.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    return shown;
}

} // namespace

int runToolGroupTests(Reporter &reporter,
                      WinEase::PluginManager &manager,
                      StubServices &services,
                      ProbeWindow &probe)
{
    Q_UNUSED(probe); // 工具组不需要目标窗口（取色用自建的标准色卡）

    const int failuresAtStart = reporter.failures();

    // 用户可能正拿着剪贴板里的一段东西 —— 整组用例结束后必须还原
    const ClipboardGuard clipboardGuard;

    // =======================================================================
    //  P1-10 JSON / XML 格式化
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-10 JSON / XML 格式化 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kFormatId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-10 插件已从 plugins 目录加载（dev.text_format）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // ---- 规格向量：直接验共享实现（缩进/键序/错误行列号都是客观规格）----
        const QString compactJson = QStringLiteral(R"({"b":1,"a":[1,2,{"c":true}]})");
        const QString prettyJson = QStringLiteral("{\n"
                                                 "  \"b\": 1,\n"
                                                 "  \"a\": [\n"
                                                 "    1,\n"
                                                 "    2,\n"
                                                 "    {\n"
                                                 "      \"c\": true\n"
                                                 "    }\n"
                                                 "  ]\n"
                                                 "}");

        const TextTools::Result pretty = TextTools::formatJson(compactJson, 2);
        reporter.check(pretty.ok && pretty.output == prettyJson,
                       QStringLiteral("P1-10 规格：紧凑 JSON 格式化成 2 空格缩进（含嵌套对象/数组）"),
                       visible(pretty.ok ? pretty.output : pretty.errorText()));
        reporter.check(pretty.ok && pretty.output.indexOf(QStringLiteral("\"b\""))
                                       < pretty.output.indexOf(QStringLiteral("\"a\"")),
                       QStringLiteral("P1-10 规格：格式化**保持原始键顺序**（b 仍在 a 之前）"),
                       QStringLiteral("QJsonObject 内部按 key 排序，用 QJsonDocument::toJson() 重排过就会变成 a 在前"));

        const TextTools::Result compactAgain = TextTools::minifyJson(pretty.output);
        reporter.check(compactAgain.ok && compactAgain.output == compactJson,
                       QStringLiteral("P1-10 规格：压缩是格式化的逆运算（minify(format(x)) == x）"),
                       visible(compactAgain.ok ? compactAgain.output : compactAgain.errorText()));

        // 非法 JSON：必须给出**能用的行列号**
        //
        // 第 1 行故意放中文，用来区分"字符列号"与"字节列号"：
        // 若把 QJsonParseError 的 UTF-8 字节偏移直接当列号，这一行会偏出 2 位/汉字
        //（本向量里会得到 20 多列而不是 9 列）。
        // 第 2 行是 `  "b": ,`：Qt 的 offset 指向非法字符**之后**一位，
        // 因此列号落在 8~9 之间，这里容许两个值，只要求"落在那一带"。
        const QString brokenJson = QStringLiteral("{\"名字\":\"张三\",\n  \"b\": ,\n}\n");
        const TextTools::Result broken = TextTools::formatJson(brokenJson, 2);
        reporter.check(!broken.ok,
                       QStringLiteral("P1-10 规格：非法 JSON 返回失败而不是原样输出"));
        reporter.check(!broken.ok && broken.line == 2 && (broken.column == 8 || broken.column == 9),
                       QStringLiteral("P1-10 规格：非法 JSON 的错误定位到第 2 行（列号按字符计，不是字节）"),
                       QStringLiteral("实际：%1").arg(broken.errorText()));

        const QString compactXml = QStringLiteral("<a><b x=\"1\"/><c>t</c></a>");
        const QString prettyXml = QStringLiteral("<a>\n  <b x=\"1\"/>\n  <c>t</c>\n</a>");
        const TextTools::Result xmlPretty = TextTools::formatXml(compactXml, 2);
        reporter.check(xmlPretty.ok && xmlPretty.output == prettyXml,
                       QStringLiteral("P1-10 规格：XML 格式化保留空元素自闭合形式 <b x=\"1\"/>"),
                       visible(xmlPretty.ok ? xmlPretty.output : xmlPretty.errorText()));
        const TextTools::Result xmlCompact = TextTools::minifyXml(prettyXml);
        reporter.check(xmlCompact.ok && xmlCompact.output == compactXml,
                       QStringLiteral("P1-10 规格：XML 压缩是格式化的逆运算"),
                       visible(xmlCompact.ok ? xmlCompact.output : xmlCompact.errorText()));

        const TextTools::Result escaped = TextTools::escapeText(QStringLiteral("a\"b\nc\td"));
        const TextTools::Result unescaped =
            escaped.ok ? TextTools::unescapeText(escaped.output) : TextTools::Result();
        reporter.check(escaped.ok && escaped.output == QStringLiteral("a\\\"b\\nc\\td")
                           && unescaped.ok && unescaped.output == QStringLiteral("a\"b\nc\td"),
                       QStringLiteral("P1-10 规格：转义/去转义互为逆运算"),
                       visible(escaped.ok ? escaped.output : escaped.errorText()));

        const TextTools::Result queried =
            TextTools::queryJson(QStringLiteral(R"({"a":{"b":[10,20,{"c":"hit"}]}})"),
                                 QStringLiteral("$.a.b[2].c"));
        reporter.check(queried.ok && queried.output == QStringLiteral("hit"),
                       QStringLiteral("P1-10 规格：JSON 路径查询 $.a.b[2].c 命中字符串"),
                       visible(queried.ok ? queried.output : queried.errorText()));

        // ---- 端到端：剪贴板 → 插件 → 剪贴板 ----
        StatusLog status;
        status.attach(plugin); // 必须挂在启用之前，否则收不到启用瞬间那条
        reporter.check(manager.setPluginEnabled(kFormatId, true), QStringLiteral("P1-10 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("缩进 2 空格")),
                       QStringLiteral("P1-10 启用时按配置读出缩进（indent=2）"),
                       status.last());
        status.clear();

        clipboardGuard.set(compactJson);
        reporter.check(dispatchAction(manager, kFormatId, QStringLiteral("default")),
                       QStringLiteral("P1-10 触发「格式化」"));
        reporter.check(clipboardText() == prettyJson,
                       QStringLiteral("P1-10 端到端：剪贴板里的紧凑 JSON 就地变成格式化结果"),
                       visible(clipboardText()));
        reporter.check(status.contains(QStringLiteral("格式化完成")),
                       QStringLiteral("P1-10 状态文本反馈格式化成功"),
                       status.last());

        status.clear();
        dispatchAction(manager, kFormatId, QStringLiteral("minify"));
        reporter.check(clipboardText() == compactJson,
                       QStringLiteral("P1-10 端到端：「压缩」把格式化结果压回原样"),
                       visible(clipboardText()));

        // ---- 失败路径：**绝不动剪贴板** ----
        status.clear();
        clipboardGuard.set(brokenJson);
        dispatchAction(manager, kFormatId, QStringLiteral("json"));
        reporter.check(clipboardText() == brokenJson,
                       QStringLiteral("P1-10 端到端：非法 JSON 失败时剪贴板**原样不动**（用户能照着行列号去改）"),
                       visible(clipboardText()));
        reporter.check(status.contains(QStringLiteral("第 2 行")),
                       QStringLiteral("P1-10 端到端：状态文本带上精确行号（用户能照着去改）"),
                       status.last());

        // ---- XML 端到端 ----
        clipboardGuard.set(compactXml);
        dispatchAction(manager, kFormatId, QStringLiteral("xml"));
        reporter.check(clipboardText() == prettyXml,
                       QStringLiteral("P1-10 端到端：「格式化 XML」动作生效"),
                       visible(clipboardText()));

        reporter.check(manager.setPluginEnabled(kFormatId, false), QStringLiteral("P1-10 插件停用成功"));
    }

    // =======================================================================
    //  P1-11 编码转换
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-11 编码转换 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kEncoderId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-11 插件已从 plugins 目录加载（dev.encoder）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // ---- 规格向量（与在线工具逐字节一致的那一组）----
        reporter.check(TextTools::toBase64(QStringLiteral("abc")) == QStringLiteral("YWJj")
                           && TextTools::toBase64(QStringLiteral("a")) == QStringLiteral("YQ==")
                           && TextTools::toBase64(QStringLiteral("ab")) == QStringLiteral("YWI="),
                       QStringLiteral("P1-11 规格：Base64 编码与标准向量一致（含 '=' 填充）"));
        reporter.check(TextTools::toBase64(QStringLiteral("hello 世界"))
                           == QStringLiteral("aGVsbG8g5LiW55WM"),
                       QStringLiteral("P1-11 规格：Base64 编码按 UTF-8 字节（中文 3 字节/字）算"),
                       TextTools::toBase64(QStringLiteral("hello 世界")));

        const TextTools::Result decoded = TextTools::fromBase64(QStringLiteral("aGVsbG8g5LiW55WM"));
        reporter.check(decoded.ok && decoded.output == QStringLiteral("hello 世界"),
                       QStringLiteral("P1-11 规格：Base64 解码回到原文本"),
                       decoded.ok ? decoded.output : decoded.errorText());

        // 宽松解码：URL 安全字母表（'-' _'）+ 换行 + 缺省填充 一次测到
        // 标准 Base64(U+1F600) = "8J+YgA=="，URL 安全写法是 "8J-YgA"
        const TextTools::Result tolerant =
            TextTools::fromBase64(QStringLiteral("8J-\nYgA"));
        reporter.check(tolerant.ok && tolerant.output == QStringLiteral("\U0001F600"),
                       QStringLiteral("P1-11 规格：Base64 解码容忍换行、URL 安全字母表、缺省填充"),
                       QStringLiteral("实际：%1").arg(visible(tolerant.ok ? tolerant.output
                                                                       : tolerant.errorText())));
        reporter.check(!TextTools::fromBase64(QStringLiteral("!!!")).ok,
                       QStringLiteral("P1-11 规格：非法 Base64 返回失败并给出中文原因"));

        reporter.check(TextTools::toUrlEncoded(QStringLiteral("a b")) == QStringLiteral("a+b")
                           && TextTools::toUrlEncoded(QStringLiteral("a b"), false)
                                  == QStringLiteral("a%20b"),
                       QStringLiteral("P1-11 规格：URL 编码（空格编成 '+' 或 %20）"));
        reporter.check(TextTools::fromUrlDecoded(QStringLiteral("a+b")).output == QStringLiteral("a b")
                           && TextTools::fromUrlDecoded(QStringLiteral("%E4%B8%96%E7%95%8C")).output
                                  == QStringLiteral("世界"),
                       QStringLiteral("P1-11 规格：URL 解码（'+' 视为空格、按 UTF-8 还原中文）"));

        // 哈希：FIPS-180 标准向量
        reporter.check(TextTools::hashText(QStringLiteral("abc"), TextTools::HashAlgorithm::Md5)
                           == QStringLiteral("900150983cd24fb0d6963f7d28e17f72"),
                       QStringLiteral("P1-11 规格：MD5(\"abc\") 与标准向量一致"));
        reporter.check(TextTools::hashText(QStringLiteral("abc"), TextTools::HashAlgorithm::Sha1)
                           == QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"),
                       QStringLiteral("P1-11 规格：SHA-1(\"abc\") 与标准向量一致"));
        reporter.check(TextTools::hashText(QStringLiteral("abc"), TextTools::HashAlgorithm::Sha256)
                           == QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
                       QStringLiteral("P1-11 规格：SHA-256(\"abc\") 与标准向量一致"));
        reporter.check(TextTools::hashText(QStringLiteral("abc"), TextTools::HashAlgorithm::Sha512)
                           == QStringLiteral("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20"
                                             "a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643"
                                             "ce80e2a9ac94fa54ca49f"),
                       QStringLiteral("P1-11 规格：SHA-512(\"abc\") 与标准向量一致"));

        // ---- 端到端 ----
        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kEncoderId, true), QStringLiteral("P1-11 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("就绪")),
                       QStringLiteral("P1-11 启用时给出状态提示"),
                       status.last());
        status.clear();

        clipboardGuard.set(QStringLiteral("abc"));
        reporter.check(dispatchAction(manager, kEncoderId, QStringLiteral("default")),
                       QStringLiteral("P1-11 触发「Base64 编码」"));
        reporter.check(clipboardText() == QStringLiteral("YWJj"),
                       QStringLiteral("P1-11 端到端：剪贴板里的 abc 变成 YWJj"),
                       clipboardText());

        status.clear();
        dispatchAction(manager, kEncoderId, QStringLiteral("decode"));
        reporter.check(clipboardText() == QStringLiteral("abc"),
                       QStringLiteral("P1-11 端到端：「Base64 解码」把结果解回原文"),
                       clipboardText());

        clipboardGuard.set(QStringLiteral("a b"));
        dispatchAction(manager, kEncoderId, QStringLiteral("url"));
        reporter.check(clipboardText() == QStringLiteral("a+b"),
                       QStringLiteral("P1-11 端到端：「URL 编码」生效"),
                       clipboardText());

        clipboardGuard.set(QStringLiteral("abc"));
        status.clear();
        dispatchAction(manager, kEncoderId, QStringLiteral("hash"));
        const QString hashClipboard = clipboardText();
        reporter.check(hashClipboard.contains(QStringLiteral("MD5"))
                           && hashClipboard.contains(QStringLiteral("SHA-1"))
                           && hashClipboard.contains(QStringLiteral("SHA-256"))
                           && hashClipboard.contains(QStringLiteral("SHA-512"))
                           && hashClipboard.contains(QStringLiteral("900150983cd24fb0d6963f7d28e17f72")),
                       QStringLiteral("P1-11 端到端：「哈希摘要」一次给出四种摘要（带算法名）"),
                       visible(hashClipboard));
        reporter.check(status.contains(QStringLiteral("哈希摘要完成")),
                       QStringLiteral("P1-11 状态文本反馈摘要完成"),
                       status.last());

        // ---- 失败路径：非法 Base64 不动剪贴板 ----
        clipboardGuard.set(QStringLiteral("!!!!"));
        status.clear();
        dispatchAction(manager, kEncoderId, QStringLiteral("decode"));
        reporter.check(clipboardText() == QStringLiteral("!!!!"),
                       QStringLiteral("P1-11 端到端：非法 Base64 失败时剪贴板原样不动"),
                       clipboardText());
        reporter.check(status.contains(QStringLiteral("失败")),
                       QStringLiteral("P1-11 端到端：失败原因通过状态文本给出"),
                       status.last());

        reporter.check(manager.setPluginEnabled(kEncoderId, false), QStringLiteral("P1-11 插件停用成功"));
    }

    // =======================================================================
    //  P1-06 屏幕取色器
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-06 屏幕取色器 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kPickerId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-06 插件已从 plugins 目录加载（monitor.color_picker）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // 自建"标准色卡"：置顶的纯色窗口，取色点一定落在这个颜色上
        const QColor expectedColor(0x33, 0x66, 0xCC);
        const SolidColorWindow card(expectedColor);
        reporter.check(WinEase::Win32::isValidWindow(card.handle()),
                       QStringLiteral("P1-06 前置：标准色卡窗口已上屏"));
        if (!WinEase::Win32::isValidWindow(card.handle())) {
            return reporter.failures() - failuresAtStart;
        }

        const QRect cardRect = card.physicalRect();
        const QPoint cardCenter = card.physicalCenter();
        const qreal dpr = WinEase::Win32::scaleFactorForWindow(card.handle());
        reporter.check(dpr > 1.0,
                       QStringLiteral("P1-06 前置：本机是非整数缩放（物理像素 ≠ 逻辑像素，这条要求才有意义）"),
                       QStringLiteral("缩放 = %1，窗口矩形 = %2").arg(dpr).arg(rectText(cardRect)));

        // 平台层取色（物理像素）
        bool colorOk = false;
        const QColor sampled = WinEase::Win32::colorAt(cardCenter, &colorOk);
        reporter.check(colorOk && sampled == expectedColor,
                       QStringLiteral("P1-06 前置：平台层 colorAt(物理坐标) 取到的就是色卡颜色"),
                       QStringLiteral("期望 %1，实测 %2")
                           .arg(expectedColor.name(QColor::HexRgb).toUpper(),
                                sampled.name(QColor::HexRgb).toUpper()));

        // 把光标放到色卡中心（SetCursorPos 用的也是物理像素）。
        // 断言"落在色卡内"而不是"精确等于中心"：色卡有 240x240 物理像素，
        // 容得下真实鼠标的细微干扰；而"坐标空间用错"会偏差到 1/3 屏，绝不可能还在卡内。
        moveCursorTo(cardCenter);
        const QPoint cursorNow = WinEase::Win32::cursorPosition();
        reporter.check(cardRect.contains(cursorNow),
                       QStringLiteral("P1-06 前置：光标落在色卡上（物理像素坐标）"),
                       QStringLiteral("色卡 %1 / 光标 %2,%3（目标中心 %4,%5）")
                           .arg(rectText(cardRect))
                           .arg(cursorNow.x())
                           .arg(cursorNow.y())
                           .arg(cardCenter.x())
                           .arg(cardCenter.y()));

        // 反向对照：若把"逻辑坐标"当成物理坐标去取色，就会落到另一个点（应当取不到这个颜色）
        const QPoint misreadPoint = WinEase::Win32::toLogical(cardCenter, dpr);
        bool misreadOk = false;
        const QColor misreadColor = WinEase::Win32::colorAt(misreadPoint, &misreadOk);
        reporter.check(!cardRect.contains(misreadPoint),
                       QStringLiteral("P1-06 前置：坐标误读点落在色卡之外（用于反向对照）"),
                       QStringLiteral("误读点 %1,%2 / 色卡 %3")
                           .arg(misreadPoint.x())
                           .arg(misreadPoint.y())
                           .arg(rectText(cardRect)));
        reporter.check(!(misreadOk && misreadColor == expectedColor),
                       QStringLiteral("P1-06 反向对照：按逻辑坐标取色会取到别的颜色"),
                       QStringLiteral("误读点取到 %1").arg(misreadColor.name(QColor::HexRgb).toUpper()));

        // ---- 端到端 ----
        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kPickerId, true), QStringLiteral("P1-06 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("取色格式：HEX")),
                       QStringLiteral("P1-06 启用时按配置读出默认格式（format=hex）"),
                       status.last());
        status.clear();

        // 配置 includePosition=true → 复制结果应带上坐标，正好用来验证"按物理像素取色"：
        // 复制出来的坐标必须落在色卡的**物理**矩形内（若插件用的是逻辑坐标，
        // 那个点会落在卡外 —— 本机缩放 1.5，误读点约 (379,339)，色卡在 (450,390) 起）
        dispatchAction(manager, kPickerId, QStringLiteral("default"));
        const QString hexText = clipboardText();
        const QStringList hexParts = hexText.split(QStringLiteral(" @ "), Qt::SkipEmptyParts);
        QPoint reportedPoint;
        bool pointParsed = false;
        if (hexParts.size() == 2) {
            const QStringList coords = hexParts.at(1).split(QLatin1Char(','));
            if (coords.size() == 2) {
                bool xOk = false;
                bool yOk = false;
                reportedPoint = QPoint(coords.at(0).trimmed().toInt(&xOk),
                                       coords.at(1).trimmed().toInt(&yOk));
                pointParsed = xOk && yOk;
            }
        }
        reporter.check(hexParts.value(0) == expectedColor.name(QColor::HexRgb).toUpper(),
                       QStringLiteral("P1-06 端到端：复制到 HEX 颜色值"),
                       QStringLiteral("剪贴板：%1").arg(hexText));
        reporter.check(pointParsed && cardRect.contains(reportedPoint),
                       QStringLiteral("P1-06 端到端：附带坐标落在色卡的**物理**矩形内（用逻辑坐标会落到卡外）"),
                       QStringLiteral("剪贴板：%1 / 色卡 %2 / 光标 %3,%4")
                           .arg(hexText, rectText(cardRect))
                           .arg(WinEase::Win32::cursorPosition().x())
                           .arg(WinEase::Win32::cursorPosition().y()));
        reporter.check(status.contains(expectedColor.name(QColor::HexRgb).toUpper()),
                       QStringLiteral("P1-06 状态文本给出取到的颜色"),
                       status.last());

        status.clear();
        dispatchAction(manager, kPickerId, QStringLiteral("rgb"));
        reporter.check(clipboardText().startsWith(QStringLiteral("rgb(51, 102, 204)")),
                       QStringLiteral("P1-06 端到端：辅助快捷键动作可强制指定格式（RGB）"),
                       clipboardText());
        reporter.check(status.contains(QStringLiteral("已复制 RGB")),
                       QStringLiteral("P1-06 状态文本标明复制的是哪种格式"),
                       status.last());

        // 历史持久化：插件应把取到的颜色写回配置
        const QStringList history =
            services.configValue(kPickerId, QStringLiteral("history"), QVariant()).toStringList();
        reporter.check(history.contains(QStringLiteral("#3366CC")),
                       QStringLiteral("P1-06 取色历史写回配置（去重、最近的排最前）"),
                       history.join(QStringLiteral(", ")));

        status.clear();
        dispatchAction(manager, kPickerId, QStringLiteral("cmyk"));
        reporter.check(clipboardText().startsWith(QStringLiteral("cmyk(")),
                       QStringLiteral("P1-06 端到端：CMYK 格式可用"),
                       clipboardText());

        reporter.check(manager.setPluginEnabled(kPickerId, false), QStringLiteral("P1-06 插件停用成功"));
    }

    // =======================================================================
    //  P1-12 端口占用查看
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-12 端口占用查看 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kPortId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-12 插件已从 plugins 目录加载（dev.port_viewer）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // 真实占用源：自己开一个回环监听套接字（端口由系统分配，不用猜"哪个端口空着"）
        LoopbackListener tcpListener;
        reporter.check(tcpListener.listenTcp(),
                       QStringLiteral("P1-12 前置：本机 TCP 回环监听套接字已就绪"),
                       QStringLiteral("端口 %1").arg(tcpListener.port()));
        if (tcpListener.port() == 0) {
            return reporter.failures() - failuresAtStart;
        }

        const quint16 port = tcpListener.port();
        const QString portText = QString::number(port);
        const QString ownName = WinEase::Win32::processName(WinEase::Win32::currentProcessId());
        const QString ownPath = WinEase::Win32::currentProcessPath();

        // ---- 平台层：端口 → PID → 进程名/路径 ----
        const QList<WinEase::Win32::PortOccupant> occupants = WinEase::Win32::portOccupants(port);
        bool foundOwnProcess = false;
        for (const WinEase::Win32::PortOccupant &occupant : occupants) {
            if (occupant.pid == WinEase::Win32::currentProcessId()) {
                foundOwnProcess = true;
            }
        }
        reporter.check(foundOwnProcess,
                       QStringLiteral("P1-12 规格：GetExtendedTcpTable 查到本进程正在监听该端口"),
                       QStringLiteral("端口 %1：%2").arg(portText).arg(
                           occupants.isEmpty() ? QStringLiteral("（查不到）") : occupants.first().describe()));
        reporter.check(!occupants.isEmpty() && occupants.first().processPath == ownPath
                           && occupants.first().processName == ownName,
                       QStringLiteral("P1-12 规格：能给出占用进程的**完整路径**与映像名"),
                       QStringLiteral("期望 %1 / %2，实测 %3 / %4")
                           .arg(ownName, ownPath,
                                occupants.isEmpty() ? QStringLiteral("-") : occupants.first().processName,
                                occupants.isEmpty() ? QStringLiteral("-") : occupants.first().processPath));
        reporter.check(WinEase::Win32::endpointsOnPort(0).isEmpty(),
                       QStringLiteral("P1-12 规格：端口 0 视为非法查询（不返回全网端点）"));

        // ---- 端到端：剪贴板给端口 → 插件查占用 → 结果写回剪贴板 ----
        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kPortId, true), QStringLiteral("P1-12 插件启用成功"));
        status.clear();

        clipboardGuard.set(portText);
        reporter.check(dispatchAction(manager, kPortId, QStringLiteral("default")),
                       QStringLiteral("P1-12 触发「查询剪贴板里的端口」"));
        const QString tcpResult = clipboardText();
        reporter.check(tcpResult.contains(portText) && tcpResult.contains(ownName)
                           && tcpResult.contains(ownPath) && tcpResult.contains(QStringLiteral("TCP")),
                       QStringLiteral("P1-12 端到端：结果写回剪贴板（端口 + 进程名 + 完整路径）"),
                       visible(tcpResult));
        reporter.check(status.contains(portText) && status.contains(ownName),
                       QStringLiteral("P1-12 状态文本给出「端口被谁占用」"),
                       status.last());

        // ---- UDP 分支 ----
        LoopbackListener udpListener;
        reporter.check(udpListener.bindUdp(),
                       QStringLiteral("P1-12 前置：本机 UDP 套接字已绑定（随机端口）"),
                       QStringLiteral("端口 %1").arg(udpListener.port()));

        if (udpListener.port() != 0) {
            status.clear();
            clipboardGuard.set(QString::number(udpListener.port()));
            dispatchAction(manager, kPortId, QStringLiteral("default"));
            const QString udpResult = clipboardText();
            reporter.check(udpResult.contains(QString::number(udpListener.port()))
                               && udpResult.contains(ownName)
                               && udpResult.contains(QStringLiteral("UDP")),
                           QStringLiteral("P1-12 端到端：UDP 端口同样能查到占用进程"),
                           visible(udpResult));
        }

        // ---- 没有端口号时的提示 ----
        status.clear();
        clipboardGuard.set(QStringLiteral("这里没有任何数字"));
        dispatchAction(manager, kPortId, QStringLiteral("default"));
        reporter.check(status.contains(QStringLiteral("没有找到端口号")),
                       QStringLiteral("P1-12 端到端：剪贴板里没有端口号时给出明确提示（不瞎猜）"),
                       status.last());

        reporter.check(manager.setPluginEnabled(kPortId, false), QStringLiteral("P1-12 插件停用成功"));
    }

    reporter.info(QStringLiteral("工具组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
