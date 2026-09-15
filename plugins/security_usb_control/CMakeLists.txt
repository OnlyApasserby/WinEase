# ============================================================================
#  P3-14 USB 设备管控（security.usb_control）
#
#  没有额外链接需求：枚举与弹出的全部实现都在平台层 `win32/StorageDevices`
#  （SetupAPI + cfgmgr32 由 WinEaseWin32 统一带上），插件只用封装好的原语。
#  这样"什么算可移动设备"这份判据在预研探针与插件之间**只有一份实现**。
# ============================================================================

winease_add_plugin(security_usb_control
    CLASS_NAME UsbControlPlugin
    SOURCES
        usb_control_plugin.h
        usb_control_plugin.cpp
        usb_control_plugin.json
)
