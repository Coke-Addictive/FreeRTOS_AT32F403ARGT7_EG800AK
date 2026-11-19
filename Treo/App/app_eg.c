#include "app_eg.h"

#include "drv_eg.h"
#include "eg_service.h"
#include "eg_basic.h"
#include "eg_network.h"
#include "eg_locate.h"
#include "eg_mqtt.h"
#include "eg_http.h"
#include "eg_timer.h"


#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"


#define LOG_TAG "EG"
#include "log.h"

#define APP_EG_BASIC_REFRESH_POLL_MS        100U      // 等待 Basic 刷新时的轮询间隔，单位 ms
#define APP_EG_BASIC_REFRESH_WAIT_MS        30000U    // 等待 Basic 全量刷新完成的最长时间，单位 ms
#define APP_EG_USER_QUERY_DELAY_MS          20000U    // 模拟用户首次查询前的等待时间，单位 ms
#define APP_EG_NETWORK_POLL_MS              500U      // 等待 Network 状态变化时的轮询间隔，单位 ms
#define APP_EG_NETWORK_CONNECT_WAIT_MS      1800000U  // 联网测试最长观察时间，覆盖本轮重试，单位 ms
#define APP_EG_NETWORK_DISCONNECT_WAIT_MS   180000U   // 断网测试最长等待时间，包含在途联网流程及清理，单位 ms
#define APP_EG_MQTT_PUBLISH_DELAY_MS        5000U     // MQTT 订阅成功到测试发布的间隔，单位 ms
#define APP_EG_MQTT_POLL_MS                 500U      // 等待 MQTT 任务状态变化的轮询间隔，单位 ms
#define APP_EG_MQTT_CONNECT_WAIT_MS         1800000U  // MQTT 测试最长观察时间，超时不自动取消连接，单位 ms
#define APP_EG_MQTT_TEST_SUB_TOPIC          "Treo/test/001"
#define APP_EG_MQTT_TEST_PUB_TOPIC          "Treo/test/002"

static SemaphoreHandle_t s_system_main_at_completion_sem = NULL; // System Main 启动测试专用的 AT 完成信号量

static void App_EG_Http_Test(void);
static void App_EG_Timer_Test(SemaphoreHandle_t completion_sem);

/**
 * @brief 打印一份 Basic 信息快照。
 * @param stage 当前快照所处的测试阶段。
 * @param snapshot 待打印的 Basic 信息快照。
 */
static void App_EG_Log_Basic_Snapshot(const char *stage, const Eg800BasicSnapshot_t *snapshot) {
    if ((stage == NULL) || (snapshot == NULL)) {
        return;
    }

    LOG("Basic 快照[%s]: update_tick=%lu, refreshing=%u", stage, (unsigned long)snapshot->update_tick, (unsigned int)snapshot->refresh_in_progress);
    LOG("Basic 状态[%s]: SIM=%u, CSQ=%u, BER=%u, operator_type=%u", stage, (unsigned int)snapshot->info.sim_state, (unsigned int)snapshot->info.csq, (unsigned int)snapshot->info.ber, (unsigned int)snapshot->info.operator_type);
    LOG("Basic 标识[%s]: IMEI=%s, IMSI=%s, ICCID=%s", stage, snapshot->info.imei, snapshot->info.imsi, snapshot->info.iccid);
    LOG("Basic 其他[%s]: phone=%s, operator=%s, firmware=%s", stage, snapshot->info.phone_number, snapshot->info.operator_name, snapshot->info.firmware_version);
}



/**
 * @brief 等待 Basic 后台全量刷新结束。
 * @param snapshot 刷新结束时的快照输出地址。
 * @return true 表示刷新已结束，false 表示读取失败或等待超时。
 */
