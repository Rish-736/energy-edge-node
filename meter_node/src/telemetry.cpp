#include "telemetry.h"
#include <stdio.h>

void telemetry_truth_str(uint8_t mask, char *buf, unsigned len)
{
    if (!len) return;

    static const struct { uint8_t bit; const char *name; } k[] = {
        { LOAD_LIGHTS, "lights" },
        { LOAD_HEATER, "heater" },
        { LOAD_MOTOR,  "motor"  },
    };

    unsigned n = 0;
    for (unsigned j = 0; j < sizeof(k) / sizeof(k[0]); j++) {
        if (mask & k[j].bit) {
            int w = snprintf(buf + n, len - n, "%s%s", n ? "+" : "", k[j].name);
            if (w < 0 || (unsigned)w >= len - n) break;
            n += (unsigned)w;
        }
    }
    if (!n) snprintf(buf, len, "off");
}
