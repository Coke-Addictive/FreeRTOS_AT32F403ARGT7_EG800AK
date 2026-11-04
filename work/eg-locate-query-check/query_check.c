
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef void *SemaphoreHandle_t;
typedef struct { size_t rx_len; } Eg800AtResultInfo_t;
#define EG800_LOCATE_GNSS_QUERY_MODE 2U
#define EG800_LOCATE_GNSS_QUERY_RESPONSE_SIZE 192U
static bool s_eg800_locate_initialized = true;
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
typedef enum {
    EG800_LOCATE_CMD_GNSS_OPEN,                     // GNSS 打开
    EG800_LOCATE_CMD_GNSS_CLOSE,                    // GNSS 禁用
    EG800_LOCATE_CMD_WIFISCAN,                      // wifi-scan
    EG800_LOCATE_CMD_BDS,
    EG800_LOCATE_CMD_GNSS_QUERY                    // GNSS 经纬度查询
}Eg800LocateCmd_e;
static const char *mock_response;
static Eg800AtResult_e mock_result;
static int calls;
static int failures;
static int tests;
static void output(const char *s) {
    register uint32_t r0 __asm("r0") = 4U;
    register const char *r1 __asm("r1") = s;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
}
__attribute__((noreturn)) void _sys_exit(int status) {
    uint32_t block[2] = {0x20026U, (uint32_t)status};
    register uint32_t r0 __asm("r0") = 0x20U;
    register uint32_t *r1 __asm("r1") = block;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
    while (1) {}
}
static Eg800AtResult_e Eg800_Locate_At_Execute(Eg800LocateCmd_e cmd_id, const char *name, const char *tx, const char *rsp_prefix, char *rx_buf, size_t rx_buf_size, uint32_t timeout_ms, Eg800AtResultInfo_t *result_info, SemaphoreHandle_t completion_sem) {
    size_t len = strlen(mock_response);
    calls++;
    if (cmd_id != EG800_LOCATE_CMD_GNSS_QUERY || name == NULL || strcmp(tx, "AT+QGPSLOC=2\r\n") != 0 || strcmp(rsp_prefix, "+QGPSLOC:") != 0 || timeout_ms != 0U || completion_sem == NULL) {
        failures++;
        output("FAIL: AT request contract\n");
    }
    if (len >= rx_buf_size) { len = rx_buf_size - 1U; }
    memcpy(rx_buf, mock_response, len);
    rx_buf[len] = '\0';
    result_info->rx_len = len;
    return mock_result;
}
Eg800AtResult_e Eg800_Locate_Gnss_Query(double *longitude, double *latitude, SemaphoreHandle_t completion_sem) {
    char command[32];
    char response[EG800_LOCATE_GNSS_QUERY_RESPONSE_SIZE] = {0};
    Eg800AtResultInfo_t result_info = {0};
    Eg800AtResult_e result;
    double parsed_longitude;
    double parsed_latitude;
    int command_len;
    unsigned int fix;
    int fields_end = 0;

    if ((s_eg800_locate_initialized == false) || (longitude == NULL) || (latitude == NULL) || (longitude == latitude) || (completion_sem == NULL) || (EG800_LOCATE_GNSS_QUERY_MODE != 2U)) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    command_len = snprintf(command, sizeof(command), "AT+QGPSLOC=%u\r\n", (unsigned int)EG800_LOCATE_GNSS_QUERY_MODE);
    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command))) {
        return EG800_AT_RESULT_PARAM_ERROR;
    }

    result = Eg800_Locate_At_Execute(EG800_LOCATE_CMD_GNSS_QUERY, "查询 GNSS 经纬度", command, "+QGPSLOC:", response, sizeof(response), 0U, &result_info, completion_sem);
    if (result != EG800_AT_RESULT_OK) {
        return result;
    }

    // Service 缓冲区不足时会截断响应，不能把截断文本作为有效查询结果。
    if ((result_info.rx_len == 0U) || (result_info.rx_len >= (sizeof(response) - 1U))) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 响应顺序为 UTC、纬度、经度、HDOP、海拔、fix；%n 确认 fix 后的逗号也已匹配。
    if ((sscanf(response, "+QGPSLOC: %*[^,\r\n],%lf,%lf,%*[^,\r\n],%*[^,\r\n],%1u,%n", &parsed_latitude, &parsed_longitude, &fix, &fields_end) != 3) || (fields_end == 0)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 使用正向范围判断，同时拒绝 NaN、无穷大和超出地理范围的数值。
    if (!((parsed_latitude >= -90.0) && (parsed_latitude <= 90.0) && (parsed_longitude >= -180.0) && (parsed_longitude <= 180.0))) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // fix 为 1 时尚未定位，只接受 2D 或 3D 定位结果。
    if ((fix != 2U) && (fix != 3U)) {
        return EG800_AT_RESULT_DATA_FAIL;
    }

    // 两个坐标均校验通过后再写入，失败时保留调用者原有坐标。
    *longitude = parsed_longitude;
    *latitude = parsed_latitude;
    return EG800_AT_RESULT_OK;
}

