#include <stdbool.h>
#include <string.h>

#include <stdio.h>
#include <stdint.h>

/**
 * @brief Write diagnostic text through simulator semihosting.
 * @param text Null-terminated diagnostic text.
 */
static void Write_Text(const char *text) {
    register uint32_t r0 __asm("r0") = 4U;
    register const char *r1 __asm("r1") = text;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
}

/**
 * @brief Terminate the simulator with a test status.
 * @param status Zero for success, nonzero for failure.
 */
__attribute__((noreturn)) void _sys_exit(int status) {
    uint32_t block[2] = {0x20026U, (uint32_t)status};
    register uint32_t r0 __asm("r0") = 0x20U;
    register uint32_t *r1 __asm("r1") = block;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
    while (1) {
    }
}


typedef struct {
    bool is_valid;              // 有效性
    uint32_t mcc;               // 移动国家码
    uint32_t mnc;               // 移动网络码
    uint32_t tac;               // 跟踪区码，模块以十六进制文本上报
    uint32_t cell_id;           // 小区标识，模块以十六进制文本上报
    int32_t rssi;               // 接收信号强度，单位dBm
} Eg800LocateLbsCell_t;
typedef enum {
    EG800_LOCATE_CMD_GNSS_OPEN,                     // GNSS 打开
    EG800_LOCATE_CMD_GNSS_CLOSE,                    // GNSS 禁用
    EG800_LOCATE_CMD_WIFISCAN,                      // wifi-scan
    EG800_LOCATE_CMD_BDS,
    EG800_LOCATE_CMD_GNSS_QUERY,                    // GNSS 经纬度查询
    EG800_LOCATE_CMD_GNSS_STOP,                     // GNSS 结束定位
    EG800_LOCATE_CMD_LBS                            // 服务小区和邻区信息查询
}Eg800LocateCmd_e;
typedef enum {
    EG800_AT_RESULT_OK = 0U,                    // 命令成功
    EG800_AT_RESULT_ERROR,                      // 模块返回 ERROR
    EG800_AT_RESULT_CME_ERROR,                  // 模块返回 +CME ERROR
    EG800_AT_RESULT_CMS_ERROR,                  // 模块返回 +CMS ERROR
    EG800_AT_RESULT_TIMEOUT,                    // 等待响应、URC 或原始数据超时
    EG800_AT_RESULT_SEND_FAIL,                  // 底层发送失败
    EG800_AT_RESULT_DATA_FAIL,                  // 原始数据处理失败
    EG800_AT_RESULT_CANCELLED,                  // 会话切换或关机取消（命令被取消）
    EG800_AT_RESULT_QUEUE_FULL,                 // AT 请求队列已满，没发送出去
    EG800_AT_RESULT_PARAM_ERROR                 // 参数错误或服务未初始化
} Eg800AtResult_e;
#define EG800_LOCATE_LBS_QUERY_TIMEOUT_MS 10000U


typedef void *SemaphoreHandle_t;
typedef struct {
    int unused;    // Detailed AT results are not requested by this query.
} Eg800AtResultInfo_t;

static bool s_eg800_locate_initialized = true;                     // Query initialization guard.
static Eg800LocateLbsCell_t s_eg800_locate_lbs_servingcell_lte;      // Shared parsed serving cell.
static Eg800AtResult_e mock_result;                               // Transport outcome for one test.
static const char *mock_line_1;                                   // First simulated response line.
static const char *mock_line_2;                                   // Optional following neighbour line.
static unsigned int mock_calls = 0U;                              // Number of submitted requests.
static unsigned int tests = 0U;                                   // Executed behavioral cases.
static int failures = 0;                                          // Failed behavioral checks.

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

/**
 * @brief Fill the response buffer before returning the simulated AT outcome.
 * @param cmd_id Requested command identifier.
 * @param name Diagnostic command name.
 * @param tx Complete AT command text.
 * @param rsp_prefix Response filter for QCELLINFO lines.
 * @param rx_buf Caller-owned response buffer.
 * @param rx_buf_size Response buffer capacity in bytes.
 * @param timeout_ms Expected LBS software timeout in milliseconds.
 * @param result_info Optional details, expected to be NULL.
 * @param completion_sem Non-NULL caller completion semaphore.
 * @return Configured mock transport outcome.
 */