static bool App_EG_Wait_Basic_Refresh(Eg800BasicSnapshot_t *snapshot) {
    TickType_t start_tick;

    if (snapshot == NULL) {
        return false;
    }

    start_tick = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(APP_EG_BASIC_REFRESH_WAIT_MS)) {
        if (Eg800_Basic_Get_Snapshot(snapshot) != RESULT_SUCCESS) {
            return false;
        }

        if (snapshot->refresh_in_progress == false) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_EG_BASIC_REFRESH_POLL_MS));
    }

    return false;
}

/**
 * @brief 轮询 Network 快照，等待指定状态并打印观察到的状态变化。
 * @param expected_state 本次等待的目标状态，测试使用 ACTIVE 或 IDLE。
 * @param timeout_ms 最长等待时间，单位 ms；超时只停止观察，不自动改变联网意图。
 * @param snapshot 最近一次读取成功的快照输出地址。
 * @return true 表示已观察到目标状态，false 表示参数无效、读取失败、进入 FAULT 或等待超时。
 * @note 仅读取缓存，不执行 PDP AT 命令；失败重试由 Network 任务负责。
 */
static bool App_EG_Wait_Network_State(Eg800NetworkState_e expected_state, uint32_t timeout_ms, Eg800NetworkSnapshot_t *snapshot) {
    TickType_t start_tick;
    Eg800NetworkState_e last_state = EG800_NETWORK_STATE_NONE;
    bool snapshot_logged = false;

    if (snapshot == NULL) {
        return false;
    }

    start_tick = xTaskGetTickCount();

    while (1) {
        if (Eg800_Network_Get_Snapshot(snapshot) != RESULT_SUCCESS) {
            LOG_ERROR("[NET_TEST] 读取 Network 快照失败");
            return false;
        }

        if ((snapshot_logged == false) || (snapshot->info.state != last_state)) {
            LOG("[NET_TEST] Network 快照: state=%u, refreshing=%u, update_tick=%lu", (unsigned int)snapshot->info.state, (unsigned int)snapshot->refresh_in_progress, (unsigned long)snapshot->update_tick);
            LOG("[NET_TEST] PDP 信息: id=%u, type=%u, operator=%u, ip=%s", (unsigned int)snapshot->info.pdp_id, (unsigned int)snapshot->info.pdp_type, (unsigned int)snapshot->info.operator_type, snapshot->info.ip_addr);
            last_state = snapshot->info.state;
            snapshot_logged = true;
        }

        if (snapshot->info.state == expected_state) {
            return true;
        }

        if (snapshot->info.state == EG800_NETWORK_STATE_FAULT) {
            LOG_ERROR("[NET_TEST] Network 已进入 FAULT，停止本次测试");
            return false;
        }

        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(timeout_ms)) {
            LOG_ERROR("[NET_TEST] 等待 Network 状态超时: expected=%u, current=%u", (unsigned int)expected_state, (unsigned int)snapshot->info.state);
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_EG_NETWORK_POLL_MS));
    }
}

/**
 * @brief 轮询 MQTT 快照，等待维护任务完成连接和订阅。
 * @note 静态保存快照，避免增加主任务栈占用；超时仅停止本次观察。
 */
static bool App_EG_Wait_Mqtt_Online(void) {
    static Eg800MqttSnapshot_t snapshot;
    TickType_t start_tick = xTaskGetTickCount();
    Eg800MqttState_e last_state = EG800_MQTT_STATE_NONE;
    bool snapshot_logged = false;

    while (1) {
        if (Eg800_Mqtt_Get_Snapshot(&snapshot) != RESULT_SUCCESS) {
            LOG_ERROR("[MQTT_TEST] 读取 MQTT 快照失败");
            return false;
        }

        if ((snapshot_logged == false) || (snapshot.info.state != last_state)) {
            LOG("[MQTT_TEST] MQTT 快照: state=%u, refreshing=%u, update_tick=%lu", (unsigned int)snapshot.info.state, (unsigned int)snapshot.refresh_in_progress, (unsigned long)snapshot.update_tick);
            last_state = snapshot.info.state;
            snapshot_logged = true;
        }

        if (snapshot.info.state == EG800_MQTT_STATE_ONLINE) {
            return true;
        }
        if (snapshot.info.state == EG800_MQTT_STATE_FAULT) {
            LOG_ERROR("[MQTT_TEST] MQTT 已进入 FAULT，停止本次测试");
            return false;
        }
        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(APP_EG_MQTT_CONNECT_WAIT_MS)) {
            LOG_ERROR("[MQTT_TEST] 等待 MQTT 在线超时: state=%u", (unsigned int)snapshot.info.state);
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_EG_MQTT_POLL_MS));
    }
}

