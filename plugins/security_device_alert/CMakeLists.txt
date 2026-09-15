# ============================================================================
#  P2-11 摄像头/麦克风使用提醒（security.device_alert）
#
#  共享件 plugins/common/DeviceUsageMonitor.*（插件与端到端自检共用同一份源码）：
#    * 纯函数：注册表键名 ⇄ exe 路径、FILETIME 换算、"正在使用"判定、警报筛选
#    * 一个 IO 函数：读 ConsentStore（只读）
#  平台层用 win32/RegistryUtils（已在 WinEaseWin32 里，插件自动链接）
# ============================================================================

winease_add_plugin(security_device_alert
    CLASS_NAME DeviceAlertPlugin
    SOURCES
        device_alert_plugin.h
        device_alert_plugin.cpp
        device_alert_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/DeviceUsageMonitor.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/DeviceUsageMonitor.cpp"
)
