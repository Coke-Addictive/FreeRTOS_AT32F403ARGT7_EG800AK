#include "eg_service.h"

#include "wk_system.h" // for NVIC_SystemReset

// 驱动层
#include "drv_eg.h"

// 总配置层
#include "eg_service_cfg.h"



// 日志
#define LOG_TAG "EG_SRV"
#include "log.h"

typedef enum {
    EG800_AT_STATE_IDLE = 0,    // 空闲，允许取下一条 AT
    EG800_AT_STATE_WAIT_FINAL,  // 已发送，等待 OK/ERROR 终止行
    EG800_AT_STATE_WAIT_URC,    // 已收到 OK，等待指定异步 URC
    EG800_AT_STATE_WAIT_PROMPT, // 已发送，等待 '>' 或 CONNECT 提示
    EG800_AT_STATE_WAIT_RAW     // 已收到 CONNECT，按长度接收原始数据
} Eg800AtState_e;

typedef struct {
    Eg800AtRequest_t request;         // AT 请求副本
    Eg800AtResultInfo_t *result_info; // 调用者的结果输出
    TaskHandle_t notify_task;         // 调用者的任务句柄
    SemaphoreHandle_t completion_sem; // 调用者提供的完成信号量
} Eg800AtQueueItem_t;

// FreeRTOS 资源
static TaskHandle_t s_eg800_service_task_handle = NULL; // 任务句柄
static QueueHandle_t s_eg800_at_queue = NULL;           // 队列句柄

static uint8_t s_eg800_rx_chunk[EG800_SERVICE_RX_CHUNK_SIZE]; // 单次读取底层 RX 环形缓冲区的临时缓冲区
static char s_eg800_line_buf[EG800_SERVICE_LINE_BUF_SIZE];    // AT 文本行缓冲区
static size_t s_eg800_line_len = 0U;                          // 当前 AT 文本行缓冲区的长度

static Eg800AtState_e s_eg800_at_state = EG800_AT_STATE_IDLE; // 当前 AT 命令的状态机状态
static TickType_t s_eg800_wait_start_tick = 0U;               // 当前等待阶段从什么时候开始
static TickType_t s_eg800_wait_timeout_tick = 0U;             // 当前阶段允许等待多长时间

static bool s_eg800_current_busy = false;       // 当前是否有AT命令正在执行 （服务任务只有在空闲时才能取下一条
static Eg800AtQueueItem_t s_eg800_current_item; // 当前正在执行的 具体AT请求

/**
 * @brief URC路由表存储区
 * @note URC 表项，只能启动前注册，启动后冻结。
 */
static Eg800UrcRegistration_t s_eg800_urc_routes[EG800_SERVICE_URC_ROUTE_MAX];

// -------------------------------------------------------------------- 函数声明 --------------------------------------------------
// 超时相关函数
static uint32_t Eg800_Get_Final_Timeout_Ms(const Eg800AtRequest_t *request);
static uint32_t Eg800_Get_Urc_Timeout_Ms(const Eg800AtRequest_t *request);
static uint32_t Eg800_Get_Data_Timeout_Ms(const Eg800AtRequest_t *request);

// 辅助工具
static const char *Eg800_Get_Prompt_Prefix(const Eg800AtRequest_t *request);
static bool Eg800_Starts_With(const char *str, const char *prefix);
static bool Eg800_Is_Prompt_Line(const Eg800AtRequest_t *request, const char *line);
static bool Eg800_Is_Echo_Line(const Eg800AtRequest_t *request, const char *line);
static int16_t Eg800_Parse_Module_Error(const char *line);
static void Eg800_Request_Append_Rx(const char *line);

// 发送相关
static void Eg800_Result_Init(Eg800AtResultInfo_t *result_info, const Eg800AtRequest_t *request, Eg800AtResult_e result);
static bool Eg800_Request_Validate(const Eg800AtRequest_t *request);

// 接收相关
static void Eg800_Line_PutChar(char ch);
static void Eg800_Handle_Line(const char *line);
static size_t Eg800_Service_Put_Raw_Data(const uint8_t *data, size_t len);


// 收到提示符后的处理
static void Eg800_Service_Handle_Prompt(void);



// URC 相关函数
static bool Eg800_Route_Urc(const char *line);
Result_t Eg800_Urc_Register(const char *prefix, uint8_t owner, Eg800AtUrcHandler_t handler);
Result_t Eg800_Urc_Register_Table(const Eg800UrcRegistration_t *routes, size_t route_count);

// 进入不同状态
static void Eg800_Service_Enter_Wait_Final(void);
static void Eg800_Service_Enter_Wait_Urc(void);
static void Eg800_Service_Enter_Wait_Raw(void);
static void Eg800_Service_Start_Request(void);



// 请求完成函数
static void Eg800_Queue_Item_Complete(Eg800AtQueueItem_t *item, Eg800AtResult_e result, int16_t module_error);
static void Eg800_Request_Complete(Eg800AtResult_e result, int16_t module_error);

// -------------------------------------- 超时相关 --------------------------------------
/**
 * @brief 获取等待终止行的超时时间。
 * @param request AT 请求描述。
 * @return 超时时间，单位 ms。
 */
static uint32_t Eg800_Get_Final_Timeout_Ms(const Eg800AtRequest_t *request) {
    if (request == NULL) {
        return EG800_AT_DEFAULT_FINAL_TIMEOUT_MS;
    }

    // 限幅
    if (request->final_timeout_ms > 0U) {
        return (request->final_timeout_ms <= EG800_AT_MAX_FINAL_TIMEOUT_MS) ? request->final_timeout_ms : EG800_AT_MAX_FINAL_TIMEOUT_MS;
    }

    return EG800_AT_DEFAULT_FINAL_TIMEOUT_MS;
}

