from pathlib import Path
import json
import re

folder = Path(__file__).resolve().parent
source = (folder.parent.parent / "Treo/Service/eg_locate.c").read_text(encoding="utf-8-sig")


def extract_function(name):
    match = re.search(r"^static (?:bool|void) " + name + r"\([^;{}]*\)\s*\{", source, re.M)
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
probe = (folder.parent / "wifi-scan-parse-check/probe.c").read_text(encoding="utf-8")
prefix = probe[:probe.index("/**\n * @brief Compare")]
serving = [
    ('+QCELLINFO: "servingcell","LTE",460,00,25ef,2c92a02,232,49,,38950,-73,-68,105', 460, 0, 0x25EF, 0x2C92A02, -68),
    ('+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102,-96,91', 460, 0, 0x25EF, 0x8E9347, -96),
    ('+QCELLINFO: "servingcell","LTE",460,01,25EF,2C92A02,232,49,,38950,-73,-68,', 460, 1, 0x25EF, 0x2C92A02, -68),
    ('+QCELLINFO: "servingcell","LTE",999,999,FFFF,FFFFFFF,,,,,,-2147483648,', 999, 999, 0xFFFF, 0xFFFFFFF, -2147483648),
]
neighbours = [
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,d65684f,209,24,,1300,-99,-88,',
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,d1e6750,230,21,,3590,-102,-97,',
    '+QCELLINFO: "neighbourcell","LTE",460,01,777d,6145102,497,12,,1650,-114,-98,',
    '+QCELLINFO: "neighbourcell","LTE",460,01,777d,6ddcc01,228,13,,3740,-113,-105,',
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,a7ca203,158,5,,38950,-118,-106,',
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,a7ca204,59,6,,38950,-117,-107,',
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,8e9342,174,19,,38400,-104,-83,',
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,8e9341,175,11,,38400,-112,-83,',
    '+QCELLINFO: "neighbourcell","LTE",460,00,25ef,8e9356,174,10,,38544,-112,-95,',
]
bad_lines = [
    '',
    'OK',
    '+QCELLINFO: "servingcell","GSM",460,00,25ef,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",,00,25ef,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,,25ef,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25eg,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e934g,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",4600,00,25ef,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,125ef,8e9347,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,18e93470,174,18,,36275,-102,-96,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102,,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102,-,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102,-96bad,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102,-2147483649,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102,2147483648,91',
    '+QCELLINFO: "servingcell","LTE",460,00,25ef,8e9347,174,18,,36275,-102',
]

body = [
    '#include <stdbool.h>\n#include <string.h>\n',
    prefix,
    cell_type,
    'static Eg800LocateLbsCell_t s_eg800_locate_lbs_servingcell_lte = {0};\n',
    extract_function('Eg800_Locate_Parse_Lbs'),
    extract_function('Eg800_Locate_Lbs_Urc_Handler'),
    'int main(void) {\n    int failures = 0;\n    unsigned int tests = 0U;\n    Eg800LocateLbsCell_t info = {0};\n',
]
for index, (line, mcc, mnc, tac, cell_id, rssi) in enumerate(serving):
    body.append(f'    tests++; if (!Eg800_Locate_Parse_Lbs({json.dumps(line)}, &info) || !info.is_valid || info.mcc != {mcc}U || info.mnc != {mnc}U || info.tac != 0x{tac:X}U || info.cell_id != 0x{cell_id:X}U || info.rssi != ({rssi}LL)) {{ failures++; Write_Text("FAIL: valid cell {index + 1}\\n"); }}\n')
body.append(f'    Eg800_Locate_Lbs_Urc_Handler({json.dumps(serving[0][0])});\n')
for index, line in enumerate(neighbours):
    body.append(f'    tests++; if (Eg800_Locate_Parse_Lbs({json.dumps(line)}, &info)) {{ failures++; Write_Text("FAIL: neighbour {index + 1} accepted\\n"); }}\n')
    body.append(f'    Eg800_Locate_Lbs_Urc_Handler({json.dumps(line)});\n')
    body.append(f'    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x2C92A02U || s_eg800_locate_lbs_servingcell_lte.rssi != -68) {{ failures++; Write_Text("FAIL: neighbour {index + 1} overwrote cache\\n"); }}\n')
body.append(f'    Eg800_Locate_Lbs_Urc_Handler({json.dumps(serving[1][0])});\n')
body.append('    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x8E9347U || s_eg800_locate_lbs_servingcell_lte.rssi != -96) { failures++; Write_Text("FAIL: new serving cell did not update cache\\n"); }\n')
for index, line in enumerate(bad_lines):
    body.append(f'    tests++; if (Eg800_Locate_Parse_Lbs({json.dumps(line)}, &info)) {{ failures++; Write_Text("FAIL: invalid cell {index + 1} accepted\\n"); }}\n')
    body.append(f'    Eg800_Locate_Lbs_Urc_Handler({json.dumps(line)});\n')
body.append('    tests++; if (!s_eg800_locate_lbs_servingcell_lte.is_valid || s_eg800_locate_lbs_servingcell_lte.cell_id != 0x8E9347U || s_eg800_locate_lbs_servingcell_lte.rssi != -96) { failures++; Write_Text("FAIL: invalid input overwrote cache\\n"); }\n')
body.append('    tests++; if (Eg800_Locate_Parse_Lbs(NULL, &info) || Eg800_Locate_Parse_Lbs("OK", NULL)) { failures++; Write_Text("FAIL: null input accepted\\n"); }\n')
body.append('    char summary[96];\n    snprintf(summary, sizeof(summary), "%s: %u LBS parser/cache checks, failures=%d\\n", failures == 0 ? "PASS" : "FAIL", tests, failures);\n    Write_Text(summary);\n    _sys_exit(failures);\n}\n')
(folder / 'regression.c').write_text('\n'.join(body), encoding='utf-8')
startup = (folder.parent / 'eg-locate-query-check/startup.s').read_text(encoding='utf-8')
(folder / 'startup.s').write_text('.eabi_attribute 24, 1\n.eabi_attribute 25, 1\n' + startup, encoding='utf-8')
print('Extracted the current LBS parser and handler into regression.c.')
