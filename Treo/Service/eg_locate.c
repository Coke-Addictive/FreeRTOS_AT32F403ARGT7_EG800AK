#include "eg_locate.h"

#include <math.h>
#include <stdio.h>

#include "eg_service.h"


#define LOG_TAG "EG_LOCATE"
#include "log.h"

#define EG800_LOCATE_GNSS_OPEN_TIMEOUT_MS       3000U    // QGPS=1 等待 OK/ERROR 的超时时间，单位 ms
#define EG800_LOCATE_GNSS_CLOSE_TIMEOUT_MS      3000U    // QGPS=0 等待 OK/ERROR 的超时时间，单位 ms
#define EG800_LOCATE_GNSS_QUERY_TIMEOUT_MS      3000U    // QGPSLOC=2 等待 OK/ERROR 的超时时间，单位 ms
#define EG800_LOCATE_GNSS_STOP_TIMEOUT_MS       3000U    // QGPSEND 等待 OK/ERROR 的超时时间，单位 ms

#define EG800_LOCATE_WIFI_SCAN_TIMEOUT_MS       12000U   // wifi-scan 总体扫描超时时间，单位 ms
#define EG800_LOCATE_LBS_QUERY_TIMEOUT_MS       10000U   // QCELLINFO? 等待 OK/ERROR 的软件超时上限，单位 ms

#define EG800_LOCATE_GNSS_QUERY_RESPONSE_SIZE   192U     // QGPSLOC 查询响应缓冲区容量，单位字节


#define EG800_LOCATE_WIFI_AP_MAX                5U       // 单轮最多缓存的热点数量，单位条


// Locate层内部AT命令编号
typedef enum {
    EG800_LOCATE_CMD_GNSS_OPEN,                     // GNSS 打开
    EG800_LOCATE_CMD_GNSS_CLOSE,                    // GNSS 禁用
    EG800_LOCATE_CMD_WIFISCAN,                      // wifi-scan
    EG800_LOCATE_CMD_BDS,
    EG800_LOCATE_CMD_GNSS_QUERY,                    // GNSS 经纬度查询
    EG800_LOCATE_CMD_GNSS_STOP,                     // GNSS 结束定位
    EG800_LOCATE_CMD_LBS                            // 服务小区和邻区信息查询
}Eg800LocateCmd_e;


// GNSS 最近确认的实际硬件级开关状态。
typedef enum {
    EG800_LOCATE_GNSS_STATE_UNKNOWN = 0U,               // 未知
    EG800_LOCATE_GNSS_STATE_ON,                         // 开启
    EG800_LOCATE_GNSS_STATE_OFF                         // 关闭
} Eg800LocateGnssState_e;


// 单次 GNSS 响应中需要保留的解析结果。
typedef struct {
    double latitude;          // 纬度，单位十进制度
    double longitude;         // 经度，单位十进制度
    double altitude;          // 海拔，单位 m，允许为负数
    uint8_t satellite_count;   // 参与定位计算的卫星数量
} Eg800LocateGnssInfo_t;



// 单条 Wi-Fi 热点信息
typedef struct {
    char mac[18];          // MAC 地址，17 个字符加结尾 '\0'
    int32_t rssi;          // 热点信号强度，单位 dBm
    uint8_t channel;       // 热点信道，范围 1～13
} Eg800LocateWifiAp_t;

// 一轮 Wi-Fi 扫描结果
typedef struct {
    bool is_valid;                                          // 有效性
    uint8_t count;                                          // 有效热点数量，范围 0～EG800_LOCATE_WIFI_AP_MAX
    Eg800LocateWifiAp_t aps[EG800_LOCATE_WIFI_AP_MAX];      // 本轮已保存的热点列表
} Eg800LocateWifiScan_t;






// AT+QCELLINFO? 中用于高德定位的 LTE 小区信息
typedef struct {
    bool is_valid;              // 有效性
    uint32_t mcc;               // 移动国家码
    uint32_t mnc;               // 移动网络码
    uint32_t tac;               // 跟踪区码，模块以十六进制文本上报
    uint32_t cell_id;           // 小区标识，模块以十六进制文本上报
    int32_t rssi;               // 接收信号强度，单位dBm
} Eg800LocateLbsCell_t;





