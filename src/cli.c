#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "format.h"
#include "ui.h"
#include "sampler.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>

#include "cli.h"
#include "version.h"
#include "usage_provider.h"
#include <errno.h>

void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage: loutre-view [startup] [options]\n\n"
        "Native macOS and Linux resource and process monitor.\n\n"
        "Options:\n"
        "  -i, --interval MS    Refresh interval (minimum %d; default 1000)\n"
        "  -n, --limit COUNT    Number of processes to display (default %d)\n"
        "  -s, --sort FIELD     Sort by cpu, mem, pid, or name (default cpu)\n"
        "      --compact        Use a dense live dashboard\n"
        "      --once           Print one report and exit\n"
        "      --json           Emit one JSON report and exit\n"
        "      --no-color       Disable terminal color\n"
        "\nCommands:\n"
        "  startup              Inspect startup services and login items\n"
        "  usage ingest NAME    Cache provider usage JSON from stdin\n"
        "  -h, --help           Show this help\n"
        "  -v, --version        Show version\n", MIN_INTERVAL_MS, DEFAULT_LIMIT);
}

static bool parse_positive(const char *value, int minimum, int *out) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < minimum || parsed > 3600000) return false;
    *out = (int)parsed;
    return true;
}

int parse_args(int argc, char **argv, Options *options) {
    *options = (Options){ .interval_ms = 1000, .limit = DEFAULT_LIMIT, .sort = SORT_CPU };
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "startup")) { options->startup = true; continue; }
        if (!strcmp(arg, "usage")) {
            if (++i >= argc || strcmp(argv[i], "ingest") != 0 || ++i >= argc) return -1;
            const UsageProvider *provider = usage_provider_find(argv[i]);
            if (!provider) return -1;
            options->usage_ingest = true;
            snprintf(options->usage_provider, sizeof(options->usage_provider), "%s", provider->name);
            continue;
        }
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { print_usage(stdout); exit(0); }
        if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) { puts("loutre-view " VERSION); exit(0); }
        if (!strcmp(arg, "--once")) { options->once = true; continue; }
        if (!strcmp(arg, "--json")) { options->json = true; options->once = true; continue; }
        if (!strcmp(arg, "--compact")) { options->compact = true; continue; }
        if (!strcmp(arg, "--no-color")) { options->no_color = true; continue; }
        if (!strcmp(arg, "-i") || !strcmp(arg, "--interval")) {
            if (++i >= argc || !parse_positive(argv[i], MIN_INTERVAL_MS, &options->interval_ms)) return -1;
            continue;
        }
        if (!strcmp(arg, "-n") || !strcmp(arg, "--limit")) {
            if (++i >= argc || !parse_positive(argv[i], 1, &options->limit)) return -1;
            continue;
        }
        if (!strcmp(arg, "-s") || !strcmp(arg, "--sort")) {
            if (++i >= argc) return -1;
            if (!strcmp(argv[i], "cpu")) options->sort = SORT_CPU;
            else if (!strcmp(argv[i], "mem")) options->sort = SORT_MEM;
            else if (!strcmp(argv[i], "pid")) options->sort = SORT_PID;
            else if (!strcmp(argv[i], "name")) options->sort = SORT_NAME;
            else return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
