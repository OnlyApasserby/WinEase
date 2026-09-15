# ============================================================================
#  P3-02 文件锁定解除（file.unlock）
#
#  "谁占用了这个文件"的全部实现都在平台层 `win32/RestartManager`
#  （Restart Manager，rstrtmgr.lib 由 WinEaseWin32 统一带上）；
#  "该不该结束这个占用者"的判定在 `plugins/common/UnlockPolicy`
#  —— 插件与端到端自检**共用同一份源码**，自检断言的才是用户跑的那套规则。
# ============================================================================

winease_add_plugin(file_unlock
    CLASS_NAME FileUnlockPlugin
    SOURCES
        file_unlock_plugin.h
        file_unlock_plugin.cpp
        file_unlock_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/UnlockPolicy.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/UnlockPolicy.cpp"

        # 快捷键入口要读剪贴板里的路径（与 P1-12 端口查看同一形态）
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
