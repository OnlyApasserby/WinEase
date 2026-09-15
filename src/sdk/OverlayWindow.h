#pragma once

// ============================================================================
//  OverlayWindow.h —— OverlayKit：悬浮层窗口基类（P0-3）
//
//  一个"透明贴在所有窗口之上、默认不接收鼠标、不抢焦点"的悬浮层，
//  供屏幕标尺、焦点高亮、放大镜、硬件监控悬浮窗、鼠标手势轨迹等功能复用。
//
//  ---------------------------------------------------------------------------
//  为什么用 QWidget 而不是 QML（这是对原始设想的一处有意偏离，理由如下）
//
//    1. **插件契约一致性**：现有插件契约是 QWidget 体系的
//       （IFeaturePlugin::createSettingsWidget(QWidget*)）。C++ 基类可直接继承；
//       若用 QML，5 个使用方各自都要加载 QML 引擎、携带 .qml 资源，
//       而这 5 个功能的绘制内容都只是"简单二维矢量图形"，收益为零、风险很大。
//    2. **只保留一套渲染栈**：引入 Qt6::Quick 会让主程序同时存在两种渲染栈
//       （raster + 场景图），且每个插件 DLL 各有一个 QML 引擎
//       ——这与本工程"绝不允许多份全局状态"的核心原则直接冲突。
//    3. **DPI 处理集中在一处**：`WinEaseWin32` 已经定义了"物理像素 + 屏幕坐标"
//       的坐标契约与 toLogical/toPhysical 换算，QWidget 的 devicePixelRatio 与之
//       天然对齐；QQuickWindow 有自己的 DPR 规则，混用是"高 DPI 下模糊"的最大风险源。
//    4. **性能可达**：5 个使用方中 3 个是低频静态绘制、1 个是位图贴图（放大镜，
//       瓶颈在抓屏而不在绘制）、1 个是折线轨迹（可用后备位图增量绘制）。
//       配合脏矩形更新完全够用，不需要场景图的渲染线程。
//    5. **保留 QML 通道**：`setContentWidget()` 允许将来把 QQuickWidget 嵌入进来，
//       基类本身不依赖 Qt6::Quick。
//  ---------------------------------------------------------------------------
//
//  ⚠ 坐标契约（违反必然在高 DPI 下错位）：
//      * 本类所有"物理像素"接口（setOverlayGeometry / overlayGeometry /
//        coverMonitor 等）一律为 **物理像素 + 屏幕坐标**，与 WinEaseWin32 一致
//      * `paintOverlay()` 的 painter 与 QRect 是 **逻辑坐标**（96 DPI 基准），
//        调用方不必也不应自己做缩放——这正是"高 DPI 下不模糊"的实现方式：
//        窗口按显示器缩放系数分配到原生分辨率的绘制表面，
//        再用缩放变换把逻辑坐标映射上去
//
//  ⚠ 跨 DPI 的硬限制（重要）：
//      一个窗口只能有一个 devicePixelRatio。若用**单个**悬浮层横跨两台不同缩放
//      的显示器（coverAllScreens），只有窗口实际所在的那台是原生清晰的，
//      另一台上会被 DWM 整体缩放而模糊——这是 Windows 的限制，Qt 无法绕过。
//      需要处处清晰时，必须**为每个显示器各建一个悬浮层**：
//          overlayHost()->createOverlaysForAllMonitors(ownerId);
//
//  ⚠ 线程约束：悬浮层只能在 GUI 线程创建与访问。
//
//  典型用法（以"鼠标手势轨迹"为例）：
//
//      class TrailOverlay : public WinEase::OverlayWindow
//      {
//      public:
//          explicit TrailOverlay(QWidget *parent = nullptr) : OverlayWindow(parent)
//          {
//              setClickThrough(true);      // 不能挡住用户操作
//              coverMonitor(0);            // 铺满主显示器
//              show();
//          }
//          void addPoint(const QPoint &logicalPoint)
//          {
//              QPixmap *layer = backingLayer();       // 累积绘制，不必整屏重绘
//              QPainter painter(layer);               // 位图已带 DPR，直接用逻辑坐标
//              painter.setPen(QPen(Qt::red, 3));
//              painter.drawLine(m_last, logicalPoint);
//              m_last = logicalPoint;
//              requestOverlayUpdate();                 // 或传入脏矩形
//          }
//      private:
//          QPoint m_last;
//      };
// ============================================================================

