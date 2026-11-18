#include "eg_http.h"

#include <stdio.h>

#include "eg_network.h"


#define LOG_TAG "EG_HTTP"
#include "log.h"



// AT 配置、提示符和终止响应超时。
#define EG800_HTTP_CONFIG_TIMEOUT_MS                    5000U           // QHTTPCFG 等待 OK/ERROR 的超时时间，单位 ms



// Http 层内部AT命令编号
typedef enum {
    EG800_HTTP_CMD_CFG_PDP_CONTEXT = 0,                                 // 配置 HTTP 使用的 PDP 场景
    EG800_HTTP_CMD_CFG_REQUEST_HEADER,                                  // 配置 HTTP 请求头生成方式
    EG800_HTTP_CMD_CFG_RESPONSE_HEADER,                                 // 配置 HTTP 读取响应时不带HTTP响应头
    EG800_HTTP_CMD_CFG_SSL_CONTEXT,                                     // 绑定 HTTP 使用的 SSL 上下文
    EG800_HTTP_CMD_CFG_VERSION,                                         // 配置 SSL/TLS 协议版本
    EG800_HTTP_CMD_CFG_SSL_CIPHERSUITE,                                 // 配置 SSL  的加密套件
    EG800_HTTP_CMD_CFG_SSL_SERCURITY,                                   // 配置 SSL  的安全级别
} Eg800HttpCmd_e;



static TaskHandle_t s_eg800_http_task_handle = NULL;                    // 任务句柄

static bool s_eg800_http_initialized = false;                           // Http 层初始化标志位

static Eg800HttpConfig_t s_eg800_http_config = {0};                     // Http 用户连接配置的内部副本，供后续连接和重连使用

static QueueHandle_t s_eg800_http_receive_queue = NULL;                 // Http 接收消息队列

static SemaphoreHandle_t s_http_at_completion_sem = NULL;               // Http 层 AT 请求完成信号量


// 实际处理URC的函数声明
static void Eg800_Http_Temp_Urc_Handler(const char *line);




// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_Http_urc_routes[] = {
    {"+TEMP:",      EG800_AT_OWNER_HTTP,        Eg800_Http_Temp_Urc_Handler},
};




// ------------------------------------------------- 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Http_Register_Urcs(void);

// 核心任务
static void Eg800_Http_Task(void *argument);

//--------------------------------------------------------URC 注册/处理-----------------------------------------
/**
 * @brief 注册 Http 层负责主动上报的URC
 */
static Result_t Eg800_Http_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_Http_urc_routes,ARRAY_SIZE(s_eg800_Http_urc_routes));
}


static void Eg800_Http_Temp_Urc_Handler(const char *line) {
    LOG("TODO:%s",line);
}


//--------------------------------------------------------关键函数实体-----------------------------------------
/**
 * @brief 构造 HTTP AT 请求，提交给 Service 并同步等待结果。
 * @note 各命令负责参数校验、指令拼接和结果解析。
 */
