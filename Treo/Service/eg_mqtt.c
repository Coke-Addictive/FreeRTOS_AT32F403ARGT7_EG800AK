#include "eg_mqtt.h"


#include "eg_network.h"


#define LOG_SUB_ENABLE              EG_MQTT_LOG_ENABLE          // 由 Service 配置控制日志输出
#define LOG_TAG "EG_MQTT"
#include "log.h"

// AT 配置、提示符和终止响应超时。
#define EG800_MQTT_CONFIG_TIMEOUT_MS                    5000U       // QMTCFG 等待 OK/ERROR 的超时时间，单位 ms
#define EG800_MQTT_FINAL_TIMEOUT_MS                     5000U       // 其他命令等待提示符或 OK/ERROR 的超时时间，单位 ms
#define EG800_MQTT_OPEN_URC_TIMEOUT_MS                  125000U     // QMTOPEN  打开网络结果，  默认 120 秒 + 余量 5 秒，单位 ms
#define EG800_MQTT_CLOSE_URC_TIMEOUT_MS                 35000U      // QMTCLOSE 关闭网络结果，  默认 30 秒 + 余量 5 秒，单位 ms
#define EG800_MQTT_DISCONNECT_URC_TIMEOUT_MS            35000U      // QMTDISC  断开连接结果，  默认 30 秒 + 余量 5 秒，单位 ms
#define EG800_MQTT_CONNECT_URC_TIMEOUT_MS               10000       // QMTCONN  登录结果，      默认 5 秒 + 余量 5 秒，单位 ms
#define EG800_MQTT_SUBSCRIBE_URC_TIMEOUT_MS             20000       // QMTSUB   订阅结果，  默认 15 秒 + 余量 5 秒，单位 ms
#define EG800_MQTT_UNSUBSCRIBE_URC_TIMEOUT_MS           20000       // QMTUNS   退订结果，  默认 15 秒 + 余量 5 秒，单位 ms
#define EG800_MQTT_PUBLISH_URC_TIMEOUT_MS               20000       // QMTPUBEX 发布结果，  默认 15 秒 + 余量 5 秒，单位 ms


// Mqtt层内部AT命令编号
typedef enum {
    EG800_MQTT_CMD_CFG_PDP_CONTEXT = 0,                 // 配置 MQTT 使用的 PDP 场景
    EG800_MQTT_CMD_CFG_VERSION,                         // 配置 MQTT 协议版本
    EG800_MQTT_CMD_CFG_KEEPALIVE,                       // 配置心跳频率
    EG800_MQTT_CMD_CFG_SESSION,                         // 配置会话类型
    EG800_MQTT_CMD_CFG_RECV_MODE,                       // 配置接收模式（URC中包含接收消息的长度信息）
    EG800_MQTT_CMD_CFG_WAKEUP_TOPIC,                    // 配置唤醒TOPIC
    EG800_MQTT_CMD_OPEN_CLIENT_NET,                     // 打开MQTT客户端的网络
    EG800_MQTT_CMD_QUERY_CLIENT_NET,                    // 查询 MQTT 客户端网络是否仍存在
    EG800_MQTT_CMD_CLOSE_CLIENT_NET,                    // 关闭MQTT客户端的网络
    EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE,         // 连接客户端到MQTT服务器 （建立会话，可携带用户名和密码）
    EG800_MQTT_CMD_DISCONNECT_CLIENT_MQTT_SERVICE,      // 断开客户端断开与MQTT服务器的连接
    EG800_MQTT_CMD_SUBSCRIBE_TOPIC,                     // 订阅TOPIC
    EG800_MQTT_CMD_UNSUBSCRIBE_TOPIC,                   // 退订TOPIC
    EG800_MQTT_CMD_PUBLISH_TOPIC,                       // 发布TOPIC
} Eg800MqttCmd_e;




static TaskHandle_t s_eg800_mqtt_task_handle = NULL;                    // 任务句柄

static bool s_eg800_mqtt_initialized = false;                           // Mqtt 层初始化标志位

static QueueHandle_t s_eg800_mqtt_receive_queue = NULL;                 // MQTT 接收消息队列

static Eg800MqttReceiveMessage_t s_eg800_mqtt_receive_message = {0};    // URC 解析缓存，仅由 Service 任务使用

static bool s_eg800_mqtt_should_connect = false;                        // 上层是否希望MQTT保持连接（目标状态）

static Eg800MqttSnapshot_t s_eg800_mqtt_snapshot = {0};                 // Mqtt 信息缓存及刷新状态

static SemaphoreHandle_t s_mqtt_at_completion_sem = NULL;               // Mqtt 层 AT 请求完成信号量

static uint8_t s_eg800_mqtt_retry_count = 0U;                           // 当前Mqtt连接流程已经尝试的次数

static uint32_t s_eg800_mqtt_receive_drop_count = 0U;                   // 接收队列满时丢弃旧消息的累计次数

static Eg800MqttSubscribeList_t s_eg800_mqtt_subscribe_list = {0};


// 实际处理URC的函数声明
static void Eg800_Mqtt_LinkStateChange_Urc_Handler(const char *line);
static void Eg800_Mqtt_Receive_Message_Urc_Handler(const char *line);
static void Eg800_Mqtt_HeartPing_Failed_Urc_Handler(const char *line);




// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_mqtt_urc_routes[] = {
    {"+QMTSTAT:",      EG800_AT_OWNER_MQTT,        Eg800_Mqtt_LinkStateChange_Urc_Handler},
    {"+QMTRECV:",      EG800_AT_OWNER_MQTT,        Eg800_Mqtt_Receive_Message_Urc_Handler},
    {"+QMTPING:",      EG800_AT_OWNER_MQTT,        Eg800_Mqtt_HeartPing_Failed_Urc_Handler},
};




// ------------------------------------------------- 函数声明 -----------------------------------------------------
// URC 注册
static Result_t Eg800_Mqtt_Register_Urcs(void);

// 过滤 MQTT 登录过程中的重传通知，仅将最终结果交给同步接口。
static bool Eg800_Mqtt_Connect_Final_Urc_Match(const char *line, const void *context);

// 按客户端编号和消息编号筛选订阅最终结果，重传通知不结束请求。
static bool Eg800_Mqtt_Subscribe_Final_Urc_Match(const char *line, const void *context);

// 按客户端编号和消息编号筛选退订最终结果，重传通知不结束请求。
static bool Eg800_Mqtt_Unsubscribe_Final_Urc_Match(const char *line, const void *context);

// 解析Mqtt消息
static bool Eg800_Mqtt_Parse_Receive_Message(const char *line, const char **topic_start, size_t *topic_len, const char **payload_start, size_t *payload_len);

static bool Eg800_Mqtt_Should_Connect(void);

static void Eg800_Mqtt_Update_Snapshot(const Eg800MqttInfo_t *info);

static Eg800AtResult_e Eg800_Mqtt_Connect_Once(Eg800MqttInfo_t *info);

static Eg800AtResult_e Eg800_Mqtt_Confirm_Network_Closed(void);
static Eg800AtResult_e Eg800_Mqtt_Disconnect_Once(void);

// 核心任务
static void Eg800_Mqtt_Task(void *argument);



//--------------------------------------------------------URC 注册/处理-----------------------------------------

/**
 * @brief 注册 Mqtt 层负责主动上报的URC
 */
static Result_t Eg800_Mqtt_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_mqtt_urc_routes,ARRAY_SIZE(s_eg800_mqtt_urc_routes));
}


//[TODO]:手册 29/39页 有8种错误码


/**
 * @brief 处理 MQTT 链路状态变化，通知 Task 链路已失效。
 * @note 仅解析、记录和通知，不执行 AT，不修改连接意图和重试次数。
 */
static void Eg800_Mqtt_LinkStateChange_Urc_Handler(const char *line) {
    uint32_t client_index = 0U;
    uint32_t error_code = 0U;
    int fields_end = 0;
    const char *reason;
    if (line == NULL) {
        return;
    }
    // 校验整行格式；错误码 9~255 为手册保留值。
    if ((sscanf(line, "+QMTSTAT: %1u , %3u %n", &client_index, &error_code, &fields_end) != 2) ||
        (fields_end == 0) || (line[fields_end] != '\0') || (error_code == 0U) || (error_code > 255U)) {
        LOG_WARN("MQTT 链路状态 URC 无效: %s", line);
        return;
    }

    // 只处理当前维护的客户端。
    if (client_index != EG800_MQTT_CLIENT_INDEX) {
        return;
    }

    switch (error_code) {
        case 1U: { reason = "服务器断开或重置连接"; break; }
        case 2U: { reason = "发送 PINGREQ 超时或失败"; break; }
        case 3U: { reason = "发送 CONNECT 超时或失败"; break; }
        case 4U: { reason = "接收 CONNACK 超时或失败"; break; }
        case 5U: { reason = "发送 DISCONNECT 后服务器断开连接，属于正常流程"; break; }
        case 6U: { reason = "连续发送失败，客户端主动断开连接"; break; }
        case 7U: { reason = "链路不可用或服务器不可用"; break; }
        case 8U: { reason = "客户端主动断开连接"; break; }
        default: { reason = "保留或未知链路错误"; break; }
    }

    LOG("MQTT 链路状态变化: client=%u, error=%u, reason=%s", client_index, error_code, reason);

    if ((s_eg800_mqtt_initialized == false) || (s_eg800_mqtt_task_handle == NULL)) {
        return;
    }

    (void)xTaskNotify(s_eg800_mqtt_task_handle, EG800_MQTT_EVENT_LINK_LOST, eSetBits);
}

/**
 * @brief 接收 MQTT 下行消息并投递队列，满时丢弃最旧消息。
 * @note 仅由 Service 任务投递；当前处理完整单行文本上报。
 */