/**
 * @brief 初始化 EG 驱动、Basic、Service 和 Network 层。
 */
void App_EG_Init(void) {
    Drv_EG_Init();
    Eg800_Basic_Init();
    Eg800_Service_Init();

    if (Eg800_Network_Init() != RESULT_SUCCESS) {
        LOG_ERROR("Network 初始化失败");
    }

    if (Eg800_Locate_Init() != RESULT_SUCCESS) {
        LOG_ERROR("Locate 初始化失败");
    }

}

/**
 * @brief 持续等待并打印 MQTT 消息，接收失败时返回。
 * @note MQTT 初始化后由单个业务任务调用，使用静态缓存接收消息。
 */
void App_EG_Mqtt_Receive(void) {
    static Eg800MqttReceiveMessage_t message;

    while (Eg800_Mqtt_Receive_Message(&message, portMAX_DELAY) == RESULT_SUCCESS) {
        LOG("[MQTT_RX] topic=%s, len=%lu, payload=%.*s", message.topic, (unsigned long)message.payload_len, (int)message.payload_len, (const char *)message.payload);
    }

    LOG_ERROR("[MQTT_RX] 接收失败，请检查 MQTT 初始化状态");
}

/**
 * @brief 启动 EG 模块，测试 Basic 信息、Network 联网、HTTP 请求和 MQTT 收发。
 * @note 仅供 System Main 任务调用；Network 和 MQTT 连接均由各自维护任务执行。
 * @note PDP 可用后先测试 HTTP，再登录 MQTT，以 QoS 0 订阅并发布一条测试消息。
 * @note 发布测试后进入持续消息接收，阻塞等待后续 MQTT 消息。
 */