#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

class QHideEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QPixmap;
class QResizeEvent;
class QShowEvent;

namespace WinEase {

class OverlayWindow : public QWidget
{
    Q_OBJECT

public:
    explicit OverlayWindow(QWidget *parent = nullptr);
    ~OverlayWindow() override;

    OverlayWindow(const OverlayWindow &) = delete;
    OverlayWindow &operator=(const OverlayWindow &) = delete;

    // ========================================================================
    //  一、窗口行为
    // ========================================================================

    /// 点击穿透（默认 true）：整层不参与鼠标命中测试，下层窗口照常响应点击。
    /// ⚠ 显示之后切换本开关走的是"直接改扩展样式"的路径（不重建原生窗口、无闪烁）；
    ///   一旦用本方法切换过，就不要再调用 setWindowFlags()——那会重建原生窗口
    ///   并把状态重置回窗口标志里记录的值。
    ///
    /// ⚠⚠ 一个必须知道的语义（实测得来，否则会做出"点了没反应"的交互层）：
    ///   半透明悬浮层是**分层窗口（WS_EX_LAYERED）**，Windows 对它的命中测试是
    ///   **逐像素按 alpha 判定**的——alpha == 0 的区域无论有没有 WS_EX_TRANSPARENT
    ///   都不会被命中。
    ///   推论：想把 clickThrough 关掉做成可交互层时，**必须在该区域真正画出不透明
    ///   内容**；只画一圈边框、中间全透明的"焦点高亮"式悬浮层，即使关闭穿透，
    ///   中间也依然点不到（这是 Windows 的行为，不是本类的缺陷）。
    bool isClickThrough() const;
    void setClickThrough(bool on);

    /// 不抢焦点（默认 true）：显示与点击都不激活本窗口，也不出现在 Alt+Tab。
    bool isNoActivate() const;
    void setNoActivate(bool on);

    // ========================================================================
    //  二、放置（参数一律为物理像素）
    // ========================================================================

    /// 铺满指定显示器（索引对应 Win32::monitors() 的顺序）
    bool coverMonitor(int monitorIndex);

    /// 铺满包含指定物理点的显示器
    bool coverMonitorAt(const QPoint &physicalScreenPoint);

    /// 铺满整个虚拟桌面（多屏拼接）。
    /// ⚠ 跨不同 DPI 显示器时会模糊，见文件头的"跨 DPI 的硬限制"
    bool coverAllScreens();

    /// 指定物理像素矩形（内部按目标显示器的缩放系数换算为逻辑像素）。
    ///
    /// ⚠ 取整损耗与"宁大勿小"策略：
    ///   Qt 的窗口几何是**整数逻辑像素**，非整数缩放下无法精确命中目标尺寸。
    ///   例如 2560 物理宽 @150% → 1706.67 逻辑 → 取整 1707 → 换算回 2561 物理，
    ///   即比目标**多 1 像素**。实测取向始终是"不小于目标"，
    ///   因此覆盖类用途不会在边缘留下缝隙（这是刻意的：漏缝比重 1 像素严重得多）。
    ///   需要精确尺寸的场景（例如按像素对齐的截图框）请自行使用
    ///   `WinEaseWin32` 的物理坐标做裁剪，不要把窗口几何当作精确依据。
    bool setOverlayGeometry(const QRect &physicalScreenRect);