static bool s_eg800_locate_initialized = false;                         // Locate 层初始化标志位
static Eg800LocateGnssState_e eg800_gnss_hardware_state = EG800_LOCATE_GNSS_STATE_UNKNOWN;  // GNSS外设硬件状态


static TaskHandle_t             s_eg800_locate_task_handle = NULL;      // Locate 后台刷新任务句柄
static SemaphoreHandle_t        s_locate_at_completion_sem = NULL;      // Locate 后台任务等待自己的 AT 请求完成



// 实际处理URC的函数声明
static void Eg800_Locate_Gnss_Urc_Handler(const char *line);
static void Eg800_Locate_WifiScan_Urc_Handler(const char *line);
static void Eg800_Locate_Lbs_Urc_Handler(const char *line);

// 路由表需要在声明之后
static const Eg800UrcRegistration_t s_eg800_locate_urc_routes[] = {
    {"+QGPS:",          EG800_AT_OWNER_LOCATION,            Eg800_Locate_Gnss_Urc_Handler},
    {"+QWIFISCAN:",     EG800_AT_OWNER_LOCATION,            Eg800_Locate_WifiScan_Urc_Handler},
    {"+QCELLINFO:",     EG800_AT_OWNER_LOCATION,            Eg800_Locate_Lbs_Urc_Handler},
};


static Eg800LocateConfig_t s_eg800_locate_config = {
    .locate_sw = true,
    .bds_sw = true,
    .lbs_sw = true,
    .wifi_sw = true
};


// 定位开关统一通过 Eg800_Locate_Read_Config() 读取，快照只保存运行进度和定位结果。
static Eg800LocationSnapshot_t s_eg800_locate_snapshot = {           // Locate 运行进度及各定位方式的结果缓存
    .refresh_in_progress = false,
    .bds_info = {.location_source = LOCATE_SOURCE_BDS},
    .wifi_info = {.location_source = LOCATE_SOURCE_WIFI},
    .lbs_info = {.location_source = LOCATE_SOURCE_LBS}
};


static Eg800LocateWifiScan_t s_eg800_locate_wifi_scan_transfer_date ={0};           //WIFI定位信息暂存处
static Eg800LocateLbsCell_t s_eg800_locate_lbs_servingcell_lte = {0};               //lbs扫描主服务小区lte制式信息


//--------------------------------------------------------函数声明-----------------------------------------
// URC 注册
static Result_t Eg800_Locate_Register_Urcs(void);
//解析处理函数
static bool Eg800_Locate_Parse_Gnss(const char *line, Eg800LocateGnssInfo_t *info);
static bool Eg800_Locate_Parse_WifiScan(const char *line, Eg800LocateWifiAp_t *info);
static bool Eg800_Locate_Parse_Lbs(const char *line, Eg800LocateLbsCell_t *info);

//更新信息数据
static void Eg800_Locate_Update_Info(Eg800LocationSource_e source, const Eg800LocationInfo_t *info, uint32_t fields);
static void Eg800_Locate_Update_Refresh_State(bool refresh_in_progress);



// 核心任务
static void Eg800_Locate_Task(void *parameters);





//--------------------------------------------------------URC 处理-----------------------------------------

static void Eg800_Locate_Gnss_Urc_Handler(const char *line) {
    // LOG("收到GNSS开启成功标志:%s",line);
    LOG("收到GNSS开启成功标志");
    eg800_gnss_hardware_state = EG800_LOCATE_GNSS_STATE_ON;
}


/**
 * @brief 将解析成功的热点保存到正在收集的 Wi-Fi 扫描缓存。
 * @param line Service 分发的完整 +QWIFISCAN 文本行。
 */
static void Eg800_Locate_WifiScan_Urc_Handler(const char *line) {
    Eg800LocateWifiAp_t parsed_wifi_info = {0};
    if (Eg800_Locate_Parse_WifiScan(line, &parsed_wifi_info) == false) {
        return;
    }
    if (s_eg800_locate_wifi_scan_transfer_date.count < EG800_LOCATE_WIFI_AP_MAX) {
        s_eg800_locate_wifi_scan_transfer_date.aps[s_eg800_locate_wifi_scan_transfer_date.count] = parsed_wifi_info;
        s_eg800_locate_wifi_scan_transfer_date.count++;
    }
}

/**
 * @brief 记录未被当前 AT 请求接收的 QCELLINFO 响应，不更新主服务小区缓存。
 * @param line Service 分发的完整 +QCELLINFO 文本行，为 NULL 时忽略。
 */
