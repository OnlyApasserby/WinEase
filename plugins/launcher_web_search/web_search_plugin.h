#pragma once

// ============================================================================
//  WebSearchPlugin —— P1-09 网页快速搜索
//
//  流程：取当前选中文本（取不到就回落到剪贴板）→ 拼搜索引擎 URL → 打开浏览器
//
//  三个设计要点：
//    1. **取词复用 `ClipboardTools::acquireText()`**：与文本格式化/编码转换插件
//       共用同一套"选中 → 回落剪贴板"的语义，不会出现"这个插件取得到、那个取不到"
//    2. **快捷前缀**：输入 `gh: qt widgets` 就用 GitHub 搜，`bd: 天气` 就用百度搜；
//       前缀大小写不敏感，中英文冒号都认（中文输入法下很容易打出全角冒号）
//    3. **dryRun（只复制链接）**：不打开浏览器，只把 URL 写进剪贴板 ——
//       这是产品上的实用模式（发给同事、贴到笔记里），也让自检可以逐字符校验 URL
// ============================================================================

#include "sdk/IFeaturePlugin.h"

class WebSearchPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "web_search_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit WebSearchPlugin(QObject *parent = nullptr);

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
    struct Engine {
        /// 别名列表（至少一个）。第一个用于展示，其余是方便输入的短别名：
        /// GitHub 同时认 "gh" 与 "github" —— 用户手打时几乎总是打短的
        QStringList aliases;
        QString name;
        QString urlTemplate; ///< 含 %1（查询词占位）

        QString primaryAlias() const { return aliases.isEmpty() ? QString() : aliases.first(); }
        QString aliasText() const { return aliases.join(QLatin1Char('/')); }
    };

    /// 引擎表（配置为空时用内置默认）
    QList<Engine> engines() const;
    /// 按别名找引擎（大小写不敏感）。
    /// ⚠ 按**值**返回而不是返回指针：引擎表每次现算（只有几条），
    ///   不能为了"省一点构造"去搞静态缓存 —— 静态库会被链接进多个模块，
    ///   静态缓存就是"每个模块一份全局状态"，而且返回的指针随时会失效
    bool findEngine(const QString &alias, Engine *engineOut) const;
    /// 解析"别名: 关键词"前缀；返回去掉前缀后的查询词
    static QString stripPrefix(const QString &text, QString *aliasOut);
    /// 拼接 URL（查询词做百分号编码）
    static QString buildUrl(const Engine &engine, const QString &query);

    /// 主流程：取词 → 选引擎 → 生成 URL → 打开或复制
    bool search(const QString &forceAlias, const QString &label);

    QString m_defaultEngine = QStringLiteral("bing");
    bool m_useSelection = true;
    bool m_dryRun = false; ///< true = 只把链接写进剪贴板，不打开浏览器
    QStringList m_customEngines; ///< "别名|名称|URL模板"
    QStringList m_lastUrls;      ///< 最近几条（便于排查拼错引擎）
};
