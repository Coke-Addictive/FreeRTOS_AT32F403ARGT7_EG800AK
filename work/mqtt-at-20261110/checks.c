#include <stdio.h>
#include <stdint.h>
#include <string.h>

// 测试直接编译当前源文件，替换 RTOS 和串口，不替换指令生成、行解析或状态机。
#include "../../Treo/Service/eg_service.c"
#undef LOG_TAG
#include "../../Treo/Service/eg_mqtt.c"

typedef struct {
    bool given;                         // 模拟二值信号量中的完成信号
} MockSemaphore_t;

static MockSemaphore_t s_mock_semaphores[8]; // 模拟信号量存储区
static unsigned int s_mock_sem_count;       // 已分配的模拟信号量数量
static unsigned int s_mock_deleted_sems;    // 初始化失败后释放的信号量数量
static Eg800AtQueueItem_t s_mock_queue_item; // 单个待取出的请求；并发用例中另一个请求已在执行
static bool s_mock_queue_pending;           // 是否存在待启动请求
static bool s_mock_queue_full;              // 模拟队列满
static bool s_mock_send_fail;               // 模拟命令发送失败
static bool s_mock_payload_fail;            // 模拟负载发送失败
static bool s_mock_task_fail;               // 模拟任务创建失败
static bool s_mock_nested_basic;            // 在长 MQTT 请求等待期间提交普通 Basic 请求
static bool s_mock_in_nested;               // 当前是否处于嵌套 Basic 调用
static unsigned int s_mock_task_count;      // 已创建的模拟任务数量
static UBaseType_t s_mock_task_priority;    // 最近创建任务的优先级
static TickType_t s_mock_tick;              // 模拟系统节拍，单位 ms
static TickType_t s_mock_nested_wait;       // 普通请求排在长请求之后的同步等待预算，单位 ms
static const char *s_mock_reply;            // 本用例通过串口注入的原始回复
static const char *s_mock_expected_command; // 本用例预期发送的完整命令
static char s_mock_command[1024];           // 最近一次实际发送的命令
static uint8_t s_mock_payload[1500];         // 实际发送的原始负载
static size_t s_mock_payload_len;           // 实际发送的负载长度
static unsigned int s_mock_send_count;      // 发送命令的总次数
static unsigned int s_check_count;          // 已执行断言数
static unsigned int s_failure_count;        // 失败断言数
static SemaphoreHandle_t s_call_sem;        // MQTT 调用任务的完成信号量
static SemaphoreHandle_t s_nested_sem;      // 模拟 Basic 调用任务的完成信号量

#define CHECK(value) Check((value), #value, __LINE__) // 输出失败断言的行号和表达式

/**
 * @brief 通过半主机接口输出测试结果。
 * @param text 以 '\0' 结尾的诊断文本。
 */
static void Write_Text(const char *text) {
    register uint32_t r0 __asm("r0") = 4U;
    register const char *r1 __asm("r1") = text;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
}

/**
 * @brief 结束模拟运行并返回状态。
 * @param status 0 表示成功，非 0 表示失败。
 */
__attribute__((noreturn)) void _sys_exit(int status) {
    uint32_t block[2] = {0x20026U, (uint32_t)status};
    register uint32_t r0 __asm("r0") = 0x20U;
    register uint32_t *r1 __asm("r1") = block;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
    while (1) {
    }
}

/**
 * @brief 记录一个测试断言。
 * @param passed 是否通过。
 * @param expression 断言表达式。
 * @param line 测试代码行号。
 */
static void Check(bool passed, const char *expression, int line) {
    char message[192];
    s_check_count++;
    if (passed == false) {
        s_failure_count++;
        (void)snprintf(message, sizeof(message), "FAIL line %d: %s\n", line, expression);
        Write_Text(message);
    }
}

/**
 * @brief 为一个新的串口事务准备输入和预期输出。
 * @param command 预期的完整 AT 命令；NULL 表示本用例禁止发送命令。
 * @param reply 模块按顺序输出的原始文本，可为 NULL 以模拟无响应。
 */
static void Prepare(const char *command, const char *reply) {
    CHECK(s_eg800_current_busy == false);
    CHECK(s_eg800_pending_wait_budget_ms == 0U);
    s_mock_expected_command = command;
    s_mock_reply = reply;
    s_mock_command[0] = '\0';
    s_mock_payload_len = 0U;
    s_mock_queue_full = false;
    s_mock_send_fail = false;
    s_mock_payload_fail = false;
}