/**
 * @brief 获取等待 URC 的超时时间。
 * @param request AT 请求描述。
 * @return 超时时间，单位 ms。
 */
static uint32_t Eg800_Get_Urc_Timeout_Ms(const Eg800AtRequest_t *request) {
    if (request == NULL) {
        return EG800_AT_DEFAULT_URC_TIMEOUT_MS;
    }

    // 限幅
    if (request->urc_timeout_ms > 0U) {
        return (request->urc_timeout_ms <= EG800_AT_MAX_URC_TIMEOUT_MS) ? request->urc_timeout_ms : EG800_AT_MAX_URC_TIMEOUT_MS;
    }

    return EG800_AT_DEFAULT_URC_TIMEOUT_MS;
}

/**
 * @brief 获取等待原始数据的超时时间。
 * @param request AT 请求描述。
 * @return 超时时间，单位 ms。
 */
static uint32_t Eg800_Get_Data_Timeout_Ms(const Eg800AtRequest_t *request) {
    if (request == NULL) {
        return EG800_AT_DEFAULT_DATA_TIMEOUT_MS;
    }

    // 限幅
    if (request->data_timeout_ms > 0U) {
        return (request->data_timeout_ms <= EG800_AT_MAX_DATA_TIMEOUT_MS) ? request->data_timeout_ms : EG800_AT_MAX_DATA_TIMEOUT_MS;
    }

    return EG800_AT_DEFAULT_DATA_TIMEOUT_MS;
}

// ------------------------------------------------------辅助工具类函数---------------------------------------------------

/**
 * @brief 获取当前请求的数据提示符前缀。
 * @param request AT 请求描述。
 * @return 提示符前缀。
 */
static const char *Eg800_Get_Prompt_Prefix(const Eg800AtRequest_t *request) {

    if ((request != NULL) && (request->prompt_prefix != NULL) && (request->prompt_prefix[0] != '\0')) {
        return request->prompt_prefix;
    }

    // EG800_AT_FLOW_RECV_RAW 默认 "CONNECT"
    if ((request != NULL) && (request->flow == EG800_AT_FLOW_RECV_RAW)) {
        return "CONNECT";
    }

    /* EG800_AT_FLOW_SEND_DATA 默认 “>” */
    return ">";
}

/**
 * @brief 判断字符串是否以指定前缀开始。
 * @param str 待检查字符串。
 * @param prefix 前缀字符串。
 * @return true：匹配；false：不匹配。
 */
static bool Eg800_Starts_With(const char *str, const char *prefix) {
    size_t prefix_len;

    if ((str == NULL) || (prefix == NULL)) {
        return false;
    }

    prefix_len = strlen(prefix);
    return strncmp(str, prefix, prefix_len) == 0;
}

/**
 * @brief 判断当前文本是否为请求等待的数据提示符。
 */
static bool Eg800_Is_Prompt_Line(const Eg800AtRequest_t *request, const char *line) {
    return Eg800_Starts_With(line, Eg800_Get_Prompt_Prefix(request));
}

/**
 * @brief 判断当前行是否为模块回显的本条 AT 指令。
 * @param request 当前 AT 请求。
 * @param line 接收到的文本行。
 * @return true：是回显行；false：不是回显行。
 */
static bool Eg800_Is_Echo_Line(const Eg800AtRequest_t *request, const char *line) {
    size_t tx_len;
    if ((request == NULL) || (request->tx == NULL) || (line == NULL)) {
        return false;
    }

    tx_len = strlen(request->tx);

    /* 请求中的命令带有\r\n，而接收拼行结果已经去掉CR/LF */
    while ((tx_len > 0U) && ((request->tx[tx_len - 1U] == '\r') || (request->tx[tx_len - 1U] == '\n'))) {
        tx_len--;
    }
    return (strlen(line) == tx_len) && (strncmp(line, request->tx, tx_len) == 0);
}

/**
 * @brief 从 +CME/+CMS ERROR 行中解析错误码。
 * @param line 错误行。
 * @return 错误码，无法解析时返回 -1。
 */
static int16_t Eg800_Parse_Module_Error(const char *line) {
    const char *cursor;
    uint32_t value;
    uint32_t digit;
    bool has_digit;

    if (line == NULL) {
        return -1;
    }

    cursor = strchr(line, ':');
    if (cursor == NULL) {
        return -1;
    }

    cursor++;

    while ((*cursor == ' ') || (*cursor == '\t')) {
        cursor++;
    }

    value = 0U;
    has_digit = false;

    while ((*cursor >= '0') && (*cursor <= '9')) {
        digit = (uint32_t)(*cursor - '0');

        if (value > (((uint32_t)INT16_MAX - digit) / 10U)) {
            return -1;
        }

        value = (value * 10U) + digit;
        has_digit = true;
        cursor++;
    }

    return (has_digit == true) ? (int16_t)value : -1;
}

/**
 * @brief 将响应行文本追加到当前请求的RX_BUF文本响应缓冲区。
 * @param line 需要保存的响应行。
 */
