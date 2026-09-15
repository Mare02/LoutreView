#ifndef LOUTRE_UI_H
#define LOUTRE_UI_H
#include "model.h"
#include "layout.h"
#include <signal.h>
#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_CYAN "\033[38;5;81m"
#define ANSI_TEAL "\033[38;5;45m"
#define ANSI_GREEN "\033[38;5;114m"
#define ANSI_AMBER "\033[38;5;221m"
#define ANSI_RED "\033[38;5;203m"
#define ANSI_SLATE "\033[38;5;246m"
#define ANSI_ALT_SCREEN "\033[?1049h"
#define ANSI_MAIN_SCREEN "\033[?1049l"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"
#define ANSI_HOME "\033[H"
#define ANSI_CLEAR_SCREEN "\033[2J\033[3J\033[H"
#define ANSI_SYNC_BEGIN "\033[?2026h"
#define ANSI_SYNC_END "\033[?2026l"
extern volatile sig_atomic_t running;
void on_signal(int signal_number);
void restore_terminal(void);
bool configure_terminal(void);
bool wait_for_input(View *current, int timeout_ms);
int terminal_width(void);
int terminal_height(void);
const char *status_color(double percent, bool color);
void print_bar(double percent, int width, bool color);
void print_view_header(const char *view_name, int interval_ms, int width, bool color);
void print_compact_header(const char *view_name, int width, bool color);
void print_network_screen(const NetworkSnapshot *snapshot, const Options *options, bool clear, bool color);
void print_compact_network_screen(const NetworkSnapshot *snapshot, bool clear, bool color);
void render_compact_dashboard(const Snapshot *snapshot, double cpu, const Options *options,
                              const TerminalLayout *layout, bool clear, bool color);
void render_dashboard(const Snapshot *snapshot, double cpu, const Options *options,
                      const double *core_usage, size_t core_count,
                      const TerminalLayout *layout, bool clear, bool color);
void print_startup_report(const Options *options);
void print_json(const Snapshot *snapshot, double cpu, const Options *options);
void json_string(const char *value);
void print_startup_json(const StartupList *list);
#endif
