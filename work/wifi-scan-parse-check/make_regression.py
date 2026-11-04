from pathlib import Path
import json
import re

folder = Path(__file__).resolve().parent
source = (folder.parent.parent / "Treo/Service/eg_locate.c").read_text(encoding="utf-8-sig")


def extract_function(name):
    match = re.search(r"^static (?:bool|void) " + name + r"\([^;{}]*\)\s*\{", source, re.M)
    if match is None:
        raise ValueError(f"Cannot find definition of {name}")
    depth = 0
    for position in range(source.index("{", match.start()), len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():position + 1]
    raise ValueError(f"Unterminated function {name}")


types = []
for name in ("Eg800LocateWifiAp_t", "Eg800LocateWifiScan_t"):
    match = re.search(r"typedef struct\s*\{[^}]*\}\s*" + name + ";", source)
    types.append(match.group(0))

probe = (folder / "probe.c").read_text(encoding="utf-8")
prefix = probe[:probe.index("/**\n * @brief Compare")]
valid_lines = [
    ('+QWIFISCAN: (-,-,-72,"00:90:5B:C8:85:9A",6)', '00:90:5B:C8:85:9A', -72, 6),
    ('+QWIFISCAN: (-,-,-74,"80:AE:54:F8:12:9B",6)', '80:AE:54:F8:12:9B', -74, 6),
    ('+QWIFISCAN: (-,-,-76,"10:82:3D:9C:54:AE",11)', '10:82:3D:9C:54:AE', -76, 11),
    ('+QWIFISCAN: (-,-,-80,"CC:6D:55:FA:74:85",6)', 'CC:6D:55:FA:74:85', -80, 6),
    ('+QWIFISCAN: (-,-,-82,"CE:0C:6E:56:22:06",1)', 'CE:0C:6E:56:22:06', -82, 1),
]
invalid_lines = [
    '+QWIFISCAN:TIMEOUT',
    '+QWIFISCAN: TIMEOUT\r\n',
    '',
    'ERROR',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:0G",11)',
    '+QWIFISCAN: (-,-,-80,"90-6A-94-EB-27-06",11)',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:0",11)',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:066",11)',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:06",0)',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:06",14)',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:06",11',
    '+QWIFISCAN: (-,-,-80,"90:6A:94:EB:27:06",11)junk',
    '+QWIFISCAN: (-,-,-2147483649,"90:6A:94:EB:27:06",11)',
    '+QWIFISCAN: (-,-,2147483648,"90:6A:94:EB:27:06",11)',
    '+QWIFISCAN: (-,-,-999999999999999999,"90:6A:94:EB:27:06",11)',
]

body = [
    "#include <stdbool.h>\n#include <string.h>\n",
    prefix,
    "#define EG800_LOCATE_WIFI_AP_MAX 5U\n",
    *types,
    "static Eg800LocateWifiScan_t s_eg800_locate_wifi_scan_transfer_date = {0};\n",
    extract_function("Eg800_Locate_Parse_WifiScan"),
    extract_function("Eg800_Locate_WifiScan_Urc_Handler"),
    "int main(void) {\n    int failures = 0;\n    unsigned int tests = 0U;\n    Eg800LocateWifiAp_t info = {0};\n",
]
for index, (line, mac, rssi, channel) in enumerate(valid_lines):
    text = json.dumps(line)
    expected_mac = json.dumps(mac)
    body.append(f"    tests++; if (!Eg800_Locate_Parse_WifiScan({text}, &info) || strcmp(info.mac, {expected_mac}) != 0 || info.rssi != ({rssi}) || info.channel != {channel}U) {{ failures++; Write_Text(\"FAIL: logged AP {index + 1}\\n\"); }}\n")
    body.append(f"    Eg800_Locate_WifiScan_Urc_Handler({text});\n")
body.append("    tests++; if (s_eg800_locate_wifi_scan_transfer_date.count != 5U) { failures++; Write_Text(\"FAIL: cache count must be 5\\n\"); }\n")
for index, (_, mac, rssi, channel) in enumerate(valid_lines):
    body.append(f"    tests++; if (strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[{index}].mac, {json.dumps(mac)}) != 0 || s_eg800_locate_wifi_scan_transfer_date.aps[{index}].rssi != ({rssi}) || s_eg800_locate_wifi_scan_transfer_date.aps[{index}].channel != {channel}U) {{ failures++; Write_Text(\"FAIL: cached AP {index + 1}\\n\"); }}\n")
body.append('    Eg800_Locate_WifiScan_Urc_Handler("+QWIFISCAN: (-,-,-80,\\\"90:6A:94:EB:27:06\\\",11)");\n'.replace('\\\"', '\\"'))
body.append("    tests++; if (s_eg800_locate_wifi_scan_transfer_date.count != 5U || strcmp(s_eg800_locate_wifi_scan_transfer_date.aps[4].mac, \"CE:0C:6E:56:22:06\") != 0) { failures++; Write_Text(\"FAIL: cache capacity\\n\"); }\n")
body.append("    memset(&s_eg800_locate_wifi_scan_transfer_date, 0, sizeof(s_eg800_locate_wifi_scan_transfer_date));\n")
for index, line in enumerate(invalid_lines):
    body.append(f"    tests++; if (Eg800_Locate_Parse_WifiScan({json.dumps(line)}, &info)) {{ failures++; Write_Text(\"FAIL: invalid case {index + 1}\\n\"); }}\n")
    body.append(f"    Eg800_Locate_WifiScan_Urc_Handler({json.dumps(line)});\n")
body.append("    tests++; if (s_eg800_locate_wifi_scan_transfer_date.count != 0U) { failures++; Write_Text(\"FAIL: invalid URCs changed count\\n\"); }\n")
body.append('    tests++; if (Eg800_Locate_Parse_WifiScan(NULL, &info) || Eg800_Locate_Parse_WifiScan("+QWIFISCAN:TIMEOUT", NULL)) { failures++; Write_Text("FAIL: null parameters\\n"); }\n')
extra_valid = '+QWIFISCAN: (-,-,-80,"90:6a:94:eb:27:06",13) \r\n'
body.append(f"    tests++; if (!Eg800_Locate_Parse_WifiScan({json.dumps(extra_valid)}, &info) || info.channel != 13U) {{ failures++; Write_Text(\"FAIL: lowercase MAC and whitespace\\n\"); }}\n")
body.append("    char summary[96];\n    snprintf(summary, sizeof(summary), \"%s: %u actual-parser/cache checks, failures=%d\\n\", failures == 0 ? \"PASS\" : \"FAIL\", tests, failures);\n    Write_Text(summary);\n    _sys_exit(failures);\n}\n")
(folder / "regression.c").write_text("\n".join(body), encoding="utf-8")
print("Generated regression.c from the current parser and URC handler.")