/**
 * @brief 模拟创建任务，但不运行无限维护循环。
 * @param task 任务入口。
 * @param name 任务名称。
 * @param stack 栈深度。
 * @param argument 任务参数。
 * @param priority 任务优先级。
 * @param handle 任务句柄输出。
 * @return pdPASS 表示成功，0 表示模拟失败。
 */
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *handle) {
    (void)task;
    (void)name;
    (void)stack;
    (void)argument;
    if (s_mock_task_fail == true) {
        return 0;
    }
    s_mock_task_count++;
    s_mock_task_priority = priority;
    *handle = (void *)(0x100U + s_mock_task_count);
    return pdPASS;
}

/**
 * @brief 获取与 Service 不同的模拟调用任务。
 * @return 固定模拟任务句柄。
 */
TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    return (void *)0x55U;
}

/**
 * @brief 获取模拟节拍。
 * @return 当前节拍，单位 ms。
 */
TickType_t xTaskGetTickCount(void) {
    return s_mock_tick;
}

/**
 * @brief 模拟任务延时。
 * @param ticks 延时时长，单位 ms。
 */
void vTaskDelay(TickType_t ticks) {
    s_mock_tick += ticks;
}

/**
 * @brief 保留未运行的维护任务所需的通知接口。
 * @param clear 是否清空通知。
 * @param ticks 等待时长。
 * @return 固定为 0。
 */
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
    (void)clear;
    (void)ticks;
    return 0U;
}

/**
 * @brief 模拟通知由同步等待驱动的 Service。
 * @param handle 任务句柄。
 */
void xTaskNotifyGive(TaskHandle_t handle) {
    (void)handle;
}

/**
 * @brief 模拟中断通知。
 * @param handle 任务句柄。
 * @param woken 是否需要切换任务的输出。
 */
void vTaskNotifyGiveFromISR(TaskHandle_t handle, BaseType_t *woken) {
    (void)handle;
    *woken = pdFALSE;
}

/**
 * @brief 模拟创建当前项目配置的请求队列。
 * @param count 队列容量。
 * @param item_size 队列项大小。
 * @return 固定非空句柄。
 */
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t item_size) {
    CHECK(count == EG800_AT_QUEUE_LENGTH);
    CHECK(item_size == sizeof(Eg800AtQueueItem_t));
    return (void *)1U;
}

/**
 * @brief 模拟删除队列。
 * @param queue 队列句柄。
 */
void vQueueDelete(QueueHandle_t queue) {
    (void)queue;
}

/**
 * @brief 按值保存队列项，保持真实 Service 的浅拷贝语义。
 * @param queue 队列句柄。
 * @param item 待入队对象。
 * @param ticks 等待时长。
 * @return pdPASS 表示成功，0 表示队列满。
 */
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks) {
    (void)queue;
    (void)ticks;
    if ((s_mock_queue_full == true) || (s_mock_queue_pending == true)) {
        return 0;
    }
    s_mock_queue_item = *(const Eg800AtQueueItem_t *)item;
    s_mock_queue_pending = true;
    return pdPASS;
}

/**
 * @brief 取出实际入队的请求。
 * @param queue 队列句柄。
 * @param item 队列项输出地址。
 * @param ticks 等待时长。
 * @return pdPASS 表示取到数据，0 表示队列空。
 */
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks) {
    (void)queue;
    (void)ticks;
    if (s_mock_queue_pending == false) {
        return 0;
    }
    *(Eg800AtQueueItem_t *)item = s_mock_queue_item;
    s_mock_queue_pending = false;
    return pdPASS;
}

/**
 * @brief 创建模拟完成信号量。
 * @return 新信号量句柄。
 */
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    CHECK(s_mock_sem_count < ARRAY_SIZE(s_mock_semaphores));
    return &s_mock_semaphores[s_mock_sem_count++];
}

/**
 * @brief 记录初始化失败时释放信号量。
 * @param sem 信号量句柄。
 */
void vSemaphoreDelete(SemaphoreHandle_t sem) {
    (void)sem;
    s_mock_deleted_sems++;
}

/**
 * @brief 向调用任务提交完成信号。
 * @param sem 信号量句柄。
 * @return 固定为 pdTRUE。
 */
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    ((MockSemaphore_t *)sem)->given = true;
    return pdTRUE;
}

