#pragma once

// ============================================================================
//  ClipboardPrivacy.h —— 剪贴板"来源标记"识别（P2-04 剪贴板历史的隐私闸门）
//
//  为什么需要它：
//      剪贴板历史做得越多，泄露面越大 —— 用户从密码管理器复制的那条密码，
//      如果被安静地写进历史文件，是**灾难级**的缺陷（本工程把它当红线）。
//      好在 Windows 上这件事有约定俗成的"标记格式"，密码管理器与浏览器会主动
//      在剪贴板上挂一个自定义格式，告诉监听者"这条别记录"：
//
//        * "Clipboard Viewer Ignore"
//              剪贴板查看器/监听程序应忽略本次内容（KeePass、KeePassXC 等在用）
//        * "ExcludeClipboardContentFromMonitorProcessing"
//              Windows 自身对"不要进剪贴板历史/不要被监视"的表述（Chromium 系等在用）
//        * "CanIncludeInClipboardHistory" = DWORD 0
//              Windows 10/11 云剪贴板（Win+V）的约定：0 表示不允许进历史
//        * "CanUploadToCloudClipboard" = DWORD 0
//              只表示"不要上传到云"，本地记录仍然是允许的（本功能不做上传，
//              因此它只作为展示信息，不参与"是否记录"的判定）
//
//  设计要点：
//      1. 本文件是**无状态查询**（不缓存、不持有剪贴板），符合平台层"纯原语"约定；
//      2. 一次 OpenClipboard 之内把"格式清单 + 标记"全部读完，避免反复开关剪贴板
//         （剪贴板是全局独占资源，开着不放会卡住别的程序）；
//      3. 标记格式名在这里集中给出（`*FormatName()`），
//         这样**产品代码与自检造素材用的是同一串名字**，不会两边写错还测不出来；
//      4. 读不到（剪贴板被别的进程占着）时**如实报告失败**，
//         由调用方决定"宁可不记录"（fail-closed）—— 这里不替调用方做主。
//
//  ⚠ GetClipboardData 返回的句柄**归剪贴板所有**，绝不能 GlobalFree
//     （与 COM 引用计数那条坑同理：谁分配的谁释放）。本文件只读不释放。
// ============================================================================

#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 剪贴板"来源信息"（一次查询的全部结果）
struct ClipboardOriginInfo {
    /// 是否成功打开剪贴板并读到格式清单。
    /// false 时 formats 为空、error 给出原因（此时"是否该记录"无人能判定）
    bool inspected = false;

    /// 剪贴板上全部格式名（诊断与自检用：能证明我们真的看了格式清单）
    QStringList formats;

    /// 命中的"禁止记录"标记格式名（未命中为空串）
    QString ignoreMarker;

    /// 应用显式声明"不要进剪贴板历史"（CanIncludeInClipboardHistory = 0）
    bool historyOptOut = false;

    /// 应用声明"不要上传到云剪贴板"（CanUploadToCloudClipboard = 0）。
    /// 本功能本来就不上传，因此这**不**影响是否记录，只用于界面说明
    bool cloudUploadOptOut = false;

    /// inspected == false 时的中文原因
    QString error;

    /// 是否应当**跳过**本次记录（隐私闸门）
    bool shouldSkipHistory() const { return !ignoreMarker.isEmpty() || historyOptOut; }

    /// 中文原因，用于状态栏/日志（例如"来源标记为'不记录'（Clipboard Viewer Ignore）"）
    QString reasonText() const;
};

/// 读取当前剪贴板的格式清单与来源标记。
/// @param retries 打不开剪贴板时的重试次数（另一个进程可能正握着它）
/// @param retryDelayMs 每次重试之间的间隔（毫秒）
ClipboardOriginInfo inspectClipboardOrigin(int retries = 5, int retryDelayMs = 5);

/// 标记格式名（产品与自检共用，避免两边各写一份字符串）
QString clipboardIgnoreFormatName();          ///< "Clipboard Viewer Ignore"
QString clipboardMonitorExcludeFormatName();  ///< "ExcludeClipboardContentFromMonitorProcessing"
QString clipboardHistoryOptOutFormatName();   ///< "CanIncludeInClipboardHistory"
QString clipboardCloudOptOutFormatName();     ///< "CanUploadToCloudClipboard"

/// 注册一个自定义剪贴板格式并返回其 id（供自检在自己的进程里造素材）。
/// 注意：RegisterClipboardFormatW 在格式不存在时会**注册**它 —— 这是无害的
/// 全局副作用（同名格式全系统只有一个 id），但不要在循环里反复调用。
unsigned int registerClipboardFormatId(const QString &formatName);

/// 判定某个（已注册或预定义的）剪贴板格式当前是否存在
bool clipboardFormatAvailable(unsigned int formatId);

} // namespace WinEase::Win32
