# ============================================================================
#  P2-10 麦克风一键静音（media.mic_mute）
#
#  没有共享引擎：这个插件的逻辑就是"问系统、改系统、报出去"，
#  真正的算术/状态机一行为零，硬凑一个 common/ 头文件只会让人误以为
#  "状态机在别处"。核心纪律写在 mic_mute_plugin.h 的注释里。
# ============================================================================

winease_add_plugin(media_mic_mute
    CLASS_NAME MicMutePlugin
    SOURCES
        mic_mute_plugin.h
        mic_mute_plugin.cpp
        mic_mute_plugin.json
)
