#include "layout.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    Options options = {0};
    TerminalLayout narrow = calculate_layout(59, 24, &options);
    assert(narrow.dashboard_bar_width == 10);
    assert(narrow.dashboard_core_columns == 1);
    assert(!narrow.compact_full_metrics && !narrow.compact_process_memory);

    TerminalLayout medium = calculate_layout(62, 20, &options);
    assert(medium.dashboard_core_columns == 2);
    assert(medium.compact_full_metrics && medium.compact_uptime);
    assert(!medium.compact_battery && !medium.compact_pressure);

    TerminalLayout wide = calculate_layout(92, 30, &options);
    assert(wide.dashboard_core_columns == 3);
    assert(wide.dashboard_side_core_columns == 2);
    assert(wide.compact_battery && wide.compact_pressure);

    TerminalLayout extra_wide = calculate_layout(100, 20, &options);
    assert(extra_wide.dashboard_bar_width == 28);
    assert(layout_uses_dashboard_side_panel(&medium, 8, 10));
    assert(layout_dashboard_side_process_rows(&medium) == 8);
    puts("Terminal layout tests passed");
    return 0;
}
