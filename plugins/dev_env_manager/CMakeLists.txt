# ============================================================================
#  P2-14 环境变量管理（dev.env_manager）
#
#  共享件 plugins/common/EnvTools.*（插件与端到端自检共用同一份源码）：
#    * 纯函数：PATH 拆分/归一化/去重、变量名校验（ASCII-only）、%VAR% 展开、导入导出
#    * 一个 IO 函数：读一个作用域的全部变量（读 HKCU / HKLM 都不需要提权）
#  用户级写入用平台层 win32/RegistryUtils + broadcastSettingChange；
#  系统级写入走提权助手的 setEnvVar。
# ============================================================================

winease_add_plugin(dev_env_manager
    CLASS_NAME EnvManagerPlugin
    SOURCES
        env_manager_plugin.h
        env_manager_plugin.cpp
        env_manager_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/EnvTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/EnvTools.cpp"
)