void App_EG_Launch(void) {
    static const uint8_t mqtt_test_payload[] = "Hello MQTT from Treo_device_001";
    static const Eg800MqttConfig_t mqtt_test_config = {
        .server_port = EG800_MQTT_SERVER_PORT,
        .server_host = EG800_MQTT_SERVER_HOST,
        .client_id = "GMD4317|860909089612569",
        .username = "GMD4317|860909089612569|1782284258",
        .password = "39d6ff2ee154ac9b25b7cf9c986cdede",
    };
    static const Eg800MqttSubscribeList_t mqtt_test_subscribe_list = {
        .count = 1U,
        .topics = {
            APP_EG_MQTT_TEST_SUB_TOPIC,
        },
    };
    Eg800AtResult_e result;
    Eg800BasicSnapshot_t snapshot;
    Eg800NetworkSnapshot_t network_snapshot = {0};
    bool network_connected;
    UBaseType_t stack_free_words;

    // 启动测试与 Basic 后台任务分别等待自己的 AT 完成信号。
    if (s_system_main_at_completion_sem == NULL) {
        s_system_main_at_completion_sem = xSemaphoreCreateBinary();
        if (s_system_main_at_completion_sem == NULL) {
            LOG_ERROR("System Main AT 完成信号量创建失败");
            return;
        }
    }

    Drv_EG_Power_SW(PWR_ON);
    vTaskDelay(1000);

    Drv_EG_Launch_SW(PWR_ON);
    vTaskDelay(1500);
    Drv_EG_Launch_SW(PWR_OFF);



    vTaskDelay(7000);
    result = Eg800_Basic_Set_Echo_Close(s_system_main_at_completion_sem);
    if(result != EG800_AT_RESULT_OK) {
        LOG_ERROR("回显命令失败\r\n");
    }

    LOG("回显设置完毕");


    // 逐项执行基础接口，单项失败后继续，便于一次启动检查全部结果。
    result = Eg800_Basic_Set_Urc_Port_Uart1(s_system_main_at_completion_sem);
    if (result == EG800_AT_RESULT_OK) {
        LOG("URC 上报端口设置为 UART1");
    } else {
        LOG_ERROR("URC 上报端口设置失败: result=%d", (int)result);
    }

    result = Eg800_Basic_Set_Ri_Physical(s_system_main_at_completion_sem);
    if (result == EG800_AT_RESULT_OK) {
        LOG("RI 信号已设置为物理引脚输出");
    } else {
        LOG_ERROR("RI 信号设置失败: result=%d", (int)result);
    }

    if (Eg800_Basic_Request_Refresh() != RESULT_SUCCESS) {
        LOG_ERROR("Basic 基础信息刷新请求失败");
        return;
    }

    LOG("Basic 基础信息刷新已请求");
    if (Eg800_Basic_Get_Snapshot(&snapshot) == RESULT_SUCCESS) {
        App_EG_Log_Basic_Snapshot("刷新开始", &snapshot);
    }

    if (App_EG_Wait_Basic_Refresh(&snapshot) == true) {
        App_EG_Log_Basic_Snapshot("刷新完成", &snapshot);
    } else {
        LOG_ERROR("等待 Basic 基础信息刷新完成超时");
    }

    vTaskDelay(pdMS_TO_TICKS(APP_EG_USER_QUERY_DELAY_MS));

    if (Eg800_Basic_Get_Snapshot(&snapshot) == RESULT_SUCCESS) {
        App_EG_Log_Basic_Snapshot("20 秒后用户查询 1", &snapshot);
    }

    if (Eg800_Basic_Get_Snapshot(&snapshot) == RESULT_SUCCESS) {
        App_EG_Log_Basic_Snapshot("20 秒后用户查询 2", &snapshot);
    }
    // 前面已等待 20 秒，再等待 50 秒，验证缓存过期后的刷新。
    vTaskDelay(pdMS_TO_TICKS(50000U));

    if (Eg800_Basic_Get_Snapshot(&snapshot) == RESULT_SUCCESS) {
        App_EG_Log_Basic_Snapshot("70 秒后用户查询", &snapshot);

        // 获取接口立即返回，等待本轮后台刷新结束后再查看新数据。
        if (App_EG_Wait_Basic_Refresh(&snapshot) == true) {
            App_EG_Log_Basic_Snapshot("70 秒触发的刷新已结束", &snapshot);
        } else {
            LOG_ERROR("等待 Basic 刷新结束失败或超时");
        }
    } else {
        LOG_ERROR("70 秒后读取 Basic 快照失败");
    }

    // Connect 只提交联网意图，APN 选择及 PDP 命令全部由 Network 任务执行。
    LOG("[NET_TEST] 1/4 提交 Network 联网请求");
    if (Eg800_Network_Connect() != RESULT_SUCCESS) {
        LOG_ERROR("[NET_TEST] Network 联网请求提交失败");
        return;
    }

    LOG("[NET_TEST] 2/4 请求已提交，等待 ACTIVE;失败重试由 Network 任务执行");
    network_connected = App_EG_Wait_Network_State(EG800_NETWORK_STATE_ACTIVE, APP_EG_NETWORK_CONNECT_WAIT_MS, &network_snapshot);
    stack_free_words = uxTaskGetStackHighWaterMark(NULL);
    LOG("[NET_TEST] System Main 栈最小剩余: %lu words (%lu bytes)", (unsigned long)stack_free_words, (unsigned long)(stack_free_words * sizeof(StackType_t)));

    if ((network_connected == false) && (network_snapshot.info.state == EG800_NETWORK_STATE_FAULT)) {
        // 保留故障状态供上层诊断，不通过重复 Connect 或模块重启掩盖故障。
        return;
    }

    if (network_connected == true) {
        if ((Eg800_Network_Is_Active() == false) || (network_snapshot.info.pdp_id != EG800_NETWORK_CONTEXT_ID) || (network_snapshot.info.pdp_type != EG800_NETWORK_CONTEXT_TYPE) || (network_snapshot.info.ip_addr[0] == '\0') || (strcmp(network_snapshot.info.ip_addr, "0.0.0.0") == 0)) {
            network_connected = false;
            LOG_ERROR("[NET_TEST] ACTIVE 状态或 PDP 信息检查失败");
        } else {
            LOG("[NET_TEST] Network 联网成功，IP=%s", network_snapshot.info.ip_addr);
        }
    }

    // // 正常联网后测试主动断网；观察超时或结果无效时也关闭意图，避免测试退出后继续重试。
    // LOG("[NET_TEST] 3/4 提交 Network 断网请求");
    // if (Eg800_Network_Disconnect() != RESULT_SUCCESS) {
    //     LOG_ERROR("[NET_TEST] Network 断网请求提交失败");
    //     return;
    // }

    // LOG("[NET_TEST] 4/4 请求已提交，等待 IDLE");
    // if (App_EG_Wait_Network_State(EG800_NETWORK_STATE_IDLE, APP_EG_NETWORK_DISCONNECT_WAIT_MS, &network_snapshot) == false) {
    //     return;
    // }

    // if ((Eg800_Network_Is_Active() == true) || (network_snapshot.info.ip_addr[0] != '\0')) {
    //     LOG_ERROR("[NET_TEST] 断网结果检查失败：活动标志或 IP 尚未清除");
    //     return;
    // }

    // if (network_connected == true) {
    //     LOG("[NET_TEST] 测试通过：Connect、ACTIVE 快照、Disconnect、IDLE 快照检查通过，当前保持断开");
    // } else {
    //     LOG_ERROR("[NET_TEST] 联网测试未通过，已完成断网清理");
    // }

#if 1
    LOG("测试开始");
    Eg800_Locate_Test();
#else
    if (network_connected == false) {
        LOG_ERROR("[EG_TEST] Network 未连接，停止 HTTP 和 MQTT 测试");
        return;
    }

    App_EG_Http_Test();
    App_EG_Timer_Test(s_system_main_at_completion_sem);

    if (Eg800_Mqtt_Init() != RESULT_SUCCESS) {
        LOG_ERROR("MQTT 初始化失败");
        return;
    }


    result = Eg800_Mqtt_Set_Version(EG800_MQTT_VERSION, s_system_main_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("MQTT 版本设置失败: result=%d", (int)result);
        return;
    } else {
        LOG("MQTT 版本设置成功");
    }
    
    result = Eg800_Mqtt_Set_PDP_Context(s_system_main_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("MQTT PDP 配置失败: result=%d", (int)result);
        return;
    } else {
        LOG("MQTT PDP 配置成功");
    }

    result = Eg800_Mqtt_Set_Receive_Mode(EG800_MQTT_RECV_MODE, EG800_MQTT_RECV_LENGTH_ENABLED, s_system_main_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("MQTT 接收方式配置失败: result=%d", (int)result);
        return;
    }
    LOG("MQTT 接收方式配置成功: URC 直接上报，包含 payload 长度");

    result = Eg800_Mqtt_Set_Keepalive(EG800_MQTT_KEEPALIVE_S, s_system_main_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("[MQTT_TEST] 保活时间配置失败: result=%d", (int)result);
        return;
    }

    result = Eg800_Mqtt_Set_Clean_Session(EG800_MQTT_CLEAN_SESSION, s_system_main_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("[MQTT_TEST] 会话类型配置失败: result=%d", (int)result);
        return;
    }
    LOG("[MQTT_TEST] MQTT 基础配置完成");

    if (Eg800_Mqtt_Set_Config(&mqtt_test_config) != RESULT_SUCCESS) {
        LOG_ERROR("[MQTT_TEST] 连接配置保存失败");
        return;
    }

    if (Eg800_Mqtt_Set_Subscribe_List(&mqtt_test_subscribe_list) != RESULT_SUCCESS) {
        LOG_ERROR("[MQTT_TEST] 订阅列表保存失败");
        return;
    }
    LOG("[MQTT_TEST] 配置已保存: host=%s, port=%u, topics=%u", mqtt_test_config.server_host, (unsigned int)mqtt_test_config.server_port, (unsigned int)mqtt_test_subscribe_list.count);

    if (Eg800_Mqtt_Connect() != RESULT_SUCCESS) {
        LOG_ERROR("[MQTT_TEST] 连接请求提交失败");
        return;
    }
    LOG("[MQTT_TEST] 连接请求已提交，等待 MQTT Task 完成连接和订阅");

    if (App_EG_Wait_Mqtt_Online() == false) {
        return;
    }
    LOG("[MQTT_TEST] MQTT 已在线，等待 5 秒后发布测试消息");
    vTaskDelay(pdMS_TO_TICKS(APP_EG_MQTT_PUBLISH_DELAY_MS));

    result = Eg800_Mqtt_Publish_Topic(0U, APP_EG_MQTT_TEST_PUB_TOPIC, EG800_MQTT_DEFAULT_QOS, EG800_MQTT_DEFAULT_RETAIN, mqtt_test_payload, sizeof(mqtt_test_payload) - 1U, s_system_main_at_completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("[MQTT_TEST] 发布测试消息失败: result=%d", (int)result);
    } else {
        LOG("[MQTT_TEST] 发布成功: topic=%s, payload=%s", APP_EG_MQTT_TEST_PUB_TOPIC, (const char *)mqtt_test_payload);
    }

    App_EG_Mqtt_Receive();
#endif
}