static void Eg800_Locate_Lbs_Urc_Handler(const char *line) {
    if (line == NULL) {
        return;
    }

    LOG_WARN("收到未归入当前查询的 QCELLINFO 响应: %s", line);
}



//----------------------------------------------------关键-------------------------------------------------

/**
 * @brief 注册 Basic 层负责的全部主动上报 URC。
 * @return RESULT_SUCCESS 表示全部注册成功，RESULT_FAIL 表示存在注册失败。
 */
static Result_t Eg800_Gnss_Register_Urcs(void) {
    return Eg800_Urc_Register_Table(s_eg800_locate_urc_routes, ARRAY_SIZE(s_eg800_locate_urc_routes));;
}


//--------------------------------------------------------工具函数-----------------------------------------

/**
 * @brief 同步执行一条 Locate 层普通文本 AT 命令。
 * @param cmd_id Locate 层内部命令编号。
 * @param name 命令日志名称，可为 NULL。
 * @param tx 完整 AT 发送字符串，必须包含 "\r\n"，调用期间保持有效。
 * @param rsp_prefix 需要保存的响应前缀，NULL 表示保存未被 URC 路由消费的文本。
 * @param rx_buf 响应接收缓冲区，调用期间保持有效；不需要响应内容时传 NULL。
 * @param rx_buf_size 响应缓冲区容量，单位字节；rx_buf 为 NULL 时必须为 0。
 * @param timeout_ms 等待 OK 或 ERROR 的超时时间，单位 ms；0 表示使用 Service 默认值。
 * @param result_info AT 执行详细结果，可为 NULL。
 * @param completion_sem 当前调用任务专用的二值信号量，调用期间保持有效。
 * @return EG800_AT_RESULT_OK 表示命令成功，其他值表示对应的执行错误。
 * @note Locate 和 AT 核心服务应已初始化；仅供任务上下文串行调用。
 * @note 禁止在 ISR、AT 核心 Service 任务及其 URC 回调中调用。
 * @note 同一信号量同一时间只允许对应一条未完成请求。
 */
static Eg800AtResult_e Eg800_Locate_At_Execute(Eg800LocateCmd_e cmd_id, const char *name, const char *tx, const char *rsp_prefix, char *rx_buf, size_t rx_buf_size, uint32_t timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    Eg800AtRequest_t request = {0};

    request.cmd.owner = EG800_AT_OWNER_LOCATION;
    request.cmd.id = (uint8_t)cmd_id;
    request.flow = EG800_AT_FLOW_TEXT;

    request.name = name;
    request.tx = tx;
    request.rsp_prefix = rsp_prefix;
    request.rx_buf = rx_buf;
    request.rx_buf_size = rx_buf_size;

    request.final_timeout_ms = timeout_ms;

    return Eg800_AT_Execute(&request, result_info, completion_sem);
}

//--------------------------------------------------------解析处理函数-----------------------------------------
/**
 * @brief 解析 +QGPSLOC 响应中的经纬度、海拔和卫星数量，并校验定位结果。
 * @param line 完整的 +QGPSLOC 文本响应，坐标格式固定为十进制度。
 * @param info GNSS 解析结果输出地址；返回 true 时全部成员有效，返回 false 时可能已写入部分成员，应忽略结果。
 * @return true 表示所需字段和定位状态均有效，false 表示参数无效或解析失败。
 */
