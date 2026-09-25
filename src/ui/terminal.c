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

void print_view_header(const char *view_name, int width, bool color) {
    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    fputs("LOUTREVIEW", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    if (width < 80) {
        printf("  /  %s\n", view_name);
        if (color) fputs(ANSI_DIM, stdout);
        fputs("1:dashboard · 2:networks · 3:ai usage\n", stdout);
        if (color) fputs(ANSI_RESET, stdout);
    } else {
        printf("  /  %s   1:dashboard · 2:networks · 3:ai usage\n", view_name);
    }
    if (color) fputs(ANSI_SLATE, stdout);
    for (int i = 0; i < width - 1; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');
}

void print_compact_header(const char *view_name, int width, bool color) {
    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    fputs("LOUTREVIEW", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    if (width < 70) {
        printf("  /  %s\n", view_name);
        if (color) fputs(ANSI_DIM, stdout);
        fputs("1:dashboard · 2:networks · 3:ai usage\n", stdout);
        if (color) fputs(ANSI_RESET, stdout);
    } else {
        printf("  /  %s   1:dashboard · 2:networks · 3:ai usage\n", view_name);
    }
}

static bool handle_input(View *current) {
    unsigned char input[32];
    ssize_t received = read(STDIN_FILENO, input, sizeof(input));
    if (received <= 0) return false;
    static int escape_state = 0;
    bool changed = false;
    for (ssize_t i = 0; i < received; i++) {
        if (escape_state == 1) {
            escape_state = input[i] == '[' ? 2 : 0;
            continue;
        }
        if (escape_state == 2) {
            if (input[i] >= 0x40 && input[i] <= 0x7e) escape_state = 0;
            continue;
        }
        if (input[i] == '\033') {
            escape_state = 1;
            continue;
        }
        View next = *current;
        if (input[i] == '1') {
            next = VIEW_DASHBOARD;
        } else if (input[i] == '2' || input[i] == 'n' || input[i] == 'N') {
            next = VIEW_NETWORKS;
        } else if (input[i] == '3' || input[i] == 'u' || input[i] == 'U') {
            next = VIEW_USAGE;
        } else if (input[i] == '\t') {
            next = *current == VIEW_DASHBOARD ? VIEW_NETWORKS :
                   *current == VIEW_NETWORKS ? VIEW_USAGE : VIEW_DASHBOARD;
        }
        if (next != *current) {
            *current = next;
            changed = true;
        }
    }
    return changed;
}

bool wait_for_input(View *current, int timeout_ms) {
    for (;;) {
        struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
        int result;
        do {
            result = poll(&descriptor, 1, timeout_ms);
        } while (result < 0 && errno == EINTR && running);

        if (result == 0) return true;
        if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if ((descriptor.revents & POLLIN) && handle_input(current)) return true;
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
