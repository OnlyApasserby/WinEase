# ============================================================================
#  P3-09 音量混合器（media.mixer）
#
#  吃 plugins/common/MixerModel（插件与 feature_smoke 编译同一份源码）：
#  "按应用合并同类会话"与"读不到音量不显示 0%"两条纪律都在那一份里，
#  自检断言的必须是插件真正跑的那份实现。
#
#  平台侧用 src/win32/AudioSessions（会话音量/静音），与 CoreAudio（端点主音量）
#  刻意分开：混在一起会让人以为"调了应用，主音量也变了"。
# ============================================================================

winease_add_plugin(media_mixer
    CLASS_NAME MixerPlugin
    SOURCES
        mixer_plugin.h
        mixer_plugin.cpp
        mixer_plugin.json
        "${CMAKE_SOURCE_DIR}/plugins/common/MixerModel.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/MixerModel.cpp"
)