// HTTP 固件下载测试配置
#define APP_EG_HTTP_TEST_DOWNLOAD_COUNT    3U        // 串行执行的固件下载轮数
#define APP_EG_HTTP_TEST_CHUNK_SIZE        2048U     // 单段请求长度，单位字节
#define APP_EG_HTTP_TEST_FILE_SIZE         75372U    // MCU_V6.0.5.bin 的参考长度，单位字节

/**
 * @brief 提交一轮固件下载并逐包接收，取到终态后返回。
 * @param request 测试固件的 URL、总长度和分包大小。
 * @param round 当前测试轮数，从 1 开始。
 * @return true 表示收到 DONE 且接收长度正确，false 表示提交、接收或下载失败。
 */
static bool App_EG_Http_Test_Download(const Eg800HttpDownloadRequest_t *request, unsigned int round) {
    Eg800HttpDownloadStartResult_e start_result;
    Eg800HttpDownloadEvent_t event;
    uint32_t received_size = 0U;

    LOG("[HTTP_TEST] 第 %u/%u 轮开始下载，总长 %lu B，分包 %lu B",
        round, APP_EG_HTTP_TEST_DOWNLOAD_COUNT, (unsigned long)request->file_size,
        (unsigned long)request->chunk_size);

    start_result = Eg800_Http_Download_Start(request);
    if (start_result != EG800_HTTP_DOWNLOAD_START_OK) {
        LOG_ERROR("[HTTP_TEST] 第 %u 轮提交下载失败，result=%d", round, (int)start_result);
        return false;
    }

    while (Eg800_Http_Download_Receive(&event, portMAX_DELAY) == RESULT_SUCCESS) {
        if (event.type == EG800_HTTP_DOWNLOAD_EVENT_DATA) {
            // OTA 或音频在这里按 event.offset 处理 event.data 中的 event.len 字节。
            // 处理完毕再接收下一包，下一次 Receive 会归还当前缓冲区。
            received_size += (uint32_t)event.len;
            LOG("[HTTP_TEST] 第 %u/%u 轮已接收 %lu B 固件包，偏移 %lu B，累计 %lu/%lu B",
                round, APP_EG_HTTP_TEST_DOWNLOAD_COUNT, (unsigned long)event.len,
                (unsigned long)event.offset, (unsigned long)received_size,
                (unsigned long)request->file_size);
            continue;
        }

        if (event.type == EG800_HTTP_DOWNLOAD_EVENT_DONE) {
            if ((received_size != request->file_size) || (event.offset != received_size)) {
                LOG_ERROR("[HTTP_TEST] 第 %u 轮长度不符，实收 %lu B，任务完成 %lu B，期望 %lu B",
                    round, (unsigned long)received_size, (unsigned long)event.offset,
                    (unsigned long)request->file_size);
                return false;
            }

            LOG("[HTTP_TEST] 第 %u/%u 轮完成，合计 %lu B",
                round, APP_EG_HTTP_TEST_DOWNLOAD_COUNT, (unsigned long)received_size);
            return true;
        }

        LOG_ERROR("[HTTP_TEST] 第 %u 轮下载失败，stage=%d，result=%d，offset=%lu B，HTTP=%u",
            round, (int)event.stage, (int)event.result, (unsigned long)event.offset,
            (unsigned int)event.http_status);
        return false;
    }

    LOG_ERROR("[HTTP_TEST] 第 %u 轮接收下载事件失败", round);
    return false;
}