static void Eg800_Request_Append_Rx(const char *line) {
    Eg800AtRequest_t *request;
    Eg800AtResultInfo_t *result_info;
    size_t line_len;
    size_t remain;
    size_t copy_len;

    request = &s_eg800_current_item.request;
    result_info = s_eg800_current_item.result_info;
    if ((line == NULL) || (result_info == NULL) || (request->rx_buf == NULL) || (request->rx_buf_size == 0U)) {
        return;
    }

    /* 至少保留一个字节写入'\0' */
    if (result_info->rx_len >= (request->rx_buf_size - 1U)) {
        return;
    }

    line_len = strlen(line);
    remain = request->rx_buf_size - 1U - result_info->rx_len;
    copy_len = (line_len < remain) ? line_len : remain;
    memcpy(&request->rx_buf[result_info->rx_len], line, copy_len);
    result_info->rx_len += copy_len;
    request->rx_buf[result_info->rx_len] = '\0';

    /* 不同行之间用\n分隔 */
    if (result_info->rx_len < (request->rx_buf_size - 1U)) {
        request->rx_buf[result_info->rx_len] = '\n';
        result_info->rx_len++;
        request->rx_buf[result_info->rx_len] = '\0';
    }
}

//-----------------------------------------------------进入不同状态-------------------------------------------------------
/**
 * @brief 进入等待OK/ERROR终止行阶段。
 */
static void Eg800_Service_Enter_Wait_Final(void) {
    s_eg800_at_state = EG800_AT_STATE_WAIT_FINAL;
    s_eg800_wait_start_tick = xTaskGetTickCount();
    s_eg800_wait_timeout_tick = pdMS_TO_TICKS(Eg800_Get_Final_Timeout_Ms(&s_eg800_current_item.request));
}

/**
 * @brief 当前请求收到 OK 后进入异步 URC 等待阶段。
 */
static void Eg800_Service_Enter_Wait_Urc(void) {
    Eg800AtRequest_t *request;

    if (s_eg800_current_busy == false) {
        return;
    }
    request = &s_eg800_current_item.request;
    s_eg800_at_state = EG800_AT_STATE_WAIT_URC;
    // 进入一个新的等待阶段时，必须重新设置下面2个等待时间
    s_eg800_wait_start_tick = xTaskGetTickCount();
    s_eg800_wait_timeout_tick = pdMS_TO_TICKS(Eg800_Get_Urc_Timeout_Ms(request));

    LOG("AT等待URC: %s", (request->expect_urc_prefix != NULL) ? request->expect_urc_prefix : "-");
}

/**
 * @brief 进入按指定长度接收原始数据阶段。
 */
static void Eg800_Service_Enter_Wait_Raw(void) {
    Eg800AtResultInfo_t *result_info;

    result_info = s_eg800_current_item.result_info;
    if (result_info == NULL) {
        Eg800_Request_Complete(EG800_AT_RESULT_DATA_FAIL, -1);
        return;
    }
    result_info->raw_len = 0U;
    s_eg800_at_state = EG800_AT_STATE_WAIT_RAW;
    s_eg800_wait_start_tick = xTaskGetTickCount();
    s_eg800_wait_timeout_tick = pdMS_TO_TICKS(Eg800_Get_Data_Timeout_Ms(&s_eg800_current_item.request));

    LOG("开始接收原始数据: expect=%lu", (unsigned long)s_eg800_current_item.request.raw_expect_len);
}



/**
 * @brief 启动当前 AT 请求。
 */
static void Eg800_Service_Start_Request(void) {
    Eg800AtRequest_t *request;
    size_t tx_len;
    size_t sent_len;

    request = &s_eg800_current_item.request;

    // AT指令合法性检查
    if (Eg800_Request_Validate(request) == false) {
        Eg800_Request_Complete(EG800_AT_RESULT_PARAM_ERROR, -1);
        return;
    }

    tx_len = strlen(request->tx);
    sent_len = Drv_EG_Send(request->tx);

    // 驱动层返回实际写入TX环形缓冲区的字节少于命令总长度表示命令没有完整提交
    if (sent_len != tx_len) {
        LOG("EG AT SEND FAIL: %s\r\n", (request->name != NULL) ? request->name : "-");
        Eg800_Request_Complete(EG800_AT_RESULT_SEND_FAIL, -1);
        return;
    }

    //TODO:这里以后可以弄一个LOG日志开关宏
    LOG_TRACE("AT TX: %s", request->tx);

    // 清除上一条请求可能留下的未完成文本行
    s_eg800_line_len = 0U;
    s_eg800_wait_start_tick = xTaskGetTickCount();
    s_eg800_wait_timeout_tick = pdMS_TO_TICKS(Eg800_Get_Final_Timeout_Ms(request));


    if ((request->flow == EG800_AT_FLOW_SEND_DATA) || (request->flow == EG800_AT_FLOW_RECV_RAW)) {
        s_eg800_at_state = EG800_AT_STATE_WAIT_PROMPT;
    } else {
        s_eg800_at_state = EG800_AT_STATE_WAIT_FINAL;
    }
}













//-------------------------------------------------------------------请求完成-------------------------------------------------
/**
 * @brief 完成指定的AT队列项并通知提交该请求的任务
 * @param item 需要完成的AT队列项
 * @param result 本次AT请求的最终执行结果
 * @param module_error 模块返回的CME/CMS错误码（未知时为 -1）
 * @note  本函数只填写结果并通知调用任务，不修改当前AT状态机
 */
