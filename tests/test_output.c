#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    FILE *capture = tmpfile(); assert(capture);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO); assert(saved >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    json_string("quote\" slash\\ newline\n tab\t escape\033");
    fflush(stdout);
    assert(dup2(saved, STDOUT_FILENO) >= 0); close(saved);
    rewind(capture);
    char output[256];
    size_t n = fread(output, 1, sizeof(output) - 1, capture); output[n] = '\0';
    fclose(capture);
    assert(output[0] == '"' && output[n - 1] == '"');
    assert(strstr(output, "\\\"") && strstr(output, "\\\\"));
    assert(!strchr(output, '\n') && !strchr(output, '\t') && !strchr(output, '\033'));
    assert(strstr(output, "\\n") || strstr(output, "\\u000a"));
    assert(strstr(output, "\\t") || strstr(output, "\\u0009"));
    assert(strstr(output, "\\u001b"));

    FILE *stream_capture = tmpfile(); assert(stream_capture);
    fflush(stdout);
    saved = dup(STDOUT_FILENO); assert(saved >= 0);
    assert(dup2(fileno(stream_capture), STDOUT_FILENO) >= 0);
    Snapshot snapshot = {
        .timestamp = 42.5,
        .cpu_status = METRIC_UNAVAILABLE,
        .processes = { .status = METRIC_PERMISSION }
    };
    SystemMetrics metrics = {
        .memory_status = METRIC_PERMISSION,
        .disk_status = METRIC_ERROR,
        .load_status = METRIC_UNAVAILABLE,
        .uptime = -1,
        .battery = { .time_remaining_minutes = -1 }
    };
    NetworkSnapshot networks = { .status = METRIC_UNAVAILABLE };
    UsageSnapshot usage = {0};
    usage_init(&usage.providers[0], "Claude Code");
    usage.providers[0].available = true;
    usage.providers[0].collected_at = 1700000000;
    usage_window_init(&usage.providers[0].windows[0], "5-hour");
    usage.providers[0].windows[0].used_percent = 23.5;
    usage.providers[0].windows[0].has_percent = true;
    usage.providers[0].windows[0].remaining_percent = 76.5;
    usage.providers[0].windows[0].resets_at = 1700003600;
    usage.providers[0].windows[0].has_reset = true;
    usage.providers[0].window_count = 1;
    usage.count = 1;
    Options options = { .interval_ms = 250, .limit = 3, .include_usage = true };
    assert(print_json_stream_frame(&snapshot, NAN, &metrics, &networks,
                                   &usage, &options, 7));
    assert(dup2(saved, STDOUT_FILENO) >= 0); close(saved);
    rewind(stream_capture);
    char frame[2048];
    n = fread(frame, 1, sizeof(frame) - 1, stream_capture);
    frame[n] = '\0';
    fclose(stream_capture);
    assert(strstr(frame, "\"schema_version\":1"));
    assert(strstr(frame, "\"sequence\":7"));
    assert(strstr(frame, "\"sample_time_monotonic\":42.500000"));
    assert(strstr(frame, "\"status\":{\"cpu\":\"unavailable\",\"processes\":\"permission_denied\""));
    assert(strstr(frame, "\"memory\":\"permission_denied\""));
    assert(strstr(frame, "\"disk\":\"error\""));
    assert(strstr(frame, "\"battery\":\"unavailable\",\"network\":\"unavailable\",\"usage\":\"available\""));
    assert(strstr(frame, "\"cpu\":{\"usage_percent\":null,\"load\":null}"));
    assert(strstr(frame, "\"memory\":{\"total_bytes\":null"));
    assert(strstr(frame, "\"processes\":null"));
    assert(strstr(frame, "\"network\":{\"status\":\"unavailable\""));
    assert(strstr(frame, "\"usage\":{\"status\":\"available\",\"providers\":["));
    assert(strstr(frame, "\"provider\":\"Claude Code\""));
    assert(strstr(frame, "\"used_percent\":23.50"));
    assert(strstr(frame, "\"remaining_percent\":76.50"));
    assert(strstr(frame, "\"resets_at\":1700003600"));
    assert(n > 0 && frame[n - 1] == '\n');
    puts("JSON escaping tests passed");
    return 0;
}
