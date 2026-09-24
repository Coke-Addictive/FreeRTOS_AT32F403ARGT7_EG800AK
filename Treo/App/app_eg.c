#include "app_eg.h"

#include "drv_eg.h"
#include "eg_service.h"
#include "eg_basic.h"
#include "eg_network.h"


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

static SemaphoreHandle_t s_system_main_at_completion_sem = NULL; // System Main 启动测试专用的 AT 完成信号量

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
 * @brief 初始化 EG 驱动、Basic、Service 和 Network 层。
 */
void App_EG_Init(void) {
    Drv_EG_Init();
    Eg800_Basic_Init();
    Eg800_Service_Init();
    if (Eg800_Network_Init() != RESULT_SUCCESS) {
        LOG_ERROR("Network 初始化失败");
    }


    
}

/**
 * @brief 启动 EG 模块，测试 Basic 信息查询和 Network 异步联网、断网接口。
 * @note 仅供 System Main 任务调用；Basic AT 使用本任务完成信号量，Network AT 由其维护任务执行。
 * @note 正常测试按 Connect、等待 ACTIVE、Disconnect、等待 IDLE 的顺序执行，结束时保持断开。
 */
void App_EG_Launch(void) {
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

    LOG("[NET_TEST] 2/4 请求已提交，等待 ACTIVE；失败重试由 Network 任务执行");
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

    // 正常联网后测试主动断网；观察超时或结果无效时也关闭意图，避免测试退出后继续重试。
    LOG("[NET_TEST] 3/4 提交 Network 断网请求");
    if (Eg800_Network_Disconnect() != RESULT_SUCCESS) {
        LOG_ERROR("[NET_TEST] Network 断网请求提交失败");
        return;
    }

    LOG("[NET_TEST] 4/4 请求已提交，等待 IDLE");
    if (App_EG_Wait_Network_State(EG800_NETWORK_STATE_IDLE, APP_EG_NETWORK_DISCONNECT_WAIT_MS, &network_snapshot) == false) {
        return;
    }

    if ((Eg800_Network_Is_Active() == true) || (network_snapshot.info.ip_addr[0] != '\0')) {
        LOG_ERROR("[NET_TEST] 断网结果检查失败：活动标志或 IP 尚未清除");
        return;
    }

    if (network_connected == true) {
        LOG("[NET_TEST] 测试通过：Connect、ACTIVE 快照、Disconnect、IDLE 快照检查通过，当前保持断开");
    } else {
        LOG_ERROR("[NET_TEST] 联网测试未通过，已完成断网清理");
    }
}