static bool Eg800_Locate_Parse_Gnss(const char *line, Eg800LocateGnssInfo_t *info) {
    const char *satellite_field;
    unsigned int fix;
    unsigned int satellite_count;
    uint8_t field_index;
    int fields_end = 0;
    int satellite_end = 0;

    if ((line == NULL) || (info == NULL)) {
        return false;
    }

    // 响应顺序为 UTC、纬度、经度、HDOP、海拔、fix；%n 确认 fix 后的逗号也已匹配。
    if ((sscanf(line, "+QGPSLOC: %*[^,\r\n],%lf,%lf,%*[^,\r\n],%lf,%1u,%n", &info->latitude, &info->longitude, &info->altitude, &fix, &fields_end) != 4) || (fields_end == 0)) {
        return false;
    }

    // 使用正向范围判断，同时拒绝 NaN、无穷大和超出地理范围的数值。
    if (!((info->latitude >= -90.0) && (info->latitude <= 90.0) && (info->longitude >= -180.0) && (info->longitude <= 180.0)) || !isfinite(info->altitude)) {
        return false;
    }

    // fix 为 1 时尚未定位，只接受 2D 或 3D 定位结果。
    if ((fix != 2U) && (fix != 3U)) {
        return false;
    }

    // 跳过航向、两种速度和日期字段，允许未使用的字段为空，但不能跨越响应行。
    satellite_field = &line[fields_end];
    for (field_index = 0U; field_index < 4U; field_index++) {
        satellite_field += strcspn(satellite_field, ",\r\n");
        if (*satellite_field != ',') {
            return false;
        }
        satellite_field++;
    }

    while ((*satellite_field == ' ') || (*satellite_field == '\t')) {
        satellite_field++;
    }

    // 先用较宽的整数接收并检查范围，避免直接写入 uint8_t 时截断。
    if ((*satellite_field < '0') || (*satellite_field > '9') || (sscanf(satellite_field, "%3u%n", &satellite_count, &satellite_end) != 1) || (satellite_count > UINT8_MAX)) {
        return false;
    }

    satellite_field += satellite_end;
    satellite_field += strspn(satellite_field, " \t\r\n");
    if (*satellite_field != '\0') {
        return false;
    }

    info->satellite_count = (uint8_t)satellite_count;
    return true;
}



/**
 * @brief 解析一条 +QWIFISCAN 热点上报，TIMEOUT 不作为有效热点。
 * @param line 完整的 +QWIFISCAN 文本行，允许末尾带空白或换行符。
 * @param info 热点信息输出地址；返回 true 时全部成员有效，返回 false 时可能已写入部分成员，应忽略结果。
 * @return true 表示热点格式和字段均有效，false 表示 TIMEOUT、参数无效或解析失败。
 */
static bool Eg800_Locate_Parse_WifiScan(const char *line, Eg800LocateWifiAp_t *info) {
    long long parsed_rssi;
    unsigned int parsed_channel;
    uint8_t index;
    int fields_end = 0;

    if ((line == NULL) || (info == NULL)) {
        return false;
    }

    // ecn 和 ssid 按手册固定为 -；限制数值宽度并用较宽类型接收，避免非法长数字导致溢出。
    // MAC 字符集显式列举，兼容 MicroLIB 不展开 0-9 等范围的行为。
    if ((sscanf(line, "+QWIFISCAN: (-,-,%11lld,\"%17[0123456789ABCDEFabcdef:]\",%2u)%n", &parsed_rssi, info->mac, &parsed_channel, &fields_end) != 3) || (fields_end == 0)) {
        return false;
    }

    if ((parsed_rssi < INT32_MIN) || (parsed_rssi > INT32_MAX) || (parsed_channel < 1U) || (parsed_channel > 13U) || (strlen(info->mac) != 17U)) {
        return false;
    }

    // 扫描格式已限制 MAC 字符集，这里进一步检查六组十六进制字节之间的冒号位置。
    for (index = 0U; index < 17U; index++) {
        if ((index % 3U) == 2U) {
            if (info->mac[index] != ':') {
                return false;
            }
        } else if (info->mac[index] == ':') {
            return false;
        }
    }

    // 必须匹配右括号，且末尾只能有空白，拒绝截断响应或附带其他字段的文本。
    line += fields_end;
    line += strspn(line, " \t\r\n");
    if (*line != '\0') {
        return false;
    }

    info->rssi = (int32_t)parsed_rssi;
    info->channel = (uint8_t)parsed_channel;
    return true;
}

/**
 * @brief 解析 +QCELLINFO 中 LTE 主服务小区的 MCC、MNC、TAC、Cell ID 和 RSSI。
 * @param line 完整的 +QCELLINFO 文本行，只接受 servingcell 和 LTE。
 * @param info 小区信息输出地址；返回 true 时全部成员有效，返回 false 时可能已写入部分成员，应忽略结果。
 * @return true 表示主服务小区所需字段有效，false 表示参数无效、非 LTE 主服务小区或解析失败。
 */