static void Eg800_Queue_Item_Complete(Eg800AtQueueItem_t *item, Eg800AtResult_e result, int16_t module_error) {
    if (item == NULL) {
        return;
    }

    // result_info由Eg800_AtExec()的调用任务提供
    if (item->result_info != NULL) {
        item->result_info->result = result;
        item->result_info->cmd = item->request.cmd;
        item->result_info->module_error = module_error;
    }

    // 通知提交该AT请求的任务
    if (item->completion_sem != NULL) {
        (void)xSemaphoreGive(item->completion_sem);
    }
}

/**
 * @brief 完成当前 AT 请求并通知调用方。
 * @param result AT 执行结果。
 * @param module_error 模块错误码，未知时为 -1。
 */
static void Eg800_Request_Complete(Eg800AtResult_e result, int16_t module_error) {
    // 没有请求正在执行时，不允许重复完成，这个检查可以防止同一个请求被通知两次
    if (s_eg800_current_busy == false) {
        return;
    }

    // 填写当前请求的结果，并通知提交该请求的调用任务
    Eg800_Queue_Item_Complete(&s_eg800_current_item, result, module_error);

    // 当前请求已经结束，恢复服务层空闲状态
    s_eg800_current_busy = false;
    s_eg800_at_state = EG800_AT_STATE_IDLE;

    // 清除当前阶段的超时计时信息
    s_eg800_wait_start_tick = 0U;
    s_eg800_wait_timeout_tick = 0U;

    // 丢弃当前尚未完成的文本行，并清空当前请求副本
    s_eg800_line_len = 0U;
    (void)memset(&s_eg800_current_item, 0, sizeof(s_eg800_current_item));
}

//-------------------------------------------------------收到提示符后的核心处理---------------------------------------------
/**
 * @brief 收到数据提示符后的处理函数。
 */
static void Eg800_Service_Handle_Prompt(void) {
    Eg800AtRequest_t *request;
    size_t queued_len;

    request = &s_eg800_current_item.request;

    // 等待"connect" 接收原始数据
    if (request->flow == EG800_AT_FLOW_RECV_RAW) {
        Eg800_Service_Enter_Wait_Raw();
        return;
    }

    // 判断流程是否为等待 ">" 发送原始数据
    if (request->flow != EG800_AT_FLOW_SEND_DATA) {
        LOG_ERROR("当前提示符流程尚未实现: flow=%u", (unsigned int)request->flow);
        Eg800_Request_Complete(EG800_AT_RESULT_DATA_FAIL, -1);
        return;
    }

    // 存在需要发送的原始数据
    if (request->tx_data_len > 0U) {
        queued_len = Drv_EG_SendData(request->tx_data, request->tx_data_len);

        if (queued_len != request->tx_data_len) {
            LOG_ERROR("AT原始数据发送失败: name=%s, len=%lu", (request->name != NULL) ? request->name : "-", (unsigned long)request->tx_data_len);
            Eg800_Request_Complete(EG800_AT_RESULT_SEND_FAIL, -1);
            return;
        }
    }

    LOG("AT原始数据已提交: len=%lu", (unsigned long)request->tx_data_len);
    Eg800_Service_Enter_Wait_Final();
}

// ------------------------------------------------------URC 相关---------------------------------------------------------
/**
 * @brief 将一行URC路由给匹配前缀的处理函数。
 * @param line 完整URC文本行。
 * @return true表示已识别并分发，false表示没有匹配路由。
 */
static bool Eg800_Route_Urc(const char *line) {
    Eg800AtUrcHandler_t handler = NULL;
    size_t index;
    size_t prefix_len;
    size_t best_len = 0U;
    uint8_t owner = EG800_AT_OWNER_NONE;

    if (line == NULL) {
        return false;
    }

    // 循环遍历URC路由表
    for (index = 0U; index < EG800_SERVICE_URC_ROUTE_MAX; index++) {
        // 检查路由表项是否有效-跳过空项
        if ((s_eg800_urc_routes[index].prefix == NULL) || (s_eg800_urc_routes[index].handler == NULL)) {
            continue;
        }

        prefix_len = strlen(s_eg800_urc_routes[index].prefix);

        if ((prefix_len > best_len) && (strncmp(line, s_eg800_urc_routes[index].prefix, prefix_len) == 0)) {
            best_len = prefix_len;
            owner = s_eg800_urc_routes[index].owner;
            handler = s_eg800_urc_routes[index].handler;
        }
    }

    if (handler == NULL) {
        return false;
    }
    LOG("收到 URC 路由: owner= %u, line= %s", (unsigned int)owner, line);
    handler(line);
    return true;
}

// ------------------------------------------------------发送相关---------------------------------------------------
/**
 * @brief 初始化 AT 结果对象。
 * @param result_info 结果对象。
 * @param request AT 请求描述。
 * @param result 初始结果。
 */
static void Eg800_Result_Init(Eg800AtResultInfo_t *result_info, const Eg800AtRequest_t *request, Eg800AtResult_e result) {
    if (result_info == NULL) {
        return;
    }

    result_info->result = result;
    result_info->rx_len = 0U;
    result_info->raw_len = 0U;
    result_info->module_error = -1;

    // 将命令身份复制到结果中，使调用者能够知道该结果属于哪个模块的哪条逻辑命令
    if (request != NULL) {
        result_info->cmd = request->cmd;
    } else {
        result_info->cmd.owner = EG800_AT_OWNER_NONE;
        result_info->cmd.id = 0U;
    }
}

/**
 * @brief 校验 AT 请求参数。
 * @param request AT 请求描述。
 * @return true：参数有效；false：参数错误。
 */