static void Eg800_Mqtt_Receive_Message_Urc_Handler(const char *line) {
    const char *topic_start;
    const char *payload_start;
    size_t topic_len;
    size_t payload_len;
    bool dropped_oldest = false;

    if ((s_eg800_mqtt_initialized == false) || (s_eg800_mqtt_receive_queue == NULL)) {
        return;
    }
    if (Eg800_Mqtt_Parse_Receive_Message(line, &topic_start, &topic_len, &payload_start, &payload_len) == false) {
        return;
    }

    // 校验通过后再腾位，检查和移除期间禁止业务任务抢先取走消息。
    taskENTER_CRITICAL();
    if (uxQueueSpacesAvailable(s_eg800_mqtt_receive_queue) == 0U) {
        if (xQueueReceive(s_eg800_mqtt_receive_queue, &s_eg800_mqtt_receive_message, 0U) == pdPASS) {
            s_eg800_mqtt_receive_drop_count++;
            dropped_oldest = true;
        }
    }
    taskEXIT_CRITICAL();

    // 腾位后复用静态缓存，复制新消息。
    (void)memset(&s_eg800_mqtt_receive_message, 0, sizeof(s_eg800_mqtt_receive_message));
    memcpy(s_eg800_mqtt_receive_message.topic, topic_start, topic_len);
    s_eg800_mqtt_receive_message.topic[topic_len] = '\0';
    s_eg800_mqtt_receive_message.payload_len = payload_len;
    memcpy(s_eg800_mqtt_receive_message.payload, payload_start, payload_len);

    if (xQueueSend(s_eg800_mqtt_receive_queue, &s_eg800_mqtt_receive_message, 0U) != pdPASS) {
        LOG_ERROR("MQTT 接收消息投递失败");
        return;
    }
    if (dropped_oldest == true) {
        LOG("MQTT 接收队列已满，丢弃最旧消息，累计=%lu", (unsigned long)s_eg800_mqtt_receive_drop_count);
    }
    LOG("MQTT 消息已入队: topic=%s, len=%lu", s_eg800_mqtt_receive_message.topic, (unsigned long)payload_len);
}



/**
 * @brief 处理 MQTT 心跳失败，通知 Task 链路已失效。
 */
static void Eg800_Mqtt_HeartPing_Failed_Urc_Handler(const char *line) {
    unsigned int client_index = 0U;
    unsigned int result = 0U;
    int fields_end = 0;

    if (line == NULL) {
        return;
    }

    // 手册规定 result=1 表示心跳失败。
    if ((sscanf(line, "+QMTPING: %1u , %1u %n", &client_index, &result, &fields_end) != 2) ||
        (fields_end == 0) || (line[fields_end] != '\0') || (result != 1U)) {
        LOG_WARN("MQTT 心跳 URC 无效: %s", line);
        return;
    }

    if (client_index != EG800_MQTT_CLIENT_INDEX) {
        return;
    }

    LOG_WARN("MQTT 心跳失败，停止自动连接: client=%u", client_index);

    if ((s_eg800_mqtt_initialized == false) || (s_eg800_mqtt_task_handle == NULL)) {
        return;
    }

    // 心跳失败，关闭 MQTT 连接意图。
    taskENTER_CRITICAL();
    s_eg800_mqtt_should_connect = false;
    taskEXIT_CRITICAL();

    // 通知 Task 处理链路失效
    (void)xTaskNotify(s_eg800_mqtt_task_handle, EG800_MQTT_EVENT_LINK_LOST, eSetBits);
}



//--------------------------------------------------------关键函数实体-----------------------------------------
/**
 * @brief 解析 MQTT 下行消息，返回主题和内容的位置及长度。
 * @note 输出指针引用 line，仅在本次 URC 处理期间有效。
 */
static bool Eg800_Mqtt_Parse_Receive_Message(const char *line, const char **topic_start, size_t *topic_len, const char **payload_start, size_t *payload_len) {
    const char *topic_end;
    const char *cursor;
    const char *line_end;
    unsigned int client_index;
    unsigned int msg_id;
    unsigned int reported_len = 0U;
    int fields_end = 0;
    bool has_length = false;

    if (line == NULL) {
        return false;
    }

    // 解析客户端编号、消息编号和主题起始引号。
    if ((sscanf(line, "+QMTRECV: %1u , %5u , \"%n", &client_index, &msg_id, &fields_end) != 2) || (fields_end == 0) || (msg_id > UINT16_MAX)) {
        LOG_ERROR("MQTT 接收头部无效");
        return false;
    }
    if (client_index != EG800_MQTT_CLIENT_INDEX) {
        return false;
    }

    // 定位并校验主题。
    *topic_start = line + fields_end;
    topic_end = strchr(*topic_start, '"');
    if (topic_end == NULL) {
        LOG_ERROR("MQTT 接收主题格式无效");
        return false;
    }
    *topic_len = (size_t)(topic_end - *topic_start);
    if ((*topic_len == 0U) || (*topic_len > EG800_MQTT_TOPIC_MAX_LENGTH)) {
        LOG_ERROR("MQTT 接收主题长度无效");
        return false;
    }

    // 跳过主题后的分隔符及空白。
    cursor = topic_end + 1;
    while ((*cursor == ' ') || (*cursor == '\t')) {
        cursor++;
    }
    if (*cursor != ',') {
        LOG_ERROR("MQTT 接收字段分隔符无效");
        return false;
    }
    cursor++;
    while ((*cursor == ' ') || (*cursor == '\t')) {
        cursor++;
    }

    // 兼容不带长度和带长度的上报格式。
    if (*cursor == '"') {
        *payload_start = cursor + 1;
    } else {
        fields_end = 0;
        if ((*cursor < '0') || (*cursor > '9') || (sscanf(cursor, "%5u , \"%n", &reported_len, &fields_end) != 1) || (fields_end == 0) || (reported_len > EG800_MQTT_RECEIVE_MAX_LENGTH)) {
            LOG_ERROR("MQTT 接收长度字段无效");
            return false;
        }
        has_length = true;
        *payload_start = cursor + fields_end;
    }

    // 用末尾引号确定边界，保留内容内部的引号和逗号。
    line_end = *payload_start + strlen(*payload_start);
    while ((line_end > *payload_start) && ((line_end[-1] == ' ') || (line_end[-1] == '\t'))) {
        line_end--;
    }
    if ((line_end <= *payload_start) || (line_end[-1] != '"')) {
        LOG_ERROR("MQTT 接收内容格式无效");
        return false;
    }
    *payload_len = (size_t)(line_end - *payload_start - 1);
    if ((*payload_len > EG800_MQTT_RECEIVE_MAX_LENGTH) || ((has_length == true) && (*payload_len != (size_t)reported_len))) {
        LOG_ERROR("MQTT 接收内容长度不匹配或超限");
        return false;
    }

    return true;
}

/**
 * @brief 筛选当前客户端的 MQTT 登录最终结果，重传通知不结束请求。
 */
static bool Eg800_Mqtt_Connect_Final_Urc_Match(const char *line, const void *context) {
    unsigned int client_index;
    unsigned int connect_result;
    int fields_end = 0;

    (void)context;

    // 无法解析的响应交给调用者返回 DATA_FAIL，避免一直等到超时。
    if ((sscanf(line, "+QMTCONN: %1u , %1u %n", &client_index, &connect_result, &fields_end) != 2) ||
        (fields_end == 0) || ((line[fields_end] != '\0') && (line[fields_end] != ','))) {
        return true;
    }

    if (client_index != EG800_MQTT_CLIENT_INDEX) {
        return false;
    }

    // result=1 仅表示模块正在重传，继续等待成功或最终失败。
    return (connect_result != 1U);
}

/**
 * @brief 筛选本次订阅的最终结果，context 指向本次请求的 uint16_t 消息编号。
 */
static bool Eg800_Mqtt_Subscribe_Final_Urc_Match(const char *line, const void *context) {
    unsigned int client_index;
    unsigned int response_msg_id;
    unsigned int subscribe_result;
    int fields_end = 0;

    // 无法解析的响应交给调用者返回 DATA_FAIL，避免一直等到超时。
    if ((context == NULL) ||
        (sscanf(line, "+QMTSUB: %1u , %5u , %1u %n", &client_index, &response_msg_id, &subscribe_result, &fields_end) != 3) ||
        (fields_end == 0) || ((line[fields_end] != '\0') && (line[fields_end] != ',')) ||
        (client_index > 5U) || (response_msg_id == 0U) || (response_msg_id > UINT16_MAX) || (subscribe_result > 2U)) {
        return true;
    }

    if ((client_index != EG800_MQTT_CLIENT_INDEX) || (response_msg_id != *(const uint16_t *)context)) {
        return false;
    }

    return (subscribe_result != 1U);
}

/**
 * @brief 筛选本次退订的最终结果，context 指向本次请求的 uint16_t 消息编号。
 */
static bool Eg800_Mqtt_Unsubscribe_Final_Urc_Match(const char *line, const void *context) {
    unsigned int client_index;
    unsigned int response_msg_id;
    unsigned int unsubscribe_result;
    int fields_end = 0;

    // 无法解析的响应交给调用者返回 DATA_FAIL，避免一直等到超时。
    if ((context == NULL) ||
        (sscanf(line, "+QMTUNS: %1u , %5u , %1u %n", &client_index, &response_msg_id, &unsubscribe_result, &fields_end) != 3) ||
        (fields_end == 0) || ((line[fields_end] != '\0') && (line[fields_end] != ',')) ||
        (client_index > 5U) || (response_msg_id == 0U) || (response_msg_id > UINT16_MAX) || (unsubscribe_result > 2U)) {
        return true;
    }

    if ((client_index != EG800_MQTT_CLIENT_INDEX) || (response_msg_id != *(const uint16_t *)context)) {
        return false;
    }

    return (unsubscribe_result != 1U);
}

/**
 * @brief 同步执行一条 MQTT 服务的 AT 请求，支持指定结果 URC 筛选函数。
 * @param cmd_id MQTT 服务内部命令编号。
 * @param flow 执行流程，支持 TEXT、TEXT_URC 和 SEND_DATA。
 * @param name 命令日志名称，可为 NULL。
 * @param tx 完整 AT 命令，必须包含 "\r\n"，调用期间保持有效。
 * @param rsp_prefix 需要保存的文本响应前缀，可为 NULL。
 * @param expect_urc_prefix 期望的结果 URC 前缀；TEXT_URC 时必须非空，SEND_DATA 时填写后也会在 OK 后继续等待该 URC。
 * @param tx_data 提示符后发送的数据，仅 SEND_DATA 使用，调用期间保持有效。
 * @param tx_data_len 发送数据长度，单位字节；非 SEND_DATA 流程传 0。
 * @param rx_buf 响应缓冲区，调用期间保持有效；不需要响应内容时传 NULL。
 * @param rx_buf_size 响应缓冲区容量，单位字节；rx_buf 为 NULL 时必须为 0。
 * @param final_timeout_ms 等待提示符或 OK/ERROR 的超时时间，单位 ms；0 表示使用 Service 默认值。
 * @param urc_timeout_ms 等待结果 URC 的超时时间，单位 ms；0 表示使用 Service 默认值。
 * @param result_info AT 执行详细结果，可为 NULL。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @param expect_urc_match 可选结果 URC 筛选函数；为 NULL 时沿用命令的默认筛选方式。
 * @param expect_urc_context 筛选上下文，引用对象必须保持有效直到本次同步调用返回。
 * @return OK 表示 AT 流程完成，其他值表示参数、发送、超时等执行错误；结果 URC 中的业务结果由调用者进一步解析。
 * @note MQTT 和 Service 应已初始化；仅供任务上下文调用。
 * @note 禁止在 ISR、Service 任务及其 URC 回调中调用。
 * @note 同一信号量同一时间只允许对应一条未完成请求。
 */
