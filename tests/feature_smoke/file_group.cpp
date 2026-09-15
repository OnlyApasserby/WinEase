// ============================================================================
//  feature_smoke / file_group.cpp —— P2-A 文件组端到端用例
//      （P2-01 批量重命名 / P2-02 快速文件预览 / P2-03 重复文件查找 / P2-12 敏感文件粉碎）
//
//  三条纪律（与前几组一致）：
//      ① 前置先量出来（没有基准就没资格谈"还原"）；
//      ② 只经真实入口驱动：`dispatchHotkey()` 打开面板、面板上真实的按钮/输入框，
//         断言读的是**磁盘与界面画出来的东西**（不是函数的返回值）；
//      ③ 用例结束把沙盒（临时目录）删干净，被拒绝的路径如实报出来。
//
//  本组三个"只能这么测"的地方：
//    ① 面板控件的抓法 = `objectName` + `QApplication::topLevelWidgets()`：
//       插件是独立 DLL，测试拿不到它的具体类（不导出 C++ 符号），
//       只能隔着 DLL 边界按名字找控件（约定见 ROADMAP 踩坑 #17）。
//    ② 引擎与插件**共用同一份源码**（RenameEngine / DuplicateFinder / FileShredder），
//       所以"引擎逐级断言"与"界面端到端断言"是对同一份实现的两条取证路线。
//    ③ 会动到用户环境的两处（都是产品行为本身，躲不开）：
//       · 批量重命名真的改临时目录里的文件名 —— 用例内**撤销回来**；
//       · 查重真的把 2 个 64 KB 的自检临时文件**移进回收站** —— 移进去就回不来了，
//         所以只保持 2 个小文件，并在输出里明说（"重复文件绝不硬删"是产品硬约束，
//         不真点一次这个按钮就等于没测）。
// ============================================================================

#include "file_group.h"

#include "DuplicateFinder.h"
#include "FileShredder.h"
#include "RenameEngine.h"
#include "TaskCancel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QWidget>

#include <windows.h>

#include <shellapi.h> // SHQueryRecycleBinW：证明"文件真的进了回收站"而不是被硬删