static bool Eg800_Request_Validate(const Eg800AtRequest_t *request) {
    // 非空检查（tx[0] == '\0'，即空字符串也算无效）
    if ((request == NULL) || (request->tx == NULL) || (request->tx[0] == '\0')) {
        return false;
    }

    // 如果rx_buf为NULL，rx_buf_size就不能声明为非零
    if ((request->rx_buf == NULL) && (request->rx_buf_size > 0U)) {
        return false;
    }

    // 不同AT执行流程需要检查不同的专用参数
    switch (request->flow) {
        case EG800_AT_FLOW_TEXT: {
            // 普通AT命令，不需要额外参数
            return true;
        }

        case EG800_AT_FLOW_TEXT_URC: {
            // TEXT_URC流程在收到OK后还要继续等待指定URC，必须提供期望的URC前缀（且不能是空字符）
            return (request->expect_urc_prefix != NULL) && (request->expect_urc_prefix[0] != '\0');
        }

        case EG800_AT_FLOW_SEND_DATA: {
            // 自定义提示符如果存在，则不能为空字符串
            if ((request->prompt_prefix != NULL) && (request->prompt_prefix[0] == '\0')) {
                return false;
            }
            // 发送长度大于0时，必须提供原始数据地址
            return (request->tx_data_len == 0U) || (request->tx_data != NULL);
        }

        case EG800_AT_FLOW_RECV_RAW: {
            // RECV_RAW流程需要提前知道接收长度，并由调用者提供足够大的原始数据缓冲区大小必须不小于期望接收长度
            if ((request->raw_expect_len == 0U) || (request->raw_expect_len > EG800_AT_MAX_RAW_SIZE) ||
                (request->raw_buf == NULL) || (request->raw_buf_size < request->raw_expect_len)) {
                return false;
            }
            return true;
        }

        default: {
            return false;
        }
    }
}

// ------------------------------------------------------接收相关---------------------------------------------------
/**
 * @brief 将单个字符加入 AT 文本行缓冲区。
 * @param ch 新收到的字符。
 * @note 实现逐字节拼行
 */
static void Eg800_Line_PutChar(char ch) {
    const Eg800AtRequest_t *request;
    const char *prompt;

    // 默认“>”提示符可能不带CR/LF，因此收到字符时立即识别
    if (s_eg800_current_busy == true) {
        request = &s_eg800_current_item.request;
        prompt = Eg800_Get_Prompt_Prefix(request);

        if ((s_eg800_at_state == EG800_AT_STATE_WAIT_PROMPT) && (strcmp(prompt, ">") == 0) && (ch == '>')) {
            s_eg800_line_len = 0U;
            Eg800_Handle_Line(">");
            return;
        }
    }

    if (ch == '\r') {
        return;
    }

    if (ch == '\n') {
        if (s_eg800_line_len == 0U) {
            return;
        }

        s_eg800_line_buf[s_eg800_line_len] = '\0';
        Eg800_Handle_Line(s_eg800_line_buf);
        s_eg800_line_len = 0U;
        return;
    }

    if (s_eg800_line_len >= (sizeof(s_eg800_line_buf) - 1U)) {
        printf("EG line overflow\r\n");
        NVIC_SystemReset();
        return;
    }

    s_eg800_line_buf[s_eg800_line_len] = ch;
    s_eg800_line_len++;
}

/**
 * @brief 处理一行完整的 EG800 串口文本。
 * @param line 不包含 CR/LF 的文本行。
 */