/**
 * @brief 使用真实 Service 状态机处理当前请求的模拟串口回复。
 * @param sem 正在等待的调用任务信号量。
 */
static void Drive_Service(SemaphoreHandle_t sem) {
    unsigned int rounds = 0U;
    const char *reply;

    while ((((MockSemaphore_t *)sem)->given == false) && (rounds++ < 12U)) {
        Eg800_Service_Try_Start_Next();
        if (s_eg800_current_busy == false) {
            continue;
        }
        reply = (s_eg800_current_item.request.cmd.owner == EG800_AT_OWNER_BASIC) ? "+CSQ: 20,99\r\nOK\r\n" : s_mock_reply;
        if (reply != NULL) {
            while (*reply != '\0') {
                Eg800_Line_PutChar(*reply++);
            }
        }
        if (s_eg800_current_busy == true) {
            s_mock_tick += s_eg800_wait_timeout_tick + 1U;
            Eg800_At_CheckTimeout();
        }
    }
    CHECK(((MockSemaphore_t *)sem)->given == true);
}

/**
 * @brief 模拟等待完成信号，在等待期间运行真实的 Service 事务。
 * @param sem 信号量句柄。
 * @param ticks 最长等待时长；0 表示只清除残留信号。
 * @return pdTRUE 表示取得信号，pdFALSE 表示无信号。
 */
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    MockSemaphore_t *state = sem;
    Eg800AtRequest_t basic = {0};

    if (ticks > 0U) {
        if (s_mock_in_nested == true) {
            s_mock_nested_wait = ticks;
        }
        if ((s_mock_nested_basic == true) && (s_mock_in_nested == false)) {
            Eg800_Service_Try_Start_Next();
            s_mock_in_nested = true;
            basic.cmd.owner = EG800_AT_OWNER_BASIC;
            basic.flow = EG800_AT_FLOW_TEXT;
            basic.tx = "AT+CSQ\r\n";
            CHECK(Eg800_AT_Execute(&basic, NULL, s_nested_sem) == EG800_AT_RESULT_OK);
            s_mock_in_nested = false;
        }
        Drive_Service(sem);
    }
    if (state->given == true) {
        state->given = false;
        return pdTRUE;
    }
    return pdFALSE;
}

/**
 * @brief 捕获真实 Service 发送的命令。
 * @param text 完整命令。
 * @return 实际发送长度；模拟失败时返回 0。
 */
size_t Drv_EG_Send(const char *text) {
    s_mock_send_count++;
    (void)snprintf(s_mock_command, sizeof(s_mock_command), "%s", text);
    if (s_mock_in_nested == false) {
        CHECK(s_mock_expected_command != NULL);
        if (s_mock_expected_command != NULL) {
            CHECK(strcmp(text, s_mock_expected_command) == 0);
        }
    }
    return (s_mock_send_fail == true) ? 0U : strlen(text);
}

/**
 * @brief 捕获提示符后提交的准确负载字节。
 * @param data 原始负载。
 * @param len 负载长度，单位字节。
 * @return 实际提交长度；模拟失败时返回 0。
 */
size_t Drv_EG_SendData(const uint8_t *data, size_t len) {
    CHECK(len <= sizeof(s_mock_payload));
    (void)memcpy(s_mock_payload, data, len);
    s_mock_payload_len = len;
    return (s_mock_payload_fail == true) ? 0U : len;
}

/**
 * @brief 提供未启用的 DMA 轮询入口。
 * @param data 读取缓冲区。
 * @param len 读取容量。
 * @return 固定为 0。
 */
size_t Drv_EG_Read(uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return 0U;
}

/**
 * @brief 忽略普通日志，测试只输出失败与最终统计。
 * @param color 日志颜色。
 * @param tag 日志来源。
 * @param format 格式字符串。
 * @param ... 格式参数。
 */
void Log_Printf(const char *color, const char *tag, const char *format, ...) {
    (void)color;
    (void)tag;
    (void)format;
}

/**
 * @brief 将错误的全局保护复位视为测试失败。
 */
void NVIC_SystemReset(void) {
    Write_Text("FAIL: unexpected system reset\n");
    _sys_exit(99);
}

/**
 * @brief 验证指令、边界、异步业务结果及 Service 排队等待。
 * @return 由半主机退出码报告全部断言的结果。
 */
