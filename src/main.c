#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "format.h"
#include "ui.h"
#include "sampler.h"
#include "platform.h"
#include "usage.h"
#include "usage_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>

#include "cli.h"
#include <signal.h>

static bool ingest_usage(const Options *options) {
    const UsageProvider *provider = usage_provider_find(options->usage_provider);
    if (!provider) return false;
    char input[65537];
    size_t used = fread(input, 1, sizeof(input) - 1, stdin);
    if (ferror(stdin) || used == sizeof(input) - 1) return false;
    input[used] = '\0';
    ProviderUsage usage;
    if (!usage_provider_parse(provider->kind, input, used, &usage)) return false;
    char path[4096];
    return usage_cache_path(provider->kind, path, sizeof(path)) && usage_cache_write(path, &usage);
}

int main(int argc, char **argv) {
    Options options;
    if (parse_args(argc, argv, &options) != 0) { print_usage(stderr); return 2; }
    if (options.usage_ingest) return ingest_usage(&options) ? 0 : 1;
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    if (options.json_stream) signal(SIGPIPE, SIG_IGN);
    if (options.startup) {
        print_startup_report(&options);
        return 0;
    }

    Snapshot previous = collect_snapshot(NULL);
    struct timespec initial_delay = { .tv_nsec = 200000000L };
    nanosleep(&initial_delay, NULL);

    bool interactive = isatty(STDOUT_FILENO) && !options.once && !options.json && !options.json_stream;
    bool color = interactive;
    if (interactive) {
        configure_terminal();
        /* Keep mouse input with the terminal so wheel scrolling does not
           wake the render loop and append another dashboard frame. */
        fputs(ANSI_ALT_SCREEN ANSI_HIDE_CURSOR, stdout);
        fflush(stdout);
    }
    View view = VIEW_DASHBOARD;
    ProcessViewState process_view = { .sort = options.sort };
    NetworkSnapshot previous_networks = {0};
    bool have_previous_networks = false;
    bool clear_screen = interactive;
    int rendered_width = 0;
    int rendered_height = 0;
    unsigned long long sequence = 0;
    while (running) {
        Snapshot current = collect_snapshot(&previous);
        double elapsed = current.timestamp - previous.timestamp;
        sort_processes(&current.processes,
                       view == VIEW_PROCESSES ? process_view.sort : options.sort);
        double cpu = sample_cpu_usage(&previous, &current);
        double core_usage[MAX_CPU_CORES] = {0};
        size_t core_count = sample_core_usage(&previous, &current, core_usage);
        NetworkSnapshot networks = {0};
        if (view == VIEW_NETWORKS || options.json_stream) {
            networks = collect_networks(have_previous_networks ? &previous_networks : NULL,
                                        elapsed);
        }
        if (options.json) print_json(&current, cpu, &options);
        else if (options.json_stream) {
            SystemMetrics metrics = collect_system_metrics();
            UsageSnapshot usage = {0};
            const UsageSnapshot *usage_pointer = NULL;
            if (options.include_usage) {
                usage = collect_usage();
                usage_pointer = &usage;
            }
            if (!print_json_stream_frame(&current, cpu, &metrics, &networks,
                                         usage_pointer, &options, sequence++)) {
                running = 0;
            }
        }
        else {
            int current_width = terminal_width();
            int current_height = terminal_height();
            TerminalLayout layout = calculate_layout(current_width, current_height, &options);
            if (interactive && (current_width != rendered_width ||
                                current_height != rendered_height)) {
                clear_screen = true;
            }
            if (interactive) {
                fputs(ANSI_SYNC_BEGIN, stdout);
                fputs(clear_screen ? ANSI_CLEAR_SCREEN : ANSI_HOME, stdout);
            }
            if (view == VIEW_USAGE) {
                print_usage_screen(clear_screen, color);
            }
            else if (view == VIEW_NETWORKS && options.compact) {
                print_compact_network_screen(&networks, clear_screen, color);
            }
            else if (view == VIEW_NETWORKS) {
                print_network_screen(&networks, &options, clear_screen, color);
            }
            else if (view == VIEW_PROCESSES) {
                print_process_screen(&current.processes, &process_view, clear_screen, color);
            }
            else if (options.compact) {
                render_compact_dashboard(&current, cpu, &options, &layout, clear_screen, color);
            }
            else {
                render_dashboard(&current, cpu, &options, core_usage, core_count, &layout,
                                 clear_screen, color);
            }
            if (interactive) {
                fputs(ANSI_SYNC_END, stdout);
                fflush(stdout);
                rendered_width = current_width;
                rendered_height = current_height;
            }
        }
        free_snapshot(&previous);
        previous = current;
        if (view == VIEW_NETWORKS || options.json_stream) {
            previous_networks = networks;
            have_previous_networks = true;
        } else {
            previous_networks = (NetworkSnapshot){0};
            have_previous_networks = false;
        }
        if (!running) break;
        if (options.once) break;
        if (interactive) {
            View old_view = view;
            if (!wait_for_input(&view, &process_view, options.interval_ms)) break;
            clear_screen = view != old_view;
        }
        else {
            struct timespec delay = { .tv_sec = options.interval_ms / 1000,
                                      .tv_nsec = (long)(options.interval_ms % 1000) * 1000000L };
            nanosleep(&delay, NULL);
        }
    }
    free_snapshot(&previous);
    if (interactive) {
        fputs(ANSI_RESET ANSI_SHOW_CURSOR ANSI_MAIN_SCREEN, stdout);
        fflush(stdout);
        restore_terminal();
    }
    return 0;
}