namespace FeatureSmoke {

namespace {

const QString kRenameId = QStringLiteral("file.batch_rename");
const QString kLookId = QStringLiteral("file.quick_look");
const QString kDupId = QStringLiteral("file.duplicate_finder");
const QString kShredId = QStringLiteral("security.file_shredder");

namespace Rename = WinEase::FeaturePlugins::RenameEngine;
namespace Dup = WinEase::FeaturePlugins::DuplicateFinder;
namespace Shred = WinEase::FeaturePlugins::FileShredder;
using WinEase::FeaturePlugins::Cancel;
using WinEase::FeaturePlugins::Progress;

// ---------------------------------------------------------------------------
//  面板控件的抓手（跨 DLL 边界）
// ---------------------------------------------------------------------------

/// 按对象名找顶层窗口。插件面板都是 `new XPanel(..., nullptr)` 造出来的顶层窗口，
/// 而测试不能 `#include` 插件的头文件（DLL 不导出 C++ 符号）→ 只有这条路
QWidget *topLevelNamed(const QString &objectName)
{
    const QList<QWidget *> widgets = QApplication::topLevelWidgets();
    for (QWidget *widget : widgets) {
        if (widget->objectName() == objectName) {
            return widget;
        }
    }
    return nullptr;
}

template <typename T>
T *childNamed(QWidget *root, const QString &name)
{
    return root == nullptr ? nullptr : root->findChild<T *>(name);
}

/// 面板当前是否"在屏幕上"（窗口可能已经被 WA_DeleteOnClose 销毁）
bool panelVisible(const QString &objectName)
{
    QWidget *widget = topLevelNamed(objectName);
    return widget != nullptr && widget->isVisible();
}

/// 造一段确定性字节（"同样内容"与"不同内容"的素材都靠它）
QByteArray patternBytes(int size, uchar seed)
{
    QByteArray data(size, '\0');
    for (int i = 0; i < size; ++i) {
        data[i] = static_cast<char>((seed + i * 31) & 0xFF);
    }
    return data;
}

bool writeBytes(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(data) == data.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

/// 目录里的文件名（排好序，断言时两边用同一套排序规则）
QStringList fileNames(const QString &dir)
{
    QStringList names = QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    names.sort();
    return names;
}

qint64 fileSize(const QString &path)
{
    return QFileInfo(path).size();
}

/// 回收站里的条目数（-1 = 查不到）。用**差分**证明"走的是回收站"：
/// 硬删之后这个数不会变，只有进回收站才会 +N —— 这是"绝不硬删"唯一的硬证据
qint64 recycleBinItemCount()
{
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof(info);
    if (SHQueryRecycleBinW(nullptr, &info) != S_OK) {
        return -1;
    }
    return info.i64NumItems;
}

/// 用例的沙盒：临时目录，析构时删干净
class Sandbox
{
public:
    Sandbox()
    {
        m_root = QDir::temp().filePath(
            QStringLiteral("winease_file_group_%1").arg(QCoreApplication::applicationPid()));
        QDir(m_root).removeRecursively(); // 上次异常退出的残留
        QDir().mkpath(m_root);
    }
    ~Sandbox() { QDir(m_root).removeRecursively(); }

    /// 取（必要时建）沙盒里的子目录
    QString dir(const QString &relative) const
    {
        const QString path = QDir(m_root).filePath(relative);
        QDir().mkpath(path);
        return QDir::cleanPath(path);
    }
    QString path(const QString &relative) const
    {
        return QDir::cleanPath(QDir(m_root).filePath(relative));
    }
    QString root() const { return m_root; }

private:
    QString m_root;
};

/// 给窗口发一个按键（界面自己处理 ← → / Esc；sendEvent 绕开焦点问题）
void sendKey(QWidget *widget, int key)
{
    if (widget == nullptr) {
        return;
    }
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
}

// ---------------------------------------------------------------------------
//  P2-01 批量重命名
// ---------------------------------------------------------------------------

void runBatchRenameCase(Reporter &reporter, WinEase::PluginManager &manager, const Sandbox &sandbox)
{
    reporter.info(QStringLiteral("---- P2-01 批量重命名 ----"));

    const QString dir = sandbox.dir(QStringLiteral("rename"));
    writeBytes(QDir(dir).filePath(QStringLiteral("IMG_1.jpg")), patternBytes(1024, 1));
    writeBytes(QDir(dir).filePath(QStringLiteral("IMG_2.jpg")), patternBytes(2048, 2));
    writeBytes(QDir(dir).filePath(QStringLiteral("IMG_3.jpg")), patternBytes(3072, 3));
    writeBytes(QDir(dir).filePath(QStringLiteral("notes.txt")), QByteArray("别动我\n"));
    const QStringList before = fileNames(dir);

    WinEase::IFeaturePlugin *plugin = manager.plugin(kRenameId);
    reporter.check(plugin != nullptr, QStringLiteral("P2-01 插件已从 plugins 目录加载（file.batch_rename）"));
    if (plugin == nullptr) {
        return;
    }

    StatusLog status;
    status.attach(plugin);
    reporter.check(manager.setPluginEnabled(kRenameId, true),
                   QStringLiteral("P2-01 插件启用成功"));
    status.clear();

    reporter.check(FeatureSmoke::dispatchAction(manager, kRenameId, QStringLiteral("default")),
                   QStringLiteral("P2-01 经快捷键入口打开改名面板"));

    QWidget *panel = nullptr;
    reporter.check(waitFor([&panel] {
                       panel = topLevelNamed(QStringLiteral("winease_batch_rename_panel"));
                       return panel != nullptr;
                   }),
                   QStringLiteral("P2-01 面板窗口已出现（按 objectName 隔 DLL 边界找到）"));
    if (panel == nullptr) {
        manager.setPluginEnabled(kRenameId, false);
        return;
    }

    auto *directoryEdit = childNamed<QLineEdit>(panel, QStringLiteral("batchRenameDirectory"));
    auto *filterEdit = childNamed<QLineEdit>(panel, QStringLiteral("batchRenameFilter"));
    auto *findEdit = childNamed<QLineEdit>(panel, QStringLiteral("batchRenameFind"));
    auto *replaceEdit = childNamed<QLineEdit>(panel, QStringLiteral("batchRenameReplace"));
    auto *prefixEdit = childNamed<QLineEdit>(panel, QStringLiteral("batchRenamePrefix"));
    auto *caseCombo = childNamed<QComboBox>(panel, QStringLiteral("batchRenameCase"));
    auto *extensionCombo = childNamed<QComboBox>(panel, QStringLiteral("batchRenameExtension"));
    auto *table = childNamed<QTableWidget>(panel, QStringLiteral("batchRenameTable"));
    auto *applyButton = childNamed<QPushButton>(panel, QStringLiteral("batchRenameApply"));
    auto *undoButton = childNamed<QPushButton>(panel, QStringLiteral("batchRenameUndo"));
    auto *statusLabel = childNamed<QLabel>(panel, QStringLiteral("batchRenameStatus"));
    const bool allFound = directoryEdit != nullptr && filterEdit != nullptr && findEdit != nullptr
        && replaceEdit != nullptr && prefixEdit != nullptr && caseCombo != nullptr
        && extensionCombo != nullptr && table != nullptr && applyButton != nullptr
        && undoButton != nullptr && statusLabel != nullptr;
    reporter.check(allFound,
                   QStringLiteral("P2-01 面板控件按对象名全部找到（目录/过滤/查找/替换/前缀/大小写/扩展名/表格/按钮/状态）"));
    if (!allFound) {
        manager.setPluginEnabled(kRenameId, false);
        return;
    }

    // 撤销按钮的前置：还没改过名，不该有东西可撤
    reporter.check(!undoButton->isEnabled(),
                   QStringLiteral("P2-01 前置：还没执行过重命名 → 「撤销」按钮是禁用的"));

    // ---- 填规则：把 IMG_<数字>.jpg 变成 2026_holiday_<数字>.JPG ----
    // 规则全部通过**界面控件**设置（不碰插件私有接口）：这才是用户真实走过的路
    findEdit->setText(QStringLiteral("IMG_(\\d+)"));
    replaceEdit->setText(QStringLiteral("holiday_\\1"));
    prefixEdit->setText(QStringLiteral("2026_"));
    extensionCombo->setCurrentIndex(extensionCombo->findData(QStringLiteral("upper")));
    filterEdit->setText(QStringLiteral("*.jpg"));
    directoryEdit->setText(QDir::toNativeSeparators(dir));

    reporter.check(waitFor([table] { return table->rowCount() == 3; }),
                   QStringLiteral("P2-01 实时预览列出了 3 个待改名文件（.txt 被「只处理 *.jpg」挡在表外）"),
                   QStringLiteral("表格行数 %1").arg(table->rowCount()));

    QHash<QString, QString> preview; // 原名 → 预览里的新名
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *original = table->item(row, 0);
        const QTableWidgetItem *renamed = table->item(row, 1);
        if (original != nullptr && renamed != nullptr) {
            preview.insert(original->text(), renamed->text());
        }
    }
    const QString expected1 = QStringLiteral("2026_holiday_1.JPG");
    const QString expected2 = QStringLiteral("2026_holiday_2.JPG");
    const QString expected3 = QStringLiteral("2026_holiday_3.JPG");
    reporter.check(preview.value(QStringLiteral("IMG_1.jpg")) == expected1
                       && preview.value(QStringLiteral("IMG_2.jpg")) == expected2
                       && preview.value(QStringLiteral("IMG_3.jpg")) == expected3,
                   QStringLiteral("P2-01 预览逐字符正确（正则捕获组 + 前缀 + 扩展名转大写叠加）"),
                   QStringLiteral("IMG_1.jpg → %1").arg(preview.value(QStringLiteral("IMG_1.jpg"))));
    reporter.check(!preview.contains(QStringLiteral("notes.txt")),
                   QStringLiteral("P2-01 过滤器之外的文件没有进入预览（不会顺手改了别的文件）"));

    // ---- 应用：预览里的名字必须就是落到磁盘上的名字 ----
    reporter.check(applyButton->isEnabled(),
                   QStringLiteral("P2-01 计划无冲突 → 「应用」按钮可用"));
    applyButton->click();

    QStringList expectedAfter = before;
    expectedAfter.removeAll(QStringLiteral("IMG_1.jpg"));
    expectedAfter.removeAll(QStringLiteral("IMG_2.jpg"));
    expectedAfter.removeAll(QStringLiteral("IMG_3.jpg"));
    expectedAfter.append(expected1);
    expectedAfter.append(expected2);
    expectedAfter.append(expected3);
    expectedAfter.sort();

    reporter.check(waitFor([&dir, &expectedAfter] { return fileNames(dir) == expectedAfter; }),
                   QStringLiteral("P2-01 端到端：磁盘上的文件名逐项等于预览里的名字（预览=落盘）"),
                   QStringLiteral("磁盘上现在是 %1").arg(fileNames(dir).join(QStringLiteral("，"))));
    reporter.check(statusLabel->text().contains(QDir::toNativeSeparators(dir)),
                   QStringLiteral("P2-01 执行结果如实报出目录（用户回头能核对改在哪里）"),
                   statusLabel->text());

    // ---- 撤销：内容也要回到原处（只改名字不动内容，靠大小核对映射没串） ----
    reporter.check(undoButton->isEnabled(),
                   QStringLiteral("P2-01 执行过之后「撤销」按钮变为可用"));

    // 顺带验证撤销的快捷键入口（面板按钮之外的第二条路）
    reporter.check(FeatureSmoke::dispatchAction(manager, kRenameId, QStringLiteral("undo")),
                   QStringLiteral("P2-01 经快捷键入口执行「撤销最近一次」"));
    reporter.check(waitFor([&dir, &before] { return fileNames(dir) == before; }),
                   QStringLiteral("P2-01 端到端：撤销后磁盘逐项回到原文件名"),
                   QStringLiteral("磁盘上现在是 %1").arg(fileNames(dir).join(QStringLiteral("，"))));
    reporter.check(fileSize(QDir(dir).filePath(QStringLiteral("IMG_2.jpg"))) == 2048,
                   QStringLiteral("P2-01 撤销后文件内容也回到原位（原名 ↔ 原内容没有串）"));
    reporter.check(status.contains(QStringLiteral("撤销")),
                   QStringLiteral("P2-01 撤销结果通过状态文本反馈"), status.last());

    // ---- 冲突必须拒绝执行（批量改名最容易出人命的一步） ----
    const QString conflictDir = sandbox.dir(QStringLiteral("rename_conflict"));
    writeBytes(QDir(conflictDir).filePath(QStringLiteral("a.txt")), QByteArray("a"));
    writeBytes(QDir(conflictDir).filePath(QStringLiteral("b.txt")), QByteArray("b"));

    findEdit->clear();
    replaceEdit->clear();
    prefixEdit->clear();
    filterEdit->setText(QStringLiteral("*"));
    // 模板写死同一个名字 → 两个文件的目标名相同 → 计划里有冲突
    childNamed<QLineEdit>(panel, QStringLiteral("batchRenameTemplate"))
        ->setText(QStringLiteral("same"));
    directoryEdit->setText(QDir::toNativeSeparators(conflictDir));

    // 改规则是 200ms 防抖后才重算的（面板有意这么做：连续敲字不该每次都算一遍计划）
    // → 等到状态栏真的把冲突说出来，再断言按钮状态（不然是在读"上一轮的结论"）
    reporter.check(waitFor([statusLabel] {
                       return statusLabel->text().contains(QStringLiteral("冲突"));
                   }),
                   QStringLiteral("P2-01 状态栏把冲突说清楚（不静默吞掉）"), statusLabel->text());
    reporter.check(!applyButton->isEnabled(),
                   QStringLiteral("P2-01 目标重名 → 「应用」按钮被禁用（界面层就不给点）"));

    applyButton->click(); // 硬点也不该动任何文件
    reporter.check(fileNames(conflictDir) == QStringList({ QStringLiteral("a.txt"),
                                                          QStringLiteral("b.txt") }),
                   QStringLiteral("P2-01 端到端：计划有冲突时磁盘上一个文件都没动"),
                   fileNames(conflictDir).join(QStringLiteral("，")));

    reporter.check(manager.setPluginEnabled(kRenameId, false),
                   QStringLiteral("P2-01 插件停用成功"));
}

// ---------------------------------------------------------------------------
//  P2-02 快速文件预览
// ---------------------------------------------------------------------------

void runQuickLookCase(Reporter &reporter,
                      WinEase::PluginManager &manager,
                      const Sandbox &sandbox,
                      const ClipboardGuard &clipboard)
{
    reporter.info(QStringLiteral("---- P2-02 快速文件预览 ----"));

    const QString dir = sandbox.dir(QStringLiteral("quick"));
    const QString bigPath = QDir(dir).filePath(QStringLiteral("big.txt"));
    const QString aPath = QDir(dir).filePath(QStringLiteral("a.txt"));
    const QString bPath = QDir(dir).filePath(QStringLiteral("b.txt"));
    // 用 BMP 而不是 PNG：BMP 是 QtGui 内置解码（不依赖 imageformats 插件），
    // 素材生成与预览解码两端都不受部署情况影响（与壁纸用例同一条经验）
    const QString picPath = QDir(dir).filePath(QStringLiteral("pic.bmp"));

    // 64 KB 文本：头尾各埋一个标记。配置里文本上限是 4 KB（见 main.cpp 的 preset），
    // 所以"看不见尾巴上的标记"就是"没有整文件载入"的证据
    QByteArray big;
    big.append("<<HEAD-MARKER>>\n");
    while (big.size() < 64 * 1024 - 40) {
        big.append("0123456789abcdef\n");
    }
    big.append("<<TAIL-MARKER>>\n");
    writeBytes(bigPath, big);
    writeBytes(aPath, QByteArray("AAA\n"));
    writeBytes(bPath, QByteArray("BBB\n"));

    QImage picture(400, 300, QImage::Format_RGB32);
    picture.fill(QColor(12, 200, 90));
    const bool pictureSaved = picture.save(picPath, "BMP");

    WinEase::IFeaturePlugin *plugin = manager.plugin(kLookId);
    reporter.check(plugin != nullptr, QStringLiteral("P2-02 插件已从 plugins 目录加载（file.quick_look）"));
    if (plugin == nullptr) {
        return;
    }

    StatusLog status;
    status.attach(plugin);
    reporter.check(manager.setPluginEnabled(kLookId, true), QStringLiteral("P2-02 插件启用成功"));
    status.clear();
    reporter.check(pictureSaved, QStringLiteral("P2-02 素材：已生成一张 400×300 的测试图片"));

    // ---- ① 剪贴板里是一段"正好是文件路径"的文本（用户复制路径的用法） ----
    clipboard.set(QDir::toNativeSeparators(bigPath));
    reporter.check(FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default")),
                   QStringLiteral("P2-02 经快捷键入口打开预览（剪贴板里是文件路径）"));
    reporter.check(waitFor([] { return panelVisible(QStringLiteral("winease_quick_look")); }),
                   QStringLiteral("P2-02 预览窗口已出现"));

    QWidget *window = topLevelNamed(QStringLiteral("winease_quick_look"));
    auto *textView = childNamed<QPlainTextEdit>(window, QStringLiteral("quickLookText"));
    auto *hintLabel = childNamed<QLabel>(window, QStringLiteral("quickLookHint"));
    auto *infoLabel = childNamed<QLabel>(window, QStringLiteral("quickLookInfo"));
    reporter.check(textView != nullptr && hintLabel != nullptr && infoLabel != nullptr,
                   QStringLiteral("P2-02 预览控件按对象名全部找到（文本区/提示条/信息条）"));
    if (textView == nullptr || hintLabel == nullptr || infoLabel == nullptr) {
        manager.setPluginEnabled(kLookId, false);
        return;
    }

    reporter.check(waitFor([textView] { return !textView->toPlainText().isEmpty(); }),
                   QStringLiteral("P2-02 文本预览已渲染出来"));
    const QString shown = textView->toPlainText();
    reporter.check(shown.contains(QStringLiteral("<<HEAD-MARKER>>")),
                   QStringLiteral("P2-02 预览读到了文件的头部内容"));
    reporter.check(!shown.contains(QStringLiteral("<<TAIL-MARKER>>")),
                   QStringLiteral("P2-02 端到端：64 KB 文件的尾部标记没有出现（文本预览只读头部，没有整文件载入）"),
                   QStringLiteral("界面上有 %1 个字符").arg(shown.size()));
    reporter.check(shown.size() <= 4 * 1024 + 32,
                   QStringLiteral("P2-02 界面上只有配置上限（4 KB）那么多内容"),
                   QStringLiteral("实际 %1 字符").arg(shown.size()));
    reporter.check(hintLabel->text().contains(QStringLiteral("已显示前")),
                   QStringLiteral("P2-02 提示条如实说明「只显示了前一部分」而不是假装完整"),
                   hintLabel->text());
    reporter.check(infoLabel->text().contains(QStringLiteral("big.txt")),
                   QStringLiteral("P2-02 顶部信息条给出文件名与类型/大小"), infoLabel->text());

    // ---- ② 再按一次 = 收起（"看一眼再收起来"的手感） ----
    FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default"));
    reporter.check(waitFor([] { return !panelVisible(QStringLiteral("winease_quick_look")); }),
                   QStringLiteral("P2-02 端到端：再按一次快捷键收起预览窗口"));

    // ---- ③ ← / → 在同目录里换文件 ----
    clipboard.set(QDir::toNativeSeparators(aPath));
    FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default"));
    reporter.check(waitFor([] { return panelVisible(QStringLiteral("winease_quick_look")); }),
                   QStringLiteral("P2-02 再次打开预览（a.txt）"));
    window = topLevelNamed(QStringLiteral("winease_quick_look"));
    infoLabel = childNamed<QLabel>(window, QStringLiteral("quickLookInfo"));
    textView = childNamed<QPlainTextEdit>(window, QStringLiteral("quickLookText"));
    reporter.check(waitFor([textView] {
                       return textView != nullptr && textView->toPlainText().contains(QStringLiteral("AAA"));
                   }),
                   QStringLiteral("P2-02 预览内容确实是 a.txt 的内容"));

    sendKey(window, Qt::Key_Right);
    reporter.check(waitFor([infoLabel] {
                       return infoLabel != nullptr
                           && infoLabel->text().contains(QStringLiteral("b.txt"));
                   }),
                   QStringLiteral("P2-02 端到端：按 → 切到同目录的下一个文件（b.txt）"),
                   infoLabel == nullptr ? QString() : infoLabel->text());
    sendKey(window, Qt::Key_Left);
    reporter.check(waitFor([infoLabel] {
                       return infoLabel != nullptr
                           && infoLabel->text().contains(QStringLiteral("a.txt"));
                   }),
                   QStringLiteral("P2-02 端到端：按 ← 又切回来（左右都能走）"));

    // ---- ④ 图片：按目标尺寸解码后画在图片页上 ----
    clipboard.set(QDir::toNativeSeparators(picPath));
    FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default")); // 收起
    FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default")); // 打开
    reporter.check(waitFor([] { return panelVisible(QStringLiteral("winease_quick_look")); }),
                   QStringLiteral("P2-02 打开图片预览"));
    window = topLevelNamed(QStringLiteral("winease_quick_look"));
    auto *imageLabel = childNamed<QLabel>(window, QStringLiteral("quickLookImage"));
    reporter.check(imageLabel != nullptr, QStringLiteral("P2-02 找到图片页控件"));
    reporter.check(waitFor([imageLabel] {
                       return imageLabel != nullptr && !imageLabel->pixmap().isNull();
                   }),
                   QStringLiteral("P2-02 端到端：图片被解码并画出来了"));
    if (imageLabel != nullptr) {
        reporter.check(imageLabel->pixmap(Qt::ReturnByValue).size() == QSize(400, 300),
                       QStringLiteral("P2-02 图片按原始尺寸显示（窗口比图大 → 不放大也不缩小）"),
                       QStringLiteral("实际 %1×%2")
                           .arg(imageLabel->pixmap(Qt::ReturnByValue).width())
                           .arg(imageLabel->pixmap(Qt::ReturnByValue).height()));
    }

    // ---- ⑤ 剪贴板里没有文件时，把"怎么用"讲清楚，而不是静默什么都不做 ----
    clipboard.set(QStringLiteral("这只是一段话，不是任何文件的路径"));
    FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default")); // 收起
    FeatureSmoke::dispatchAction(manager, kLookId, QStringLiteral("default")); // 打开并提示
    reporter.check(waitFor([] { return panelVisible(QStringLiteral("winease_quick_look")); }),
                   QStringLiteral("P2-02 剪贴板里没有文件时窗口照样打开（有话说）"));
    window = topLevelNamed(QStringLiteral("winease_quick_look"));
    textView = childNamed<QPlainTextEdit>(window, QStringLiteral("quickLookText"));
    reporter.check(waitFor([textView] {
                       return textView != nullptr
                           && textView->toPlainText().contains(QStringLiteral("剪贴板里没有文件"));
                   }),
                   QStringLiteral("P2-02 端到端：明确告诉用户「剪贴板里没有文件」，而不是假装成功"),
                   textView == nullptr ? QString() : textView->toPlainText());
    reporter.check(status.contains(QStringLiteral("剪贴板里没有文件")),
                   QStringLiteral("P2-02 失败原因也通过状态文本给出"), status.last());

    // ---- ⑥ Esc 关闭 ----
    sendKey(topLevelNamed(QStringLiteral("winease_quick_look")), Qt::Key_Escape);
    reporter.check(waitFor([] { return !panelVisible(QStringLiteral("winease_quick_look")); }),
                   QStringLiteral("P2-02 端到端：Esc 关闭预览"));

    reporter.check(manager.setPluginEnabled(kLookId, false), QStringLiteral("P2-02 插件停用成功"));
}

// ---------------------------------------------------------------------------
//  P2-03 重复文件查找
// ---------------------------------------------------------------------------

void runDuplicateFinderCase(Reporter &reporter,
                            WinEase::PluginManager &manager,
                            const Sandbox &sandbox)
{
    reporter.info(QStringLiteral("---- P2-03 重复文件查找 ----"));

    const QString dir = sandbox.dir(QStringLiteral("dupes"));
    const QByteArray content = patternBytes(64 * 1024, 7);
    // 名字特意以 aaa_ 开头：引擎"每组只留一份"留的是**字典序最小**的那份，
    // 素材名字得让"该留谁"一目了然（否则断言会和实现的排序规则打架）
    const QString keepPath = QDir(dir).filePath(QStringLiteral("aaa_keep.bin"));
    const QString dup1Path = QDir(dir).filePath(QStringLiteral("dup1.bin"));
    const QString dup2Path = QDir(dir).filePath(QStringLiteral("dup2.bin"));
    const QString otherPath = QDir(dir).filePath(QStringLiteral("other.bin"));
    const QString smallPath = QDir(dir).filePath(QStringLiteral("small.bin"));
    writeBytes(keepPath, content);
    writeBytes(dup1Path, content);
    writeBytes(dup2Path, content);
    writeBytes(otherPath, patternBytes(64 * 1024, 9)); // 同样 64 KB，但内容不同
    writeBytes(smallPath, patternBytes(100, 3));       // 小于阈值 → 不参与

    // ---- 引擎逐级断言（与插件共用同一份源码，见 CMakeLists 的说明） ----
    Dup::ScanOptions options;
    options.roots = QStringList({ dir });
    options.minSizeBytes = 4096;
    Cancel cancel;
    Progress silent = Progress();
    QString error;

    Dup::Stats stats;
    const QVector<Dup::Entry> entries = Dup::scan(options, &cancel, silent, &stats, &error);
    reporter.check(error.isEmpty() && entries.size() == 4,
                   QStringLiteral("P2-03 阶段①枚举：5 个文件里 4 个进入候选"),
                   QStringLiteral("%1；%2").arg(error, stats.stageText()));
    reporter.check(stats.skippedSmall == 1,
                   QStringLiteral("P2-03 阶段①：小于阈值的文件被单独统计出来（不是悄悄漏掉）"),
                   QStringLiteral("跳过 %1 个小文件").arg(stats.skippedSmall));

    const QVector<QVector<Dup::Entry>> sizeGroups = Dup::groupBySize(entries, &stats);
    reporter.check(sizeGroups.size() == 1 && sizeGroups.first().size() == 4,
                   QStringLiteral("P2-03 阶段②按大小分组：4 个同大小文件落在同一组"),
                   QStringLiteral("组数 %1").arg(sizeGroups.size()));

    const QVector<QVector<Dup::Entry>> sampled =
        Dup::filterBySampleHash(sizeGroups, 4096, &cancel, silent, &stats, &error);
    reporter.check(sampled.size() == 1 && sampled.first().size() == 3,
                   QStringLiteral("P2-03 阶段③抽样哈希：内容不同的那个被筛掉，剩 3 个候选"));
    reporter.check(stats.partialHashBytes > 0 && stats.partialHashBytes <= 4 * 4096,
                   QStringLiteral("P2-03 阶段③抽样只读了每个文件的头部 4 KB（不是全量读盘）"),
                   QStringLiteral("抽样读了 %1 字节").arg(stats.partialHashBytes));

    const QVector<Dup::Group> confirmed =
        Dup::confirmByFullHash(sampled, &cancel, silent, &stats, &error);
    reporter.check(confirmed.size() == 1 && confirmed.first().files.size() == 3,
                   QStringLiteral("P2-03 阶段④全量 SHA-256：确认 1 组、3 个内容完全相同的文件"));
    reporter.check(stats.duplicateFiles == 2 && stats.duplicateBytes == 2 * 64 * 1024,
                   QStringLiteral("P2-03 可回收空间算成 2 × 64 KB（每组只留一份）"),
                   QStringLiteral("%1 个文件 / %2 字节")
                       .arg(stats.duplicateFiles)
                       .arg(stats.duplicateBytes));

    const QStringList victims = Dup::suggestDeletions(confirmed);
    reporter.check(victims.size() == 2 && !victims.contains(keepPath) && victims.contains(dup1Path)
                       && victims.contains(dup2Path),
                   QStringLiteral("P2-03 建议删除项保留字典序最小的一份（aaa_keep.bin 不被建议删除）"),
                   victims.join(QStringLiteral("；")));

    // ---- 面板端到端 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kDupId);
    reporter.check(plugin != nullptr, QStringLiteral("P2-03 插件已从 plugins 目录加载（file.duplicate_finder）"));
    if (plugin == nullptr) {
        return;
    }
    StatusLog status;
    status.attach(plugin);
    reporter.check(manager.setPluginEnabled(kDupId, true), QStringLiteral("P2-03 插件启用成功"));
    status.clear();

    reporter.check(FeatureSmoke::dispatchAction(manager, kDupId, QStringLiteral("default")),
                   QStringLiteral("P2-03 经快捷键入口打开查重面板"));
    QWidget *panel = nullptr;
    reporter.check(waitFor([&panel] {
                       panel = topLevelNamed(QStringLiteral("winease_duplicate_finder"));
                       return panel != nullptr;
                   }),
                   QStringLiteral("P2-03 面板窗口已出现"));
    if (panel == nullptr) {
        manager.setPluginEnabled(kDupId, false);
        return;
    }

    auto *rootsEdit = childNamed<QPlainTextEdit>(panel, QStringLiteral("dupFinderRoots"));
    auto *minSizeSpin = childNamed<QSpinBox>(panel, QStringLiteral("dupFinderMinSize"));
    auto *sampleSpin = childNamed<QSpinBox>(panel, QStringLiteral("dupFinderSample"));
    auto *startButton = childNamed<QPushButton>(panel, QStringLiteral("dupFinderStart"));
    auto *recycleButton = childNamed<QPushButton>(panel, QStringLiteral("dupFinderRecycle"));
    auto *table = childNamed<QTableWidget>(panel, QStringLiteral("dupFinderTable"));
    auto *statusLabel = childNamed<QLabel>(panel, QStringLiteral("dupFinderStatus"));
    const bool allFound = rootsEdit != nullptr && minSizeSpin != nullptr && sampleSpin != nullptr
        && startButton != nullptr && recycleButton != nullptr && table != nullptr
        && statusLabel != nullptr;
    reporter.check(allFound, QStringLiteral("P2-03 面板控件按对象名全部找到"));
    if (!allFound) {
        manager.setPluginEnabled(kDupId, false);
        return;
    }

    reporter.check(!recycleButton->isEnabled(),
                   QStringLiteral("P2-03 前置：还没扫描过 → 「移到回收站」是禁用的（没有结果就没有可删项）"));

    rootsEdit->setPlainText(QDir::toNativeSeparators(dir));
    minSizeSpin->setValue(4);
    sampleSpin->setValue(1); // 抽样改成 1 KB：这个数字必须真的生效
    startButton->click();

    reporter.check(waitFor([table, statusLabel] {
                       return table->rowCount() > 0
                           && statusLabel->text().contains(QStringLiteral("确认"));
                   },
                           8000),
                   QStringLiteral("P2-03 面板扫描完成（后台线程 + 排队回界面）"), statusLabel->text());
    reporter.check(table->rowCount() == 3,
                   QStringLiteral("P2-03 表格列出同一组的 3 个文件（保留 1 + 建议删除 2）"),
                   QStringLiteral("表格 %1 行").arg(table->rowCount()));

    int keepRows = 0;
    int deleteRows = 0;
    QString keepDisplayed;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *stateItem = table->item(row, 0);
        const QTableWidgetItem *pathItem = table->item(row, 2);
        if (stateItem == nullptr || pathItem == nullptr) {
            continue;
        }
        if (stateItem->text() == QStringLiteral("保留")) {
            ++keepRows;
            keepDisplayed = pathItem->text();
        } else if (stateItem->text() == QStringLiteral("建议删除")) {
            ++deleteRows;
        }
    }
    reporter.check(keepRows == 1 && deleteRows == 2,
                   QStringLiteral("P2-03 表格的「处置」列：1 项保留、2 项建议删除"),
                   QStringLiteral("保留 %1 行 / 建议删除 %2 行").arg(keepRows).arg(deleteRows));
    reporter.check(keepDisplayed.endsWith(QStringLiteral("aaa_keep.bin")),
                   QStringLiteral("P2-03 保留的是字典序最小的那一份，且界面上说清楚了留哪个"), keepDisplayed);

