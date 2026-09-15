# ============================================================================
#  P2-07 定时任务（monitor.scheduler）
#
#  两个共用件（插件与端到端自检**共用同一份源码**，断言才有意义）：
#    * plugins/common/ScheduleEngine.*  时间决策纯函数（跨零点 / 休眠补触发）
#    * plugins/common/PowerControl.*    电源动作 + 倒计时可取消（与 P2-06 同一条纪律）
# ============================================================================

winease_add_plugin(monitor_scheduler
    CLASS_NAME SchedulerPlugin
    SOURCES
        scheduler_plugin.h
        scheduler_plugin.cpp
        scheduler_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/PowerControl.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/PowerControl.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/common/ScheduleEngine.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ScheduleEngine.cpp"
)
