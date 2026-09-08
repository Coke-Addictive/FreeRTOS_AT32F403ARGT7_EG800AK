#include "system_main.h"

// FreeRTOS资源
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_eg.h"

#define LOG_TAG "SYS"
#include "log.h"

static TaskHandle_t s_system_main_task_handle = NULL;

// 函数声明
static void System_Main_Task(void *parameter);

/**
 * @brief 创建System Main任务和业务事件队列。
 */
Result_t System_Main_Init(void) {
    BaseType_t result;
    result = xTaskCreate(System_Main_Task,
                         "主任务",
                         512,
                         NULL,
                         10,
                         &s_system_main_task_handle);

    if (result != pdPASS) {
        s_system_main_task_handle = NULL;
        return RESULT_FAIL;
    }

    return RESULT_SUCCESS;
}

static void System_Main_Task(void *parameter) {

    LOG("主任务启动成功");
    App_EG_Init();
    App_EG_Launch();

    while (1) {
        LOG("Main Task Running...");
        vTaskDelay(30000);
    }
}
