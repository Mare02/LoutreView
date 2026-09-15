#ifndef LOUTRE_LAYOUT_H
#define LOUTRE_LAYOUT_H

#include "model.h"

typedef struct {
    int width;
    int height;
    int dashboard_bar_width;
    int dashboard_core_columns;
    int dashboard_side_core_columns;
    bool compact_full_metrics;
    bool compact_battery;
    bool compact_uptime;
    bool compact_pressure;
    bool compact_process_memory;
} TerminalLayout;

TerminalLayout calculate_layout(int width, int height, const Options *options);
bool layout_uses_dashboard_side_panel(const TerminalLayout *layout, size_t core_count,
                                      size_t process_count);
int layout_dashboard_side_process_rows(const TerminalLayout *layout);

#endif