/**
 * @brief 联网后配置 HTTP，串行验证三次固件下载和逐包接收。
 * @note 由 System Main 在 PDP 可用后调用。
 */
static void App_EG_Http_Test(void) {
    // 三轮复用同一个测试文件，按文件总长度确定最后一包。
    static const Eg800HttpDownloadRequest_t request = {
        .url = "http://beidou.gmd-cloud.com/admin/data/media/ota/firmware/GMD4317/MCU_V6.0.5.bin",
        .file_size = APP_EG_HTTP_TEST_FILE_SIZE,
        .chunk_size = APP_EG_HTTP_TEST_CHUNK_SIZE
    };
    Eg800AtResult_e result;
    unsigned int round;

    if (Eg800_Http_Download_Init() != RESULT_SUCCESS) {
        LOG_ERROR("[HTTP_TEST] 初始化下载任务失败");
        return;
    }

    result = Eg800_Http_Configure_Basic();
    if (result != EG800_AT_RESULT_OK) {
        LOG_ERROR("[HTTP_TEST] 基础配置失败，result=%d", (int)result);
        return;
    }

    for (round = 1U; round <= APP_EG_HTTP_TEST_DOWNLOAD_COUNT; round++) {
        if (App_EG_Http_Test_Download(&request, round) == false) {
            return;
        }
    }
}

