#pragma once

// ============================================================================
//  FileConvertPlugin —— P3-03 批量格式转换（file.convert）
//
//  解决的问题：**"这一堆图要换成另一种格式 / 压小一点 / 加个水印，一张张另存太烦"**，
//  以及中文用户的老毛病：**"老程序导出的 GBK 文本在新工具里全是乱码"**。
//
//  两种转换类型共用同一套外壳（文件清单 / 输出目录 / 命名模板 / 冲突策略 /
//  进度条 / 取消 / 逐条结果），只有"选项区"不同：
//      * 图片：目标格式 / 质量 / 缩放 / 水印 / 保留元数据
//      * 文本：目标字符集 / 换行风格
//
//  ---------------------------------------------------------------------------
//  六条设计纪律（细节与理由都写在 ImageConvertEngine.h / TextEncodingTools.h 里）：
//
//   1. **绝不原地覆盖源文件**：目标与源重合的项直接标出来、不转换。
//   2. **计划先于执行**：面板上随时显示"将转换几张 / 几张有问题 / 输出到哪"，
//      用户点"开始"之前就知道会发生什么；执行用的是**同一份计划**。
//   3. **有损转换必须提前说**（JPG/WebP 丢透明、丢细节；GBK 装不下 emoji）。
//   4. **能取消**：转换跑在工作线程上，取消只在"每个文件开工前"生效 ——
//      已经开始的那一张一定写完，磁盘上不会留下半张图。
//   5. **失败项逐条列出**：不做"跳过 3 个"这种总结。
//   6. **不原地重写、不做隐式覆盖**：这条与 P2-01 批量重命名系列保持一致。
//
//  ---------------------------------------------------------------------------
//  ⚠ 两处有意裁剪（与 P3-01 / P3-02 同理，写在这里以免以后被当成"漏做"）：
//    * **不做资源管理器右键菜单集成**：WinEase.exe 没有"带参数打开某个插件面板"
//      的入口（裁剪 C9），为这一项去改主程序启动流程与单实例转发不划算。
//      面板支持**拖入文件/文件夹**、**文件对话框**、**从剪贴板取路径**三条入口。
//    * **不声明全局快捷键**（裁剪见 ROADMAP C11）：批量任务的对象是"一批文件"，
//      快捷键按下时无从得知用户想转哪一批；而"转换剪贴板里的图"这种入口会
//      在自检里覆盖用户的剪贴板图片（还原不了原图），代价大于收益。
//
//  ⚠ 元数据边界（如实写在界面上）：Qt 读图只保留"文本型图像属性 + ICC 色彩配置 +
//    文件修改时间"，**EXIF 拍摄信息在把 JPEG 解成像素时就丢了** —— 本功能不假装能保住它。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

// 两套引擎与插件**共用同一份源码**（自检断言的才是用户跑的那一套）
// ⚠ 插件构建把 plugins/common 直接加进了 include 路径（见 cmake/WinEasePlugin.cmake），
//   所以这里写扁平名字
#include "ImageConvertEngine.h"
#include "TaskCancel.h"
#include "TextEncodingTools.h"

#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>
#include <thread>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;

class FileConvertPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "convert_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    /// 转换类型：两套选项共用一个外壳
    enum class Kind { Image = 0, Text };

    explicit FileConvertPlugin(QObject *parent = nullptr);
    ~FileConvertPlugin() override;

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QString detailedDescription() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    void onDisable() override;
    /// 面板支持"把文件/文件夹拖进来"（QWidget 的拖动事件只能在 QObject 层拦）
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ---- 清单 ----
    /// 加入路径（文件或文件夹都收；文件夹按 `recursive` 展开）
    void addPaths(const QStringList &paths);
    void addFilesViaDialog();
    void addFolderViaDialog();
    /// 从剪贴板取路径（一行一个；与 P1-12 / P3-02 同一形态）
    void addPathsFromClipboard();
    void clearPaths();
    void removeSelectedPaths();
    /// 重填清单控件（**只在数据变化时调用**）
    void refreshFileList();

    // ---- 计划 ----
    /// 由界面控件组装图片选项
    WinEase::FeaturePlugins::ImageConvertEngine::Options currentImageOptions() const;
    /// 组装文本选项
    void currentTextOptions(WinEase::FeaturePlugins::TextEncodingTools::Encoding *encodingOut,
                            WinEase::FeaturePlugins::TextEncodingTools::LineEnding *lineEndingOut) const;
    /// 重算计划并刷新"将转换…"那一行（清单或选项变化时调用）
    void refreshPlan();
    /// 当前是否处于"文本模式"
    bool isTextMode() const;
    /// 按模式切换两组选项的可见性
    void updateOptionVisibility();

    // ---- 执行 ----
    void startConversion();
    void requestCancel();
    /// 工作线程回报进度（**必须排队回界面线程**）
    void updateProgress(const QString &stage, int done, int total);
    void finishConversion(bool cancelled, const QStringList &lines, const QString &summary,
                          const QString &error);
    /// 取消 + 收尸（停用 / 关闭 / 析构都要走）
    void stopWorker();
    /// 开新线程前收掉上一轮已跑完的线程对象（`std::thread` 赋值给 joinable 的自己 = terminate）
    void reapFinishedWorker();

    // ---- 配置 ----
    void loadSettings();
    void saveSettings();

    // ---- 界面 ----
    void setStatus(const QString &text);

    // ---- 数据 ----
    QStringList m_paths;         ///< 用户加入的文件（去重后的绝对路径）
    bool m_running = false;      ///< 转换是否在跑（跑的时候不许再点"开始"）
    QString m_lastSummary;       ///< 上一次转换的结果摘要（自检要断言"动作被消费了"）
    QStringList m_lastResultLines; ///< 上一次转换的逐条结果
    int m_lastConverted = 0;
    int m_lastFailed = 0;

    QPointer<QWidget> m_panel;
    QPointer<QListWidget> m_fileList;
    QPointer<QLabel> m_planLabel;
    QPointer<QLabel> m_stageLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QListWidget> m_resultList;
    QPointer<QProgressBar> m_progress;
    QPointer<QComboBox> m_kindCombo;
    QPointer<QGroupBox> m_imageGroup;
    QPointer<QGroupBox> m_textGroup;
    QPointer<QComboBox> m_formatCombo;
    QPointer<QSpinBox> m_qualitySpin;
    QPointer<QComboBox> m_resizeCombo;
    QPointer<QSpinBox> m_resizeSpin;
    QPointer<QLineEdit> m_watermarkEdit;
    QPointer<QComboBox> m_watermarkPosCombo;
    QPointer<QSpinBox> m_watermarkOpacitySpin;
    QPointer<QSpinBox> m_watermarkSizeSpin;
    QPointer<QCheckBox> m_keepMetadataCheck;
    QPointer<QComboBox> m_textEncodingCombo;
    QPointer<QComboBox> m_lineEndingCombo;
    QPointer<QRadioButton> m_sameDirRadio;
    QPointer<QRadioButton> m_customDirRadio;
    QPointer<QLineEdit> m_outputDirEdit;
    QPointer<QPushButton> m_outputBrowseButton;
    QPointer<QLineEdit> m_templateEdit;
    QPointer<QSpinBox> m_numberStartSpin;
    QPointer<QComboBox> m_conflictCombo;
    QPointer<QCheckBox> m_recursiveCheck;
    QPointer<QPushButton> m_startButton;
    QPointer<QPushButton> m_cancelButton;

    std::shared_ptr<WinEase::FeaturePlugins::Cancel> m_cancel;
    std::thread m_worker;
};
