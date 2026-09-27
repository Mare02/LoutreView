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

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>

volatile sig_atomic_t running = 1;
static struct termios original_terminal;
static bool terminal_configured = false;

int terminal_width(void);
int terminal_height(void);

void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

void restore_terminal(void) {
    if (terminal_configured) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_terminal);
        terminal_configured = false;
    }
}

bool configure_terminal(void) {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &original_terminal) != 0) return false;

    struct termios terminal = original_terminal;
    terminal.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    terminal.c_cc[VMIN] = 0;
    terminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal) != 0) return false;
    terminal_configured = true;
    return true;
}

static const char *VIEW_NAVIGATION =
    "1: Dashboard · 2: Processes · 3: Networks · 4: AI usage · 5: Disks";
enum { VIEW_NAVIGATION_COLUMNS = 66 };

static int view_navigation_rows(int width) {
    return width < 28 ? 5 : width < 40 ? 3 : width < 67 ? 2 : 1;
}

static bool view_header_fits_inline(const char *view_name, int width) {
    size_t title_width = strlen("LOUTREVIEW  /  ") + strlen(view_name);
    size_t menu_width = VIEW_NAVIGATION_COLUMNS;
    return title_width + 3 + menu_width <= (size_t)(width > 0 ? width - 1 : 0);
}

static void print_view_navigation(int width, bool color, bool inline_header) {
    if (inline_header || width >= 67) {
        fputs(VIEW_NAVIGATION, stdout);
        if (!inline_header) putchar('\n');
    } else if (width < 28) {
        fputs("1: Dashboard\n2: Processes\n3: Networks\n4: AI usage\n5: Disks\n", stdout);
    } else if (width < 40) {
        fputs("1: Dashboard\n2: Processes · 3: Networks\n4: AI usage · 5: Disks\n", stdout);
    } else {
        fputs("1: Dashboard · 2: Processes\n", stdout);
        fputs("3: Networks · 4: AI usage · 5: Disks\n", stdout);
    }
    if (color) fputs(ANSI_RESET, stdout);
}

int view_header_rows(const char *view_name, int width, bool compact) {
    int title_rows = compact ? 1 : 2;
    if (view_header_fits_inline(view_name, width)) return compact ? 1 : 2;
    return title_rows + view_navigation_rows(width);
}

