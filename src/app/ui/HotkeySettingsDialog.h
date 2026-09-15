#pragma once

// ============================================================================
//  HotkeySettingsDialog.h —— 全局快捷键设置对话框
//
//  以表格形式列出全部已登记快捷键（主程序 + 各插件），支持：
//    * 双击/选择后按键盘录入新的按键组合（QKeySequenceEdit）
//    * 冲突检测与错误提示
//    * 清除绑定（留空表示不启用）
//    * 一键恢复各插件声明的默认快捷键
// ============================================================================

#include <QDialog>
#include <QHash>
#include <QString>

class QPushButton;
class QLabel;
class QTableWidget;
class QKeySequenceEdit;

namespace WinEase {

class GlobalHotkeyManager;
class PluginManager;

class HotkeySettingsDialog : public QDialog
{
    Q_OBJECT

public:
    HotkeySettingsDialog(GlobalHotkeyManager *hotkeyManager,
                         PluginManager *pluginManager,
                         QWidget *parent = nullptr);
    ~HotkeySettingsDialog() override;

public Q_SLOTS:
    /// 重新从管理器拉取绑定列表（每次显示前调用）
    void reload();

private Q_SLOTS:
    void onCurrentBindingChanged();
    /// 编辑框内容变化时实时做"合法性 + 冲突"校验
    void onSequenceEdited();
    void onApplyClicked();
    void onClearClicked();
    void onRestoreDefaultsClicked();

private:
    void setupUi();
    void fillTable();
    void refreshHintStyle();
    QString selectedHotkeyId() const;

    GlobalHotkeyManager *m_hotkeyManager = nullptr;
    PluginManager *m_pluginManager = nullptr;

    QTableWidget *m_table = nullptr;
    QKeySequenceEdit *m_sequenceEdit = nullptr;
    QLabel *m_hintLabel = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_defaultsButton = nullptr;

    /// hotkeyId -> 默认按键文本（来自插件声明）
    QHash<QString, QString> m_defaults;
};

} // namespace WinEase
