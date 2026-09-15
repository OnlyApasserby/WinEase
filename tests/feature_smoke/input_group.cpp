#include "input_group.h"

#include "sdk/HookService.h"
#include "win32/ClipboardPrivacy.h"
#include "win32/CoreAudio.h"
#include "win32/WindowUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QWidget>

#include <windows.h>

namespace FeatureSmoke {

namespace {

namespace Win32 = WinEase::Win32;

const QString kClipId = QStringLiteral("input.clipboard_history");
const QString kWheelId = QStringLiteral("input.wheel_enhance");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kPanelName = QStringLiteral("cliphistPanel");
const QString kSearchEdit = QStringLiteral("cliphistSearchEdit");
const QString kFilterCombo = QStringLiteral("cliphistFilterCombo");
const QString kList = QStringLiteral("cliphistList");
const QString kBodyEdit = QStringLiteral("cliphistBodyEdit");
const QString kCopyButton = QStringLiteral("cliphistCopyButton");
const QString kFavoriteButton = QStringLiteral("cliphistFavoriteButton");
const QString kDeleteButton = QStringLiteral("cliphistDeleteButton");
const QString kClearButton = QStringLiteral("cliphistClearButton");
const QString kMaxKbSpin = QStringLiteral("cliphistMaxKbSpin");
const QString kImageModeCombo = QStringLiteral("cliphistImageModeCombo");
const QString kSummaryLabel = QStringLiteral("cliphistSummaryLabel");
const QString kPrivacyLabel = QStringLiteral("cliphistPrivacyLabel");
const QString kStatusLabel = QStringLiteral("cliphistStatusLabel");

// 滚轮插件的设置面板控件对象名
const QString kWheelStepSpin = QStringLiteral("wheelStepSpin");
const QString kWheelModifierCombo = QStringLiteral("wheelModifierCombo");
const QString kWheelInvertCheck = QStringLiteral("wheelInvertCheck");
const QString kWheelVolumeLabel = QStringLiteral("wheelCurrentVolumeLabel");

// 配置里预设的步长（用它而不是"代码里的 5"，才能证明配置真的被读走了）
constexpr int kWheelConfiguredStep = 7;

// ---------------------------------------------------------------------------
//  小工具：直接读"产品写出来的那些文件"
// ---------------------------------------------------------------------------

QJsonArray historyItems(const QString &storeDir)
{
    QFile file(QDir(storeDir).filePath(QStringLiteral("index.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonArray();
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return QJsonArray();
    }
    return document.object().value(QStringLiteral("items")).toArray();
}

/// 历史条目数（索引文件不存在 = 0）
int historyItemCount(const QString &storeDir)
{
    return historyItems(storeDir).size();
}

/// payload 目录里的正文文件（文件名列表）——"磁盘上真的多了/少了什么"
QStringList payloadFiles(const QString &storeDir)
{
    return QDir(QDir(storeDir).filePath(QStringLiteral("payload")))
        .entryList(QDir::Files, QDir::Name);
}

/// 索引里第一条指向的正文内容：用来核对"列表里显示的"与"磁盘上存的"是不是同一份
QString firstEntryBody(const QString &storeDir)
{
    const QJsonArray items = historyItems(storeDir);
    if (items.isEmpty()) {
        return QString();
    }
    const QString payloadFile =
        items.first().toObject().value(QStringLiteral("payloadFile")).toString();
    QFile file(QDir(QDir(storeDir).filePath(QStringLiteral("payload"))).filePath(payloadFile));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

/// 静默等一会儿（照常转事件循环）。
/// 用途只有一个：验证"某件事**没有**发生"之前，必须先给它足够的机会发生
/// —— 否则"没发生"可能只是"还没轮到"（踩坑 #22 的同一条道理）。
void settle(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

QWidget *topLevelNamed(const QString &objectName)
{
    const QWidgetList widgets = QApplication::topLevelWidgets();
    for (QWidget *widget : widgets) {
        if (widget != nullptr && widget->objectName() == objectName) {
            return widget;
        }
    }
    return nullptr;
}

template <typename T>
T *childNamed(QWidget *root, const QString &objectName)
{
    return root != nullptr ? root->findChild<T *>(objectName) : nullptr;
}

QString percentText(float volume)
{
    return QStringLiteral("%1%").arg(qRound(volume * 100.0f));
}

// ---------------------------------------------------------------------------
//  素材：把"带来源标记"的内容放进**真实剪贴板**
// ---------------------------------------------------------------------------

/// 模拟密码管理器：文本 + 一个"别记录"的标记格式。
/// ⚠ 必须走真实剪贴板 API —— 若用 Qt 自己 setMimeData 造一个假格式，这条隐私
///   断言就变成了"测我们自己造的假数据"，等于没测。
bool setClipboardWithMarker(const QString &text,
                            const QString &markerFormat,
                            bool markerHasDword,
                            quint32 dwordValue,
                            QString *errorOut)
{
    const UINT markerId = ::RegisterClipboardFormatW(
        reinterpret_cast<const wchar_t *>(markerFormat.utf16()));
    if (markerId == 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("注册标记格式失败");
        }
        return false;
    }

    if (::OpenClipboard(nullptr) == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("打不开剪贴板（被其它程序占用）");
        }
        return false;
    }
    ::EmptyClipboard();

    // ① 文本（CF_UNICODETEXT）
    {
        const size_t chars = static_cast<size_t>(text.size());
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, (chars + 1) * sizeof(wchar_t));
        if (memory != nullptr) {
            void *locked = ::GlobalLock(memory);
            if (locked != nullptr) {
                memcpy(locked, text.utf16(), chars * sizeof(wchar_t));
                static_cast<wchar_t *>(locked)[chars] = L'\0';
                ::GlobalUnlock(memory);
            }
            // 成功后内存所有权归剪贴板（**不能**再 GlobalFree）
            if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
                ::GlobalFree(memory);
            }
        }
    }

    // ② 标记格式（带一个 DWORD 值；没有值时给 1，表示"存在即不记录"）
    {
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (memory != nullptr) {
            void *locked = ::GlobalLock(memory);
            if (locked != nullptr) {
                *static_cast<DWORD *>(locked) = markerHasDword ? dwordValue : 1;
                ::GlobalUnlock(memory);
            }
            if (::SetClipboardData(markerId, memory) == nullptr) {
                ::GlobalFree(memory);
            }
        }
    }

    ::CloseClipboard();
    return true;
}

// ---------------------------------------------------------------------------
//  任务栏几何：它不占"工作区"，两者之差就是任务栏条带
// ---------------------------------------------------------------------------

QRect taskbarStrip(const Win32::MonitorInfo &monitor)
{
    const QRect full = monitor.geometry;
    const QRect work = monitor.workArea;

    if (work.height() < full.height()) {
        if (work.top() > full.top()) { // 顶部任务栏
            return QRect(full.left(), full.top(), full.width(), work.top() - full.top());
        }
        return QRect(full.left(), work.bottom() + 1, full.width(),
                     full.bottom() - work.bottom());
    }
    if (work.width() < full.width()) {
        if (work.left() > full.left()) { // 左侧任务栏
            return QRect(full.left(), full.top(), work.left() - full.left(), full.height());
        }
        return QRect(work.right() + 1, full.top(), full.right() - work.right(), full.height());
    }
    return QRect(); // 量不到：任务栏自动隐藏，或工作区被别的设备占了
}

// ===========================================================================
//  P2-04 剪贴板历史（含隐私清理）
//
//  外在状态 = 磁盘（index.json + payload/*）+ 面板控件 + 系统剪贴板
// ===========================================================================

void runClipboardHistoryCase(Reporter &reporter,
                             WinEase::PluginManager &manager,
                             StubServices &services,
                             const QString &storeDir)
{
    Q_UNUSED(services); // 配置预设写在 main.cpp（插件在 loadPlugins 时就把它读走了）
    QDir(storeDir).removeRecursively();

    WinEase::IFeaturePlugin *plugin = manager.plugin(kClipId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-04 插件已从插件目录加载（input.clipboard_history）"));
    if (plugin == nullptr) {
        return;
    }

    // ---- 设置面板：证明"配置真的被读走"（而不是界面上另写一份默认值）----
    {
        QWidget *settings = plugin->createSettingsWidget(nullptr);
        reporter.check(settings != nullptr, QStringLiteral("P2-04 设置面板可创建"));
        if (settings != nullptr) {
            QSpinBox *maxKb = childNamed<QSpinBox>(settings, kMaxKbSpin);
            QComboBox *imageMode = childNamed<QComboBox>(settings, kImageModeCombo);
            reporter.check(maxKb != nullptr && maxKb->value() == 4,
                           QStringLiteral("P2-04 设置面板显示的「单条上限」== 配置里的 4 KB"
                                          "（配置真的被读走）"),
                           maxKb != nullptr ? QString::number(maxKb->value())
                                            : QStringLiteral("控件没找到"));
            reporter.check(imageMode != nullptr
                               && imageMode->currentData().toString() == QLatin1String("thumbnail"),
                           QStringLiteral("P2-04 设置面板显示的图片策略 == 配置里的「只存缩略图」"),
                           imageMode != nullptr ? imageMode->currentData().toString()
                                                : QStringLiteral("控件没找到"));
            delete settings;
        }
    }

    ClipboardGuard clipboard; // 用完把用户原来的剪贴板内容还回去
    StatusLog status;
    status.attach(plugin);

    reporter.check(manager.setPluginEnabled(kClipId, true),
                   QStringLiteral("P2-04 插件启用成功"));
    status.clear();

    // ---- ① 普通文本：磁盘上必须真的多出一条 ----
    const QString first = QStringLiteral("winease-clip-alpha 第一段正文");
    clipboard.set(first);
    reporter.check(waitFor([&storeDir] { return historyItemCount(storeDir) == 1; }),
                   QStringLiteral("P2-04 端到端：复制文本后**磁盘上的历史**多了一条"),
                   QStringLiteral("索引里现在 %1 条").arg(historyItemCount(storeDir)));
    reporter.check(firstEntryBody(storeDir) == first,
                   QStringLiteral("P2-04 端到端：磁盘上那条的正文与复制的文本逐字相同"));
    reporter.check(payloadFiles(storeDir).size() == 1,
                   QStringLiteral("P2-04 正文按「一条一个文件」分片存储"),
                   QStringLiteral("payload 目录：%1")
                       .arg(payloadFiles(storeDir).join(QStringLiteral(", "))));

    // ---- ② 再复制两条（后面筛/删/清空都要有东西可看）----
    const QString second = QStringLiteral("winease-clip-beta 第二段正文");
    clipboard.set(second);
    clipboard.set(QStringLiteral("winease-clip-gamma 第三段"));
    reporter.check(waitFor([&storeDir] { return historyItemCount(storeDir) == 3; }),
                   QStringLiteral("P2-04 又复制两条 → 历史 3 条"));

    // ---- ③ 面板：控件按对象名全部找得到，列表行数 = 条目数 ----
    reporter.check(dispatchAction(manager, kClipId, QStringLiteral("default")),
                   QStringLiteral("P2-04 按快捷键打开历史面板"));
    QWidget *panel = topLevelNamed(kPanelName);
    reporter.check(panel != nullptr && panel->isVisible(),
                   QStringLiteral("P2-04 面板窗口出现（对象名 %1）").arg(kPanelName));
    if (panel == nullptr) {
        manager.setPluginEnabled(kClipId, false);
        return;
    }

    QLineEdit *searchEdit = childNamed<QLineEdit>(panel, kSearchEdit);
    QComboBox *filterCombo = childNamed<QComboBox>(panel, kFilterCombo);
    QListWidget *list = childNamed<QListWidget>(panel, kList);
    QPlainTextEdit *bodyEdit = childNamed<QPlainTextEdit>(panel, kBodyEdit);
    QPushButton *copyButton = childNamed<QPushButton>(panel, kCopyButton);
    QPushButton *favoriteButton = childNamed<QPushButton>(panel, kFavoriteButton);
    QPushButton *deleteButton = childNamed<QPushButton>(panel, kDeleteButton);
    QPushButton *clearButton = childNamed<QPushButton>(panel, kClearButton);
    QLabel *summaryLabel = childNamed<QLabel>(panel, kSummaryLabel);
    QLabel *privacyLabel = childNamed<QLabel>(panel, kPrivacyLabel);
    QLabel *statusLabel = childNamed<QLabel>(panel, kStatusLabel);
    reporter.check(searchEdit != nullptr && filterCombo != nullptr && list != nullptr
                       && bodyEdit != nullptr && copyButton != nullptr && favoriteButton != nullptr
                       && deleteButton != nullptr && clearButton != nullptr
                       && summaryLabel != nullptr && privacyLabel != nullptr
                       && statusLabel != nullptr,
                   QStringLiteral("P2-04 面板控件按对象名全部找到（跨 DLL 边界用的是运行期元对象）"));
    if (list == nullptr || searchEdit == nullptr || filterCombo == nullptr
        || bodyEdit == nullptr || copyButton == nullptr || favoriteButton == nullptr
        || deleteButton == nullptr || clearButton == nullptr || summaryLabel == nullptr
        || privacyLabel == nullptr || statusLabel == nullptr) {
        manager.setPluginEnabled(kClipId, false);
        return;
    }

    reporter.check(list->count() == 3,
                   QStringLiteral("P2-04 面板列表行数 == 历史条目数（3）"),
                   QStringLiteral("实际 %1 行").arg(list->count()));
    reporter.check(summaryLabel->text().contains(QStringLiteral("共 3 条")),
                   QStringLiteral("P2-04 面板报出总条数与上限（并说明「收藏不计入」）"),
                   summaryLabel->text());
    reporter.check(!bodyEdit->toPlainText().isEmpty(),
                   QStringLiteral("P2-04 右栏显示选中那条的正文（正文真的是从磁盘读回来的）"));

    // ---- ④ 隐私闸门：密码管理器样式的内容一个字都不能落盘 ----
    QString markerError;
    const QString secret = QStringLiteral("口令 hunter2 绝不该进历史");
    reporter.check(setClipboardWithMarker(secret, Win32::clipboardIgnoreFormatName(), false, 0,
                                          &markerError),
                   QStringLiteral("P2-04 素材：把「Clipboard Viewer Ignore」标记 + 文本放进真实剪贴板"),
                   markerError);
    settle(500); // 先给剪贴板通知足够机会被处理（否则"没多出来"可能只是"还没轮到"）
    reporter.check(historyItemCount(storeDir) == 3 && payloadFiles(storeDir).size() == 3,
                   QStringLiteral("P2-04 隐私：带「不记录」标记的内容**一条都没进历史**，"
                                  "磁盘上一个字节都没多"),
                   QStringLiteral("索引 %1 条 / payload %2 个")
                       .arg(historyItemCount(storeDir))
                       .arg(payloadFiles(storeDir).size()));
    reporter.check(!firstEntryBody(storeDir).contains(QStringLiteral("hunter2")),
                   QStringLiteral("P2-04 隐私：口令那几个字**没有**出现在任何一条历史里"));
    reporter.check(privacyLabel->text().contains(QStringLiteral("已跳过 1 条")),
                   QStringLiteral("P2-04 面板上看得见「跳过了 1 条」（说明插件确实看见了这次复制，"
                                  "是主动不记，而不是没收到通知）"),
                   privacyLabel->text());

    // ---- ⑤ 隐私闸门第二条路：应用声明 CanIncludeInClipboardHistory = 0 ----
    reporter.check(setClipboardWithMarker(QStringLiteral("企业策略禁止记录的一段话"),
                                          Win32::clipboardHistoryOptOutFormatName(), true, 0,
                                          &markerError),
                   QStringLiteral("P2-04 素材：把 DWORD 形式的「不要进剪贴板历史」声明放上剪贴板"),
                   markerError);
    settle(500);
    reporter.check(historyItemCount(storeDir) == 3,
                   QStringLiteral("P2-04 隐私：DWORD 形式的「不要进历史」同样被尊重（还是不记）"));
    reporter.check(privacyLabel->text().contains(QStringLiteral("已跳过 2 条")),
                   QStringLiteral("P2-04 面板上的跳过计数累加到 2（两条路各算一次）"),
                   privacyLabel->text());

    // ---- ⑥ 去重：同样内容只留一份，并提到最前 ----
    clipboard.set(first);
    reporter.check(waitFor([&storeDir] { return historyItemCount(storeDir) == 3; }),
                   QStringLiteral("P2-04 重复复制同样内容 → 条数不变（去重）"));
    reporter.check(firstEntryBody(storeDir) == first,
                   QStringLiteral("P2-04 重复复制的那条被提到最前"),
                   QStringLiteral("索引首条正文：%1").arg(firstEntryBody(storeDir).left(40)));

    // ---- ⑦ 单条上限：超过上限的内容不记录（配置 4 KB）----
    const QString huge = QString(8192, QLatin1Char('x'));
    clipboard.set(huge);
    settle(400);
    reporter.check(historyItemCount(storeDir) == 3 && payloadFiles(storeDir).size() == 3,
                   QStringLiteral("P2-04 单条上限：8 KB 的文本超过 4 KB 上限 → 不记录"));
    reporter.check(privacyLabel->text().contains(QStringLiteral("超过单条上限跳过 1 条")),
                   QStringLiteral("P2-04 面板如实报出「因超上限跳过了几条」（上限不是静默的）"),
                   privacyLabel->text());

    // ---- ⑧ 面板：搜索（含正文）/ 分类 ----
    searchEdit->setText(QStringLiteral("beta"));
    reporter.check(waitFor([list] { return list->count() == 1; }),
                   QStringLiteral("P2-04 搜索「beta」→ 列表筛成 1 行（面板有防抖，等它跑）"),
                   QStringLiteral("实际 %1 行").arg(list->count()));
    reporter.check(list->count() == 1
                       && list->item(0)->text().contains(QStringLiteral("beta")),
                   QStringLiteral("P2-04 筛出来的那一行就是含 beta 的那条"));
    reporter.check(summaryLabel->text().contains(QStringLiteral("当前筛出 1 条")),
                   QStringLiteral("P2-04 页脚同时报出「总数」与「筛出数」"),
                   summaryLabel->text());

    // 正文搜索：关键字在"第二段正文"里（预览只显示前 120 字符也必须搜得到）
    searchEdit->setText(QStringLiteral("第二段正文"));
    reporter.check(waitFor([list] { return list->count() == 1; }),
                   QStringLiteral("P2-04 搜索正文内容也能命中"));
    searchEdit->clear();
    reporter.check(waitFor([list] { return list->count() == 3; }),
                   QStringLiteral("P2-04 清空搜索框 → 列表恢复到 3 行"));

    filterCombo->setCurrentIndex(filterCombo->findData(QStringLiteral("favorite")));
    reporter.check(waitFor([list] { return list->count() == 0; }),
                   QStringLiteral("P2-04 分类「收藏」→ 现在一行都没有（还没收藏过）"));
    filterCombo->setCurrentIndex(filterCombo->findData(QStringLiteral("all")));
    reporter.check(waitFor([list] { return list->count() == 3; }),
                   QStringLiteral("P2-04 切回「全部」→ 3 行"));

    // ---- ⑨ 收藏：界面看得见，而且落到了磁盘索引里 ----
    list->setCurrentRow(0);
    const QString favoritedId = list->currentItem()->data(Qt::UserRole).toString();
    const QString favoritedRow = list->currentItem()->text();
    favoriteButton->click();
    reporter.check(waitFor([list] {
                       return list->count() > 0
                           && list->item(0)->text().startsWith(QStringLiteral("★ "));
                   }),
                   QStringLiteral("P2-04 点「收藏」→ 列表里那条带上 ★ 标记（界面看得见）"),
                   QStringLiteral("操作的是：%1").arg(favoritedRow.left(30)));
    reporter.check(historyItems(storeDir).first().toObject().value(QStringLiteral("favorite")).toBool(),
                   QStringLiteral("P2-04 收藏状态**落到了磁盘索引里**（重启后还在）"));
    filterCombo->setCurrentIndex(filterCombo->findData(QStringLiteral("favorite")));
    reporter.check(waitFor([list] { return list->count() == 1; }),
                   QStringLiteral("P2-04 分类「收藏」→ 1 行"));
    filterCombo->setCurrentIndex(filterCombo->findData(QStringLiteral("all")));
    reporter.check(waitFor([list] { return list->count() == 3; }),
                   QStringLiteral("P2-04 切回「全部」→ 3 行"));

    // ---- ⑩ 仅复制：只动剪贴板，不碰前台窗口，也不把历史搞乱 ----
    list->setCurrentRow(0);
    const QString bodyText = bodyEdit->toPlainText();
    copyButton->click();
    reporter.check(clipboardText() == bodyText && !bodyText.isEmpty(),
                   QStringLiteral("P2-04 点「仅复制到剪贴板」→ 剪贴板内容 == 右栏显示的正文"),
                   QStringLiteral("剪贴板：%1").arg(clipboardText().left(30)));
    reporter.check(statusLabel->text().contains(QStringLiteral("已复制"))
                       && statusLabel->text().contains(QStringLiteral("到剪贴板"))
                       && !statusLabel->text().contains(QStringLiteral("已粘贴")),
                   QStringLiteral("P2-04 「仅复制」的反馈说清了自己没去粘贴前台窗口"),
                   statusLabel->text());
    reporter.check(list->count() == 3,
                   QStringLiteral("P2-04 从历史里复制不会把历史搞乱（还是 3 条）"));

    // ---- ⑪ 删除单条：磁盘上的正文文件也要一起没 ----
    deleteButton->click();
    reporter.check(waitFor([list] { return list->count() == 2; }),
                   QStringLiteral("P2-04 点「删除这条」→ 列表少一行"));
    reporter.check(historyItemCount(storeDir) == 2 && payloadFiles(storeDir).size() == 2,
                   QStringLiteral("P2-04 删除时**磁盘上的正文文件也一起删掉**（不留孤儿文件）"),
                   QStringLiteral("索引 %1 / payload %2")
                       .arg(historyItemCount(storeDir))
                       .arg(payloadFiles(storeDir).size()));
    {
        bool stillThere = false;
        const QJsonArray items = historyItems(storeDir);
        for (const QJsonValue &value : items) {
            if (value.toObject().value(QStringLiteral("id")).toString() == favoritedId) {
                stillThere = true;
            }
        }
        reporter.check(!stillThere,
                       QStringLiteral("P2-04 被删掉的那条确实从索引里消失了（按 id 核对）"));
    }

    // ---- ⑫ 一键清空：历史空 + 系统剪贴板也空 ----
    clearButton->click();
    reporter.check(waitFor([list, &storeDir] {
                       return list->count() == 0 && historyItemCount(storeDir) == 0;
                   }),
                   QStringLiteral("P2-04 一键清空 → 列表 0 行、索引 0 条"));
    reporter.check(payloadFiles(storeDir).isEmpty(),
                   QStringLiteral("P2-04 一键清空把正文文件也清了（磁盘上不残留剪贴板内容）"),
                   QStringLiteral("payload 目录还剩：%1")
                       .arg(payloadFiles(storeDir).join(QStringLiteral(", "))));
    reporter.check(clipboardText().isEmpty(),
                   QStringLiteral("P2-04 一键清空**同时清空了系统剪贴板**（验收项：清空后剪贴板为空）"),
                   QStringLiteral("剪贴板里还有：%1").arg(clipboardText().left(30)));
    reporter.check(statusLabel->text().contains(QStringLiteral("已清空")),
                   QStringLiteral("P2-04 面板报出清空结果"), statusLabel->text());

    // ---- ⑬ 停用：监听真的停了，但历史文件还在（不删用户数据）----
    reporter.check(manager.setPluginEnabled(kClipId, false),
                   QStringLiteral("P2-04 插件停用成功"));
    reporter.check(waitFor([] { return topLevelNamed(kPanelName) == nullptr; }),
                   QStringLiteral("P2-04 停用时面板自动关闭（停用=彻底退出，不留悬浮界面）"));
    clipboard.set(QStringLiteral("停用之后复制的东西不该被记录"));
    settle(400);
    reporter.check(historyItemCount(storeDir) == 0,
                   QStringLiteral("P2-04 停用后复制新内容 → 历史仍然是 0 条"
                                  "（监听真的断了，不是「记了但看不见」）"));
    reporter.check(QFile::exists(QDir(storeDir).filePath(QStringLiteral("index.json"))),
                   QStringLiteral("P2-04 停用**不删**历史文件（历史是用户自己的数据）"));

    // ---- ⑭ 重新启用：又能记了 ----
    reporter.check(manager.setPluginEnabled(kClipId, true),
                   QStringLiteral("P2-04 重新启用成功"));
    clipboard.set(QStringLiteral("重新启用后复制的内容"));
    reporter.check(waitFor([&storeDir] { return historyItemCount(storeDir) == 1; }),
                   QStringLiteral("P2-04 重新启用后又能记录（启用/停用是可逆的）"));
    manager.setPluginEnabled(kClipId, false);
}

// ===========================================================================
//  P2-05 滚轮增强（任务栏滚动调音量）
//
//  外在状态 = 真实系统音量（IAudioEndpointVolume 读回）+ 钩子拦截计数。
//  自检用的是**真实的合成滚轮事件**（SendInput）→ 走真实钩子 → 真的改音量，
//  所以本用例会短暂改动系统音量（结束由 AudioVolumeGuard 还原）。
// ===========================================================================

void runWheelEnhanceCase(Reporter &reporter,
                         WinEase::PluginManager &manager,
                         StubServices &services,
                         ProbeWindow &probe)
{
    WinEase::IFeaturePlugin *plugin = manager.plugin(kWheelId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-05 插件已从插件目录加载（input.wheel_enhance）"));
    if (plugin == nullptr) {
        return;
    }

    // ---- 前置 1：钩子服务在跑（否则整组只能测"函数返回值"）----
    WinEase::HookService *hooks = services.hookService();
    reporter.check(hooks != nullptr && hooks->isRunning(),
                   QStringLiteral("P2-05 前置：全局输入钩子服务已启动并在工作"),
                   hooks != nullptr ? hooks->lastError() : QStringLiteral("钩子服务为空"));
    if (hooks == nullptr || !hooks->isRunning()) {
        return;
    }

    // ---- 前置 2：本机有默认音频输出设备（否则音量根本没地方可调）----
    AudioVolumeGuard volume;
    reporter.check(volume.valid(),
                   QStringLiteral("P2-05 前置：本机有默认音频输出设备（原始音量 %1）")
                       .arg(percentText(volume.originalVolume())),
                   volume.error());
    if (!volume.valid()) {
        reporter.info(QStringLiteral("P2-05：无音频端点，端到端断言无法进行（如实报出，不做假通过）"));
        return;
    }

    QString audioError;
    Win32::AudioEndpoint endpoint =
        Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Render, &audioError);
    reporter.check(endpoint.isValid(),
                   QStringLiteral("P2-05 前置：自检侧也拿到了同一个默认音频端点（用于读回断言）"),
                   audioError);
    if (!endpoint.isValid()) {
        return;
    }

    // ---- 前置 3：能量出任务栏、并把光标真的放到它上面 ----
    const Win32::MonitorInfo monitor = Win32::primaryMonitor();
    const QRect strip = taskbarStrip(monitor);
    reporter.check(strip.isValid(),
                   QStringLiteral("P2-05 前置：能量出任务栏条带（任务栏没被自动隐藏、没被移到别处）"),
                   QStringLiteral("完整区 %1 / 可用区 %2")
                       .arg(rectText(monitor.geometry), rectText(monitor.workArea)));
    if (!strip.isValid()) {
        return;
    }

    const QPoint taskbarPoint = strip.center();
    const QString taskbarClass = Win32::windowClassName(Win32::topLevelWindowAt(taskbarPoint));
    reporter.check(taskbarClass == QLatin1String("Shell_TrayWnd"),
                   QStringLiteral("P2-05 前置：任务栏条带中心那个点底下确实是任务栏窗口"),
                   QStringLiteral("条带 %1 中心 %2,%3 底下是 %4")
                       .arg(rectText(strip))
                       .arg(taskbarPoint.x())
                       .arg(taskbarPoint.y())
                       .arg(taskbarClass.isEmpty() ? QStringLiteral("(无窗口)") : taskbarClass));
    if (taskbarClass != QLatin1String("Shell_TrayWnd")) {
        return;
    }

    reporter.check(moveCursorTo(taskbarPoint),
                   QStringLiteral("P2-05 前置：光标已移到任务栏上"),
                   QStringLiteral("%1,%2").arg(taskbarPoint.x()).arg(taskbarPoint.y()));

    const QPoint outsidePoint = probe.rect().center();
    const QString outsideClass = Win32::windowClassName(Win32::topLevelWindowAt(outsidePoint));
    reporter.check(outsideClass != QLatin1String("Shell_TrayWnd"),
                   QStringLiteral("P2-05 前置：任务栏外的那个对照点不在任务栏上"),
                   QStringLiteral("%1 底下是 %2")
                       .arg(rectText(probe.rect()))
                       .arg(outsideClass.isEmpty() ? QStringLiteral("(桌面或本进程窗口)")
                                                   : outsideClass));

    // ---- 设置面板：本用例靠**用户在界面上改设置**来切换行为 ----
    //  为什么不改配置再重新启用插件：插件在 initialize() 里读一次配置，
    //  而"在设置面板上改一下"本来就是用户真实的路径（控件直接改成员 + 落配置），
    //  走这条路测到的就是用户会碰到的那条链。
    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr, QStringLiteral("P2-05 设置面板可创建"));
    if (settings == nullptr) {
        return;
    }
    QSpinBox *stepSpin = childNamed<QSpinBox>(settings, kWheelStepSpin);
    QComboBox *modifierCombo = childNamed<QComboBox>(settings, kWheelModifierCombo);
    QCheckBox *invertCheck = childNamed<QCheckBox>(settings, kWheelInvertCheck);
    QLabel *volumeLabel = childNamed<QLabel>(settings, kWheelVolumeLabel);
    reporter.check(stepSpin != nullptr && modifierCombo != nullptr && invertCheck != nullptr
                       && volumeLabel != nullptr,
                   QStringLiteral("P2-05 设置面板控件按对象名全部找到"));
    if (stepSpin == nullptr || modifierCombo == nullptr || invertCheck == nullptr
        || volumeLabel == nullptr) {
        delete settings;
        return;
    }

    reporter.check(stepSpin->value() == kWheelConfiguredStep,
                   QStringLiteral("P2-05 设置面板显示的步长 == 配置里的 %1%（不是代码里的默认 5%）")
                       .arg(kWheelConfiguredStep),
                   QStringLiteral("控件显示 %1").arg(stepSpin->value()));
    reporter.check(modifierCombo->currentData().toString() == QLatin1String("none"),
                   QStringLiteral("P2-05 设置面板显示的修饰键要求 == 配置里的「不按修饰键」"),
                   modifierCombo->currentData().toString());
    reporter.check(!invertCheck->isChecked(),
                   QStringLiteral("P2-05 设置面板显示的「方向反转」== 配置里的关"));
    reporter.check(volumeLabel->text().contains(QStringLiteral("当前音量")),
                   QStringLiteral("P2-05 设置面板显示当前音量（读的是真实端点）"),
                   volumeLabel->text());

    reporter.check(manager.setPluginEnabled(kWheelId, true),
                   QStringLiteral("P2-05 插件启用成功"));

    // ---- ① 一格 = 配置的 7%（不是写死的数）----
    const float base = 0.40f;
    reporter.check(endpoint.setVolume(base) && qAbs(endpoint.volume() - base) < 0.02f,
                   QStringLiteral("P2-05 基准：音量已设为 40%"),
                   QStringLiteral("读回 %1").arg(percentText(endpoint.volume())));

    const WinEase::HookService::Stats statsBefore = hooks->stats();
    WinEase::HookService::sendMouseWheel(120);
    reporter.check(waitFor([&endpoint] { return qAbs(endpoint.volume() - 0.47f) < 0.02f; }),
                   QStringLiteral("P2-05 端到端：任务栏上滚一格 → **真实系统音量** 40% → 47%"
                                  "（步长 7% 来自配置）"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));
    const WinEase::HookService::Stats statsAfter = hooks->stats();
    reporter.check(statsAfter.mouseWheelEvents > statsBefore.mouseWheelEvents,
                   QStringLiteral("P2-05 前置：钩子确实收到了那个合成滚轮事件"
                                  "（这样下面「被吞掉」才不是空话）"),
                   QStringLiteral("滚轮计数 %1 → %2")
                       .arg(statsBefore.mouseWheelEvents)
                       .arg(statsAfter.mouseWheelEvents));
    reporter.check(statsAfter.consumedEvents > statsBefore.consumedEvents,
                   QStringLiteral("P2-05 滚轮事件在任务栏上被吞掉 → 任务栏自己没收到它"
                                  "（也就不会触发 Aero Peek 一类的任务栏自身行为）"),
                   QStringLiteral("拦截计数 %1 → %2")
                       .arg(statsBefore.consumedEvents)
                       .arg(statsAfter.consumedEvents));
    reporter.check(statsAfter.degradedSubscribers == 0,
                   QStringLiteral("P2-05 监听器没有被降级（钩子线程里的活够轻）"),
                   QStringLiteral("最大回调 %1 ms / 预算 %2 ms")
                       .arg(statsAfter.maxCallbackMs, 0, 'f', 3)
                       .arg(hooks->callbackBudgetMs()));

    // ---- ② 连续 3 格向下：47% → 26%（不丢事件）----
    for (int i = 0; i < 3; ++i) {
        WinEase::HookService::sendMouseWheel(-120);
    }
    reporter.check(waitFor([&endpoint] { return qAbs(endpoint.volume() - 0.26f) < 0.02f; }),
                   QStringLiteral("P2-05 连续 3 格向下 → 47% → 26%（每格都被处理，没有丢事件）"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));

    // ---- ③ 边界：顶到 100% / 0% 就停住 ----
    endpoint.setVolume(1.0f);
    settle(120);
    WinEase::HookService::sendMouseWheel(120);
    settle(400);
    reporter.check(qAbs(endpoint.volume() - 1.0f) < 0.005f,
                   QStringLiteral("P2-05 已经 100% 时再向上滚 → 停在 100%（不回绕、不越界）"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));
    endpoint.setVolume(0.0f);
    settle(120);
    WinEase::HookService::sendMouseWheel(-120);
    settle(400);
    reporter.check(endpoint.volume() < 0.005f,
                   QStringLiteral("P2-05 已经 0% 时再向下滚 → 停在 0%"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));

    // ---- ④ 非任务栏位置：事件原样放行、音量一个数都不动 ----
    endpoint.setVolume(0.60f);
    settle(120);
    reporter.check(moveCursorTo(outsidePoint),
                   QStringLiteral("P2-05 前置：光标已移到任务栏外的对照点上"));
    settle(150);
    const WinEase::HookService::Stats outsideBefore = hooks->stats();
    WinEase::HookService::sendMouseWheel(120);
    settle(400);
    const WinEase::HookService::Stats outsideAfter = hooks->stats();
    reporter.check(outsideAfter.mouseWheelEvents > outsideBefore.mouseWheelEvents,
                   QStringLiteral("P2-05 前置：钩子收到了这一次滚轮（所以下面「没被吞」是真的没吞）"));
    reporter.check(outsideAfter.consumedEvents == outsideBefore.consumedEvents,
                   QStringLiteral("P2-05 光标不在任务栏 → 滚轮**原样放行**（不吞非任务栏上的事件）"),
                   QStringLiteral("拦截计数 %1 → %2")
                       .arg(outsideBefore.consumedEvents)
                       .arg(outsideAfter.consumedEvents));
    reporter.check(qAbs(endpoint.volume() - 0.60f) < 0.005f,
                   QStringLiteral("P2-05 光标不在任务栏 → 音量一个数都没动"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));

    // ---- ⑤ 修饰键门槛：在设置面板上改成「按住 Ctrl」----
    reporter.check(moveCursorTo(taskbarPoint),
                   QStringLiteral("P2-05 光标回到任务栏上"));
    const int ctrlIndex = modifierCombo->findData(QStringLiteral("ctrl"));
    reporter.check(ctrlIndex >= 0,
                   QStringLiteral("P2-05 设置面板里有「按住 Ctrl」这一项"));
    if (ctrlIndex < 0) {
        delete settings;
        manager.setPluginEnabled(kWheelId, false);
        return;
    }
    modifierCombo->setCurrentIndex(ctrlIndex);
    settle(120);

    endpoint.setVolume(0.50f);
    settle(120);
    const WinEase::HookService::Stats modBefore = hooks->stats();
    WinEase::HookService::sendMouseWheel(120);
    settle(400);
    const WinEase::HookService::Stats modAfter = hooks->stats();
    reporter.check(modAfter.mouseWheelEvents > modBefore.mouseWheelEvents,
                   QStringLiteral("P2-05 前置：钩子收到了这次滚轮"));
    reporter.check(modAfter.consumedEvents == modBefore.consumedEvents
                       && qAbs(endpoint.volume() - 0.50f) < 0.005f,
                   QStringLiteral("P2-05 要求按住 Ctrl 时，没按 Ctrl 的任务栏滚轮**不接管**"
                                  "（事件放行、音量不动）"),
                   QStringLiteral("拦截 %1 → %2，音量 %3")
                       .arg(modBefore.consumedEvents)
                       .arg(modAfter.consumedEvents)
                       .arg(percentText(endpoint.volume())));

    reporter.check(WinEase::HookService::sendKey(VK_CONTROL, true),
                   QStringLiteral("P2-05 前置：按下 Ctrl（合成按键）"));
    settle(150);
    WinEase::HookService::sendMouseWheel(120);
    reporter.check(waitFor([&endpoint] { return qAbs(endpoint.volume() - 0.57f) < 0.02f; }),
                   QStringLiteral("P2-05 按住 Ctrl 后在任务栏滚一格 → 生效（50% → 57%）"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));
    reporter.check(WinEase::HookService::sendKey(VK_CONTROL, false),
                   QStringLiteral("P2-05 前置：松开 Ctrl"));
    settle(120);

    // ---- ⑥ 步长：在设置面板上改成 10% → 一格就是 10% ----
    reporter.check(modifierCombo->currentData().toString() == QLatin1String("ctrl"),
                   QStringLiteral("P2-05 前置：修饰键要求现在是 Ctrl"));
    modifierCombo->setCurrentIndex(modifierCombo->findData(QStringLiteral("none")));
    stepSpin->setValue(10);
    settle(120);

    endpoint.setVolume(0.30f);
    settle(120);
    WinEase::HookService::sendMouseWheel(120);
    reporter.check(waitFor([&endpoint] { return qAbs(endpoint.volume() - 0.40f) < 0.02f; }),
                   QStringLiteral("P2-05 在设置面板上把步长改成 10% → 一格 30% → 40%"
                                  "（改设置立刻生效，不用重启）"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));

    // ---- ⑦ 方向反转 ----
    invertCheck->setChecked(true);
    settle(120);
    endpoint.setVolume(0.40f);
    settle(120);
    WinEase::HookService::sendMouseWheel(120);
    reporter.check(waitFor([&endpoint] { return qAbs(endpoint.volume() - 0.30f) < 0.02f; }),
                   QStringLiteral("P2-05 打开「方向反转」：向上滚一格 → 音量 40% → 30%"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));

    // ---- ⑧ 停用：滚轮恢复原样（连"被吞"都不该再发生）----
    reporter.check(manager.setPluginEnabled(kWheelId, false),
                   QStringLiteral("P2-05 插件停用成功"));
    reporter.check(moveCursorTo(taskbarPoint),
                   QStringLiteral("P2-05 前置：光标停在任务栏上（准备验证停用后不再插手）"));
    endpoint.setVolume(0.70f);
    settle(120);
    const WinEase::HookService::Stats stoppedBefore = hooks->stats();
    WinEase::HookService::sendMouseWheel(120);
    settle(450);
    const WinEase::HookService::Stats stoppedAfter = hooks->stats();
    reporter.check(stoppedAfter.mouseWheelEvents > stoppedBefore.mouseWheelEvents,
                   QStringLiteral("P2-05 前置：钩子收到了这次滚轮"));
    reporter.check(stoppedAfter.consumedEvents == stoppedBefore.consumedEvents,
                   QStringLiteral("P2-05 停用后滚轮不再被吞（监听器真的拆掉了）"),
                   QStringLiteral("拦截计数 %1 → %2")
                       .arg(stoppedBefore.consumedEvents)
                       .arg(stoppedAfter.consumedEvents));
    reporter.check(qAbs(endpoint.volume() - 0.70f) < 0.005f,
                   QStringLiteral("P2-05 停用后任务栏滚轮不再改音量（还原成原样）"),
                   QStringLiteral("现在 %1").arg(percentText(endpoint.volume())));

    delete settings;
}

} // namespace

QString clipboardHistoryDir(const QString &fixtureRoot)
{
    return QDir(fixtureRoot).filePath(QStringLiteral("cliphist"));
}

int runInputGroupTests(Reporter &reporter,
                       WinEase::PluginManager &manager,
                       StubServices &services,
                       ProbeWindow &probe,
                       const QString &fixtureRoot)
{
    const int failuresAtStart = reporter.failures();

    const QString storeDir = clipboardHistoryDir(fixtureRoot);
    reporter.info(QStringLiteral("输入组沙盒：剪贴板历史目录 %1")
                      .arg(QDir::toNativeSeparators(storeDir)));
    reporter.info(QStringLiteral("说明：本组会**短暂改动真实系统**——"
                                 "P2-04 会改系统剪贴板（结束还原），"
                                 "P2-05 会移动光标到任务栏、发合成滚轮、真的改系统音量（结束还原）"));

    runClipboardHistoryCase(reporter, manager, services, storeDir);
    runWheelEnhanceCase(reporter, manager, services, probe);

    reporter.info(QStringLiteral("输入组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