    /// 实际生效的物理像素矩形（通过 GetWindowRect 读回，而非回读请求值）
    QRect overlayGeometry() const;

    /// 当前所在显示器的缩放系数（1.0 = 100%，1.5 = 150%）
    qreal overlayScaleFactor() const;

    /// 悬浮层的逻辑矩形（原点为左上角；绘制用，尺寸 = 逻辑像素）
    QRect overlayLogicalRect() const;

    // ========================================================================
    //  三、绘制契约
    // ========================================================================

    /// 在此绘制悬浮内容。painter 已配置好渲染提示，坐标系为**逻辑坐标**，
    /// 原点为悬浮层左上角；在高 DPI 屏上会自动映射到更高的物理分辨率。
    /// @param dirtyLogicalRect 需要重绘的区域（逻辑坐标），可用于裁剪优化
    virtual void paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect);

    /// 请求重绘；传空矩形表示整层重绘。
    /// **只更新变化区域**是悬浮层流畅的关键（全屏半透明窗口的整层重绘代价很高）
    void requestOverlayUpdate(const QRect &dirtyLogicalRect = QRect());

    /// 后备位图（惰性创建，尺寸 = 逻辑尺寸 × 显示器缩放系数，已设置 devicePixelRatio）。
    /// 适合"内容只增不改"的绘制（鼠标手势轨迹、逐段累积的标注）：
    /// 把历史内容留在位图里，每次只把新增的一笔画上去，避免整层重绘。
    /// ⚠ 尺寸随窗口变化会自动重建（内容丢失），跨显示器移动后需要重画
    QPixmap *backingLayer();

    /// 清空后备位图
    void clearBackingLayer();

    /// 把后备位图按当前缩放绘制到 painter（本类默认在 paintOverlay() 之前自动调用）
    void paintBackingLayer(QPainter &painter);

    /// 是否自动绘制后备位图（默认 true）。设为 false 时由子类自行决定绘制时机
    void setBackingLayerAutoPaint(bool on);
    bool isBackingLayerAutoPaint() const;

    // ========================================================================
    //  四、可选内容控件（保留 QML 等逃生通道）
    // ========================================================================

    /// 把一个控件铺满悬浮层。传入 QQuickWidget 即可在悬浮层里使用 QML，
    /// 但基类本身不依赖 Qt6::Quick。
    /// @return 被接管的控件（所有权移交本窗口）
    QWidget *setContentWidget(QWidget *content);

    // ========================================================================
    //  五、生命周期
    // ========================================================================

    bool isOverlayVisible() const;

    /// 关闭并销毁悬浮层：先隐藏再走一次事件循环，最后 deleteLater()。
    /// "先隐藏"是必要的——半透明分层窗口若直接销毁，DWM 可能残留一帧画面（残影）。
    /// 可重复调用
    void closeOverlay();

    /// 是否为本工程的悬浮层窗口（供宿主与自检识别，基于窗口属性标记）。
    /// nativeHandle 为 Win32 的 HWND，用 void* 传递以免本头文件必须包含 windows.h
    static bool isOverlayWindow(void *nativeHandle);

    /// 本类打在本工程悬浮层窗口上的属性名（宽字符串字面量，供原生代码使用）
    static const wchar_t *overlayMarkerProperty();

Q_SIGNALS:
    /// 以下鼠标信号**仅在 isClickThrough() == false 时**才会发出
    void overlayMousePressed(const QPoint &logicalPos, int button);
    void overlayMouseMoved(const QPoint &logicalPos);
    void overlayMouseReleased(const QPoint &logicalPos, int button);

    void overlayShown();
    void overlayHidden();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void ensureNativeWindow();
    void updateBackingLayerSize();

    bool m_clickThrough = true;
    bool m_noActivate = true;
    bool m_autoPaintBackingLayer = true;
    QPixmap *m_backingLayer = nullptr;
    QWidget *m_contentWidget = nullptr;
};

} // namespace WinEase
