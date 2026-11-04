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

/**
 * @brief Compare range and explicit-character scansets using Arm MicroLIB.
 * @return Zero when all explicit-character scansets parse the supplied lines.
 */
int main(void) {
    const char *lines[] = {
        "+QWIFISCAN: (-,-,-72,\"00:90:5B:C8:85:9A\",6)",
        "+QWIFISCAN: (-,-,-74,\"80:AE:54:F8:12:9B\",6)",
        "+QWIFISCAN: (-,-,-76,\"10:82:3D:9C:54:AE\",11)",
        "+QWIFISCAN: (-,-,-80,\"CC:6D:55:FA:74:85\",6)",
        "+QWIFISCAN: (-,-,-82,\"CE:0C:6E:56:22:06\",1)"
    };
    const char *formats[] = {
        "+QWIFISCAN: (-,-,%11lld,\"%17[0-9A-Fa-f:]\",%2u)%n",
        "+QWIFISCAN: (-,-,%11lld,\"%17[0123456789ABCDEFabcdef:]\",%2u)%n"
    };
    int failures = 0;

    for (unsigned int variant = 0U; variant < 2U; variant++) {
        for (unsigned int index = 0U; index < 5U; index++) {
            long long rssi = 0;
            unsigned int channel = 0U;
            char mac[18] = {0};
            char diagnostic[160];
            int fields_end = 0;
            int fields = sscanf(lines[index], formats[variant], &rssi, mac, &channel, &fields_end);
            snprintf(diagnostic, sizeof(diagnostic), "variant=%u line=%u fields=%d end=%d rssi=%ld mac=%s channel=%u\n", variant, index + 1U, fields, fields_end, (long)rssi, mac, channel);
            Write_Text(diagnostic);
            if ((variant == 1U) && ((fields != 3) || (fields_end == 0))) {
                failures++;
            }
        }
    }

    _sys_exit(failures);
}
