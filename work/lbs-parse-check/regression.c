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
static Eg800LocateLbsCell_t s_eg800_locate_lbs_servingcell_lte = {0};

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
static void Eg800_Locate_Lbs_Urc_Handler(const char *line) {
    Eg800LocateLbsCell_t parsed_lbs_info = {0};

    if (Eg800_Locate_Parse_Lbs(line, &parsed_lbs_info) == false) {
        return;
    }

    s_eg800_locate_lbs_servingcell_lte = parsed_lbs_info;
}
int main(void) {
    int failures = 0;
    unsigned int tests = 0U;
    Eg800LocateLbsCell_t info = {0};

    tests++; if (!Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,2c92a02,232,49,,38950,-73,-68,105", &info) || !info.is_valid || info.mcc != 460U || info.mnc != 0U || info.tac != 0x25EFU || info.cell_id != 0x2C92A02U || info.rssi != (-68LL)) { failures++; Write_Text("FAIL: valid cell 1\n"); }

    tests++; if (!Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-96,91", &info) || !info.is_valid || info.mcc != 460U || info.mnc != 0U || info.tac != 0x25EFU || info.cell_id != 0x8E9347U || info.rssi != (-96LL)) { failures++; Write_Text("FAIL: valid cell 2\n"); }

    tests++; if (!Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,01,25EF,2C92A02,232,49,,38950,-73,-68,", &info) || !info.is_valid || info.mcc != 460U || info.mnc != 1U || info.tac != 0x25EFU || info.cell_id != 0x2C92A02U || info.rssi != (-68LL)) { failures++; Write_Text("FAIL: valid cell 3\n"); }

    tests++; if (!Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",999,999,FFFF,FFFFFFF,,,,,,-2147483648,", &info) || !info.is_valid || info.mcc != 999U || info.mnc != 999U || info.tac != 0xFFFFU || info.cell_id != 0xFFFFFFFU || info.rssi != (-2147483648LL)) { failures++; Write_Text("FAIL: valid cell 4\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,2c92a02,232,49,,38950,-73,-68,105");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,d65684f,209,24,,1300,-99,-88,", &info)) { failures++; Write_Text("FAIL: neighbour 1 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,d65684f,209,24,,1300,-99,-88,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 1 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,d1e6750,230,21,,3590,-102,-97,", &info)) { failures++; Write_Text("FAIL: neighbour 2 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,d1e6750,230,21,,3590,-102,-97,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 2 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,01,777d,6145102,497,12,,1650,-114,-98,", &info)) { failures++; Write_Text("FAIL: neighbour 3 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,01,777d,6145102,497,12,,1650,-114,-98,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 3 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,01,777d,6ddcc01,228,13,,3740,-113,-105,", &info)) { failures++; Write_Text("FAIL: neighbour 4 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,01,777d,6ddcc01,228,13,,3740,-113,-105,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 4 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,a7ca203,158,5,,38950,-118,-106,", &info)) { failures++; Write_Text("FAIL: neighbour 5 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,a7ca203,158,5,,38950,-118,-106,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 5 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,a7ca204,59,6,,38950,-117,-107,", &info)) { failures++; Write_Text("FAIL: neighbour 6 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,a7ca204,59,6,,38950,-117,-107,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 6 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9342,174,19,,38400,-104,-83,", &info)) { failures++; Write_Text("FAIL: neighbour 7 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9342,174,19,,38400,-104,-83,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 7 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9341,175,11,,38400,-112,-83,", &info)) { failures++; Write_Text("FAIL: neighbour 8 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9341,175,11,,38400,-112,-83,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 8 overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9356,174,10,,38544,-112,-95,", &info)) { failures++; Write_Text("FAIL: neighbour 9 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"neighbourcell\",\"LTE\",460,00,25ef,8e9356,174,10,,38544,-112,-95,");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) { failures++; Write_Text("FAIL: neighbour 9 overwrote cache\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x8E9347U || s_eg800_locate_lbs_servingcell_lte.rssi != -96) { failures++; Write_Text("FAIL: new serving cell did not update cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs("", &info)) { failures++; Write_Text("FAIL: invalid cell 1 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("");

    tests++; if (Eg800_Locate_Parse_Lbs("OK", &info)) { failures++; Write_Text("FAIL: invalid cell 2 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("OK");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"GSM\",460,00,25ef,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 3 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"GSM\",460,00,25ef,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",,00,25ef,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 4 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",,00,25ef,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,,25ef,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 5 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,,25ef,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 6 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 7 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25eg,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 8 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25eg,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e934g,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 9 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e934g,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",4600,00,25ef,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 10 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",4600,00,25ef,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,125ef,8e9347,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 11 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,125ef,8e9347,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,18e93470,174,18,,36275,-102,-96,91", &info)) { failures++; Write_Text("FAIL: invalid cell 12 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,18e93470,174,18,,36275,-102,-96,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,,91", &info)) { failures++; Write_Text("FAIL: invalid cell 13 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-,91", &info)) { failures++; Write_Text("FAIL: invalid cell 14 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-96bad,91", &info)) { failures++; Write_Text("FAIL: invalid cell 15 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-96bad,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-2147483649,91", &info)) { failures++; Write_Text("FAIL: invalid cell 16 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,-2147483649,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,2147483648,91", &info)) { failures++; Write_Text("FAIL: invalid cell 17 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102,2147483648,91");

    tests++; if (Eg800_Locate_Parse_Lbs("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102", &info)) { failures++; Write_Text("FAIL: invalid cell 18 accepted\n"); }

    Eg800_Locate_Lbs_Urc_Handler("+QCELLINFO: \"servingcell\",\"LTE\",460,00,25ef,8e9347,174,18,,36275,-102");

    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x8E9347U || s_eg800_locate_lbs_servingcell_lte.rssi != -96) { failures++; Write_Text("FAIL: invalid input overwrote cache\n"); }

    tests++; if (Eg800_Locate_Parse_Lbs(NULL, &info) || Eg800_Locate_Parse_Lbs("OK", NULL)) { failures++; Write_Text("FAIL: null input accepted\n"); }

    char summary[96];
    snprintf(summary, sizeof(summary), "%s: %u LBS parser/cache checks, failures=%d\n", failures == 0 ? "PASS" : "FAIL", tests, failures);
    Write_Text(summary);
    _sys_exit(failures);
}
