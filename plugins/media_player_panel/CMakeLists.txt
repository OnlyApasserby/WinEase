# ============================================================================
#  P3-11 媒体控制面板（media.player_panel）
#
#  吃 plugins/common/PlayerModel（插件与 feature_smoke 编译同一份源码）：
#  "读不到就写原因"与"进度外推"两条纪律都在那一份纯函数里，
#  自检断言的必须是插件真正跑的那份实现。
#
#  平台侧用 src/win32/MediaSessions（系统媒体会话 SMTC），与 AudioSessions
#  （音频会话/音量）刻意分开：一个是"谁在放歌"，一个是"谁在出声"。
# ============================================================================

winease_add_plugin(media_player_panel
    CLASS_NAME MediaPlayerPanelPlugin
    SOURCES
        player_panel_plugin.h
        player_panel_plugin.cpp
        player_panel_plugin.json
        "${CMAKE_SOURCE_DIR}/plugins/common/PlayerModel.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/PlayerModel.cpp"
)
