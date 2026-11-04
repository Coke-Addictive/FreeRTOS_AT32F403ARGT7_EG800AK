from pathlib import Path
import re

folder = Path(__file__).resolve().parent
root = folder.parent.parent
source = (root / "Treo/Service/eg_locate.c").read_text(encoding="utf-8-sig")
service_header = (root / "Treo/Service/eg_service.h").read_text(encoding="utf-8-sig")


def extract_function(name):
    match = re.search(r"^(?:static )?(?:bool|void|Eg800AtResult_e) " + name + r"\([^;{}]*\)\s*\{", source, re.M)
    if match is None:
        raise ValueError(f"Cannot find {name}")
    depth = 0
    for position in range(source.index("{", match.start()), len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():position + 1]
    raise ValueError(f"Unterminated function {name}")


cell_type = re.search(r"typedef struct\s*\{[^}]*\}\s*Eg800LocateLbsCell_t;", source).group(0)
command_type = re.search(r"typedef enum\s*\{[^}]*\}\s*Eg800LocateCmd_e;", source).group(0)
result_type = re.search(r"typedef enum\s*\{[^}]*\}\s*Eg800AtResult_e;", service_header).group(0)
timeout = re.search(r"^#define EG800_LOCATE_LBS_QUERY_TIMEOUT_MS\s+(\d+U)", source, re.M).group(1)
probe = (folder.parent / "wifi-scan-parse-check/probe.c").read_text(encoding="utf-8")
prefix = probe[:probe.index("/**\n * @brief Compare")]
fixture = r'''
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
'''
transport = r'''
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
'''
test_cases = r'''
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
'''
output = [
    '#include <stdbool.h>\n#include <string.h>\n', prefix,
    cell_type, command_type, result_type,
    f'#define EG800_LOCATE_LBS_QUERY_TIMEOUT_MS {timeout}\n', fixture,
    extract_function('Eg800_Locate_Parse_Lbs'),
    transport,
    extract_function('Eg800_Locate_Lbs'),
    test_cases,
]
(folder / 'query_regression.c').write_text('\n'.join(output), encoding='utf-8')
print('Extracted current LBS query, parser and handler into query_regression.c.')