static Eg800AtResult_e Eg800_Locate_At_Execute(Eg800LocateCmd_e cmd_id, const char *name, const char *tx, const char *rsp_prefix, char *rx_buf, size_t rx_buf_size, uint32_t timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    size_t used = 0U;
    mock_calls++;
    if ((cmd_id != EG800_LOCATE_CMD_LBS) || (name == NULL) || (strcmp(tx, "AT+QCELLINFO?\r\n") != 0) || (rsp_prefix == NULL) || (strcmp(rsp_prefix, "+QCELLINFO:") != 0) || (rx_buf == NULL) || (rx_buf_size == 0U) || (timeout_ms != EG800_LOCATE_LBS_QUERY_TIMEOUT_MS) || (result_info != NULL) || (completion_sem == NULL) || s_eg800_locate_lbs_servingcell_lte.is_valid) {
        failures++;
        Write_Text("FAIL: AT contract or old validity was not reset\n");
        return EG800_AT_RESULT_PARAM_ERROR;
    }
    rx_buf[0] = '\0';
    if (mock_line_1 != NULL) {
        used = (size_t)snprintf(rx_buf, rx_buf_size, "%s\n", mock_line_1);
    }
    if ((mock_line_2 != NULL) && (used < (rx_buf_size - 1U))) {
        (void)snprintf(&rx_buf[used], rx_buf_size - used, "%s\n", mock_line_2);
    }
    return mock_result;
}