/* Keep the full menu beside the title only when it cannot trigger auto-wrap. */
static void print_view_heading(const char *view_name, int width, bool color, bool compact) {
    bool inline_header = view_header_fits_inline(view_name, width);
    if (color) fputs(ANSI_BRAND_BRIGHT ANSI_BOLD, stdout);
    fputs("LOUTREVIEW", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    fputs("  /  ", stdout);
    if (color) fputs(ANSI_BRAND_BRIGHT ANSI_BOLD, stdout);
    fputs(view_name, stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    if (inline_header) {
        fputs("   ", stdout);
        print_view_navigation(width, color, true);
        putchar('\n');
    } else {
        putchar('\n');
        if (compact) fputs(ANSI_ERASE_LINE, stdout);
        print_view_navigation(width, color, false);
    }
}

void print_view_header(const char *view_name, int width, bool color) {
    print_view_heading(view_name, width, color, false);
    if (color) fputs(ANSI_SLATE, stdout);
    for (int i = 0; i < width - 1; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');
}

void print_compact_header(const char *view_name, int width, bool color) {
    print_view_heading(view_name, width, color, true);
}

static bool handle_input(View *current, ProcessViewState *process_view) {
    unsigned char input[32];
    ssize_t received = read(STDIN_FILENO, input, sizeof(input));
    if (received <= 0) return false;
    static int escape_state = 0;
    bool changed = false;
    for (ssize_t i = 0; i < received; i++) {
        if (escape_state == 1) {
            if (input[i] == '[') {
                escape_state = 2;
                continue;
            }
            escape_state = 0;
        }
        if (escape_state == 2) {
            if (input[i] >= 0x40 && input[i] <= 0x7e) escape_state = 0;
            continue;
        }
        if (input[i] == '\033') {
            if (*current == VIEW_PROCESSES && process_view->filtering) {
                process_view->filtering = false;
                changed = true;
            }
            escape_state = 1;
            continue;
        }
        if (*current == VIEW_PROCESSES && process_view->filtering) {
            if (input[i] == '\r' || input[i] == '\n') {
                process_view->filtering = false;
                changed = true;
            } else if (input[i] == 0x7f || input[i] == '\b') {
                size_t length = strlen(process_view->filter);
                if (length) {
                    size_t start = length - 1;
                    while (start > 0 &&
                           ((unsigned char)process_view->filter[start] & 0xc0) == 0x80) start--;
                    process_view->filter[start] = '\0';
                }
                process_view->offset = 0;
                changed = true;
            } else if (input[i] >= 0x20 && input[i] != 0x7f) {
                size_t length = strlen(process_view->filter);
                if (length + 1 < sizeof(process_view->filter)) {
                    process_view->filter[length] = (char)input[i];
                    process_view->filter[length + 1] = '\0';
                    process_view->offset = 0;
                    changed = true;
                }
            }
            continue;
        }

        if (*current == VIEW_PROCESSES) {
            bool handled = false;
            if (input[i] == 'c' || input[i] == 'C') { process_view->sort = SORT_CPU; handled = true; }
            else if (input[i] == 'm' || input[i] == 'M') { process_view->sort = SORT_MEM; handled = true; }
            else if (input[i] == 't' || input[i] == 'T') { process_view->sort = SORT_THREADS; handled = true; }
            else if (input[i] == 'p' || input[i] == 'P') { process_view->sort = SORT_PID; handled = true; }
            else if (input[i] == 'n' || input[i] == 'N') { process_view->sort = SORT_NAME; handled = true; }
            else if (input[i] == '/') { process_view->filtering = true; handled = true; }
            else if (input[i] == 0x7f || input[i] == '\b') {
                if (process_view->filter[0]) {
                    process_view->filter[0] = '\0';
                    process_view->offset = 0;
                    handled = true;
                }
            } else if (input[i] == 'j' || input[i] == 'J') {
                size_t amount = input[i] == 'J' ? (size_t)(terminal_height() > 8 ? terminal_height() - 8 : 10) : 1;
                process_view->offset += amount;
                handled = true;
            } else if (input[i] == 'k' || input[i] == 'K') {
                size_t amount = input[i] == 'K' ? (size_t)(terminal_height() > 8 ? terminal_height() - 8 : 10) : 1;
                process_view->offset = process_view->offset > amount ? process_view->offset - amount : 0;
                handled = true;
            }
            if (handled) { changed = true; continue; }
        }

        View next = *current;
        if (input[i] == '1') {
            next = VIEW_DASHBOARD;
        } else if (input[i] == '2' || input[i] == 'p' || input[i] == 'P') {
            next = VIEW_PROCESSES;
        } else if (input[i] == '3' || input[i] == 'n' || input[i] == 'N') {
            next = VIEW_NETWORKS;
        } else if (input[i] == '4' || input[i] == 'u' || input[i] == 'U') {
            next = VIEW_USAGE;
        } else if (input[i] == '5' || input[i] == 'd' || input[i] == 'D') {
            next = VIEW_DISKS;
        } else if (input[i] == '\t') {
            next = *current == VIEW_DASHBOARD ? VIEW_PROCESSES :
                   *current == VIEW_PROCESSES ? VIEW_NETWORKS :
                   *current == VIEW_NETWORKS ? VIEW_USAGE :
                   *current == VIEW_USAGE ? VIEW_DISKS : VIEW_DASHBOARD;
        }
        if (next != *current) {
            *current = next;
            changed = true;
        }
    }
    return changed;
}

bool wait_for_input(View *current, ProcessViewState *process_view, int timeout_ms) {
    for (;;) {
        struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
        int result;
        do {
            result = poll(&descriptor, 1, timeout_ms);
        } while (result < 0 && errno == EINTR && running);

        if (result == 0) return true;
        if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if ((descriptor.revents & POLLIN) && handle_input(current, process_view)) return true;
        /* Ignore scroll sequences and other input that does not change view. */
    }
}

int terminal_width(void) {
    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0) return window.ws_col;
    return 100;
}

int terminal_height(void) {
    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_row > 0) return window.ws_row;
    return 0;
}

const char *status_color(double percent, bool color) {
    if (!color) return "";
    if (percent >= 90.0) return ANSI_RED;
    if (percent >= 70.0) return ANSI_AMBER;
    return ANSI_GREEN;
}

static void print_colored_bar(double percent, int width, bool color, bool reverse_thresholds) {
    if (!isfinite(percent)) { printf("%*s", width, "n/a"); return; }
    int filled = (int)((percent / 100.0) * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    fputs(status_color(reverse_thresholds ? 100.0 - percent : percent, color), stdout);
    for (int i = 0; i < filled; i++) fputs("█", stdout);
    if (color) fputs(ANSI_SLATE, stdout);
    for (int i = filled; i < width; i++) fputs("░", stdout);
    if (color) fputs(ANSI_RESET, stdout);
}

void print_bar(double percent, int width, bool color) {
    print_colored_bar(percent, width, color, false);
}

void print_battery_bar(double percent, int width, bool color) {
    print_colored_bar(percent, width, color, true);
}
