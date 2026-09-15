#ifndef LOUTRE_FORMAT_H
#define LOUTRE_FORMAT_H
#include "model.h"
const char *format_bytes(unsigned long long bytes, char *buffer, size_t size);
const char *format_compact_bytes(unsigned long long bytes, char *buffer, size_t size);
void format_uptime(double seconds, char *buffer, size_t size);
void format_duration_minutes(int minutes, char *buffer, size_t size);
void format_start_time(time_t value, char *buffer, size_t size);
const char *sort_name(SortMode sort);
void print_spaces(int count);
const char *metric_status_name(MetricStatus status);
const char *startup_kind_name(StartupKind kind);
const char *startup_kind_short_name(StartupKind kind);
void format_pressure(const SystemMetrics *metrics, char *buffer, size_t size, bool compact);
size_t text_utf8_width(const unsigned char *text);
void print_safe_text(const char *text, int max_columns);
void print_percent(double value, int width, int precision, bool suffix);
void format_rate(double value, char *buffer, size_t size);
#endif
