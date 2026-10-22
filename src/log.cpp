// Writes TrueDrawFPS.log next to the plugin when Log=1 is set. A new log is started on each launch.
#include "tdfps.h"

char logPath[MAX_PATH];
bool logOn = true; // set from the Log setting (default 0); the self-tests don't load the ini, so it stays on there

void logf(const char* fmt, ...) {
    if (!logOn) return;
    FILE* f = fopen(logPath, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputs("\n", f); fclose(f);
}