static Eg800AtResult_e Eg800_Mqtt_At_Execute_Matched(Eg800MqttCmd_e cmd_id, Eg800AtFlow_e flow, const char *name, const char *tx, const char *rsp_prefix, const char *expect_urc_prefix, const uint8_t *tx_data, size_t tx_data_len, char *rx_buf, size_t rx_buf_size, uint32_t final_timeout_ms, uint32_t urc_timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem, Eg800AtUrcMatch_t expect_urc_match, const void *expect_urc_context) {
    Eg800AtRequest_t request = {0};

    request.cmd.owner = EG800_AT_OWNER_MQTT;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = flow;

    request.name = name;
    request.tx = tx;
    request.rsp_prefix = rsp_prefix;
    request.expect_urc_prefix = expect_urc_prefix;
    request.expect_urc_match = expect_urc_match;
    request.expect_urc_context = expect_urc_context;

    if ((cmd_id == EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE) && (request.expect_urc_match == NULL)) {
        request.expect_urc_match = Eg800_Mqtt_Connect_Final_Urc_Match;
    }

    request.tx_data = tx_data;
    request.tx_data_len = tx_data_len;

    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    request.final_timeout_ms = final_timeout_ms;
    request.urc_timeout_ms = urc_timeout_ms;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}

// 保留现有基础接口的调用方式，不需要自定义结果筛选时使用默认参数。
static Eg800AtResult_e Eg800_Mqtt_At_Execute(Eg800MqttCmd_e cmd_id, Eg800AtFlow_e flow, const char *name, const char *tx, const char *rsp_prefix, const char *expect_urc_prefix, const uint8_t *tx_data, size_t tx_data_len, char *rx_buf, size_t rx_buf_size, uint32_t final_timeout_ms, uint32_t urc_timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    return Eg800_Mqtt_At_Execute_Matched(cmd_id, flow, name, tx, rsp_prefix, expect_urc_prefix, tx_data, tx_data_len, rx_buf, rx_buf_size, final_timeout_ms, urc_timeout_ms, result_info, completion_sem, NULL, NULL);
}


/**
 * @brief 将 Mqtt 层保存的信息恢复为未知状态。
 */
static void Eg800_Mqtt_Info_Reset(void) {
    (void)memset(&s_eg800_mqtt_snapshot, 0, sizeof(s_eg800_mqtt_snapshot));
    s_eg800_mqtt_snapshot.info.state = EG800_MQTT_STATE_NONE;
}



/**
 * @brief 在临界区内读取 MQTT 连接意图。
 * @return true 表示希望保持连接，false 表示应停止连接和重试。
 */
static bool Eg800_Mqtt_Should_Connect(void) {
    bool should_connect;

    taskENTER_CRITICAL();
    should_connect = s_eg800_mqtt_should_connect;
    taskEXIT_CRITICAL();

    return should_connect;
}

/**
 * @brief 更新 MQTT 全局快照。
 * @param info MQTT 任务维护的运行信息。
 * @note 仅由 MQTT Task 调用；参数为空时保持原快照。
 * @note 只同步运行状态，保留 config 和 config_valid。
 */
static void Eg800_Mqtt_Update_Snapshot(const Eg800MqttInfo_t *info) {
    TickType_t update_tick;

    if (info == NULL) {
        LOG_ERROR("MQTT 快照更新参数为空");
        return;
    }

    update_tick = xTaskGetTickCount();

    taskENTER_CRITICAL();

    s_eg800_mqtt_snapshot.info.state = info->state;
    s_eg800_mqtt_snapshot.update_tick = update_tick;
    s_eg800_mqtt_snapshot.refresh_in_progress = (info->state == EG800_MQTT_STATE_CHECK_NETWORK) ||
                                                (info->state == EG800_MQTT_STATE_CONNECTING) ||
                                                (info->state == EG800_MQTT_STATE_DISCONNECTING);

    taskEXIT_CRITICAL();
}




/**
 * @brief 根据已保存的配置，执行一次 MQTT 网络打开、登录和订阅。
 * @param info MQTT 任务维护的信息；仅返回 OK 时可作为本次成功结果。
 * @return CANCELLED 表示连接意图已关闭；PARAM_ERROR 表示未初始化或配置无效。
 * @return DATA_FAIL 表示网络未激活；AT 失败时返回原错误码。
 * @note 仅供 MQTT Task 串行调用，使用 MQTT 专用 AT 完成信号量。
 * @note 不执行重试、冷却或资源清理；最终 ONLINE 快照由 Task 确认后发布。
 */
