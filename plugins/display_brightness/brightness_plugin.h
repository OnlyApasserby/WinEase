#pragma once

// ============================================================================
//  brightness_plugin.h —— P2-08 亮度 / 色温 / 护眼模式（display.brightness）
//
//  三项功能合成一个插件（D4 决策）：它们共用**同一套显示状态**，
//  拆成两个插件会互相覆盖（护眼模式按时间改色温，而手动色温也在改同一条 gamma 管线）。
//
//  两条通路各管一件事，互不干扰：
//      亮度 → 硬件通路（外接屏 DDC/CI、内置屏 WMI），不动 gamma
//      色温/护眼 → 显卡 gamma ramp，不动硬件亮度
//
//  ---------------------------------------------------------------------------
//  三条设计纪律（都能在自检里被断言）：
//
//  1. **亮度不自己记状态**：面板上的亮度永远是**刚从系统读回来的真值**
//     （`readBrightnessPercent`），不是"我们上次写进去的值"。用户完全可能用
//     Fn 键、系统托盘、外接显示器自己的按钮改亮度 —— 我们记的那个值立刻就是错的。
//     所以亮度也**不持久化**。
//  2. **色温是叠在系统上的滤镜**：启用时捕获每块屏的原始 ramp 当基准，
//     之后每次调节都基于这份基准重算（否则多次调节会层层叠加把画面搞花），
//     停用时**逐元素还原**到基准 —— 这是"可完整还原"在停用上的落点。
//  3. **停用不还原亮度**（与 mic_mute 的 fail-closed 例外同类）：亮度是硬件状态，
//     屏幕现在多亮就是用户看到的样子，停用一项功能时把它改回去反而是新的副作用。
//     色温必须还原，亮度必须不还原 —— 两条都要被自检钉住。
//
//  ---------------------------------------------------------------------------
//  6500K 的含义：面板上的色温范围 2500~6500 K，**6500K 就是"不调色"档**
//  （直接还原原始 ramp）。理由是 `colorTemperatureGains(6500)` 并不等于
//  一组全 1.0 的增益（实测 R 1.000 / G 0.997 / B 0.981），套上去会引入
//  谁也说不清的轻微偏色；既然用户选的是"最不暖"的那一档，那就干脆当作不改变。
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/DisplayControl.h"

#include <QList>
#include <QPointer>
#include <QTime>

class QCheckBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTimeEdit;
class QTimer;
class QWidget;

class BrightnessPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "brightness_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit BrightnessPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    // ---------------- 亮度 ----------------
    /// 逐屏重新解析亮度通路（DDC/CI 优先，失败回退 WMI 内置屏）
    void rescanChannels();
    /// 面板/快捷键请求改亮度：走 150ms 防抖（拖滑块不该产生几十次硬件写入）
    void requestBrightness(double percent);
    /// 真正写硬件：逐屏写 → **回读确认** → 用读回的真值刷新界面
    void applyBrightness(double percent);
    /// 轮询真值：Fn 键 / 系统托盘 / 显示器自己的按钮改过亮度时，面板上的数字要能跟上
    void pollBrightness();
    void syncPollTimer();

    // ---------------- 色温 / 护眼 ----------------
    /// 把"当前应有的色温"应用到所有屏（基于启用时捕获的基准）
    void applyColorTemperature();
    /// 时段色温没变、且盘上还是我们写下去的那份 ramp → 没必要重复写 gamma
    bool colorTemperatureSettled() const;
    /// 当前应有的色温：护眼开启时由时段决定，否则用手动值
    int scheduledKelvin() const;
    bool isNightTime(const QTime &now) const;
    /// 下一次时段的切换时刻（状态行显示用）
    QTime nextSwitchTime(const QTime &now) const;

    void setKelvin(int kelvin, bool persist);
    void setEyeCareEnabled(bool enabled, bool persist);
    void startScheduleTimer();
    void stopScheduleTimer();

    // ---------------- 面板 ----------------
    void refreshPanel();
    QString statusText() const;
    QString channelText() const;
    QString scheduleText() const;
    void updatePreviewColor();
    void syncKelvinControls();

    // ---------------- 状态 ----------------
    QList<WinEase::Win32::BrightnessChannel> m_channels;
    /// 启用时捕获的原始 gamma（每屏一份）——停用还原的唯一依据
    QList<WinEase::Win32::GammaSnapshot> m_gammaBaselines;
    /// 我们**写下去之后读回来**的 gamma（每屏一份）：
    /// 系统（锁屏 / 切换显示模式）或别的调色软件可能把它重置，光靠"我记得我改过"不算数
    QList<WinEase::Win32::GammaSnapshot> m_appliedRamps;
    /// 上一次应用色温是否全军覆没（用来避免每分钟一次的轮询把日志刷爆）
    bool m_lastApplyFailed = false;

    double m_brightnessPercent = 0.0; ///< **真值**，来自系统读回
    int m_kelvin = 6500;              ///< 手动色温（6500 = 不调色）
    int m_appliedKelvin = -1;         ///< 最近一次真正写进 gamma 的色温
    bool m_eyeCareEnabled = false;
    int m_dayKelvin = 6500;
    int m_nightKelvin = 3400;
    QTime m_nightStart = QTime(21, 0);
    QTime m_nightEnd = QTime(7, 0);

    QTimer *m_brightnessTimer = nullptr; ///< 亮度防抖（150ms）
    QTimer *m_scheduleTimer = nullptr;   ///< 护眼时段轮询（60s）
    QTimer *m_pollTimer = nullptr;       ///< 亮度真值轮询（5s，只读不写）

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_channelLabel;
    QPointer<QLabel> m_scheduleLabel;
    QPointer<QLabel> m_previewLabel;
    QPointer<QSlider> m_brightnessSlider;
    QPointer<QSpinBox> m_brightnessSpin;
    QPointer<QSlider> m_kelvinSlider;
    QPointer<QSpinBox> m_kelvinSpin;
    QPointer<QCheckBox> m_eyeCareCheck;
    QPointer<QSpinBox> m_dayKelvinSpin;
    QPointer<QSpinBox> m_nightKelvinSpin;
    QPointer<QTimeEdit> m_nightStartEdit;
    QPointer<QTimeEdit> m_nightEndEdit;
};