static void check(const char *name, const char *response, Eg800AtResult_e at_result, Eg800AtResult_e expected, double expected_lon, double expected_lat) {
    double lon = 12.345;
    double lat = 67.89;
    mock_response = response;
    mock_result = at_result;
    Eg800AtResult_e result = Eg800_Locate_Gnss_Query(&lon, &lat, (void *)1);
    tests++;
    bool pass = (result == expected);
    if (expected == EG800_AT_RESULT_OK) {
        double dl = lon - expected_lon;
        double da = lat - expected_lat;
        pass = pass && (dl < 0.0000001) && (dl > -0.0000001) && (da < 0.0000001) && (da > -0.0000001);
    } else {
        pass = pass && (lon == 12.345) && (lat == 67.89);
    }
    if (!pass) { failures++; output("FAIL: "); output(name); output("\n"); }
}
#define BAD(name, response) check(name, response, EG800_AT_RESULT_OK, EG800_AT_RESULT_DATA_FAIL, 0, 0)
int main(void) {
    check("northeast", "+QGPSLOC: 063416.40,31.72158,117.21776,0.6,224.9,2,162.57,17.6,9.5,110620,07\n", EG800_AT_RESULT_OK, EG800_AT_RESULT_OK, 117.21776, 31.72158);
    check("southwest", "+QGPSLOC: 063416.40,-31.72158,-117.21776,0.6,224.9,3,,0.0,0.0,110620,07\n", EG800_AT_RESULT_OK, EG800_AT_RESULT_OK, -117.21776, -31.72158);
    check("zero", "+QGPSLOC: 000000.00,0.0,0.0,0.6,0,2,,0,0,110620,07\n", EG800_AT_RESULT_OK, EG800_AT_RESULT_OK, 0, 0);
    check("bounds", "+QGPSLOC: 000000.00,-90,180,0.6,0,3,,0,0,110620,07\n", EG800_AT_RESULT_OK, EG800_AT_RESULT_OK, 180, -90);
    BAD("no response", "");
    BAD("wrong prefix", "+OTHER: 063416.40,31.7,117.2,0.6,224.9,2,1\n");
    BAD("missing UTC", "+QGPSLOC: ,31.7,117.2,0.6,224.9,2,1\n");
    BAD("missing latitude", "+QGPSLOC: 063416.40,,117.2,0.6,224.9,2,1\n");
    BAD("missing longitude", "+QGPSLOC: 063416.40,31.7,,0.6,224.9,2,1\n");
    BAD("numeric suffix", "+QGPSLOC: 063416.40,31.7,117.2xyz,0.6,224.9,2,1\n");
    BAD("latitude too large", "+QGPSLOC: 063416.40,90.00001,117.2,0.6,224.9,2,1\n");
    BAD("longitude too small", "+QGPSLOC: 063416.40,31.7,-180.00001,0.6,224.9,2,1\n");
    BAD("NaN latitude", "+QGPSLOC: 063416.40,nan,117.2,0.6,224.9,2,1\n");
    BAD("infinite longitude", "+QGPSLOC: 063416.40,31.7,inf,0.6,224.9,2,1\n");
    BAD("no fix", "+QGPSLOC: 063416.40,31.7,117.2,0.6,224.9,1,1\n");
    BAD("invalid fix", "+QGPSLOC: 063416.40,31.7,117.2,0.6,224.9,4,1\n");
    BAD("fix suffix", "+QGPSLOC: 063416.40,31.7,117.2,0.6,224.9,20,1\n");
    BAD("partial line", "+QGPSLOC: 063416.40,31.7,117.2");
    char overlong[240];
    memset(overlong, '7', sizeof(overlong) - 1);
    memcpy(overlong, "+QGPSLOC: 063416.40,31.7,117.2,0.6,224.9,2,", 41);
    overlong[sizeof(overlong) - 1] = '\0';
    BAD("buffer truncation", overlong);
    for (int status = EG800_AT_RESULT_ERROR; status <= EG800_AT_RESULT_PARAM_ERROR; status++) {
        check("AT error propagation", "", (Eg800AtResult_e)status, (Eg800AtResult_e)status, 0, 0);
    }
    double lon = 1, lat = 2;
    int before = calls;
    s_eg800_locate_initialized = false;
    tests++;
    if (Eg800_Locate_Gnss_Query(&lon, &lat, (void *)1) != EG800_AT_RESULT_PARAM_ERROR) { failures++; }
    s_eg800_locate_initialized = true;
    tests += 4;
    if (Eg800_Locate_Gnss_Query(NULL, &lat, (void *)1) != EG800_AT_RESULT_PARAM_ERROR) { failures++; }
    if (Eg800_Locate_Gnss_Query(&lon, NULL, (void *)1) != EG800_AT_RESULT_PARAM_ERROR) { failures++; }
    if (Eg800_Locate_Gnss_Query(&lon, &lat, NULL) != EG800_AT_RESULT_PARAM_ERROR) { failures++; }
    if (Eg800_Locate_Gnss_Query(&lon, &lon, (void *)1) != EG800_AT_RESULT_PARAM_ERROR) { failures++; }
    if (before != calls || lon != 1 || lat != 2) { failures++; output("FAIL: invalid args side effects\n"); }
    if (failures == 0) { output("PASS: 33 GNSS query cases, actual function with mocked AT transport\n"); }
    _sys_exit(failures != 0);
}