/**
 * @brief 测试模组的网络授时配置和 RTC 查询。
 * @param completion_sem System Main 使用的 AT 完成信号量。
 */
static void App_EG_Timer_Test(SemaphoreHandle_t completion_sem) {
    Eg800TimerClock_t clock;
    Eg800AtResult_e timer_result;

    timer_result = Eg800_Timer_Set_Auto_Update(EG800_TIMER_AUTO_UPDATE_LOCAL, completion_sem);
    if (timer_result == EG800_AT_RESULT_OK) {
        LOG("[TIMER_TEST] CTZU=3 设置成功，重启模组后生效");
    } else {
        LOG_ERROR("[TIMER_TEST] CTZU=3 设置失败，result=%d", (int)timer_result);
    }

    timer_result = Eg800_Timer_Query_Clock(&clock, completion_sem);
    if (timer_result != EG800_AT_RESULT_OK) {
        LOG_ERROR("[TIMER_TEST] 查询模组 RTC 失败，result=%d", (int)timer_result);
        return;
    }

    LOG("[TIMER_TEST] 模组 RTC %04u-%02u-%02u %02u:%02u:%02u，UTC 偏移 %+d 分钟",
        (unsigned int)clock.year, (unsigned int)clock.month, (unsigned int)clock.day,
        (unsigned int)clock.hour, (unsigned int)clock.minute, (unsigned int)clock.second,
        (int)clock.timezone_quarters * 15);
}
