static void Eg800_Handle_Line(const char *line) {
    Eg800AtRequest_t *request;

    if (line == NULL) {
        return;
    }

    // 忽略提示符后可能附带的空格或制表符
    while ((*line == ' ') || (*line == '\t')) {
        line++;
    }

    // 字符串为空
    if (line[0] == '\0') {
        return;
    }

    /* 当前没有AT请求时，所有文本都按URC打印 */
    if (s_eg800_current_busy == false) {
        // 尝试按照已注册的URC分发
        if (Eg800_Route_Urc(line) == false) {
            LOG("未注册URC: %s", line);
        }
        return;
    }

    // 获取当前正在处理的AT请求
    request = &s_eg800_current_item.request;

    // 回显检查
    if (Eg800_Is_Echo_Line(request, line) == true) {
        LOG("AT 回显: %s", line);
        return;
    }

    /*
     * 等待数据提示符。
     * SEND_DATA等待“>”，RECV_RAW后续等待“CONNECT”。
     */
    if (s_eg800_at_state == EG800_AT_STATE_WAIT_PROMPT) {
        if (Eg800_Is_Prompt_Line(request, line) == true) {
            LOG("收到数据提示符: %s", line);
            Eg800_Service_Handle_Prompt();
            return;
        }

        // 模块可能在给出提示符之前直接拒绝命令
        if (strcmp(line, "ERROR") == 0) {
            LOG("AT 结果: ERROR");
            Eg800_Request_Complete(EG800_AT_RESULT_ERROR, -1);
            return;
        }
        if (Eg800_Starts_With(line, "+CME ERROR") == true) {
            LOG("AT 结果: %s", line);
            Eg800_Request_Complete(EG800_AT_RESULT_CME_ERROR, Eg800_Parse_Module_Error(line));
            return;
        }
        if (Eg800_Starts_With(line, "+CMS ERROR") == true) {
            LOG("AT 结果: %s", line);
            Eg800_Request_Complete(EG800_AT_RESULT_CMS_ERROR, Eg800_Parse_Module_Error(line));
            return;
        }

        if (Eg800_Route_Urc(line) == false) {
            LOG("等待提示符期间收到未识别文本: %s", line);
        }
        return;
    }

    // 已收到OK，继续等待当前请求指定的异步URC
    if (s_eg800_at_state == EG800_AT_STATE_WAIT_URC) {
        if ((request->expect_urc_prefix != NULL) && (Eg800_Starts_With(line, request->expect_urc_prefix) == true)) {
            LOG("收到期望URC: %s", line);

            Eg800_Request_Append_Rx(line);
            Eg800_Request_Complete(EG800_AT_RESULT_OK, -1);
        } else {
            if (Eg800_Route_Urc(line) == false) {
                LOG("等待期间收到未注册URC: %s", line);
            }
        }

        return;
    }

    // 等待普通AT终止结果
    if (s_eg800_at_state == EG800_AT_STATE_WAIT_FINAL) {
        if (strcmp(line, "OK") == 0) {
            LOG("AT 结果: OK");
            /*
             * TEXT_URC固定需要等待URC；SEND_DATA和RECV_RAW可按需等待URC。
             * 分别用于MQTT发布，以及HTTP读取等在OK之后还有结果URC的流程。
             */
            if ((request->flow == EG800_AT_FLOW_TEXT_URC) || (((request->flow == EG800_AT_FLOW_SEND_DATA) || (request->flow == EG800_AT_FLOW_RECV_RAW)) &&
                                                              (request->expect_urc_prefix != NULL) && (request->expect_urc_prefix[0] != '\0'))) {
                Eg800_Service_Enter_Wait_Urc();
            } else {
                Eg800_Request_Complete(EG800_AT_RESULT_OK, -1);
            }
            return;
        }

        if (strcmp(line, "ERROR") == 0) {
            LOG("AT 结果: ERROR");
            Eg800_Request_Complete(EG800_AT_RESULT_ERROR, -1);
            return;
        }

        if (Eg800_Starts_With(line, "+CME ERROR") == true) {
            LOG("AT 结果: %s", line);
            Eg800_Request_Complete(EG800_AT_RESULT_CME_ERROR, Eg800_Parse_Module_Error(line));
            return;
        }

        if (Eg800_Starts_With(line, "+CMS ERROR") == true) {
            LOG("AT 结果: %s", line);
            Eg800_Request_Complete(EG800_AT_RESULT_CMS_ERROR, Eg800_Parse_Module_Error(line));
            return;
        }

        /*
         * 提供rsp_prefix时，只保存匹配该前缀的响应。
         * 例如AT+CSQ使用"+CSQ:"。
         */
        if (request->rsp_prefix != NULL) {
            if (Eg800_Starts_With(line, request->rsp_prefix) == true) {
                Eg800_Request_Append_Rx(line);
                LOG("AT 回复: %s", line);
            } else if (Eg800_Route_Urc(line) == false) {
                LOG("AT未匹配文本: %s", line);
            }
            return;
        }

        // 没有指定rsp_prefix时，先排除已注册URC
        if (Eg800_Route_Urc(line) == false) {
            Eg800_Request_Append_Rx(line);
            LOG("AT 回复: %s", line);
        }
        return;
    }

    /* WAIT_RAW数据不经过文本行处理 */
    LOG_WARN("当前状态未处理文本: state=%u, line=%s", (unsigned int)s_eg800_at_state, line);
}

/**
 * @brief 将接收数据保存到当前请求的原始数据缓冲区。
 * @param data 新收到的数据。
 * @param len 当前可用数据长度。
 * @return 本次实际消费的字节数。
 */
static size_t Eg800_Service_Put_Raw_Data(const uint8_t *data, size_t len) {
    Eg800AtRequest_t *request;
    Eg800AtResultInfo_t *result_info;
    size_t remain_len;
    size_t used_len;

    if ((s_eg800_current_busy == false) || (data == NULL) || (len == 0U)) {
        return 0U;
    }

    request = &s_eg800_current_item.request;
    result_info = s_eg800_current_item.result_info;

    if ((result_info == NULL) || (request->raw_buf == NULL) || (request->raw_expect_len == 0U)) {
        return 0U;
    }

    if (result_info->raw_len >= request->raw_expect_len) {
        Eg800_Service_Enter_Wait_Final();
        return 0U;
    }

    remain_len = request->raw_expect_len - result_info->raw_len;
    used_len = (len < remain_len) ? len : remain_len;

    memcpy(&request->raw_buf[result_info->raw_len], data, used_len);
    result_info->raw_len += used_len;

    if (result_info->raw_len >= request->raw_expect_len) {
        LOG("原始数据接收完成: len=%lu", (unsigned long)result_info->raw_len);
        Eg800_Service_Enter_Wait_Final();
    }

    return used_len;
}

// --------------------------------------------------核心处理任务-----------------------------------------
/**
 * @brief 等待RX中断、新AT请求或周期超时检查事件。
 */
static void Eg800_Service_Wait_Event(void) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EG800_SERVICE_EVENT_WAIT_MS));
}

/**
 * @brief 连续读取底层RX环形缓冲区，并送入RAW或文本解析流程。
 */
