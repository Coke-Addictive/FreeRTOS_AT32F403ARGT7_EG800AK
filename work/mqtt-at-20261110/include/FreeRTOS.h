#ifndef FREERTOS_H_
#define FREERTOS_H_

#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdPASS                  1               // 模拟成功结果
#define pdTRUE                  1               // 模拟布尔真
#define pdFALSE                 0               // 模拟布尔假
#define portMAX_DELAY           UINT32_MAX      // 模拟无限等待
#define configTICK_RATE_HZ      1000U           // 模拟每秒 1000 个节拍
#define pdMS_TO_TICKS(ms)        ((TickType_t)(ms)) // 模拟 1 kHz 系统节拍
#define taskENTER_CRITICAL()    ((void)0)       // 单线程模拟无需屏蔽中断
#define taskEXIT_CRITICAL()     ((void)0)       // 单线程模拟无需恢复中断
#define portYIELD_FROM_ISR(x)   ((void)(x))     // 模拟中断退出

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *handle);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t handle);
void vTaskNotifyGiveFromISR(TaskHandle_t handle, BaseType_t *woken);
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t item_size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t sem);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);

#endif // FREERTOS_H_