int main(void) {
    char text[1024];
    char long_text[258];
    uint16_t keepalive;
    uint8_t timeout;
    uint8_t retries;
    uint8_t granted;
    bool flag;
    unsigned int sends_before;
    const uint8_t binary[] = {'o', 0U, '\r', '\n', '"', '\\', 0x1AU};
    const char *bad_keepalive[] = {
        "+QMTCFG: \"keepalive\",3601\r\nOK\r\n",
        "+QMTCFG: \"keepalive\",-1\r\nOK\r\n",
        "+QMTCFG: \"keepalive\",9999999999999999\r\nOK\r\n",
        "+QMTCFG: \"keepalive\",120,0\r\nOK\r\n",
        "+QMTCFG: \"keepalive\",120junk\r\nOK\r\n",
        "+QMTCFG: \"session\",1\r\nOK\r\n",
        "+QMTCFG: \"keepalive\",\r\n120\r\nOK\r\n",
        "OK\r\n"
    };

    Prepare(NULL, NULL);
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, (void *)1U) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Service_Init() == RESULT_SUCCESS);
    s_mock_task_fail = true;
    CHECK(Eg800_Mqtt_Init() == RESULT_FAIL);
    CHECK(s_mock_deleted_sems == 1U);
    CHECK(s_mqtt_at_completion_sem == NULL);
    s_mock_task_fail = false;
    CHECK(Eg800_Mqtt_Init() == RESULT_SUCCESS);
    CHECK(s_mock_task_priority == EG800_MQTT_TASK_PRIORITY);
    CHECK(Eg800_Mqtt_Init() == RESULT_SUCCESS);
    s_call_sem = xSemaphoreCreateBinary();
    s_nested_sem = xSemaphoreCreateBinary();

    Prepare("AT+QMTCFG=\"version\",0,4\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Version(4U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"pdpcid\",0,1\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_PDP_Context(1U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"ssl\",0,0\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_SSL(false, 255U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"ssl\",0,1,2\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_SSL(true, 2U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"keepalive\",0,120\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"session\",0,1\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Clean_Session(true, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"timeout\",0,5,3,1\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Packet_Timeout(5U, 3U, true, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"dataformat\",0,0,0\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Data_Format(0U, 0U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"recv/mode\",0,0,1\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Receive_Mode(0U, true, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"edit/timeout\",0,1,30\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Edit_Timeout(true, 30U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"will\",0,0\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Will(false, 99U, false, NULL, NULL, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"will\",0,1,1,1,\"device/001/status\",\"offline\"\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Will(true, 1U, true, "device/001/status", "offline", s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"willex\",0,1,1,1,\"device/001/status\",7\r\n", "\r\n> \r\nOK\r\n");
    CHECK(Eg800_Mqtt_Set_Will_Ex(true, 1U, true, "device/001/status", (const uint8_t *)"offline", 7U, s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(s_mock_payload_len == 7U);
    CHECK(memcmp(s_mock_payload, "offline", 7U) == 0);
    Prepare("AT+QMTCFG=\"willex\",0,1,1,1,\"device/001/status\",7\r\n", ">\r\nOK\r\n");
    CHECK(Eg800_Mqtt_Set_Will_Ex(true, 1U, true, "device/001/status", binary, sizeof(binary), s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(s_mock_payload_len == sizeof(binary));
    CHECK(memcmp(s_mock_payload, binary, sizeof(binary)) == 0);
    Prepare("AT+QMTCFG=\"willex\",0,1,0,0,\"status\",0\r\n", ">\r\nOK\r\n");
    CHECK(Eg800_Mqtt_Set_Will_Ex(true, 0U, false, "status", NULL, 0U, s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(s_mock_payload_len == 0U);

    Prepare("AT+QSSLCFG=\"cacert\",2,\"UFS:cacert.pem\"\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_SSL_CA_Cert(2U, "UFS:cacert.pem", s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QSSLCFG=\"clientcert\",2,\"UFS:client.pem\"\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_SSL_Client_Cert(2U, "UFS:client.pem", s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QSSLCFG=\"clientkey\",2,\"UFS:user_key.pem\"\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_SSL_Client_Key(2U, "UFS:user_key.pem", s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"aliauth\",0,\"ProductKey\",\"DeviceName\",\"DeviceSecret\"\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Aliyun_Auth("ProductKey", "DeviceName", "DeviceSecret", s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCFG=\"onenet\",0,\"ProductID\",\"AccessKey\"\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_OneNET_Auth("ProductID", "AccessKey", s_call_sem) == EG800_AT_RESULT_OK);

    Prepare("AT+QMTCFG=?\r\n", "+QMTCFG: \"version\",(0-5),(3,4)\r\n+QMTCFG: \"pdpcid\",(0-5),(1-15)\r\nOK\r\n");
    CHECK(Eg800_Mqtt_Query_Config_Support(text, sizeof(text), s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(strstr(text, "pdpcid") != NULL);
    Prepare("AT+QMTCFG=?\r\n", "+QMTCFG: \"version\",(0-5),(3,4)\r\nOK\r\n");
    CHECK(Eg800_Mqtt_Query_Config_Support(text, 8U, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);
    Prepare("AT+QMTCFG=\"keepalive\",0\r\n", "+QMTCFG: \"keepalive\",120\r\nOK\r\n");
    keepalive = 999U;
    CHECK(Eg800_Mqtt_Query_Keepalive(&keepalive, s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(keepalive == 120U);
    Prepare("AT+QMTCFG=\"session\",0\r\n", "+QMTCFG: \"session\",1\r\nOK\r\n");
    flag = false;
    CHECK(Eg800_Mqtt_Query_Clean_Session(&flag, s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(flag == true);
    Prepare("AT+QMTCFG=\"timeout\",0\r\n", "+QMTCFG: \"timeout\" , 5,3,1\r\nOK\r\n");
    CHECK(Eg800_Mqtt_Query_Packet_Timeout(&timeout, &retries, &flag, s_call_sem) == EG800_AT_RESULT_OK);
    CHECK((timeout == 5U) && (retries == 3U) && (flag == true));
    for (size_t index = 0U; index < ARRAY_SIZE(bad_keepalive); index++) {
        Prepare("AT+QMTCFG=\"keepalive\",0\r\n", bad_keepalive[index]);
        keepalive = 999U;
        CHECK(Eg800_Mqtt_Query_Keepalive(&keepalive, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);
        CHECK(keepalive == 999U);
    }

    Prepare("AT+QMTOPEN=0,\"broker.emqx.io\",1883\r\n", "OK\r\n+QMTOPEN: 0,0\r\n");
    CHECK(Eg800_Mqtt_Open_Network("broker.emqx.io", 1883U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTOPEN=0,\"broker.emqx.io\",1883\r\n", "OK\r\n+QMTOPEN: 0,4\r\n");
    CHECK(Eg800_Mqtt_Open_Network("broker.emqx.io", 1883U, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTOPEN=0,\"broker.emqx.io\",1883\r\n", "OK\r\n+QMTOPEN: 0,-1\r\n");
    CHECK(Eg800_Mqtt_Open_Network("broker.emqx.io", 1883U, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTOPEN=0,\"broker.emqx.io\",1883\r\n", "OK\r\n+QMTOPEN: 5,0\r\n+QMTOPEN: 0,0\r\n");
    CHECK(Eg800_Mqtt_Open_Network("broker.emqx.io", 1883U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTOPEN=0,\"broker.emqx.io\",1883\r\n", "OK\r\n+QMTOPEN: 0,1\r\n");
    CHECK(Eg800_Mqtt_Open_Network("broker.emqx.io", 1883U, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTOPEN=0,\"broker.emqx.io\",1883\r\n", "OK\r\n+QMTOPEN: 0,0,junk\r\n");
    CHECK(Eg800_Mqtt_Open_Network("broker.emqx.io", 1883U, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);

    Prepare("AT+QMTCONN=0,\"device001\",\"user\",\"pass\"\r\n", "OK\r\n+QMTCONN: 0,0,0\r\n");
    CHECK(Eg800_Mqtt_Connect_Server("device001", "user", "pass", s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCONN=0,\"device001\"\r\n", "OK\r\n+QMTCONN: 0,1\r\n+QMTCONN: 0,0,0\r\n");
    CHECK(Eg800_Mqtt_Connect_Server("device001", NULL, NULL, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTCONN=0,\"device001\",\"user\"\r\n", "OK\r\n+QMTCONN: 0,0,4\r\n");
    CHECK(Eg800_Mqtt_Connect_Server("device001", "user", NULL, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTCONN=0,\"device001\"\r\n", "OK\r\n+QMTCONN: 0,2\r\n");
    CHECK(Eg800_Mqtt_Connect_Server("device001", NULL, NULL, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTCONN=0,\"device001\"\r\n", "OK\r\n+QMTCONN: 0,1\r\n");
    CHECK(Eg800_Mqtt_Connect_Server("device001", NULL, NULL, s_call_sem) == EG800_AT_RESULT_TIMEOUT);

    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,0,1\r\n");
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "device/001/down", 1U, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,2,0,1\r\n+QMTSUB: 0,1,1,1\r\n+QMTSUB: 0,1,0,0\r\n");
    granted = 99U;
    CHECK(Eg800_Mqtt_Subscribe_Topic_Ex(1U, "device/001/down", 1U, &granted, s_call_sem) == EG800_AT_RESULT_OK);
    CHECK(granted == 0U);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,0,128\r\n");
    granted = 99U;
    CHECK(Eg800_Mqtt_Subscribe_Topic_Ex(1U, "device/001/down", 1U, &granted, s_call_sem) == EG800_AT_RESULT_ERROR);
    CHECK(granted == 99U);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,2\r\n");
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "device/001/down", 1U, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,0,2\r\n");
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "device/001/down", 1U, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,1,1\r\n");
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "device/001/down", 1U, s_call_sem) == EG800_AT_RESULT_TIMEOUT);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,1\r\n");
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "device/001/down", 1U, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);
    Prepare("AT+QMTCONN=0,\"device001\"\r\n", "OK\r\n+QMTCONN: 0,1,999\r\n");
    CHECK(Eg800_Mqtt_Connect_Server("device001", NULL, NULL, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);

    (void)memset(long_text, 'x', 100U);
    long_text[100] = '\0';
    (void)snprintf(text, sizeof(text), "AT+QMTOPEN=0,\"%s\",65535\r\n", long_text);
    Prepare(text, "OK\r\n+QMTOPEN: 0,0\r\n");
    CHECK(Eg800_Mqtt_Open_Network(long_text, 65535U, s_call_sem) == EG800_AT_RESULT_OK);
    (void)memset(long_text, 'x', 255U);
    long_text[255] = '\0';
    (void)snprintf(text, sizeof(text), "AT+QMTCONN=0,\"%s\",\"%s\",\"%s\"\r\n", long_text, long_text, long_text);
    Prepare(text, "OK\r\n+QMTCONN: 0,0,0\r\n");
    CHECK(Eg800_Mqtt_Connect_Server(long_text, long_text, long_text, s_call_sem) == EG800_AT_RESULT_OK);
    (void)memset(long_text, 'x', 256U);
    long_text[256] = '\0';
    (void)snprintf(text, sizeof(text), "AT+QMTSUB=0,65535,\"%s\",2\r\n", long_text);
    Prepare(text, "OK\r\n+QMTSUB: 0,65535,0,2\r\n");
    CHECK(Eg800_Mqtt_Subscribe_Topic(65535U, long_text, 2U, s_call_sem) == EG800_AT_RESULT_OK);
    (void)snprintf(text, sizeof(text), "AT+QMTCFG=\"will\",0,1,2,1,\"%s\",\"%s\"\r\n", long_text, long_text);
    Prepare(text, "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Will(true, 2U, true, long_text, long_text, s_call_sem) == EG800_AT_RESULT_OK);

    Prepare("AT+QMTCFG=\"timeout\",0,60,10,1\r\n", "ERROR\r\n");
    CHECK(Eg800_Mqtt_Set_Packet_Timeout(60U, 10U, true, s_call_sem) == EG800_AT_RESULT_ERROR);
    CHECK((s_eg800_mqtt_packet_timeout_s == 5U) && (s_eg800_mqtt_packet_retry_times == 3U));
    Prepare("AT+QMTCFG=\"timeout\",0\r\n", "+QMTCFG: \"timeout\",60,11,1\r\nOK\r\n");
    timeout = 99U;
    retries = 99U;
    flag = false;
    CHECK(Eg800_Mqtt_Query_Packet_Timeout(&timeout, &retries, &flag, s_call_sem) == EG800_AT_RESULT_DATA_FAIL);
    CHECK((timeout == 99U) && (retries == 99U) && (flag == false));
    CHECK((s_eg800_mqtt_packet_timeout_s == 5U) && (s_eg800_mqtt_packet_retry_times == 3U));

    Prepare("AT+QMTCFG=\"timeout\",0,60,10,1\r\n", "OK\r\n");
    CHECK(Eg800_Mqtt_Set_Packet_Timeout(60U, 10U, true, s_call_sem) == EG800_AT_RESULT_OK);
    Prepare("AT+QMTSUB=0,1,\"device/001/down\",1\r\n", "OK\r\n+QMTSUB: 0,1,0,1\r\n");
    s_mock_nested_basic = true;
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "device/001/down", 1U, s_call_sem) == EG800_AT_RESULT_OK);
    s_mock_nested_basic = false;
    CHECK(s_mock_nested_wait > 665000U);
    CHECK(s_eg800_pending_wait_budget_ms == 0U);

    Prepare("AT+QMTCFG=\"keepalive\",0,120\r\n", "ERROR\r\n");
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, s_call_sem) == EG800_AT_RESULT_ERROR);
    Prepare("AT+QMTCFG=\"keepalive\",0,120\r\n", "+CME ERROR: 3\r\n");
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, s_call_sem) == EG800_AT_RESULT_CME_ERROR);
    Prepare("AT+QMTCFG=\"keepalive\",0,120\r\n", NULL);
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, s_call_sem) == EG800_AT_RESULT_TIMEOUT);
    Prepare("AT+QMTCFG=\"keepalive\",0,120\r\n", NULL);
    s_mock_send_fail = true;
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, s_call_sem) == EG800_AT_RESULT_SEND_FAIL);
    Prepare("AT+QMTCFG=\"willex\",0,1,0,0,\"status\",7\r\n", ">\r\n");
    s_mock_payload_fail = true;
    CHECK(Eg800_Mqtt_Set_Will_Ex(true, 0U, false, "status", binary, sizeof(binary), s_call_sem) == EG800_AT_RESULT_SEND_FAIL);
    Prepare(NULL, NULL);
    s_mock_queue_full = true;
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, s_call_sem) == EG800_AT_RESULT_QUEUE_FULL);
    CHECK(s_eg800_pending_wait_budget_ms == 0U);
    s_mock_queue_full = false;

    Prepare(NULL, NULL);
    sends_before = s_mock_send_count;
    CHECK(Eg800_Mqtt_Set_Version(2U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_PDP_Context(0U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_PDP_Context(16U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_SSL(true, 6U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Keepalive(3601U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Keepalive(120U, NULL) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Packet_Timeout(0U, 3U, true, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Packet_Timeout(61U, 3U, true, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Packet_Timeout(5U, 11U, true, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Receive_Mode(2U, true, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Data_Format(2U, 0U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Data_Format(0U, 2U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Edit_Timeout(true, 0U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Edit_Timeout(true, 121U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Will(true, 3U, false, "status", "offline", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Will(true, 0U, false, "status/#", "offline", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Will(true, 0U, false, "status", "a\"b", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Will_Ex(true, 0U, false, "status", NULL, 1U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Will_Ex(true, 0U, false, "status", binary, 257U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_SSL_CA_Cert(6U, "UFS:ca.pem", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_SSL_Client_Cert(0U, "a\rb", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_SSL_Client_Key(0U, "a\\b", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_Aliyun_Auth("", "name", "secret", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Set_OneNET_Auth("id", "a\nb", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Open_Network(NULL, 1883U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Open_Network("host", 0U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Connect_Server("id", NULL, "pass", s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Connect_Server("", NULL, NULL, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Subscribe_Topic(0U, "topic", 1U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "topic", 3U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, "a\nb", 1U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Query_Config_Support(NULL, 0U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Query_Keepalive(NULL, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Query_Clean_Session(NULL, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Query_Packet_Timeout(&timeout, NULL, &flag, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    (void)memset(long_text, 'x', sizeof(long_text));
    long_text[sizeof(long_text) - 1U] = '\0';
    CHECK(Eg800_Mqtt_Subscribe_Topic(1U, long_text, 1U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Open_Network(long_text, 1883U, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(Eg800_Mqtt_Connect_Server(long_text, NULL, NULL, s_call_sem) == EG800_AT_RESULT_PARAM_ERROR);
    CHECK(s_mock_send_count == sends_before);
    CHECK(s_eg800_pending_wait_budget_ms == 0U);
    (void)snprintf(text, sizeof(text), "%s: %u MQTT/Service checks, failures=%u\n", s_failure_count == 0U ? "PASS" : "FAIL", s_check_count, s_failure_count);
    Write_Text(text);
    _sys_exit((int)s_failure_count);
}