static void Eg800_Service_Poll_Rx(void) {
    size_t rx_len;
    size_t index;
    size_t used_len;

    do {
        // 尝试读取
        rx_len = Drv_EG_Read(s_eg800_rx_chunk, sizeof(s_eg800_rx_chunk));
        index = 0U;

        // 如果读取到了
        while (index < rx_len) {
            // 必须先检查是否为需要处理的原始数据
            if (s_eg800_at_state == EG800_AT_STATE_WAIT_RAW) {
                used_len = Eg800_Service_Put_Raw_Data(&s_eg800_rx_chunk[index], rx_len - index);
                if (used_len > 0U) {
                    index += used_len;
                    continue;
                }
            }

            Eg800_Line_PutChar((char)s_eg800_rx_chunk[index]);
            index++;
        }
    } while (rx_len == sizeof(s_eg800_rx_chunk));
}


/**
 * @brief 当前阶段是否超时
 */
static void Eg800_At_CheckTimeout(void) {
    TickType_t elapsed_tick;

    // 当前是否有正在处理的指令
    if ((s_eg800_current_busy == false) || (s_eg800_at_state == EG800_AT_STATE_IDLE) || (s_eg800_wait_timeout_tick == 0U)) {
        return;
    }

    // 计算已经度过的时间
    elapsed_tick = xTaskGetTickCount() - s_eg800_wait_start_tick;

    if (elapsed_tick >= s_eg800_wait_timeout_tick) {
        LOG_ERROR("EG AT TIMEOUT: %s\r\n", (s_eg800_current_item.request.name != NULL) ? s_eg800_current_item.request.name : "-");
        Eg800_Request_Complete(EG800_AT_RESULT_TIMEOUT, -1);
    }
}

/**
 * @brief 空闲时尝试启动下一条 AT 请求。
 */
static void Eg800_Service_Try_Start_Next(void) {
    Eg800AtQueueItem_t item;

    // 安全检查
    if ((s_eg800_at_queue == NULL) || (s_eg800_current_busy == true) || (s_eg800_at_state != EG800_AT_STATE_IDLE)) {
        return;
    }

    if (xQueueReceive(s_eg800_at_queue,&item,0U) != pdPASS) {
        return;
    }

    // 成功取到
    s_eg800_current_item = item;
    s_eg800_current_busy = true;

    Eg800_Service_Start_Request();
}



static void Eg800_Service_Task(void *parameters) {
    (void)parameters;

    while (1) {
        Eg800_Service_Wait_Event();
        Eg800_Service_Poll_Rx();
        Eg800_At_CheckTimeout();
        Eg800_Service_Try_Start_Next();
    }
}

// ------------------------------------------------------公开的API函数---------------------------------------------------
/**
 * @brief 初始化EG800服务层并创建服务任务。
 * @return RESULT_SUCCESS表示成功，RESULT_FAIL表示失败。
 */
Result_t Eg800_Service_Init(void) {
    BaseType_t result;

    // 句柄检查
    if (s_eg800_service_task_handle != NULL) {
        return RESULT_SUCCESS;
    }

    // 创建 AT 发送命令队列
    s_eg800_at_queue = xQueueCreate(EG800_AT_QUEUE_LENGTH, sizeof(Eg800AtQueueItem_t));
    if (s_eg800_at_queue == NULL) {
        return RESULT_FAIL;
    }

    // 创建任务
    result = xTaskCreate(Eg800_Service_Task,
                         "eg800_srv",
                         EG800_SERVICE_TASK_STACK_SIZE,
                         NULL,
                         EG800_SERVICE_TASK_PRIORITY,
                         &s_eg800_service_task_handle);
    if (result != pdPASS) {
        vQueueDelete(s_eg800_at_queue);
        s_eg800_at_queue = NULL;
        s_eg800_service_task_handle = NULL;
        return RESULT_FAIL;
    }

    return RESULT_SUCCESS;
}

/**
 * @brief 注册一条URC前缀路由。
 * @param prefix URC前缀，字符串必须长期有效。
 * @param owner URC归属模块，参考Eg800AtOwner_e。
 * @param handler 匹配后的处理函数。
 * @return RESULT_SUCCESS表示注册成功，RESULT_FAIL表示参数错误或路由表已满。
 */
Result_t Eg800_Urc_Register(const char *prefix, uint8_t owner, Eg800AtUrcHandler_t handler) {
    size_t index;
    size_t empty_index = EG800_SERVICE_URC_ROUTE_MAX;
    Result_t result = RESULT_FAIL;

    if ((prefix == NULL) || (prefix[0] == '\0') || (handler == NULL)) {
        return RESULT_FAIL;
    }

    for (index = 0U; index < EG800_SERVICE_URC_ROUTE_MAX; index++) {
        if (s_eg800_urc_routes[index].prefix == NULL) {
            if (empty_index == EG800_SERVICE_URC_ROUTE_MAX) {
                empty_index = index;
            }
            continue;
        }

        // 相同前缀重复注册时，更新归属和回调
        if (strcmp(s_eg800_urc_routes[index].prefix, prefix) == 0) {
            s_eg800_urc_routes[index].owner = owner;
            s_eg800_urc_routes[index].handler = handler;
            result = RESULT_SUCCESS;
            break;
        }
    }

    if ((result != RESULT_SUCCESS) && (empty_index < EG800_SERVICE_URC_ROUTE_MAX)) {
        s_eg800_urc_routes[empty_index].prefix = prefix;
        s_eg800_urc_routes[empty_index].owner = owner;
        s_eg800_urc_routes[empty_index].handler = handler;
        result = RESULT_SUCCESS;
    }

    return result;
}

/**
 * @brief 批量注册 URC 路由。
 * @param routes 待注册的 URC 路由数组，数组中的字符串必须长期有效。
 * @param route_count URC 路由数量。
 * @return RESULT_SUCCESS 表示全部注册成功，RESULT_FAIL 表示参数错误或注册失败。
 */