static Eg800AtResult_e Eg800_Http_At_Execute(uint8_t cmd_id, Eg800AtFlow_e flow, const char *name, const char *tx, const char *rsp_prefix, const char *expect_urc_prefix, const char *prompt_prefix, const uint8_t *tx_data, size_t tx_data_len, uint8_t *raw_buf, size_t raw_buf_size, size_t raw_expect_len, char *rx_buf, size_t rx_buf_size, uint32_t final_timeout_ms, uint32_t urc_timeout_ms, uint32_t data_timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {

    Eg800AtRequest_t request = {0};

    // 命令身份与执行流程。
    request.cmd.owner = EG800_AT_OWNER_HTTP;
    request.cmd.id = cmd_id;
    request.flow = flow;
    request.name = name;
    request.tx = tx;

    // 文本响应、最终结果 URC 和数据提示符。
    request.rsp_prefix = rsp_prefix;
    request.expect_urc_prefix = expect_urc_prefix;
    request.prompt_prefix = prompt_prefix;
    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    // SEND_DATA：等待提示符后发送 URL 等原始数据。
    request.tx_data = tx_data;
    request.tx_data_len = tx_data_len;

    // RECV_RAW：等待 CONNECT 后接收指定长度的二进制数据。
    request.raw_buf = raw_buf;
    request.raw_buf_size = raw_buf_size;
    request.raw_expect_len = raw_expect_len;

    // 各阶段超时，单位 ms；传入 0 时使用 Service 默认值。
    request.final_timeout_ms = final_timeout_ms;
    request.urc_timeout_ms = urc_timeout_ms;
    request.data_timeout_ms = data_timeout_ms;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}















//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
/**
 * @brief 配置 HTTP 使用 Network 层的 PDP 场景。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量无效或命令生成失败返回 PARAM_ERROR。
 * @note 只绑定 PDP 场景，PDP 的激活由 Network 层负责。
 */
Eg800AtResult_e Eg800_Http_Set_PDP_Context(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPCFG=\"contextid\",%u\r\n", (unsigned int)EG800_NETWORK_CONTEXT_ID);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_PDP_CONTEXT, EG800_AT_FLOW_TEXT, "配置 HTTP PDP 场景", command, NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

/**
 * @brief 绑定 HTTP 使用的 SSL 上下文。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、信号量无效或命令生成失败返回 PARAM_ERROR。
 * @note 绑定 HTTP 使用的 SSL 上下文。
 */
Eg800AtResult_e Eg800_Http_Set_SSL_Context(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_http_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QHTTPCFG=\"sslctxid\",%u\r\n", (unsigned int)EG800_HTTP_SSL_CONTEXT_ID);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Http_At_Execute(EG800_HTTP_CMD_CFG_SSL_CONTEXT, EG800_AT_FLOW_TEXT, "配置 HTTP SSL 场景", command, NULL, NULL, NULL, NULL, 0U, NULL, 0U, 0U, NULL, 0U, EG800_HTTP_CONFIG_TIMEOUT_MS, 0U, 0U, NULL, completion_sem);
}

















































//--------------------------------------------------------公开 API-----------------------------------------
/**
 * @brief 初始化 Http 层并注册基础 URC。
 * @return RESULT_SUCCESS 表示初始化成功。
 * @note 本函数不发送 AT 命令，可以在模块上电前调用。
 */
Result_t Eg800_Http_Init(void) {
    BaseType_t task_result;

    // 幂等检查
    if (s_eg800_http_initialized == true) {
        return RESULT_SUCCESS;
    }

    // 注册URC
    if (Eg800_Http_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }

    // 创建接收消息队列
    s_eg800_http_receive_queue = xQueueCreate(EG800_HTPP_RECEIVE_QUEUE_LENGTH, sizeof(Eg800HttpReceiveMessage_t));
    if (s_eg800_http_receive_queue == NULL) {
        LOG_ERROR("MQTT 接收队列创建失败");
        return RESULT_FAIL;
    }

    // 创建 MQTT 任务专用的 AT 完成信号量。
    s_http_at_completion_sem = xSemaphoreCreateBinary();
    if (s_http_at_completion_sem == NULL) {
        vQueueDelete(s_eg800_http_receive_queue);
        s_eg800_http_receive_queue = NULL;
        LOG_ERROR("HTTP AT 完成信号量创建失败");
        return RESULT_FAIL;
    }

    // 创建 HTTP 维护任务
    task_result = xTaskCreate(Eg800_Http_Task,
                              "EG HTTP Core Task",
                              EG800_HTPP_TASK_STACK_SIZE,
                              NULL,
                              EG800_HTPP_TASK_PRIORITY,
                              &s_eg800_http_task_handle);

    if (task_result != pdPASS) {
        s_eg800_http_task_handle = NULL;
        vSemaphoreDelete(s_http_at_completion_sem);
        s_http_at_completion_sem = NULL;
        vQueueDelete(s_eg800_http_receive_queue);
        s_eg800_http_receive_queue = NULL;

        LOG_ERROR("HTTP 后台任务创建失败");
        return RESULT_FAIL;
    }

    s_eg800_http_initialized = true;

    return RESULT_SUCCESS;
}














//-----------------------------------------------HTTP层核心维护任务-------------------------------------------------------
static void Eg800_Http_Task(void *argument) {
    (void)argument;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
