#pragma once

// ============================================================================
//  TextFormatPlugin —— P1-10 JSON / XML 格式化
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+J）把**选中文本**（取不到则用剪贴板内容）
//      识别为 JSON 或 XML 后格式化，结果写回剪贴板；
//      默认还会发一次 Ctrl+V 就地替换选中内容（可关，见 pasteBack 配置）
//    * Ctrl+Shift+Alt+J：压缩（去掉所有可省的空白）
//    * 设置面板里还有：强制按 JSON/XML、转义/去转义、JSONPath 查询
//
//  ⚠ 一条硬规则：**转换失败时绝不动剪贴板**。
//      用户手里的原文还在原地，才能照着"第 3 行第 12 列"去改。
//      失败只通过状态文本 + 气泡说明原因。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "TextTools.h"

class TextFormatPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "text_format_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit TextFormatPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 完整流水线：取文本 → 转换 → 写回剪贴板（+ 可选就地替换）
    /// @param label 状态文本里的动作名，如"格式化"
    void runPipeline(const QString &action, const QString &label);

    int m_indent = 2;
    bool m_useSelection = true; ///< 是否先尝试"复制选中"
    bool m_pasteBack = true;    ///< 是否把结果粘贴回原处（就地替换）
};
