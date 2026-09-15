#pragma once

// ============================================================================
//  ColorFormats.h —— 颜色文本格式（P1-06 取色器）
//
//  为什么单独抽出来：
//    取色器要"一键复制成 HEX / RGB / HSL / CMYK"，而**CMYK 的取整规则**
//    各家工具并不一致（先算 k 再除 (1-k)，还是按比例缩放）。
//    把规则固定在一个地方，自检才能用固定向量把它钉死。
//
//  ⚠ 本头文件不含 Q_OBJECT，且全部为 inline 纯函数（可被多个插件各自编入）。
// ============================================================================

#include <QColor>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::ColorFormats {

enum class Format {
    Hex = 0,
    Rgb,
    Hsl,
    Cmyk
};

inline QList<Format> allFormats()
{
    return { Format::Hex, Format::Rgb, Format::Hsl, Format::Cmyk };
}

inline QString formatName(Format format)
{
    switch (format) {
    case Format::Hex:
        return QStringLiteral("HEX");
    case Format::Rgb:
        return QStringLiteral("RGB");
    case Format::Hsl:
        return QStringLiteral("HSL");
    case Format::Cmyk:
        return QStringLiteral("CMYK");
    }
    return QStringLiteral("未知");
}

/// 配置键里的短名（"hex"/"rgb"/"hsl"/"cmyk"）
inline QString formatKey(Format format)
{
    return formatName(format).toLower();
}

inline Format formatFromKey(const QString &key, Format fallback = Format::Hex)
{
    const QString lowered = key.trimmed().toLower();
    if (lowered == QLatin1String("rgb")) {
        return Format::Rgb;
    }
    if (lowered == QLatin1String("hsl")) {
        return Format::Hsl;
    }
    if (lowered == QLatin1String("cmyk")) {
        return Format::Cmyk;
    }
    if (lowered == QLatin1String("hex")) {
        return Format::Hex;
    }
    return fallback;
}

/// CMYK 百分比（整数，"半上调"取整）：
///     k = 1 - max(r, g, b)，c = (1 - r - k) / (1 - k)，其余同理
/// 纯黑（k == 1）时 c/m/y 约定为 0 —— 否则会出现除零。
inline void toCmyk(const QColor &color, int *c, int *m, int *y, int *k)
{
    const qreal r = color.redF();
    const qreal g = color.greenF();
    const qreal b = color.blueF();

    const qreal black = 1.0 - qMax(r, qMax(g, b));
    if (black >= 1.0) {
        *c = *m = *y = 0;
        *k = 100;
        return;
    }
    const qreal divisor = 1.0 - black;
    *c = qRound((1.0 - r - black) / divisor * 100.0);
    *m = qRound((1.0 - g - black) / divisor * 100.0);
    *y = qRound((1.0 - b - black) / divisor * 100.0);
    *k = qRound(black * 100.0);
}

inline QString toCmykText(const QColor &color)
{
    int c = 0;
    int m = 0;
    int y = 0;
    int k = 0;
    toCmyk(color, &c, &m, &y, &k);
    return QStringLiteral("cmyk(%1%, %2%, %3%, %4%)").arg(c).arg(m).arg(y).arg(k);
}

/// 按指定格式输出（HEX 为大写 "#RRGGBB"，与画图/取色工具一致）
inline QString format(const QColor &color, Format format)
{
    switch (format) {
    case Format::Hex:
        return color.name(QColor::HexRgb).toUpper();
    case Format::Rgb:
        return QStringLiteral("rgb(%1, %2, %3)").arg(color.red()).arg(color.green()).arg(color.blue());
    case Format::Hsl: {
        // 灰阶的 hue 返回 -1（无效）→ 显示为 0，避免出现 "hsl(-1, 0%, 50%)"
        const int hue = qMax(0, color.hslHue());
        return QStringLiteral("hsl(%1, %2%, %3%)")
            .arg(hue)
            .arg(qRound(color.hslSaturationF() * 100.0))
            .arg(qRound(color.lightnessF() * 100.0));
    }
    case Format::Cmyk:
        return toCmykText(color);
    }
    return color.name(QColor::HexRgb).toUpper();
}

/// 解析用户输入的颜色文本；无法识别时返回无效 QColor（isValid() == false）
/// 支持：#RGB / #RRGGBB / #AARRGGBB / 颜色名 / rgb(r,g,b) / (r,g,b) / r,g,b
inline QColor parse(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return QColor();
    }

    static const QRegularExpression rgbPattern(
        QStringLiteral("(?:rgba?\\s*\\()?\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rgbPattern.match(trimmed);
    if (match.hasMatch()) {
        const int r = match.captured(1).toInt();
        const int g = match.captured(2).toInt();
        const int b = match.captured(3).toInt();
        if (r <= 255 && g <= 255 && b <= 255) {
            return QColor(r, g, b);
        }
        return QColor();
    }

    // 命名色 / #RRGGBB / #AARRGGBB（Qt 6.6+ 提供 fromString）
    const QColor named = QColor::fromString(trimmed);
    return named.isValid() ? named : QColor();
}

/// 一行摘要："#3366CC · rgb(51, 102, 204) · hsl(210, 50%, 50%) · cmyk(...)"
inline QString describe(const QColor &color)
{
    QStringList parts;
    for (const Format each : allFormats()) {
        parts.append(format(color, each));
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace WinEase::FeaturePlugins::ColorFormats
