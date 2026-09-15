#pragma once

// ============================================================================
//  EncoderPlugin —— P1-11 编码转换
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+E）把选中文本（取不到则用剪贴板）做 Base64 编码，
//      结果写回剪贴板（默认再发一次 Ctrl+V 就地替换）
//    * Ctrl+Shift+Alt+E：一次算出 MD5 / SHA-1 / SHA-256 / SHA-512（四行，带名字）
//    * 设置面板：Base64 / URL 编解码双向即时转换 + 哈希表，一键复制
//
//  为什么哈希一次给四种：
//      用户来查摘要时，通常并不知道自己需要哪一种（要看对方系统收哪一种）；
//      一次全给比"选错了再来一遍"省事，反正开销可以忽略。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "TextTools.h"

class EncoderPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "encoder_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit EncoderPlugin(QObject *parent = nullptr);

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
    /// 取文本 → 转换 → 写回（+ 可选就地替换）
    void runPipeline(const QString &action, const QString &label);

    bool m_useSelection = true;
    bool m_pasteBack = true;
};
