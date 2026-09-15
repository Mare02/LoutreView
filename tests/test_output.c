#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include <assert.h>
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
    puts("JSON escaping tests passed");
    return 0;
}
