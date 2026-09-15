#pragma once

// ============================================================================
//  ClipboardTools.h —— "取文本 → 转换 → 写回/替换"流水线（P1-09/10/11 共用）
//
//  被三个插件共用，是为了让**取词失败时的回退行为彻底一致**：
//      优先"复制当前选中内容"（向前台窗口发 Ctrl+C），
//      拿不到（没选中 / 前台窗口不接受注入 / UIPI 拦截）就回落到剪贴板里已有的内容。
//  如果各插件自己写一份，必然出现"这个插件能用、那个插件不行"的玄学 bug。
//
//  ⚠ 本头文件不含 Q_OBJECT。
// ============================================================================

#include <QString>

namespace WinEase::FeaturePlugins::ClipboardTools {

/// "取待处理文本"的结果
struct AcquireResult {
    bool ok = false;
    QString text;
    /// true = 来自"复制选中"，false = 回落到剪贴板原有内容
    bool fromSelection = false;
    /// 失败原因（中文）；ok 为 false 时非空
    QString error;
};

/// 剪贴板里的纯文本（无内容 / 非文本时空串）
QString clipboardText();

/// 写入剪贴板，并**读回校验**（剪贴板可能被别的进程短暂占用）。
/// 失败时最多重试 3 次，每次间隔 20ms。
bool setText(const QString &text);

/// 取"待处理文本"。
/// @param useSelection 是否尝试复制当前选中内容；false 时只读剪贴板（自检用）
AcquireResult acquireText(bool useSelection);

/// 就地把剪贴板内容粘贴回前台窗口（会向前台窗口注入 Ctrl+V）
bool pasteBack();

} // namespace WinEase::FeaturePlugins::ClipboardTools