static Eg800AtResult_e Eg800_Mqtt_Connect_Once(Eg800MqttInfo_t *info) {
    Eg800AtResult_e result;
    const char *username;
    const char *password;
    size_t topic_index;
    int subscribe_success_count = 0;

    if ((s_eg800_mqtt_initialized == false) || (info == NULL) || (s_mqtt_at_completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    if (Eg800_Mqtt_Should_Connect() == false) {
        return EG800_AT_RESULT_CANCELLED;
    }

    // 读取已保存的配置和有效标志，不清空整个 info。
    taskENTER_CRITICAL();
    info->config_valid = s_eg800_mqtt_snapshot.info.config_valid;
    if (info->config_valid == true) {
        info->config = s_eg800_mqtt_snapshot.info.config;
    }
    taskEXIT_CRITICAL();

    if (info->config_valid == false) {
        LOG_ERROR("单次 MQTT 连接失败: 未设置有效配置");
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 只检查当前网络，不等待 Network 联网。
    if (Eg800_Network_Is_Active() == false) {
        LOG_WARN("单次 MQTT 连接失败: PDP 未激活");
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 匿名连接时省略用户名和密码。
    username = (info->config.username[0] == '\0') ? NULL : info->config.username;
    password = (username == NULL) ? NULL : info->config.password;

    // 前置检查期间可能收到取消请求，发送第一条 AT 前再次确认。
    if (Eg800_Mqtt_Should_Connect() == false) {
        return EG800_AT_RESULT_CANCELLED;
    }

    info->state = EG800_MQTT_STATE_CONNECTING;
    Eg800_Mqtt_Update_Snapshot(info);

    LOG("单次 MQTT 连接开始: host=%s, port=%u", info->config.server_host, (unsigned int)info->config.server_port);

    // 1. 打开 MQTT 客户端网络。
    result = Eg800_Mqtt_Open_Network_Client(info->config.server_host, info->config.server_port, s_mqtt_at_completion_sem);

    // 等待期间可能收到取消请求，返回后优先检查连接意图。
    if (Eg800_Mqtt_Should_Connect() == false) {
        return EG800_AT_RESULT_CANCELLED;
    }

    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("单次 MQTT 打开客户端网络失败: result=%d", (int)result);
        return result;
    }

    // 2. 登录 MQTT 服务器。
    result = Eg800_Mqtt_Connect_Client_To_Server(info->config.client_id, username, password, s_mqtt_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("单次 MQTT 登录服务器失败: result=%d", (int)result);
        return result;
    }

    // 3. 每次连接成功后恢复订阅。
    for (topic_index = 0U; topic_index < s_eg800_mqtt_subscribe_list.count; topic_index++) {
        if (Eg800_Mqtt_Should_Connect() == false) {
            return EG800_AT_RESULT_CANCELLED;
        }

        result = Eg800_Mqtt_Subscribe_Topic((uint16_t)(topic_index + 1U), s_eg800_mqtt_subscribe_list.topics[topic_index], EG800_MQTT_DEFAULT_QOS, s_mqtt_at_completion_sem);
        if (result != EG800_AT_RESULT_OK) {
            LOG_ERROR("单次 MQTT 订阅失败: topic=%s, result=%d", s_eg800_mqtt_subscribe_list.topics[topic_index], (int)result);
            continue;
        }

        subscribe_success_count++;
        LOG("MQTT 订阅成功: topic=%s", s_eg800_mqtt_subscribe_list.topics[topic_index]);
    }

    // 配置了主题，但全部订阅失败，本次连接失败。
    if ((s_eg800_mqtt_subscribe_list.count > 0U) && (subscribe_success_count == 0)) {
        LOG_ERROR("单次 MQTT 连接失败: 所有主题订阅失败");
        return EG800_AT_RESULT_ERROR;
    }

    LOG("MQTT 订阅结束: success=%d, total=%u", subscribe_success_count, (unsigned int)s_eg800_mqtt_subscribe_list.count);

    // 只填写本次成功结果，由 Task 确认后发布 ONLINE。
    info->state = EG800_MQTT_STATE_ONLINE;


    return EG800_AT_RESULT_OK;
}



/**
 * @brief 查询客户端网络列表，确认当前客户端已不存在。
 * @return OK 表示查询成功且目标客户端不存在；其他结果不能确认清理完成。
 * @note 仅在关闭失败后核实状态，普通 ERROR 本身不作为已关闭的依据。
 */
static Eg800AtResult_e Eg800_Mqtt_Confirm_Network_Closed(void) {
    char response[6U * (EG800_MQTT_HOST_MAX_LENGTH + 40U)] = {0};
    const char *cursor = response;
    unsigned int client_index;
    int fields_end;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_QUERY_CLIENT_NET, EG800_AT_FLOW_TEXT, "确认 MQTT 客户端网络已关闭", "AT+QMTOPEN?\r\n", "+QMTOPEN:", NULL, NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, 0U, &result_info, s_mqtt_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if (result_info.rx_len >= (sizeof(response) - 1U)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 模块最多支持六个客户端；只有目标客户端不存在，才确认已关闭。
    while (*cursor != '\0') {
        fields_end = 0;
        if ((sscanf(cursor, "+QMTOPEN: %1u , %n", &client_index, &fields_end) != 1) ||
            (fields_end == 0) || (client_index > 5U)) {
            return EG800_AT_RESULT_DATA_FAIL;
        }

        if (client_index == EG800_MQTT_CLIENT_INDEX) {
            return EG800_AT_RESULT_ERROR;
        }

        cursor = strchr(cursor, '\n');
        if (cursor == NULL) {
            break;
        }
        cursor++;
    }

    return EG800_AT_RESULT_OK;
}

/**
 * @brief 执行一次 MQTT 会话断开和客户端网络关闭。
 * @return OK 表示客户端网络关闭成功；其他值表示清理失败。
 * @note 仅供 MQTT Task 串行调用，使用 MQTT 专用 AT 完成信号量。
 * @note QMTDISC 失败时仍继续 QMTCLOSE；关闭失败时通过掉线通知或查询确认是否已关闭。
 * @note 连接意图、状态更新、通知处理和重试由 Task 管理。
 */
static Eg800AtResult_e Eg800_Mqtt_Disconnect_Once(void) {
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (s_mqtt_at_completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    if ((ulTaskNotifyValueClear(NULL, 0U) & EG800_MQTT_EVENT_LINK_LOST) != 0U) {
        LOG("MQTT 已收到掉线通知，跳过重复清理");
        return EG800_AT_RESULT_OK;
    }

    LOG("单次 MQTT 断开清理开始");

    // 1. 断开 MQTT 服务器连接。
    result = Eg800_Mqtt_Disconnect_Client_To_Server(s_mqtt_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_WARN("单次 MQTT 断开会话失败，继续关闭客户端网络: result=%d", (int)result);
    }

    // 2. 关闭 MQTT 客户端网络，释放连接资源。
    result = Eg800_Mqtt_Close_Network_Client(s_mqtt_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        if (((ulTaskNotifyValueClear(NULL, 0U) & EG800_MQTT_EVENT_LINK_LOST) != 0U) ||
            (Eg800_Mqtt_Confirm_Network_Closed() == EG800_AT_RESULT_OK)) {
            LOG("MQTT 客户端网络已关闭，清理完成");
            return EG800_AT_RESULT_OK;
        }

        LOG_ERROR("单次 MQTT 关闭客户端网络失败: result=%d", (int)result);
        return result;
    }

    LOG("单次 MQTT 断开清理完成");

    return EG800_AT_RESULT_OK;
}

























































































//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------
//----------------------------------------------------基础 AT 命令封装----------------------------------------------------

/**
 * @brief 配置默认 MQTT 客户端使用的 PDP 场景。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Set_PDP_Context(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"pdpcid\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)EG800_NETWORK_CONTEXT_ID);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_PDP_CONTEXT, EG800_AT_FLOW_TEXT, "配置 MQTT PDP 场景", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}



/**
 * @brief 配置固定 MQTT 客户端的协议版本。
 * @param version 协议版本，3 表示 MQTT v3.1，4 表示 MQTT v3.1.1。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 客户端编号固定使用 EG800_MQTT_CLIENT_INDEX；MQTT 和 Service 应已初始化，且目标 MQTT 连接尚未建立。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Version(uint8_t version, SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || ((version != 3U) && (version != 4U)) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"version\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)version);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_VERSION, EG800_AT_FLOW_TEXT, "配置 MQTT 协议版本", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}


/**
 * @brief 配置由模块维护的 MQTT 保活间隔。
 * @param keepalive_s 保活间隔，范围 0~3600，单位 s；0 禁用保活。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Keepalive(uint16_t keepalive_s, SemaphoreHandle_t completion_sem) {
    char command[32];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (keepalive_s > 3600U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"keepalive\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)keepalive_s);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_KEEPALIVE, EG800_AT_FLOW_TEXT, "配置 MQTT 保活间隔", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}



/**
 * @brief 配置连接时是否清除旧会话。
 * @param enabled true 请求干净会话，false 请求保留会话。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Clean_Session(bool enabled, SemaphoreHandle_t completion_sem) {
    char command[32];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"session\",%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)enabled);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_SESSION, EG800_AT_FLOW_TEXT, "配置 MQTT 会话类型", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}


/**
 * @brief 配置服务器下行消息的接收方式和负载长度上报。
 * @param mode 0 直接通过 URC 上报，1 缓存在模块后主动读取。
 * @param include_length 是否在直接上报的 URC 中包含负载长度。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；参数无效返回 PARAM_ERROR；其他值为 AT 执行错误。
 */
Eg800AtResult_e Eg800_Mqtt_Set_Receive_Mode(uint8_t mode, bool include_length, SemaphoreHandle_t completion_sem) {
    char command[32];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (mode > 1U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"recv/mode\",%u,%u,%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)mode, (unsigned int)include_length);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_RECV_MODE, EG800_AT_FLOW_TEXT, "配置 MQTT 接收方式", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}




/**
 * @brief 清除固定 MQTT 客户端的全部唤醒主题。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示清除成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note wakeup_topic_enable 设为 0 且省略 topic_string，清除全部唤醒主题。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Clear_Wakeup_Topics(SemaphoreHandle_t completion_sem) {
    char command[40];
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"wakeup/topic\",%u,0\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_WAKEUP_TOPIC, EG800_AT_FLOW_TEXT, "清除 MQTT 唤醒主题", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}





/**
 * @brief 为固定 MQTT 客户端添加一个唤醒主题。
 * @param topic 非空且以 '\0' 结尾的主题。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示配置成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 主题不接受 ASCII 控制字符、双引号和反斜杠；配置唤醒主题不等同于订阅主题。
 * @note 应在 MQTT 连接建立前调用；仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Add_Wakeup_Topic(const char *topic, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + 48U];
    size_t topic_len;
    uint8_t ch;
    int command_len;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (topic == NULL) || (topic[0] == '\0')) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    for (topic_len = 0U; topic_len <= EG800_MQTT_TOPIC_MAX_LENGTH; topic_len++) {
        ch = (uint8_t)topic[topic_len];
        if (ch == '\0') {
            break;
        }

        if ((topic_len == EG800_MQTT_TOPIC_MAX_LENGTH) || (ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCFG=\"wakeup/topic\",%u,1,\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, topic);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    return Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CFG_WAKEUP_TOPIC, EG800_AT_FLOW_TEXT, "添加 MQTT 唤醒主题", command, NULL, NULL, NULL, 0U, NULL, 0U, EG800_MQTT_CONFIG_TIMEOUT_MS, 0U, NULL, completion_sem);
}




/**
 * @brief 打开MQTT网络客户端
 * @param host 非空且以 '\0' 结尾的服务器域名或 IP 地址
 * @param port 服务器端口
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示网络打开成功；模块返回失败结果时返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 收到 OK 后继续等待 +QMTOPEN 结果，仅结果码为 0 时成功；本函数不执行 MQTT 登录。
 * @note 调用前应完成 PDP 联网和 MQTT 参数配置；仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Open_Network_Client(const char *host, uint16_t port, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_HOST_MAX_LENGTH + 40U];
    char response[64] = {0};
    size_t host_len;
    uint8_t ch;
    int command_len;
    unsigned int client_index;
    int open_result;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (host == NULL) || (host[0] == '\0') || (port == 0U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    for (host_len = 0U; host_len <= EG800_MQTT_HOST_MAX_LENGTH; host_len++) {
        ch = (uint8_t)host[host_len];
        if (ch == '\0') {
            break;
        }

        if ((host_len == EG800_MQTT_HOST_MAX_LENGTH) || (ch <= 0x20U) || (ch > 0x7EU) || (ch == '"') || (ch == '\\')) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTOPEN=%u,\"%s\",%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, host, (unsigned int)port);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // OK 仅表示命令已受理，继续等待模块返回打开网络的实际结果。
    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_OPEN_CLIENT_NET, EG800_AT_FLOW_TEXT_URC, "打开 MQTT 客户端网络", command, "+QMTOPEN:", "+QMTOPEN:", NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_OPEN_URC_TIMEOUT_MS, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // 限制数值字段宽度，并检查响应是否被截断或含有多余内容。
    if ((result_info.rx_len >= (sizeof(response) - 1U)) ||
        (sscanf(response, "+QMTOPEN: %1u , %2d %n", &client_index, &open_result, &fields_end) != 2) ||
        (fields_end == 0) || (response[fields_end] != '\0') ||
        (client_index != EG800_MQTT_CLIENT_INDEX) || (open_result < -1) || (open_result > 5)) {
        LOG_ERROR("MQTT 打开网络响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (open_result != 0) {
        LOG_ERROR("MQTT 打开网络失败: client=%u, result=%d", client_index, open_result);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}



/**
 * @brief 关闭 MQTT 客户端的网络连接。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示网络关闭成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；模块关闭失败返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 收到 OK 后继续等待 +QMTCLOSE 结果，仅结果码为 0 时成功；本函数不去激活 PDP。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Close_Network_Client(SemaphoreHandle_t completion_sem) {
    char command[32];
    char response[64] = {0};
    int command_len;
    unsigned int client_index;
    int close_result;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTCLOSE=%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // OK 仅表示命令已受理，继续等待模块返回关闭网络的实际结果。
    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CLOSE_CLIENT_NET, EG800_AT_FLOW_TEXT_URC, "关闭 MQTT 客户端网络", command, "+QMTCLOSE:", "+QMTCLOSE:", NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_CLOSE_URC_TIMEOUT_MS, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // 限制数值字段宽度，并检查响应是否被截断或含有多余内容。
    if ((result_info.rx_len >= (sizeof(response) - 1U)) ||
        (sscanf(response, "+QMTCLOSE: %1u , %2d %n", &client_index, &close_result, &fields_end) != 2) ||
        (fields_end == 0) || (response[fields_end] != '\0') ||
        (client_index != EG800_MQTT_CLIENT_INDEX) || (close_result < -1) || (close_result > 0)) {
        LOG_ERROR("MQTT 关闭网络响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (close_result != 0) {
        LOG_ERROR("MQTT 关闭网络失败: client=%u, result=%d", client_index, close_result);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}


/**
 * @brief 连接 MQTT 客户端到服务器
 * @param client_id 客户端标识符
 * @param username 登录用户名
 * @param password 登录密码
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示服务器接受连接；参数无效返回 PARAM_ERROR；连接失败或被服务器拒绝返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 用户名为NULL时，密码也必须为NULL
 * @note 每个字符串最多 EG800_MQTT_CONFIG_TEXT_SIZE-1 字节，不接受 ASCII 控制字符、双引号和反斜杠
 * @note 应先成功执行 QMTOPEN；收到 OK 后继续等待最终 +QMTCONN，重传通知不会结束请求
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用
 */
Eg800AtResult_e Eg800_Mqtt_Connect_Client_To_Server(const char *client_id, const char *username, const char *password, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_CONFIG_TEXT_SIZE * 3U + 48U];
    char response[64] = {0};
    const char *fields[] = {client_id, username, password};
    size_t field_index;
    size_t field_len;
    uint8_t ch;
    int command_len;
    unsigned int client_index;
    unsigned int connect_result;
    unsigned int return_code = 0U;
    bool has_return_code;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (client_id == NULL) || (client_id[0] == '\0') || ((username == NULL) && (password != NULL))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 分别检查客户端标识、用户名和密码，NULL 表示省略对应的可选参数。
    for (field_index = 0U; field_index < ARRAY_SIZE(fields); field_index++) {
        if (fields[field_index] == NULL) {
            continue;
        }

        for (field_len = 0U; field_len < EG800_MQTT_CONFIG_TEXT_SIZE; field_len++) {
            ch = (uint8_t)fields[field_index][field_len];
            if (ch == '\0') {
                break;
            }

            if ((ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
                return EG800_AT_RESULT_PARAM_ERROR;
            }
        }

        if (field_len == EG800_MQTT_CONFIG_TEXT_SIZE) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    if (username == NULL) {
        command_len = snprintf(command, sizeof(command), "AT+QMTCONN=%u,\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, client_id);
    } else if (password == NULL) {
        command_len = snprintf(command, sizeof(command), "AT+QMTCONN=%u,\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, client_id, username);
    } else {
        command_len = snprintf(command, sizeof(command), "AT+QMTCONN=%u,\"%s\",\"%s\",\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, client_id, username, password);
    }

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_CONNECT_CLIENT_MQTT_SERVICE, EG800_AT_FLOW_TEXT_URC, "连接 MQTT 服务器", command, "+QMTCONN:", "+QMTCONN:", NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_CONNECT_URC_TIMEOUT_MS, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if (result_info.rx_len >= (sizeof(response) - 1U)) {
        LOG_ERROR("MQTT 登录响应可能被截断");
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 成功或被服务器拒绝时包含第三个字段；发送失败时该字段可以省略。
    has_return_code = (sscanf(response, "+QMTCONN: %1u , %1u , %1u %n", &client_index, &connect_result, &return_code, &fields_end) == 3);
    if (has_return_code == false) {
        fields_end = 0;
        if (sscanf(response, "+QMTCONN: %1u , %1u %n", &client_index, &connect_result, &fields_end) != 2) {
            LOG_ERROR("MQTT 登录响应无效: %s", response);
            return EG800_AT_RESULT_DATA_FAIL;
        }
    }

    if ((fields_end == 0) || (response[fields_end] != '\0') || (client_index != EG800_MQTT_CLIENT_INDEX) ||
        ((connect_result != 0U) && (connect_result != 2U)) ||
        ((has_return_code == true) && (return_code > 5U)) || ((connect_result == 0U) && (has_return_code == false))) {
        LOG_ERROR("MQTT 登录响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if ((connect_result != 0U) || (return_code != 0U)) {
        LOG_ERROR("MQTT 登录失败: client=%u, result=%u, ret_code=%d", client_index, connect_result, (has_return_code == true) ? (int)return_code : -1);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}



/**
 * @brief 断开 MQTT 客户端与服务器的连接
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示断开成功；未初始化、参数无效或命令生成失败返回 PARAM_ERROR；模块断开失败返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 收到 OK 后继续等待 +QMTDISC 结果，仅结果码为 0 时成功；
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Disconnect_Client_To_Server(SemaphoreHandle_t completion_sem) {
    char command[32];
    char response[64] = {0};
    int command_len;
    unsigned int client_index;
    int disconnect_result;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTDISC=%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // OK 仅表示命令已受理，继续等待模块返回断开连接的实际结果。
    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_DISCONNECT_CLIENT_MQTT_SERVICE, EG800_AT_FLOW_TEXT_URC, "断开 MQTT 服务器连接", command, "+QMTDISC:", "+QMTDISC:", NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_DISCONNECT_URC_TIMEOUT_MS, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // 限制数值字段宽度，并检查响应是否被截断或含有多余内容。
    if ((result_info.rx_len >= (sizeof(response) - 1U)) ||
        (sscanf(response, "+QMTDISC: %1u , %2d %n", &client_index, &disconnect_result, &fields_end) != 2) ||
        (fields_end == 0) || (response[fields_end] != '\0') ||
        (client_index != EG800_MQTT_CLIENT_INDEX) || (disconnect_result < -1) || (disconnect_result > 0)) {
        LOG_ERROR("MQTT 断开连接响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (disconnect_result != 0) {
        LOG_ERROR("MQTT 断开连接失败: client=%u, result=%d", client_index, disconnect_result);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}


/**
 * @brief MQTT 客户端订阅一个主题，当前仅支持 QoS 0。
 * @param msg_id 主题编号
 * @param topic 非空的主题过滤器，支持合法的 + 和 # 通配符。
 * @param qos 当前仅支持 0；非 0 时记录 TODO 日志并返回 PARAM_ERROR。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示订阅被接受；参数无效返回 PARAM_ERROR；发送失败或订阅被拒绝返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 应在 MQTT 登录成功后调用；收到 OK 后继续等待当前客户端和 msg_id 对应的最终 +QMTSUB。
 * @note 禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Subscribe_Topic(uint16_t msg_id, const char *topic, uint8_t qos, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + 48U];
    char response[64] = {0};
    size_t topic_len;
    uint8_t ch;
    int command_len;
    unsigned int client_index;
    unsigned int response_msg_id;
    unsigned int subscribe_result;
    unsigned int granted_qos = 0U;
    bool has_granted_qos;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if (qos != 0U) {
        LOG_ERROR("TODO:暂不支持的Qos=%d", (int)qos);
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 合法性检查
    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (msg_id == 0U) ||
        (topic == NULL) || (topic[0] == '\0')) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 检查主题字符
    for (topic_len = 0U; topic_len <= EG800_MQTT_TOPIC_MAX_LENGTH; topic_len++) {
        ch = (uint8_t)topic[topic_len];
        if (ch == '\0') {
            break;
        }

        if ((topic_len == EG800_MQTT_TOPIC_MAX_LENGTH) || (ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }

        // # 只能位于最后一级；+ 必须独占一个主题层级。
        if ((ch == '#') && (((topic_len > 0U) && (topic[topic_len - 1U] != '/')) || (topic[topic_len + 1U] != '\0'))) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
        if ((ch == '+') && (((topic_len > 0U) && (topic[topic_len - 1U] != '/')) || ((topic[topic_len + 1U] != '\0') && (topic[topic_len + 1U] != '/')))) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    // 拼接AT指令
    command_len = snprintf(command, sizeof(command), "AT+QMTSUB=%u,%u,\"%s\",%u\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)msg_id, topic, (unsigned int)qos);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // 调用
    result = Eg800_Mqtt_At_Execute_Matched(EG800_MQTT_CMD_SUBSCRIBE_TOPIC, EG800_AT_FLOW_TEXT_URC, "订阅 MQTT 主题", command, "+QMTSUB:", "+QMTSUB:", NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_SUBSCRIBE_URC_TIMEOUT_MS, &result_info, completion_sem, Eg800_Mqtt_Subscribe_Final_Urc_Match, &msg_id);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if (result_info.rx_len >= (sizeof(response) - 1U)) {
        LOG_ERROR("MQTT 订阅响应可能被截断");
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 单主题的 SUBACK 只应包含一个 QoS 返回值，128 表示服务器拒绝订阅。
    has_granted_qos = (sscanf(response, "+QMTSUB: %1u , %5u , %1u , %3u %n", &client_index, &response_msg_id, &subscribe_result, &granted_qos, &fields_end) == 4);
    if (has_granted_qos == false) {
        fields_end = 0;
        if (sscanf(response, "+QMTSUB: %1u , %5u , %1u %n", &client_index, &response_msg_id, &subscribe_result, &fields_end) != 3) {
            LOG_ERROR("MQTT 订阅响应无效: %s", response);
            return EG800_AT_RESULT_DATA_FAIL;
        }
    }

    if ((fields_end == 0) || (response[fields_end] != '\0') || (client_index != EG800_MQTT_CLIENT_INDEX) ||
        (response_msg_id != msg_id) || ((subscribe_result != 0U) && (subscribe_result != 2U)) ||
        ((subscribe_result == 0U) && (has_granted_qos == false)) || ((subscribe_result == 2U) && (has_granted_qos == true)) ||
        ((has_granted_qos == true) && (granted_qos > 2U) && (granted_qos != 128U)) ||
        ((has_granted_qos == true) && (granted_qos != 128U) && (granted_qos > qos))) {
        LOG_ERROR("MQTT 订阅响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if ((subscribe_result != 0U) || (granted_qos == 128U)) {
        LOG_ERROR("MQTT 订阅失败: client=%u, msgid=%u, result=%u, value=%d", client_index, response_msg_id, subscribe_result, (has_granted_qos == true) ? (int)granted_qos : -1);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}




/**
 * @brief MQTT 客户端退订一个主题。
 * @param msg_id 主题编号
 * @param topic 要退订的主题过滤器，必须与订阅时使用的过滤器一致，最多 EG800_MQTT_TOPIC_MAX_LENGTH 字节。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示退订已确认；参数无效返回 PARAM_ERROR；发送失败返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 应在 MQTT 登录成功后调用；收到 OK 后继续等待当前客户端和 msg_id 对应的最终 +QMTUNS。
 * @note 主题支持合法的 + 和 # 通配符，不接受 ASCII 控制字符、双引号和反斜杠。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Unsubscribe_Topic(uint16_t msg_id, const char *topic, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + 48U];
    char response[64] = {0};
    size_t topic_len;
    uint8_t ch;
    int command_len;
    unsigned int client_index;
    unsigned int response_msg_id;
    unsigned int unsubscribe_result;
    unsigned int response_value = 0U;
    bool has_value;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (msg_id == 0U) ||
        (topic == NULL) || (topic[0] == '\0')) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    for (topic_len = 0U; topic_len <= EG800_MQTT_TOPIC_MAX_LENGTH; topic_len++) {
        ch = (uint8_t)topic[topic_len];
        if (ch == '\0') {
            break;
        }

        if ((topic_len == EG800_MQTT_TOPIC_MAX_LENGTH) || (ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }

        if ((ch == '#') && (((topic_len > 0U) && (topic[topic_len - 1U] != '/')) || (topic[topic_len + 1U] != '\0'))) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
        if ((ch == '+') && (((topic_len > 0U) && (topic[topic_len - 1U] != '/')) || ((topic[topic_len + 1U] != '\0') && (topic[topic_len + 1U] != '/')))) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    command_len = snprintf(command, sizeof(command), "AT+QMTUNS=%u,%u,\"%s\"\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)msg_id, topic);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    result = Eg800_Mqtt_At_Execute_Matched(EG800_MQTT_CMD_UNSUBSCRIBE_TOPIC, EG800_AT_FLOW_TEXT_URC, "退订 MQTT 主题", command, "+QMTUNS:", "+QMTUNS:", NULL, 0U, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_UNSUBSCRIBE_URC_TIMEOUT_MS, &result_info, completion_sem, Eg800_Mqtt_Unsubscribe_Final_Urc_Match, &msg_id);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    if (result_info.rx_len >= (sizeof(response) - 1U)) {
        LOG_ERROR("MQTT 退订响应可能被截断");
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 手册的正常退订示例只有三个字段，同时兼容语法中列出的可选 value。
    has_value = (sscanf(response, "+QMTUNS: %1u , %5u , %1u , %3u %n", &client_index, &response_msg_id, &unsubscribe_result, &response_value, &fields_end) == 4);
    if (has_value == false) {
        fields_end = 0;
        if (sscanf(response, "+QMTUNS: %1u , %5u , %1u %n", &client_index, &response_msg_id, &unsubscribe_result, &fields_end) != 3) {
            LOG_ERROR("MQTT 退订响应无效: %s", response);
            return EG800_AT_RESULT_DATA_FAIL;
        }
    }

    if ((fields_end == 0) || (response[fields_end] != '\0') || (client_index != EG800_MQTT_CLIENT_INDEX) ||
        (response_msg_id != msg_id) || ((unsubscribe_result != 0U) && (unsubscribe_result != 2U)) ||
        ((has_value == true) && ((unsubscribe_result == 2U) || (response_value > 2U)))) {
        LOG_ERROR("MQTT 退订响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (unsubscribe_result != 0U) {
        LOG_ERROR("MQTT 退订失败: client=%u, msgid=%u, result=%u", client_index, response_msg_id, unsubscribe_result);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}

/**
 * @brief 向指定主题发布一条定长 MQTT 消息，当前仅支持 QoS 0。
 * @param msg_id 当前仅支持 QoS 0，消息编号必须为 0。
 * @param topic 非空的发布主题，最多 EG800_MQTT_TOPIC_MAX_LENGTH 字节，不接受 + 和 # 通配符。
 * @param qos 当前仅支持 0；非 0 时记录 TODO 日志并返回 PARAM_ERROR。
 * @param retain 是否要求服务器保留该主题的最后一条保留消息。
 * @param payload 原始负载地址，数据不要求以 '\0' 结尾；payload_len 为 0 时可为 NULL。
 * @param payload_len 负载字节数，最大为 EG800_MQTT_PUBLISH_MAX_LENGTH，不自动附加 '\0'、CR/LF 或 Ctrl+Z。
 * @param completion_sem 当前调用任务专用的完成信号量。
 * @return OK 表示模块报告发布成功；参数无效返回 PARAM_ERROR；模块发布失败返回 ERROR；响应格式错误返回 DATA_FAIL；其他 AT 错误原样返回。
 * @note 调用前应已完成 MQTT 登录；payload 指向的内容必须保持有效且不修改，直到本次同步调用返回。
 * @note 流程为发送指令、等待 >、发送 payload_len 字节、等待 OK、等待最终 +QMTPUBEX。
 * @note QoS 0 不等待服务器确认；空负载是否被接受取决于模块固件，模块拒绝时原样返回 AT 错误。
 * @note 仅供任务上下文调用，禁止在 ISR、Service 任务及其 URC 回调中调用。
 */
Eg800AtResult_e Eg800_Mqtt_Publish_Topic(uint16_t msg_id, const char *topic, uint8_t qos, bool retain, const uint8_t *payload, size_t payload_len, SemaphoreHandle_t completion_sem) {
    char command[EG800_MQTT_TOPIC_MAX_LENGTH + 64U];
    char response[64] = {0};
    size_t topic_len;
    uint8_t ch;
    int command_len;
    unsigned int client_index;
    unsigned int response_msg_id;
    unsigned int publish_result;
    int fields_end = 0;
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;


    if (qos != 0U) {
        LOG_ERROR("TODO:暂不支持的Qos=%d", (int)qos);
        return EG800_AT_RESULT_PARAM_ERROR;
    }


    if ((s_eg800_mqtt_initialized == false) || (completion_sem == NULL) || (topic == NULL) || (topic[0] == '\0') ||
        (msg_id != 0U) || (payload_len > EG800_MQTT_PUBLISH_MAX_LENGTH) || ((payload_len > 0U) && (payload == NULL))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }



    for (topic_len = 0U; topic_len <= EG800_MQTT_TOPIC_MAX_LENGTH; topic_len++) {
        ch = (uint8_t)topic[topic_len];
        if (ch == '\0') {
            break;
        }

        if ((topic_len == EG800_MQTT_TOPIC_MAX_LENGTH) || (ch < 0x20U) || (ch == 0x7FU) ||
            (ch == '"') || (ch == '\\') || (ch == '+') || (ch == '#')) {
            return EG800_AT_RESULT_PARAM_ERROR;
        }
    }

    // AT 指令只携带主题和负载长度，负载本身不拼入指令字符串。
    command_len = snprintf(command, sizeof(command), "AT+QMTPUBEX=%u,0,0,%u,\"%s\",%lu\r\n", (unsigned int)EG800_MQTT_CLIENT_INDEX, (unsigned int)retain, topic, (unsigned long)payload_len);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    // Service 收到 > 后，按 tx_data_len 原样发送 tx_data，再等待 OK 和最终发布结果。
    result = Eg800_Mqtt_At_Execute(EG800_MQTT_CMD_PUBLISH_TOPIC, EG800_AT_FLOW_SEND_DATA, "发布 MQTT 消息", command, "+QMTPUBEX:", "+QMTPUBEX:", payload, payload_len, response, sizeof(response), EG800_MQTT_FINAL_TIMEOUT_MS, EG800_MQTT_PUBLISH_URC_TIMEOUT_MS, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // QoS 0 的消息编号固定为 0，最终结果只接受成功 0 或失败 2。
    if ((result_info.rx_len >= (sizeof(response) - 1U)) ||
        (sscanf(response, "+QMTPUBEX: %1u , %5u , %1u %n", &client_index, &response_msg_id, &publish_result, &fields_end) != 3) ||
        (fields_end == 0) || (response[fields_end] != '\0') || (client_index != EG800_MQTT_CLIENT_INDEX) ||
        (response_msg_id != 0U) || ((publish_result != 0U) && (publish_result != 2U))) {
        LOG_ERROR("MQTT 发布响应无效: %s", response);
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (publish_result != 0U) {
        LOG_ERROR("MQTT 发布失败: client=%u, result=%u", client_index, publish_result);
        return EG800_AT_RESULT_ERROR;
    }

    return EG800_AT_RESULT_OK;
}

/**
 * @brief 从接收队列取出一条 MQTT 消息。
 * @param message 调用者提供的接收缓冲区，不能为 NULL。
 * @param wait_ticks 等待 tick 数；0 立即返回，portMAX_DELAY 一直等待。
 * @return RESULT_SUCCESS 表示取出成功；RESULT_FAIL 表示参数无效、未初始化或等待失败。
 * @note 仅供业务任务调用，禁止在 ISR 或 Service 任务中调用。
 * @note 成功后消息从队列移除，主题和内容复制到 message。
 */
Result_t Eg800_Mqtt_Receive_Message(Eg800MqttReceiveMessage_t *message, TickType_t wait_ticks) {
    if ((message == NULL) || (s_eg800_mqtt_initialized == false) || (s_eg800_mqtt_receive_queue == NULL)) {
        return RESULT_FAIL;
    }

    if (xQueueReceive(s_eg800_mqtt_receive_queue, message, wait_ticks) != pdPASS) {
        return RESULT_FAIL;
    }

    return RESULT_SUCCESS;
}




//--------------------------------------------------------公开 API-----------------------------------------
/**
 * @brief 初始化 Mqtt 层并注册基础 URC。
 * @return RESULT_SUCCESS 表示初始化成功。
 * @note 本函数不发送 AT 命令，可以在模块上电前调用。
 */
Result_t Eg800_Mqtt_Init(void) {
    BaseType_t task_result;

    // 幂等检查
    if (s_eg800_mqtt_initialized == true) {
        return RESULT_SUCCESS;
    }

    // 注册URC
    if (Eg800_Mqtt_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }

    // 创建接收消息队列
    s_eg800_mqtt_receive_queue = xQueueCreate(EG800_MQTT_RECEIVE_QUEUE_LENGTH, sizeof(Eg800MqttReceiveMessage_t));
    if (s_eg800_mqtt_receive_queue == NULL) {
        LOG_ERROR("MQTT 接收队列创建失败");
        return RESULT_FAIL;
    }

    // 创建 MQTT 任务专用的 AT 完成信号量。
    s_mqtt_at_completion_sem = xSemaphoreCreateBinary();
    if (s_mqtt_at_completion_sem == NULL) {
        vQueueDelete(s_eg800_mqtt_receive_queue);
        s_eg800_mqtt_receive_queue = NULL;
        LOG_ERROR("MQTT AT 完成信号量创建失败");
        return RESULT_FAIL;
    }

    // 创建 MQTT 维护任务
    task_result = xTaskCreate(Eg800_Mqtt_Task,
                              "EG MQTT Core Task",
                              EG800_MQTT_TASK_STACK_SIZE,
                              NULL,
                              EG800_MQTT_TASK_PRIORITY,
                              &s_eg800_mqtt_task_handle);

    if (task_result != pdPASS) {
        s_eg800_mqtt_task_handle = NULL;
        vSemaphoreDelete(s_mqtt_at_completion_sem);
        s_mqtt_at_completion_sem = NULL;
        vQueueDelete(s_eg800_mqtt_receive_queue);
        s_eg800_mqtt_receive_queue = NULL;

        LOG_ERROR("MQTT 后台任务创建失败");
        return RESULT_FAIL;
    }

    s_eg800_mqtt_initialized = true;

    return RESULT_SUCCESS;
}


/**
 * @brief 设置 MQTT 连接配置，复制到 Service 内部保存。
 * @return RESULT_SUCCESS 表示配置保存成功，RESULT_FAIL 表示参数无效或当前不允许修改。
 * @note 成功保存后置 config_valid；失败保留原配置及有效标志。
 * @note 仅保存配置，不发送 AT 命令、不自动连接。
 * @note 初始化后，仅首次连接前或断开清理完成后允许修改；匿名连接时用户名和密码均为空字符串。
 * @note 仅供任务上下文调用，调用期间 config 必须保持有效且不修改。
 */
Result_t Eg800_Mqtt_Set_Config(const Eg800MqttConfig_t *config) {
    const char *fields[4];
    size_t field_index;
    size_t field_len;
    uint8_t ch;
    Result_t result = RESULT_FAIL;

    if ((s_eg800_mqtt_initialized == false) || (config == NULL) || (config->server_port == 0U) || (config->server_host[0] == '\0') || (config->client_id[0] == '\0') || ((config->username[0] == '\0') && (config->password[0] != '\0'))) {
        return RESULT_FAIL;
    }

    fields[0] = config->server_host;
    fields[1] = config->client_id;
    fields[2] = config->username;
    fields[3] = config->password;

    // 按数组容量检查终止符和 AT 字段字符；服务器地址另受 100 字节上限约束。
    for (field_index = 0U; field_index < ARRAY_SIZE(fields); field_index++) {
        for (field_len = 0U; field_len < EG800_MQTT_CONFIG_TEXT_SIZE; field_len++) {
            ch = (uint8_t)fields[field_index][field_len];
            if (ch == '\0') {
                break;
            }

            if ((ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
                return RESULT_FAIL;
            }
            if ((field_index == 0U) && ((field_len >= EG800_MQTT_HOST_MAX_LENGTH) || (ch == ' ') || (ch > 0x7EU))) {
                return RESULT_FAIL;
            }
        }

        if (field_len == EG800_MQTT_CONFIG_TEXT_SIZE) {
            return RESULT_FAIL;
        }
    }

    // 状态检查和配置复制放在同一临界区，避免连接请求在两者之间生效。
    taskENTER_CRITICAL();
    if ((s_eg800_mqtt_should_connect == false) && (s_eg800_mqtt_snapshot.refresh_in_progress == false) && ((s_eg800_mqtt_snapshot.info.state == EG800_MQTT_STATE_NONE) || (s_eg800_mqtt_snapshot.info.state == EG800_MQTT_STATE_IDLE))) {
        s_eg800_mqtt_snapshot.info.config = *config;
        s_eg800_mqtt_snapshot.info.config_valid = true;
        s_eg800_mqtt_snapshot.update_tick = xTaskGetTickCount();
        result = RESULT_SUCCESS;
    }
    taskEXIT_CRITICAL();

    return result;
}




/**
 * @brief 设置订阅列表，校验后复制到 MQTT 内部保存。
 * @param list 订阅列表；count 为 0 时清空列表。
 * @return RESULT_SUCCESS 表示保存成功，RESULT_FAIL 表示参数无效或当前不允许修改。
 * @note 仅首次连接前或断开清理完成后允许修改，不发送 AT、不自动订阅。
 * @note 失败保留原列表；调用期间 list 必须保持有效且不修改。
 */
Result_t Eg800_Mqtt_Set_Subscribe_List(const Eg800MqttSubscribeList_t *list) {
    size_t topic_index;
    size_t topic_len;
    const char *topic;
    uint8_t ch;
    Result_t result = RESULT_FAIL;

    if ((s_eg800_mqtt_initialized == false) || (list == NULL) || (list->count > EG800_MQTT_SUBSCRIBE_MAX_COUNT)) {
        return RESULT_FAIL;
    }

    // 校验所有有效主题，未使用的数组项不参与检查。
    for (topic_index = 0U; topic_index < list->count; topic_index++) {
        topic = list->topics[topic_index];

        if (topic[0] == '\0') {
            return RESULT_FAIL;
        }

        // 包含结尾 '\0' 的位置，保证字符串没有超过数组容量。
        for (topic_len = 0U; topic_len <= EG800_MQTT_TOPIC_MAX_LENGTH; topic_len++) {
            ch = (uint8_t)topic[topic_len];
            if (ch == '\0') {
                break;
            }

            if ((topic_len == EG800_MQTT_TOPIC_MAX_LENGTH) || (ch < 0x20U) || (ch == 0x7FU) || (ch == '"') || (ch == '\\')) {
                return RESULT_FAIL;
            }

            // # 必须独占最后一级。
            if ((ch == '#') && (((topic_len > 0U) && (topic[topic_len - 1U] != '/')) || (topic[topic_len + 1U] != '\0'))) {
                return RESULT_FAIL;
            }

            // + 必须独占一个主题层级。
            if ((ch == '+') && (((topic_len > 0U) && (topic[topic_len - 1U] != '/')) || ((topic[topic_len + 1U] != '\0') && (topic[topic_len + 1U] != '/')))) {
                return RESULT_FAIL;
            }
        }
    }

    // 状态检查和列表复制放在同一临界区，避免连接请求在两者之间生效。
    taskENTER_CRITICAL();
    if ((s_eg800_mqtt_should_connect == false) &&
        (s_eg800_mqtt_snapshot.refresh_in_progress == false) &&
        ((s_eg800_mqtt_snapshot.info.state == EG800_MQTT_STATE_NONE) ||
         (s_eg800_mqtt_snapshot.info.state == EG800_MQTT_STATE_IDLE))) {
        s_eg800_mqtt_subscribe_list = *list;
        s_eg800_mqtt_snapshot.update_tick = xTaskGetTickCount();
        result = RESULT_SUCCESS;
    }
    taskEXIT_CRITICAL();

    return result;
}
























/**
 * @brief 提交建立并保持 MQTT 连接的请求。
 * @return RESULT_SUCCESS 表示请求已提交；RESULT_FAIL 表示 MQTT 未初始化或任务不存在。
 * @note 仅供任务上下文调用；MQTT 任务在建链时检查连接配置和 PDP 状态。
 */
Result_t Eg800_Mqtt_Connect(void) {
    if ((s_eg800_mqtt_initialized == false) || (s_eg800_mqtt_task_handle == NULL)) {
        return RESULT_FAIL;
    }

    // 先更新连接意图，确保任务被唤醒后能够读到目标状态。
    taskENTER_CRITICAL();
    s_eg800_mqtt_should_connect = true;
    taskEXIT_CRITICAL();

    (void)xTaskNotify(s_eg800_mqtt_task_handle, EG800_MQTT_EVENT_CONNECT, eSetBits);

    return RESULT_SUCCESS;
}



/**
 * @brief 关闭保持连接的意图，并提交 MQTT 断开请求。
 * @return RESULT_SUCCESS 表示请求已提交，不代表连接已经断开；RESULT_FAIL 表示 MQTT 未初始化或任务不存在。
 * @note 仅供任务上下文调用，实际断开和清理由 MQTT 任务执行。
 */
Result_t Eg800_Mqtt_Disconnect(void) {
    if ((s_eg800_mqtt_initialized == false) || (s_eg800_mqtt_task_handle == NULL)) {
        return RESULT_FAIL;
    }

    // 先关闭连接意图，任务处理请求时据此停止后续连接尝试。
    taskENTER_CRITICAL();
    s_eg800_mqtt_should_connect = false;
    taskEXIT_CRITICAL();

    (void)xTaskNotify(s_eg800_mqtt_task_handle, EG800_MQTT_EVENT_DISCONNECT, eSetBits);

    return RESULT_SUCCESS;
}



/**
 * @brief 判断 MQTT 当前缓存状态是否在线。
 * @return 已初始化且状态为 ONLINE 时返回 true，否则返回 false。
 * @note 仅供任务上下文调用，读取软件缓存，不发送 AT 命令。
 */
bool Eg800_Mqtt_Is_Online(void) {
    bool online;

    if (s_eg800_mqtt_initialized == false) {
        return false;
    }

    taskENTER_CRITICAL();
    online = (s_eg800_mqtt_snapshot.info.state == EG800_MQTT_STATE_ONLINE);
    taskEXIT_CRITICAL();

    return online;
}


/**
 * @brief 获取 MQTT 当前保存的状态快照。
 * @param snapshot 快照输出地址；返回成功时有效，返回失败时不修改其内容。
 * @return RESULT_SUCCESS 表示快照复制成功，不代表 MQTT 已在线；RESULT_FAIL 表示未初始化或输出地址为空。
 * @note 仅供任务上下文调用，读取软件缓存，不发送 AT 命令。
 */
Result_t Eg800_Mqtt_Get_Snapshot(Eg800MqttSnapshot_t *snapshot) {
    if ((s_eg800_mqtt_initialized == false) || (snapshot == NULL)) {
        return RESULT_FAIL;
    }

    taskENTER_CRITICAL();
    *snapshot = s_eg800_mqtt_snapshot;
    taskEXIT_CRITICAL();

    return RESULT_SUCCESS;
}











//-----------------------------------------------MQTT层核心维护任务-------------------------------------------------------
/**
 * @brief 串行维护 MQTT 连接、断开、掉线恢复和有限次数重试。
 * @param argument 任务创建参数，当前未使用。
 * @note 基础 AT 配置由调用者完成；本任务等待连接请求后才执行连接。
 * @note 在线时等待链路状态和连接控制通知，PDP 状态仅在建链时检查。
 * @note 首次连接不清理；已收到掉线通知时跳过重复清理，FAULT 须由明确的断开请求解除。
 */
static void Eg800_Mqtt_Task(void *argument) {
    Eg800AtResult_e result;
    Eg800AtResult_e cleanup_result;
    uint32_t events;                                    // 通知事件
    uint32_t pending_events;                            // 同步 AT 期间积压的通知
    BaseType_t wait_result;                             // 等待通知的结果
    TickType_t wait_ticks = portMAX_DELAY;              // 下一次等待通知的时长
    bool should_connect;                                // MQTT 连接意图
    bool client_closed = true;                          // 模块随 MCU 上电，首次连接前无需清理
    Eg800MqttInfo_t mqtt_info = {0};                    // MQTT 任务维护的运行信息
    TickType_t retry_start_tick = 0U;                   // 重试冷却期的开始时刻
    TickType_t retry_elapsed_ticks;                     // 冷却期已经经过的 tick 数
    Eg800MqttState_e previous_state = EG800_MQTT_STATE_NONE; // 处理事件前的状态
    bool is_need_clean_mqtt;                            // 本轮是否需要清理 MQTT
    const TickType_t retry_interval_ticks = pdMS_TO_TICKS(EG800_MQTT_RETRY_INTERVAL_MS);
    (void)argument;

    // 首次启动保持 NONE，尚未建立 MQTT 连接。
    mqtt_info.state = EG800_MQTT_STATE_NONE;
    Eg800_Mqtt_Update_Snapshot(&mqtt_info);




    while (1) {
        events = 0U;
        is_need_clean_mqtt = false;

        // ==================== 1. 等待通知 ====================
        wait_result = xTaskNotifyWait(0U, UINT32_MAX, &events, wait_ticks);
        if (wait_result == pdTRUE) {
            LOG("MQTT 收到事件: events=0x%08lX", (unsigned long)events);
        }

        // 等待期间连接意图可能变化，醒来后读取最新值。
        should_connect = Eg800_Mqtt_Should_Connect();

        // ==================== 2. 识别事件 ====================
        if ((events & EG800_MQTT_EVENT_LINK_LOST) != 0U) {
            previous_state = mqtt_info.state;
            client_closed = true;

            if (previous_state == EG800_MQTT_STATE_FAULT) {
                LOG_WARN("MQTT 收到掉线事件，保持 FAULT");

            } else if (should_connect == false) {
                s_eg800_mqtt_retry_count = 0U;
                mqtt_info.state = EG800_MQTT_STATE_IDLE;
                LOG("MQTT 已掉线，保持断开");

            } else if (previous_state == EG800_MQTT_STATE_RETRY_WAIT) {
                // 重复通知保留尝试次数和原冷却起点。
                LOG("MQTT 收到掉线事件，继续等待重试");

            } else {
                if (previous_state == EG800_MQTT_STATE_ONLINE) {
                    s_eg800_mqtt_retry_count = 0U;
                }
                mqtt_info.state = EG800_MQTT_STATE_IDLE;
                LOG_WARN("MQTT 已掉线，准备重新连接");
            }
            Eg800_Mqtt_Update_Snapshot(&mqtt_info);
        }

        if (((events & EG800_MQTT_EVENT_DISCONNECT) != 0U) && (should_connect == false)) {
            previous_state = mqtt_info.state;
            if (client_closed == true) {
                // 客户端已关闭，直接结束本轮重试。
                s_eg800_mqtt_retry_count = 0U;
                mqtt_info.state = EG800_MQTT_STATE_IDLE;
                LOG("MQTT 已保持断开");
            } else {
                mqtt_info.state = EG800_MQTT_STATE_DISCONNECTING;
                is_need_clean_mqtt = true;
                LOG("MQTT 处理主动断开请求");
            }
            Eg800_Mqtt_Update_Snapshot(&mqtt_info);
        }

        if (((events & EG800_MQTT_EVENT_CONNECT) != 0U) && (should_connect == true)) {
            if (mqtt_info.state == EG800_MQTT_STATE_FAULT) {
                LOG_WARN("MQTT 处于 FAULT，忽略连接请求，请先断开");
            } else if (mqtt_info.state == EG800_MQTT_STATE_ONLINE) {
                LOG("MQTT 已在线");
            } else if (mqtt_info.state == EG800_MQTT_STATE_RETRY_WAIT) {
                LOG("MQTT 正在等待重试");
            } else {
                LOG("MQTT 收到连接请求");
            }
        }

        // 每轮都检查冷却期限，其他通知不能延长重试等待。
        if (mqtt_info.state == EG800_MQTT_STATE_RETRY_WAIT) {
            retry_elapsed_ticks = xTaskGetTickCount() - retry_start_tick;
            if (retry_elapsed_ticks >= retry_interval_ticks) {
                mqtt_info.state = EG800_MQTT_STATE_IDLE;
                Eg800_Mqtt_Update_Snapshot(&mqtt_info);
            }
        }

        // ==================== 3. 统一执行 ====================
        if (is_need_clean_mqtt == true) {
            // 事件识别后若又收到模块已断开的通知，跳过重复清理。
            pending_events = ulTaskNotifyValueClear(NULL, 0U);
            cleanup_result = ((pending_events & EG800_MQTT_EVENT_LINK_LOST) != 0U) ? EG800_AT_RESULT_OK : Eg800_Mqtt_Disconnect_Once();
            pending_events = ulTaskNotifyValueClear(NULL, EG800_MQTT_EVENT_LINK_LOST);
            if ((pending_events & EG800_MQTT_EVENT_LINK_LOST) != 0U) {
                cleanup_result = EG800_AT_RESULT_OK;
            }
            client_closed = (cleanup_result == EG800_AT_RESULT_OK);
            should_connect = Eg800_Mqtt_Should_Connect();

            if (cleanup_result != EG800_AT_RESULT_OK) {
                mqtt_info.state = EG800_MQTT_STATE_FAULT;
                LOG_ERROR("MQTT 清理失败: result=%d", (int)cleanup_result);

            } else if ((previous_state == EG800_MQTT_STATE_FAULT) && (should_connect == true)) {
                mqtt_info.state = EG800_MQTT_STATE_FAULT;
                LOG_WARN("MQTT 清理完成，保持 FAULT");

            } else if (should_connect == false) {
                s_eg800_mqtt_retry_count = 0U;
                mqtt_info.state = EG800_MQTT_STATE_IDLE;
                LOG("MQTT 清理完成，保持断开");

            } else {
                s_eg800_mqtt_retry_count = 0U;
                mqtt_info.state = EG800_MQTT_STATE_IDLE;
                LOG("MQTT 清理完成，准备重新连接");
            }
            Eg800_Mqtt_Update_Snapshot(&mqtt_info);
        }

        should_connect = Eg800_Mqtt_Should_Connect();
        if (((mqtt_info.state == EG800_MQTT_STATE_NONE) || (mqtt_info.state == EG800_MQTT_STATE_IDLE)) && (should_connect == true)) {
            mqtt_info.state = EG800_MQTT_STATE_CHECK_NETWORK;
            Eg800_Mqtt_Update_Snapshot(&mqtt_info);

            // 清除上一轮已关闭连接的通知，保留连接和断开请求。
            (void)ulTaskNotifyValueClear(NULL, EG800_MQTT_EVENT_LINK_LOST);
            s_eg800_mqtt_retry_count++;
            LOG("MQTT 开始第 %u 次连接尝试", (unsigned int)s_eg800_mqtt_retry_count);

            result = Eg800_Mqtt_Connect_Once(&mqtt_info);

            // 成功快照与最终检查一起完成，避免发布已取消或已掉线的连接。
            taskENTER_CRITICAL();
            should_connect = s_eg800_mqtt_should_connect;
            pending_events = ulTaskNotifyValueClear(NULL, 0U);

            if (should_connect == false) {
                result = EG800_AT_RESULT_CANCELLED;
            } else if ((result == EG800_AT_RESULT_OK) && ((Eg800_Network_Is_Active() == false) || ((pending_events & EG800_MQTT_EVENT_LINK_LOST) != 0U))) {
                result = EG800_AT_RESULT_DATA_FAIL;
            }

            if (result == EG800_AT_RESULT_OK) {
                client_closed = false;
                s_eg800_mqtt_retry_count = 0U;
                Eg800_Mqtt_Update_Snapshot(&mqtt_info);
            }
            taskEXIT_CRITICAL();

            if (result == EG800_AT_RESULT_OK) {
                LOG("MQTT 连接成功，订阅流程已结束");
                // [TODO]:通知上层，MQTT 已在线。

            } else {
                LOG_WARN("MQTT 连接未完成: result=%d", (int)result);
                client_closed = (mqtt_info.state == EG800_MQTT_STATE_CHECK_NETWORK) || ((pending_events & EG800_MQTT_EVENT_LINK_LOST) != 0U);

                // 前置检查失败时尚未发送 QMTOPEN；已收到掉线通知时也无需再次关闭。
                cleanup_result = EG800_AT_RESULT_OK;
                if (client_closed == false) {
                    mqtt_info.state = EG800_MQTT_STATE_DISCONNECTING;
                    Eg800_Mqtt_Update_Snapshot(&mqtt_info);
                    cleanup_result = Eg800_Mqtt_Disconnect_Once();
                }
                pending_events = ulTaskNotifyValueClear(NULL, EG800_MQTT_EVENT_LINK_LOST);
                if ((pending_events & EG800_MQTT_EVENT_LINK_LOST) != 0U) {
                    cleanup_result = EG800_AT_RESULT_OK;
                }
                client_closed = (cleanup_result == EG800_AT_RESULT_OK);
                should_connect = Eg800_Mqtt_Should_Connect();

                if (cleanup_result != EG800_AT_RESULT_OK) {
                    mqtt_info.state = EG800_MQTT_STATE_FAULT;
                    LOG_ERROR("MQTT 连接后清理失败: result=%d", (int)cleanup_result);

                } else if (should_connect == false) {
                    s_eg800_mqtt_retry_count = 0U;
                    mqtt_info.state = EG800_MQTT_STATE_IDLE;
                    LOG("MQTT 已取消连接");

                } else if (result == EG800_AT_RESULT_PARAM_ERROR) {
                    mqtt_info.state = EG800_MQTT_STATE_FAULT;
                    LOG_ERROR("MQTT 参数无效，停止自动连接");

                } else if (result == EG800_AT_RESULT_CANCELLED) {
                    s_eg800_mqtt_retry_count = 0U;
                    mqtt_info.state = EG800_MQTT_STATE_IDLE;
                    LOG("MQTT 收到新的连接意图，准备重新连接");

                } else if (s_eg800_mqtt_retry_count >= EG800_MQTT_RETRY_MAX) {
                    mqtt_info.state = EG800_MQTT_STATE_FAULT;
                    LOG_ERROR("MQTT 连接尝试次数已耗尽");

                } else {
                    retry_start_tick = xTaskGetTickCount();
                    mqtt_info.state = EG800_MQTT_STATE_RETRY_WAIT;
                    LOG("MQTT 连接失败，进入重试等待");
                }
                Eg800_Mqtt_Update_Snapshot(&mqtt_info);
            }
        }

        // ==================== 4. 设置下次等待 ====================
        wait_ticks = portMAX_DELAY;
        if (mqtt_info.state == EG800_MQTT_STATE_RETRY_WAIT) {
            retry_elapsed_ticks = xTaskGetTickCount() - retry_start_tick;
            wait_ticks = (retry_elapsed_ticks < retry_interval_ticks) ? (retry_interval_ticks - retry_elapsed_ticks) : 0U;
        } else if (((mqtt_info.state == EG800_MQTT_STATE_NONE) || (mqtt_info.state == EG800_MQTT_STATE_IDLE)) && (Eg800_Mqtt_Should_Connect() == true)) {
            wait_ticks = 0U;
        }
    }
}