Result_t Eg800_Urc_Register_Table(const Eg800UrcRegistration_t *routes, size_t route_count) {
    const Eg800UrcRegistration_t *route;
    size_t index;

    if ((routes == NULL) || (route_count == 0U) || (route_count > EG800_SERVICE_URC_ROUTE_MAX)) {
        return RESULT_FAIL;
    }

    // 先检查整张表，避免遇到非法项时已经注册了前面的部分路由。
    for (index = 0U; index < route_count; index++) {
        route = &routes[index];

        if ((route->prefix == NULL) || (route->prefix[0] == '\0') || (route->handler == NULL) ||
            (route->owner <= EG800_AT_OWNER_NONE) || (route->owner > EG800_AT_OWNER_SYSTEM)) {
            LOG_ERROR("URC 注册表参数错误: index=%u", (unsigned int)index);
            return RESULT_FAIL;
        }
    }

    for (index = 0U; index < route_count; index++) {
        route = &routes[index];

        if (Eg800_Urc_Register(route->prefix, route->owner, route->handler) != RESULT_SUCCESS) {
            LOG_ERROR("URC 批量注册失败: %s", route->prefix);
            return RESULT_FAIL;
        }
    }

    return RESULT_SUCCESS;
}

/**
 * @brief 执行一条 AT 请求。
 * @param request AT 请求描述。
 * @param result_info 执行结果输出，可为 NULL。
 * @return AT 执行结果。
 * @note 服务层不能调用此接口，否则否则服务任务会等待自己完成请求导致死锁
 */
Eg800AtResult_e Eg800_AT_Execute(const Eg800AtRequest_t *request, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    Eg800AtQueueItem_t item;               // 待入队的请求包
    Eg800AtResultInfo_t local_result_info; // 备用结果对象
    Eg800AtResultInfo_t *out_info;         // 实际使用的结果指针（二选一，指向调用者result_info的或者临时的local_result_info）
    TaskHandle_t caller_task;              // 调用此函数的任务句柄，用于检查是否为 Service 自身，并指定请求完成后通知谁
    BaseType_t wait_result;                // 等待完成通知时取得的通知结果

    /* 调用者不需要详细结果时，使用函数内部的临时结果对象 */
    out_info = (result_info != NULL) ? result_info : &local_result_info;

    Eg800_Result_Init(out_info, request, EG800_AT_RESULT_PARAM_ERROR);

    /* 服务未初始化或者请求参数不合法 */
    if ((s_eg800_at_queue == NULL) || (s_eg800_service_task_handle == NULL) || (completion_sem == NULL) || (Eg800_Request_Validate(request) == false)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 服务任务不能调用同步接口，否则服务任务会等待自己完成请求形成死锁
    caller_task = xTaskGetCurrentTaskHandle();
    if ((caller_task == NULL) || (caller_task == s_eg800_service_task_handle)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    /* 清空调用者提供的文本响应缓冲区 */
    if ((request->rx_buf != NULL) && (request->rx_buf_size > 0U)) {
        request->rx_buf[0] = '\0';
    }

    /* 构造要发送给服务任务的队列项 */
    (void)memset(&item, 0, sizeof(item));
    item.request = *request;
    item.result_info = out_info;
    item.notify_task = caller_task;
    item.completion_sem = completion_sem;

    // 清除本次调用使用的完成信号量中残留的旧信号
    (void)xSemaphoreTake(completion_sem, 0U);

    /* 队列已满时立即返回，不在这里阻塞等待队列空间 */
    if (xQueueSend(s_eg800_at_queue, &item, 0U) != pdPASS) {
        LOG_ERROR("AT发送队列以及满了,该条丢弃");
        out_info->result = EG800_AT_RESULT_QUEUE_FULL;
        return EG800_AT_RESULT_QUEUE_FULL;
    }

    xTaskNotifyGive(s_eg800_service_task_handle); // 通知核心Service服务

    wait_result  = xSemaphoreTake(completion_sem, pdMS_TO_TICKS(EG800_AT_SYNC_MAX_WAIT_MS));
    if (wait_result != pdTRUE) {
        // 这 xxx 秒包括：请求在队列中等待，+ AT 命令执行，+ 等待响应/URC/数据。走到这里通常说明服务任务卡住状态机异常或请求没有被正确完成
        LOG_ERROR("EG AT等待超过%u毫秒，系统即将复位，命令：%s\r\n", (unsigned int)EG800_AT_SYNC_MAX_WAIT_MS, (request->name != NULL) ? request->name : "-");
        out_info->result = EG800_AT_RESULT_TIMEOUT;
        NVIC_SystemReset();
    }

    return out_info->result;
}

/**
 * @brief 从接收中断唤醒EG800服务任务。
 * @note 仅供USART IDLE和RX DMA HT/TC中断调用，中断中不搬运、不解析数据。
 */
void Eg800_Service_Rx_Notify_From_ISR(void) {
    // 假设当前没有更高优先级的任务被唤醒
    BaseType_t higher_priority_task_woken = pdFALSE;

    // 检查句柄
    if (s_eg800_service_task_handle == NULL) {
        return;
    }

    // 通知目标任务
    vTaskNotifyGiveFromISR(s_eg800_service_task_handle, &higher_priority_task_woken);

    // 如果需要，触发上下文切换
    portYIELD_FROM_ISR(higher_priority_task_woken);
}