static bool Eg800_Locate_Parse_Lbs(const char *line, Eg800LocateLbsCell_t *info) {
    const char *rssi_field;
    long long parsed_rssi;
    uint8_t field_index;
    int fields_end = 0;
    int rssi_end = 0;

    if ((line == NULL) || (info == NULL)) {
        return false;
    }

    // 固定匹配 LTE 主服务小区；MCC/MNC 按十进制读取，TAC/Cell ID 按十六进制读取。
    if ((sscanf(line, "+QCELLINFO: \"servingcell\",\"LTE\",%3u,%3u,%4x,%7x,%n", &info->mcc, &info->mnc, &info->tac, &info->cell_id, &fields_end) != 4) || (fields_end == 0)) {
        return false;
    }

    if ((info->mcc > 999U) || (info->mnc > 999U) || (info->tac > 0xFFFFU) || (info->cell_id > 0x0FFFFFFFU)) {
        return false;
    }

    // Cell ID 后依次跳过 PCI、RX_lev、RX_dbm、EARFCN、RSRP，空字段仍占一个位置。
    rssi_field = &line[fields_end];
    for (field_index = 0U; field_index < 5U; field_index++) {
        rssi_field += strcspn(rssi_field, ",\r\n");
        if (*rssi_field != ',') {
            return false;
        }
        rssi_field++;
    }

    // RSSI 后应有 SINR 字段的分隔逗号；SINR 及可能追加的字段不参与本接口解析。
    if ((sscanf(rssi_field, "%11lld,%n", &parsed_rssi, &rssi_end) != 1) || (rssi_end == 0) || (parsed_rssi < INT32_MIN) || (parsed_rssi > INT32_MAX)) {
        return false;
    }

    info->rssi = (int32_t)parsed_rssi;
    info->is_valid = true;
    return true;
}
















//--------------------------------------------------------更新信息数据-----------------------------------------
/**
 * @brief 在临界区内按字段掩码更新一份定位结果，未选中的成员保持原值。
 * @param source 选择待更新的 BDS、Wi-Fi 或 LBS 结果缓存，其他值不更新；不自动修改 location_source 成员。
 * @param info 新成员值的来源地址，调用期间应保持稳定；为 NULL 时不更新。
 * @param fields EG800_LOCATE_UPDATE_* 的按位或组合；0 表示不更新，未定义的位忽略。
 */
static void Eg800_Locate_Update_Info(Eg800LocationSource_e source, const Eg800LocationInfo_t *info, uint32_t fields) {
    Eg800LocationInfo_t *dst = NULL;

    if (info == NULL) {
        return;
    }

    if (source == LOCATE_SOURCE_BDS) {
        dst = &s_eg800_locate_snapshot.bds_info;
    } else if (source == LOCATE_SOURCE_WIFI) {
        dst = &s_eg800_locate_snapshot.wifi_info;
    } else if (source == LOCATE_SOURCE_LBS) {
        dst = &s_eg800_locate_snapshot.lbs_info;
    } else {
        return ;
    }

    taskENTER_CRITICAL();

    if ((fields & EG800_LOCATE_UPDATE_IS_VALID) != 0U) {
        dst->is_valid = info->is_valid;
    }
    if ((fields & EG800_LOCATE_UPDATE_LONGITUDE) != 0U) {
        dst->longitude = info->longitude;
    }
    if ((fields & EG800_LOCATE_UPDATE_LATITUDE) != 0U) {
        dst->latitude = info->latitude;
    }
    if ((fields & EG800_LOCATE_UPDATE_ALTITUDE) != 0U) {
        dst->altitude = info->altitude;
    }
    if ((fields & EG800_LOCATE_UPDATE_ERROR_CODE) != 0U) {
        dst->error_code = info->error_code;
    }
    if ((fields & EG800_LOCATE_UPDATE_LOCATION_SOURCE) != 0U) {
        dst->location_source = info->location_source;
    }
    if ((fields & EG800_LOCATE_UPDATE_SUCCESS_TIME) != 0U) {
        dst->success_time = info->success_time;
    }

    taskEXIT_CRITICAL();
}

/**
 * @brief 更新定位刷新标志
 * @param refresh_in_progress true 表示正在定位或收尾，false 表示本轮定位流程已结束。
 */
static void Eg800_Locate_Update_Refresh_State(bool refresh_in_progress) {
    taskENTER_CRITICAL();
    s_eg800_locate_snapshot.refresh_in_progress = refresh_in_progress;
    taskEXIT_CRITICAL();
}





































//--------------------------------------------------------公开的API接口--------------------------------------------------------