    const QString stageText = statusLabel->text();
    const int sampledKb = firstNumberAfter(stageText, QStringLiteral("抽样读取"));
    const int totalKb = firstNumberAfter(stageText, QStringLiteral("共"));
    reporter.check(sampledKb > 0 && totalKb > 0 && sampledKb * 8 < totalKb,
                   QStringLiteral("P2-03 状态栏如实报出抽样字节数，且它远小于全量（抽样不是摆设）"),
                   stageText);
    reporter.check(recycleButton->text().contains(QStringLiteral("2 项")),
                   QStringLiteral("P2-03 按钮上写明将要移走几项（点之前就知道会发生什么）"),
                   recycleButton->text());

    // ---- 真的点一次：只许进回收站，不许硬删 ----
    const qint64 recycleBefore = recycleBinItemCount();
    recycleButton->click();
    reporter.check(waitFor(
                       [&dup1Path, &dup2Path] {
                           return !QFileInfo::exists(dup1Path) && !QFileInfo::exists(dup2Path);
                       },
                       8000),
                   QStringLiteral("P2-03 端到端：建议删除的 2 个文件已不在原目录"));
    reporter.check(QFileInfo::exists(keepPath) && QFileInfo::exists(otherPath)
                       && QFileInfo::exists(smallPath),
                   QStringLiteral("P2-03 端到端：保留项、内容不同的文件、被跳过的小文件一个都没动"),
                   QStringLiteral("目录里还剩 %1").arg(fileNames(dir).join(QStringLiteral("，"))));

