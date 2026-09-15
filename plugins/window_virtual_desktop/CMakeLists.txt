# ============================================================================
#  P3-01 虚拟桌面增强（window.virtual_desktop）
#
#  没有额外链接需求：接口调用与探测/搬迁/切换全部在平台层 `win32/VirtualDesktop`
#  （只用到 IVirtualDesktopManager，系统自带；切换桌面复用 `win32/InputUtils` 的合成按键）。
#  目标窗口策略沿用窗口类插件共用的 `plugins/common/WindowFeatureState.h`。
# ============================================================================

winease_add_plugin(window_virtual_desktop
    CLASS_NAME VirtualDesktopPlugin
    SOURCES
        virtual_desktop_plugin.h
        virtual_desktop_plugin.cpp
        virtual_desktop_plugin.json
)