Result_t Eg800_Locate_Init(void) {
    BaseType_t task_result;

    // 幂等检查
    if (s_eg800_locate_initialized == true) {
        return RESULT_SUCCESS;
    }

    // 注册URC
    if (Eg800_Gnss_Register_Urcs() != RESULT_SUCCESS) {
        return RESULT_FAIL;
    }


    // 创建Gnss 信号量
    s_locate_at_completion_sem = xSemaphoreCreateBinary();
    if (s_locate_at_completion_sem == NULL) {
        LOG_ERROR("Gnss AT 完成信号量创建失败");
        return RESULT_FAIL;
    }


    task_result = xTaskCreate(Eg800_Locate_Task,
                          "EG Gnss Info Maintain",
                          EG800_GNSS_TASK_STACK_SIZE,
                          NULL,
                          EG800_GNSS_TASK_PRIORITY,
                          &s_eg800_locate_task_handle);


    if (task_result != pdPASS) {
        s_eg800_locate_task_handle = NULL;
        LOG_ERROR("Gnss 后台刷新任务创建失败");
        return RESULT_FAIL;
    }

    s_eg800_locate_initialized = true;
    eg800_gnss_hardware_state = EG800_LOCATE_GNSS_STATE_OFF;

    return RESULT_SUCCESS;
}

void Eg800_Locate_Config_SW_Update(bool locate_sw, bool bds_sw, bool wifi_sw, bool lbs_sw) {
#if 0
    s_eg800_locate_config.locate_sw = locate_sw;
    s_eg800_locate_config.bds_sw = bds_sw;
    s_eg800_locate_config.wifi_sw = wifi_sw;
    s_eg800_locate_config.lbs_sw = lbs_sw;
#else
    s_eg800_locate_config.locate_sw = locate_sw;
    s_eg800_locate_config.bds_sw = bds_sw;
    s_eg800_locate_config.wifi_sw = wifi_sw;
    s_eg800_locate_config.lbs_sw = lbs_sw;
#endif
}





/**
 * @brief 开启或关闭 GNSS 功能。
 * @param enabled true 表示开启 GNSS，false 表示关闭 GNSS。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return OK 表示开关命令执行成功；未初始化或信号量为空时返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 开启成功不代表已经取得定位结果；终止响应超时由对应的 GNSS 命令超时宏设置，不执行搜星等待或重试。
 * @note 仅供任务上下文调用，禁止在 Service 任务、URC 回调或 ISR 中调用。
 */