Eg800AtResult_e Eg800_Locate_Lbs(SemaphoreHandle_t completion_sem) {
    char response[1024] = {0};
    Eg800LocateLbsCell_t parsed_lbs_info = {0};
    char *line;
    char *line_end;
    Eg800AtResult_e result;

    if ((s_eg800_locate_initialized == false) || (completion_sem == NULL)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    s_eg800_locate_lbs_servingcell_lte.is_valid = false;
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
 * @brief Check query result and cache validity for one response sequence.
 * @param name Test description for failure reporting.
 * @param at_result Transport outcome.
 * @param line_1 First response line or NULL.
 * @param line_2 Second response line or NULL.
 * @param expected Expected final query outcome.
 */
static void Check_Query(const char *name, Eg800AtResult_e at_result, const char *line_1, const char *line_2, Eg800AtResult_e expected) {
    s_eg800_locate_lbs_servingcell_lte = (Eg800LocateLbsCell_t){.is_valid = true, .mcc = 111U, .mnc = 2U, .tac = 222U, .cell_id = 333U, .rssi = -99};
    mock_result = at_result;
    mock_line_1 = line_1;
    mock_line_2 = line_2;
    tests++;
    Eg800AtResult_e result = Eg800_Locate_Lbs((void *)1);
    bool passed = result == expected;
    if (expected == EG800_AT_RESULT_OK) {
        passed = passed && s_eg800_locate_lbs_servingcell_lte.is_valid && s_eg800_locate_lbs_servingcell_lte.mcc == 460U && s_eg800_locate_lbs_servingcell_lte.mnc == 0U && s_eg800_locate_lbs_servingcell_lte.tac == 0x25EFU && s_eg800_locate_lbs_servingcell_lte.cell_id == 0x8E9347U && s_eg800_locate_lbs_servingcell_lte.rssi == -96;
    } else {
        passed = passed && !s_eg800_locate_lbs_servingcell_lte.is_valid && s_eg800_locate_lbs_servingcell_lte.mcc == 111U && s_eg800_locate_lbs_servingcell_lte.cell_id == 333U;
    }
    if (!passed) {
        failures++;
        Write_Text("FAIL: ");
        Write_Text(name);
        Write_Text("\n");
    }
}

/**
 * @brief Check AT errors, missing data, old-cache invalidation, and successful parsing.
 * @return Simulator status is reported by _sys_exit().
 */
int main(void) {
    const char *serving = "+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-96,91";
    const char *neighbour = "+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,a7ca203,158,5,,38950,-118,-106,";
    const char *malformed = "+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,,91";
    Check_Query("main serving cell", EG800_AT_RESULT_OK, serving, NULL, EG800_AT_RESULT_OK);
    Check_Query("neighbour must not overwrite main cell", EG800_AT_RESULT_OK, serving, neighbour, EG800_AT_RESULT_OK);
    Check_Query("only neighbours", EG800_AT_RESULT_OK, neighbour, NULL, EG800_AT_RESULT_DATA_FAIL);
    Check_Query("malformed serving cell", EG800_AT_RESULT_OK, malformed, neighbour, EG800_AT_RESULT_DATA_FAIL);
    Check_Query("empty response must not reuse old cache", EG800_AT_RESULT_OK, NULL, NULL, EG800_AT_RESULT_DATA_FAIL);
    Check_Query("serving cell after neighbour", EG800_AT_RESULT_OK, neighbour, serving, EG800_AT_RESULT_OK);
    Check_Query("blank lines before serving cell", EG800_AT_RESULT_OK, "\n\r\n", serving, EG800_AT_RESULT_OK);
    Check_Query("malformed line before valid serving cell", EG800_AT_RESULT_OK, malformed, serving, EG800_AT_RESULT_OK);
    Check_Query("only blank lines", EG800_AT_RESULT_OK, "\n\r\n", NULL, EG800_AT_RESULT_DATA_FAIL);
    Check_Query("missing RSSI cannot cross into next line", EG800_AT_RESULT_OK, "+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,", "-96,91", EG800_AT_RESULT_DATA_FAIL);
    char crlf[256];
    snprintf(crlf, sizeof(crlf), "%s\r\n%s\r\n", neighbour, serving);
    Check_Query("CRLF response", EG800_AT_RESULT_OK, crlf, NULL, EG800_AT_RESULT_OK);
    char oversized[1200];
    memset(oversized, 'X', sizeof(oversized) - 1U);
    memcpy(oversized, serving, strlen(serving));
    oversized[strlen(serving)] = '\n';
    oversized[sizeof(oversized) - 1U] = '\0';
    Check_Query("full response buffer rejected even with serving cell", EG800_AT_RESULT_OK, oversized, NULL, EG800_AT_RESULT_DATA_FAIL);
    const char *user_response =
        "+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9342,174,24,,38400,-100,-88,63\n"
        "+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,a7ca203,158,4,,38950,-119,-107,\n"
        "+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9347,174,20,,36275,-103,-99,\n"
        "+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9356,174,22,,38544,-100,-96,\n"
        "+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,d1e0250,22,6,,3590,-117,-97,\n"
        "+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9341,175,18,,38400,-104,-65,\n";
    mock_line_1 = user_response;
    mock_line_2 = NULL;
    mock_result = EG800_AT_RESULT_OK;
    tests++;
    if ((Eg800_Locate_Lbs((void *)1) != EG800_AT_RESULT_OK) || !s_eg800_locate_lbs_servingcell_lte.is_valid || (s_eg800_locate_lbs_servingcell_lte.mcc != 460U) || (s_eg800_locate_lbs_servingcell_lte.mnc != 0U) || (s_eg800_locate_lbs_servingcell_lte.tac != 0x25EFU) || (s_eg800_locate_lbs_servingcell_lte.cell_id != 0x8E9342U) || (s_eg800_locate_lbs_servingcell_lte.rssi != -88)) {
        failures++;
        Write_Text("FAIL: supplied six-line hardware response\n");
    }
    for (int status = EG800_AT_RESULT_ERROR; status <= EG800_AT_RESULT_PARAM_ERROR; status++) {
        Check_Query("AT error propagation with no data", (Eg800AtResult_e)status, NULL, NULL, (Eg800AtResult_e)status);
        Check_Query("AT error after parsed data invalidates cache", (Eg800AtResult_e)status, serving, NULL, (Eg800AtResult_e)status);
    }
    unsigned int calls_before = mock_calls;
    s_eg800_locate_lbs_servingcell_lte.is_valid = true;
    s_eg800_locate_initialized = false;
    tests++;
    if (Eg800_Locate_Lbs((void *)1) != EG800_AT_RESULT_PARAM_ERROR) {
        failures++;
    }
    s_eg800_locate_initialized = true;
    tests++;
    if (Eg800_Locate_Lbs(NULL) != EG800_AT_RESULT_PARAM_ERROR) {
        failures++;
    }
    tests++;
    if (mock_calls != calls_before || !s_eg800_locate_lbs_servingcell_lte.is_valid) {
        failures++;
        Write_Text("FAIL: invalid parameters submitted AT or changed cache\n");
    }
    char summary[96];
    snprintf(summary, sizeof(summary), "%s: %u LBS query checks, failures=%d\n", failures == 0 ? "PASS" : "FAIL", tests, failures);
    Write_Text(summary);
    _sys_exit(failures);
}
