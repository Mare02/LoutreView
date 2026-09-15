#include "layout.h"

TerminalLayout calculate_layout(int width, int height, const Options *options) {
    (void)options;
    TerminalLayout layout = {
        .width = width,
        .height = height,
        .dashboard_bar_width = width >= 100 ? 28 : width >= 78 ? 18 : 10,
        .dashboard_core_columns = width >= 92 ? 3 : width >= 62 ? 2 : 1,
        .dashboard_side_core_columns = width >= 78 ? 2 : 1,
        .compact_full_metrics = width >= 60,
        .compact_battery = width >= 70,
        .compact_uptime = width >= 60,
        .compact_pressure = width >= 78,
        .compact_process_memory = width >= 60,
    };
    return layout;
}

bool layout_uses_dashboard_side_panel(const TerminalLayout *layout, size_t core_count,
                                      size_t process_count) {
    const int metric_rows = 6;
    size_t standard_core_rows = (core_count + (size_t)layout->dashboard_core_columns - 1) /
                                (size_t)layout->dashboard_core_columns;
    int standard_rows = metric_rows + 3 + (int)standard_core_rows + 2 + (int)process_count;
    size_t side_core_rows = (core_count + (size_t)layout->dashboard_side_core_columns - 1) /
                            (size_t)layout->dashboard_side_core_columns;

    return layout->width >= 62 && layout->height > 0 && layout->height < standard_rows &&
           layout->height >= metric_rows + 2 + (int)side_core_rows;
}

int layout_dashboard_side_process_rows(const TerminalLayout *layout) {
    int rows = layout->height - 9;
    if (rows > 8) return 8;
    return rows < 1 ? 1 : rows;
}