Eg800AtResult_e Eg800_Locate_Gnss_Set_Enabled(bool enabled, SemaphoreHandle_t completion_sem) {
    if ((s_eg800_locate_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    if (enabled) {
        return Eg800_Locate_At_Execute(EG800_LOCATE_CMD_GNSS_OPEN, "开启 GNSS", "AT+QGPS=1\r\n", NULL, NULL, 0U, EG800_LOCATE_GNSS_OPEN_TIMEOUT_MS, NULL, completion_sem);
    } else {
        return Eg800_Locate_At_Execute(EG800_LOCATE_CMD_GNSS_CLOSE, "关闭 GNSS", "AT+QGPS=0\r\n", NULL, NULL, 0U, EG800_LOCATE_GNSS_CLOSE_TIMEOUT_MS, NULL, completion_sem);
    }
}




/**
 * @brief 查询一次 GNSS 经纬度和海拔，并更新 BDS 结果缓存。
 * @param longitude 经度输出地址，单位十进制度，仅成功时写入。
 * @param latitude 纬度输出地址，单位十进制度，仅成功时写入。
 * @param altitude 海拔输出地址，单位 m，仅成功时写入。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return OK 表示查询及解析成功；未初始化或参数无效返回 PARAM_ERROR；响应缺失、截断、所需字段非法或未定位返回 DATA_FAIL；其他值为 AT 执行错误。
 * @note 仅供任务上下文调用，禁止在 Service 任务、URC 回调或 ISR 中调用；成功时更新 BDS 结果缓存，保留 success_time 和刷新标志。
 */
Eg800AtResult_e Eg800_Locate_Gnss_Query(double *longitude, double *latitude, double *altitude, SemaphoreHandle_t completion_sem) {
    char response[EG800_LOCATE_GNSS_QUERY_RESPONSE_SIZE] = {0};
    Eg800LocateGnssInfo_t parsed_gnss_info = {0};
    Eg800LocationInfo_t info = {0};
    Eg800AtResult_e result;

    if ((s_eg800_locate_initialized == false) || (longitude == NULL) || (latitude == NULL) || (altitude == NULL) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    result = Eg800_Locate_At_Execute(EG800_LOCATE_CMD_GNSS_QUERY, "查询 GNSS 经纬度", "AT+QGPSLOC=2\r\n", "+QGPSLOC:", response, sizeof(response), EG800_LOCATE_GNSS_QUERY_TIMEOUT_MS, NULL, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // Service 缓冲区不足时会截断响应，不能把截断文本作为有效查询结果。
    if ((response[0] == '\0') || (strlen(response) >= (sizeof(response) - 1U))) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    if (Eg800_Locate_Parse_Gnss(response, &parsed_gnss_info) == false) {
        return EG800_AT_RESULT_DATA_FAIL;
    }


    // 传递数据给调用者
    *longitude = parsed_gnss_info.longitude;
    *latitude = parsed_gnss_info.latitude;
    *altitude = parsed_gnss_info.altitude;

    // 更新本地快照
    info.is_valid = true;
    info.longitude = parsed_gnss_info.longitude;
    info.latitude = parsed_gnss_info.latitude;
    info.altitude = parsed_gnss_info.altitude;
    info.error_code = 0;
    info.location_source = LOCATE_SOURCE_BDS;
    Eg800_Locate_Update_Info(LOCATE_SOURCE_BDS, &info, EG800_LOCATE_UPDATE_IS_VALID | EG800_LOCATE_UPDATE_LONGITUDE | EG800_LOCATE_UPDATE_LATITUDE | EG800_LOCATE_UPDATE_ALTITUDE | EG800_LOCATE_UPDATE_ERROR_CODE | EG800_LOCATE_UPDATE_LOCATION_SOURCE);

    return EG800_AT_RESULT_OK;
}

/**
 * @brief 通过 AT+QGPSEND 停止 GNSS。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return OK 表示停止命令执行成功；未初始化或信号量为空时返回 PARAM_ERROR；其他值为 AT 执行错误。
 * @note 仅供任务上下文调用，禁止在 Service 任务、URC 回调或 ISR 中调用；不更新定位快照或 GNSS 状态缓存。
 */
Eg800AtResult_e Eg800_Locate_Gnss_Stop(SemaphoreHandle_t completion_sem) {
    if ((s_eg800_locate_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Locate_At_Execute(EG800_LOCATE_CMD_GNSS_STOP, "结束 GNSS", "AT+QGPSEND\r\n", NULL, NULL, 0U, EG800_LOCATE_GNSS_STOP_TIMEOUT_MS, NULL, completion_sem);
}




Eg800AtResult_e Eg800_Locate_WifiScan(SemaphoreHandle_t completion_sem) {
    if ((s_eg800_locate_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    return Eg800_Locate_At_Execute(EG800_LOCATE_CMD_WIFISCAN, "启动Wifi扫描", "AT+QWIFISCAN\r\n", NULL, NULL, 0U, EG800_LOCATE_WIFI_SCAN_TIMEOUT_MS, NULL, completion_sem);
}

/**
 * @brief 查询小区信息，逐行解析响应并保存有效的 LTE 主服务小区。
 * @param completion_sem 当前调用任务专用的 AT 完成信号量，同一时间仅供一条请求使用。
 * @return OK 表示 AT 执行及主服务小区解析成功；未初始化或信号量为空返回 PARAM_ERROR；响应为空、截断或缺少有效主服务小区返回 DATA_FAIL；其他值为 AT 执行错误。
 * @note +QCELLINFO 响应匹配后写入 response，不进入 URC 回调；本轮开始时清除缓存有效性，解析成功后整体更新缓存。
 * @note 仅供任务上下文调用，禁止在 Service 任务、URC 回调或 ISR 中调用。
 */
Eg800AtResult_e Eg800_Locate_Lbs(SemaphoreHandle_t completion_sem) {
    char response[1024] = {0};
    Eg800LocateLbsCell_t parsed_lbs_info = {0};
    char *line;
    char *line_end;
    Eg800AtResult_e result;

    if ((s_eg800_locate_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    result = Eg800_Locate_At_Execute(EG800_LOCATE_CMD_LBS, "查询 LBS 小区信息", "AT+QCELLINFO?\r\n", "+QCELLINFO:", response, sizeof(response), EG800_LOCATE_LBS_QUERY_TIMEOUT_MS, NULL, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // Service 缓冲区写满时可能已截断响应，不能将其作为本轮有效结果。
    if ((response[0] == '\0') || (strlen(response) >= (sizeof(response) - 1U))) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 逐行终止文本，避免解析器跨行读取；邻区和无效行继续跳过。
    line = response;
    while (*line != '\0') {
        line_end = strchr(line, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        }

        if (Eg800_Locate_Parse_Lbs(line, &parsed_lbs_info) == true) {
            s_eg800_locate_lbs_servingcell_lte = parsed_lbs_info;
            return EG800_AT_RESULT_OK;
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
    }

    return EG800_AT_RESULT_DATA_FAIL;
}





/**
 * @brief 开启 GNSS 后测试 Wi-Fi 扫描和 LBS 小区查询，并打印缓存结果。
 */
void Eg800_Locate_Test(void) {
    Eg800AtResult_e result;
    uint8_t wifi_ap_count;
    double longitude = 0.0;
    double latitude = 0.0;
    double altitude = 0.0;

    Eg800_Locate_Gnss_Set_Enabled(true, s_locate_at_completion_sem);
    vTaskDelay(pdMS_TO_TICKS(3000U));

    // LOG("开始查询 GNSS 经纬度");
    // for (int i=0;i<5;i++) {
    //     result = Eg800_Locate_Gnss_Query(&longitude, &latitude, &altitude, s_locate_at_completion_sem);
    //     if (result == EG800_AT_RESULT_OK) {
    //         LOG("GNSS 查询成功: longitude=%.6f, latitude=%.6f, altitude=%.1f m", longitude, latitude, altitude);
    //     } else {
    //         LOG_ERROR("GNSS 查询失败: result=%d", (int)result);
    //     }
    //     vTaskDelay(pdMS_TO_TICKS(60000));
    // }

    // // result = Eg800_Locate_Gnss_Set_Enabled(false, s_locate_at_completion_sem);
    
    // result = Eg800_Locate_Gnss_Stop(s_locate_at_completion_sem);
    // if(result == EG800_AT_RESULT_OK) {
    //     eg800_gnss_hardware_state = EG800_LOCATE_GNSS_STATE_OFF;
    // }
    LOG("开始查询 wifi-scan");
    result = Eg800_Locate_WifiScan(s_locate_at_completion_sem);
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    // 固定本次打印的条数，打印期间新到达的热点留待下次观察。
    wifi_ap_count = s_eg800_locate_wifi_scan_transfer_date.count;
    LOG("Wi-Fi 扫描观察: AT结果=%d, is_valid=%u, 热点数量=%u", (int)result, (unsigned int)s_eg800_locate_wifi_scan_transfer_date.is_valid, (unsigned int)wifi_ap_count);
    for (size_t i = 0; i < wifi_ap_count; i++) {
        const Eg800LocateWifiAp_t *ap = &s_eg800_locate_wifi_scan_transfer_date.aps[i];
        LOG("Wi-Fi 热点[%u]: MAC=%s, RSSI=%ld dBm, 信道=%u", (unsigned int)(i + 1U), ap->mac, (long)ap->rssi, (unsigned int)ap->channel);
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));


    result = Eg800_Locate_Lbs(s_locate_at_completion_sem);

    vTaskDelay(pdMS_TO_TICKS(1000));

    const Eg800LocateLbsCell_t *lbs_info = &s_eg800_locate_lbs_servingcell_lte;
    LOG("LBS 查询观察: AT结果=%d, is_valid=%u", (int)result, (unsigned int)lbs_info->is_valid);
    LOG("LBS 主服务小区: MCC=%lu, MNC=%02lu, TAC=0x%04lX, Cell_ID=0x%lX, RSSI=%ld dBm", (unsigned long)lbs_info->mcc, (unsigned long)lbs_info->mnc, (unsigned long)lbs_info->tac, (unsigned long)lbs_info->cell_id, (long)lbs_info->rssi);

}





























//-----------------------------------------------定位层核心维护任务-------------------------------------------------------
/**
 * @brief 保留 Locate 后台任务，等待后续加入定位维护逻辑。
 * @param parameters 未使用。
 */
static void Eg800_Locate_Task(void *parameters) {
    (void)parameters;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}