    const qint64 recycleAfter = recycleBinItemCount();
    if (recycleBefore >= 0 && recycleAfter >= 0) {
        reporter.check(recycleAfter == recycleBefore + 2,
                       QStringLiteral("P2-03 端到端：回收站条目数 +2 —— 走的是回收站（用户能还原），不是硬删"),
                       QStringLiteral("回收站 %1 → %2").arg(recycleBefore).arg(recycleAfter));
    } else {
        reporter.info(QStringLiteral("P2-03 提示：本机查不到回收站条目数，"
                                     "只能以「文件已不在原目录」为准（少了硬删/入回收站的区分度）"));
    }
    reporter.check(statusLabel->text().contains(QStringLiteral("回收站")),
                   QStringLiteral("P2-03 状态栏说明去向是回收站（可还原）"), statusLabel->text());

    reporter.check(manager.setPluginEnabled(kDupId, false), QStringLiteral("P2-03 插件停用成功"));
}

// ---------------------------------------------------------------------------
//  P2-12 敏感文件粉碎
// ---------------------------------------------------------------------------

void runShredderCase(Reporter &reporter, WinEase::PluginManager &manager, const Sandbox &sandbox)
{
    reporter.info(QStringLiteral("---- P2-12 敏感文件粉碎 ----"));

    const QString dir = sandbox.dir(QStringLiteral("shred"));
    const QString secretPath = QDir(dir).filePath(QStringLiteral("secret.bin"));
    const QByteArray payload = patternBytes(256 * 1024, 0xAB);
    writeBytes(secretPath, payload);

    // ---- ① 覆写实现本身：先证明"写下去的字节真的变了" ----
    Cancel cancel;
    qint64 written = 0;
    int passes = 0;
    int verified = 0;
    QString error;
    reporter.check(Shred::overwriteAndVerify(secretPath, Shred::PassMode::ZeroSinglePass, 64 * 1024,
                                             true, &cancel, &written, &passes, &verified, &error),
                   QStringLiteral("P2-12 覆写 1 遍 0x00 且逐遍回读校验通过"), error);
    reporter.check(written == payload.size() && passes == 1 && verified == 1,
                   QStringLiteral("P2-12 覆写统计对得上（写入 256 KB、1 遍、校验 1 遍）"),
                   QStringLiteral("写入 %1 / %2 遍 / 校验 %3 遍")
                       .arg(written)
                       .arg(passes)
                       .arg(verified));
    const QByteArray afterOverwrite = readBytes(secretPath);
    reporter.check(afterOverwrite.size() == payload.size()
                       && afterOverwrite == QByteArray(payload.size(), '\0'),
                   QStringLiteral("P2-12 端到端：磁盘上的这 256 KB 真的变成了全 0（不是只动了文件大小/元数据）"),
                   QStringLiteral("回读 %1 字节").arg(afterOverwrite.size()));

    // ---- ② 覆写方式的语义 ----
    reporter.check(Shred::passCount(Shred::PassMode::DodThreePass) == 3
                       && Shred::passDescriptions(Shred::PassMode::DodThreePass).size() == 3,
                   QStringLiteral("P2-12 三遍方式如实报出 3 遍，并逐遍说明写的是什么"),
                   Shred::passDescriptions(Shred::PassMode::DodThreePass).join(QStringLiteral(" → ")));
    reporter.check(Shred::passModeFromKey(Shred::passModeKey(Shred::PassMode::RandomThreePass))
                       == Shred::PassMode::RandomThreePass,
                   QStringLiteral("P2-12 覆写方式的键名可双向转换（配置存得下、读得回）"));

    // ---- ③ 安全阀：系统目录与盘根一律拒绝 ----
    QString reason;
    reporter.check(Shred::isProtectedPath(QStringLiteral("C:\\Windows"), &reason),
                   QStringLiteral("P2-12 安全阀：系统目录被拒绝"), reason);
    reporter.check(Shred::isProtectedPath(QStringLiteral("C:\\"), &reason),
                   QStringLiteral("P2-12 安全阀：盘根被拒绝"), reason);
    reporter.check(!Shred::isProtectedPath(secretPath, &reason),
                   QStringLiteral("P2-12 安全阀：临时目录里的普通文件不会被误伤"), reason);

    // ---- ④ 批量：被拒绝的那项如实上报，其余照做 ----
    const QString treeDir = sandbox.dir(QStringLiteral("shred_tree"));
    writeBytes(QDir(treeDir).filePath(QStringLiteral("a.bin")), patternBytes(4096, 1));
    writeBytes(QDir(treeDir).filePath(QStringLiteral("b.bin")), patternBytes(4096, 2));
    const QString onePath = QDir(dir).filePath(QStringLiteral("one.bin"));
    writeBytes(onePath, patternBytes(4096, 3));

    Shred::Options options;
    options.mode = Shred::PassMode::RandomSinglePass;
    options.renameBeforeDelete = true;
    options.verifyEachPass = false;
    const Shred::Result batch = Shred::shredPaths(
        QStringList({ QStringLiteral("C:\\Windows\\notepad.exe"), onePath, treeDir }), options,
        &cancel, Progress());
    reporter.check(batch.stats.skipped.size() == 1 && !QFileInfo::exists(onePath)
                       && !QFileInfo::exists(treeDir),
                   QStringLiteral("P2-12 端到端：批量粉碎中被拒的那项如实报告，其余照做（文件与目录都不在了）"),
                   batch.stats.summaryText());
    reporter.check(batch.stats.renamedFiles >= 3,
                   QStringLiteral("P2-12 端到端：删除前先改名这一步真的执行了（按原名恢复目录项也失效）"),
                   QStringLiteral("改名 %1 个").arg(batch.stats.renamedFiles));

    // ---- ⑤ 面板端到端 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kShredId);
    reporter.check(plugin != nullptr, QStringLiteral("P2-12 插件已从 plugins 目录加载（security.file_shredder）"));
    if (plugin == nullptr) {
        return;
    }
    StatusLog status;
    status.attach(plugin);
    reporter.check(manager.setPluginEnabled(kShredId, true), QStringLiteral("P2-12 插件启用成功"));
    status.clear();

    reporter.check(FeatureSmoke::dispatchAction(manager, kShredId, QStringLiteral("default")),
                   QStringLiteral("P2-12 经快捷键入口打开粉碎面板"));
    QWidget *panel = nullptr;
    reporter.check(waitFor([&panel] {
                       panel = topLevelNamed(QStringLiteral("winease_file_shredder"));
                       return panel != nullptr;
                   }),
                   QStringLiteral("P2-12 面板窗口已出现"));
    if (panel == nullptr) {
        manager.setPluginEnabled(kShredId, false);
        return;
    }

    auto *disclosure = childNamed<QLabel>(panel, QStringLiteral("shredDisclosure"));
    auto *pathsEdit = childNamed<QPlainTextEdit>(panel, QStringLiteral("shredderPaths"));
    auto *modeCombo = childNamed<QComboBox>(panel, QStringLiteral("shredderMode"));
    auto *ackCheck = childNamed<QCheckBox>(panel, QStringLiteral("shredderAcknowledge"));
    auto *renameCheck = childNamed<QCheckBox>(panel, QStringLiteral("shredderRename"));
    auto *verifyCheck = childNamed<QCheckBox>(panel, QStringLiteral("shredderVerify"));
    auto *startButton = childNamed<QPushButton>(panel, QStringLiteral("shredderStart"));
    auto *statusLabel = childNamed<QLabel>(panel, QStringLiteral("shredderStatus"));
    const bool allFound = disclosure != nullptr && pathsEdit != nullptr && modeCombo != nullptr
        && ackCheck != nullptr && renameCheck != nullptr && verifyCheck != nullptr
        && startButton != nullptr && statusLabel != nullptr;
    reporter.check(allFound, QStringLiteral("P2-12 面板控件按对象名全部找到"));
    if (!allFound) {
        manager.setPluginEnabled(kShredId, false);
        return;
    }

    // 这是本工具集里唯一不可撤销的操作：说明必须写在明面上，且说清"防不了什么"
    reporter.check(disclosure->text().contains(QStringLiteral("无法恢复"))
                       && disclosure->text().contains(QStringLiteral("TRIM"))
                       && disclosure->text().contains(QStringLiteral("卷影副本")),
                   QStringLiteral("P2-12 面板正文原样说明「无法恢复、不提供撤销」与「防不了 SSD 磨损均衡/TRIM、卷影副本」"));
    reporter.check(modeCombo->currentData().toString() == QStringLiteral("zero1")
                       && renameCheck->isChecked() && verifyCheck->isChecked()
                       && !ackCheck->isChecked(),
                   QStringLiteral("P2-12 面板打开时的默认值来自配置（覆写方式 zero1、改名开、回读校验开），"
                                  "风险确认默认不勾"),
                   QStringLiteral("方式 %1 / 改名 %2 / 校验 %3 / 确认 %4")
                       .arg(modeCombo->currentData().toString())
                       .arg(renameCheck->isChecked())
                       .arg(verifyCheck->isChecked())
                       .arg(ackCheck->isChecked()));

    const QString panelSecret = QDir(dir).filePath(QStringLiteral("panel_secret.bin"));
    writeBytes(panelSecret, patternBytes(8192, 5));
    pathsEdit->setPlainText(QDir::toNativeSeparators(panelSecret));

    // 未勾风险确认 → 拒绝执行（不可撤销操作的最后一道闸）
    startButton->click();
    reporter.check(statusLabel->text().contains(QStringLiteral("勾选")),
                   QStringLiteral("P2-12 未勾「无法恢复」确认 → 面板拒绝执行"), statusLabel->text());
    reporter.check(QFileInfo::exists(panelSecret),
                   QStringLiteral("P2-12 被拒绝时文件一个字节都没动"));

    ackCheck->setChecked(true);
    modeCombo->setCurrentIndex(0); // 自检用一遍 0x00（够证明流程，不浪费磁盘时间）
    startButton->click();
    reporter.check(waitFor([statusLabel] {
                       return statusLabel->text().contains(QStringLiteral("粉碎完成"));
                   },
                           15000),
                   QStringLiteral("P2-12 端到端：面板完成粉碎（工作线程 + 排队回界面）"), statusLabel->text());
    reporter.check(!QFileInfo::exists(panelSecret),
                   QStringLiteral("P2-12 端到端：文件已从磁盘上消失"));
    reporter.check(statusLabel->text().contains(QStringLiteral("成功 1 个文件")),
                   QStringLiteral("P2-12 状态栏如实报告成功项数"), statusLabel->text());
    reporter.check(statusLabel->text().contains(QStringLiteral("回读校验")),
                   QStringLiteral("P2-12 状态栏报出回读校验遍数（面板默认开着校验）"));
    reporter.check(status.contains(QStringLiteral("粉碎完成")),
                   QStringLiteral("P2-12 结果通过状态文本上报（卡片上能看到）"), status.last());

    reporter.check(manager.setPluginEnabled(kShredId, false), QStringLiteral("P2-12 插件停用成功"));
}

} // namespace

int runFileGroupTests(Reporter &reporter,
                      WinEase::PluginManager &manager,
                      StubServices &services,
                      ProbeWindow &probe)
{
    Q_UNUSED(probe);
    Q_UNUSED(services); // 本组的配置预设写在 main.cpp（插件在 loadPlugins 时就把它读走了）
    const int failuresAtStart = reporter.failures();

    const Sandbox sandbox;
    reporter.info(QStringLiteral("文件组沙盒：%1").arg(sandbox.root()));
    reporter.info(QStringLiteral("说明：查重用例会真的把 2 个 64 KB 的自检临时文件移进回收站"
                                 "（产品硬约束是「只进回收站」，不点一次等于没测；移进去无法程序化还原）"));

    // 快速预览用例要借剪贴板当"入口"（用户在资源管理器里复制的就是这个）：
    // 只写纯文本，且用完由守卫还原，不动用户原来的图片/文件类剪贴板内容
    const ClipboardGuard clipboard;

    runBatchRenameCase(reporter, manager, sandbox);
    runQuickLookCase(reporter, manager, sandbox, clipboard);
    runDuplicateFinderCase(reporter, manager, sandbox);
    runShredderCase(reporter, manager, sandbox);

    reporter.info(QStringLiteral("文件组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
