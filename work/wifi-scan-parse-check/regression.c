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


#define EG800_LOCATE_WIFI_AP_MAX 5U

typedef struct {
    char mac[18];          // MAC 地址，17 个字符加结尾 '\0'
    int32_t rssi;          // 热点信号强度，单位 dBm
    uint8_t channel;       // 热点信道，范围 1～13
} Eg800LocateWifiAp_t;
typedef struct {
    bool is_valid;                                          // 有效性
    uint8_t count;                                          // 有效热点数量，范围 0～EG800_LOCATE_WIFI_AP_MAX
    Eg800LocateWifiAp_t aps[EG800_LOCATE_WIFI_AP_MAX];      // 本轮已保存的热点列表
} Eg800LocateWifiScan_t;
static Eg800LocateWifiScan_t s_eg800_locate_wifi_scan_transfer_date = {0};

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
int main(void) {
    int failures = 0;
    unsigned int tests = 0U;
    Eg800LocateWifiAp_t info = {0};

    tests++; if (!Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-72,\"00:90:5B:C8:85:9A\",6)", &info) || strcmp(info.mac, "00:90:5B:C8:85:9A") != 0 || info.rssi != (-72) || info.channel != 6U) { failures++; Write_Text("FAIL: logged AP 1\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-72,\"00:90:5B:C8:85:9A\",6)");

    tests++; if (!Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-74,\"80:AE:54:F8:12:9B\",6)", &info) || strcmp(info.mac, "80:AE:54:F8:12:9B") != 0 || info.rssi != (-74) || info.channel != 6U) { failures++; Write_Text("FAIL: logged AP 2\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-74,\"80:AE:54:F8:12:9B\",6)");

    tests++; if (!Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-76,\"10:82:3D:9C:54:AE\",11)", &info) || strcmp(info.mac, "10:82:3D:9C:54:AE") != 0 || info.rssi != (-76) || info.channel != 11U) { failures++; Write_Text("FAIL: logged AP 3\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-76,\"10:82:3D:9C:54:AE\",11)");

    tests++; if (!Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"CC:6D:55:FA:74:85\",6)", &info) || strcmp(info.mac, "CC:6D:55:FA:74:85") != 0 || info.rssi != (-80) || info.channel != 6U) { failures++; Write_Text("FAIL: logged AP 4\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"CC:6D:55:FA:74:85\",6)");

    tests++; if (!Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-82,\"CE:0C:6E:56:22:06\",1)", &info) || strcmp(info.mac, "CE:0C:6E:56:22:06") != 0 || info.rssi != (-82) || info.channel != 1U) { failures++; Write_Text("FAIL: logged AP 5\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-82,\"CE:0C:6E:56:22:06\",1)");

    tests++; if (s_eg800_locate_wifi_scan_transfer_date.count != 5U) { failures++; Write_Text("FAIL: cache count must be 5\n"); }

    tests++; if (strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[0].mac, "00:90:5B:C8:85:9A") != 0 || s_eg800_locate_wifi_scan_transfer_date.aps[0].rssi != (-72) || s_eg800_locate_wifi_scan_transfer_date.aps[0].channel != 6U) { failures++; Write_Text("FAIL: cached AP 1\n"); }

    tests++; if (strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[1].mac, "80:AE:54:F8:12:9B") != 0 || s_eg800_locate_wifi_scan_transfer_date.aps[1].rssi != (-74) || s_eg800_locate_wifi_scan_transfer_date.aps[1].channel != 6U) { failures++; Write_Text("FAIL: cached AP 2\n"); }

    tests++; if (strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[2].mac, "10:82:3D:9C:54:AE") != 0 || s_eg800_locate_wifi_scan_transfer_date.aps[2].rssi != (-76) || s_eg800_locate_wifi_scan_transfer_date.aps[2].channel != 11U) { failures++; Write_Text("FAIL: cached AP 3\n"); }

    tests++; if (strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[3].mac, "CC:6D:55:FA:74:85") != 0 || s_eg800_locate_wifi_scan_transfer_date.aps[3].rssi != (-80) || s_eg800_locate_wifi_scan_transfer_date.aps[3].channel != 6U) { failures++; Write_Text("FAIL: cached AP 4\n"); }

    tests++; if (strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[4].mac, "CE:0C:6E:56:22:06") != 0 || s_eg800_locate_wifi_scan_transfer_date.aps[4].rssi != (-82) || s_eg800_locate_wifi_scan_transfer_date.aps[4].channel != 1U) { failures++; Write_Text("FAIL: cached AP 5\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",11)");

    tests++; if (s_eg800_locate_wifi_scan_transfer_date.count != 5U || strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[4].mac, "CE:0C:6E:56:22:06") != 0) { failures++; Write_Text("FAIL: cache capacity\n"); }

    memset(&s_eg800_locate_wifi_scan_transfer_date, 0, sizeof(s_eg800_locate_wifi_scan_transfer_date));

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN:TIMEOUT", &info)) { failures++; Write_Text("FAIL: invalid case 1\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN:TIMEOUT");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: TIMEOUT\r\n", &info)) { failures++; Write_Text("FAIL: invalid case 2\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: TIMEOUT\r\n");

    tests++; if (Eg800_Locate_Parse_WifiScan("", &info)) { failures++; Write_Text("FAIL: invalid case 3\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("");

    tests++; if (Eg800_Locate_Parse_WifiScan("ERROR", &info)) { failures++; Write_Text("FAIL: invalid case 4\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("ERROR");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:0G\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 5\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:0G\",11)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90-6A-94-EB-27-06\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 6\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90-6A-94-EB-27-06\",11)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:0\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 7\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:0\",11)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:066\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 8\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:066\",11)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",0)", &info)) { failures++; Write_Text("FAIL: invalid case 9\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",0)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",14)", &info)) { failures++; Write_Text("FAIL: invalid case 10\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",14)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",11", &info)) { failures++; Write_Text("FAIL: invalid case 11\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",11");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",11)junk", &info)) { failures++; Write_Text("FAIL: invalid case 12\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\"90:6A:94:EB:27:06\",11)junk");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-2147483649,\"90:6A:94:EB:27:06\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 13\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-2147483649,\"90:6A:94:EB:27:06\",11)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,2147483648,\"90:6A:94:EB:27:06\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 14\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,2147483648,\"90:6A:94:EB:27:06\",11)");

    tests++; if (Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-999999999999999999,\"90:6A:94:EB:27:06\",11)", &info)) { failures++; Write_Text("FAIL: invalid case 15\n"); }

    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-999999999999999999,\"90:6A:94:EB:27:06\",11)");

    tests++; if (s_eg800_locate_wifi_scan_transfer_date.count != 0U) { failures++; Write_Text("FAIL: invalid URCs changed count\n"); }

    tests++; if (Eg800_Locate_Parse_WifiScan(NULL, &info) || Eg800_Locate_Parse_WifiScan("+QWIFISCAN:TIMEOUT", NULL)) { failures++; Write_Text("FAIL: null parameters\n"); }

    tests++; if (!Eg800_Locate_Parse_WifiScan("+QWIFISCAN: (-,-,-80,\"90:6a:94:eb:27:06\",13) \r\n", &info) || info.channel != 13U) { failures++; Write_Text("FAIL: lowercase MAC and whitespace\n"); }

    char summary[96];
    snprintf(summary, sizeof(summary), "%s: %u actual-parser/cache checks, failures=%d\n", failures == 0 ? "PASS" : "FAIL", tests, failures);
    Write_Text(summary);
    _sys_exit(failures);
}
