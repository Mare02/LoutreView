#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
const char *platform_name(void) { return "macos"; }

MetricStatus platform_memory(SystemMetrics *out);
static bool read_cpu_ticks(CpuTicks *out, CpuTicks *cores, size_t *core_count) {
    natural_t cpu_count = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t info_count = 0;
    kern_return_t result = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                               &cpu_count, &info, &info_count);
    if (result != KERN_SUCCESS || info == NULL) return false;

    memset(out, 0, sizeof(*out));
    *core_count = cpu_count < MAX_CPU_CORES ? cpu_count : MAX_CPU_CORES;
    for (natural_t i = 0; i < cpu_count; i++) {
        processor_cpu_load_info_t cpu =
            (processor_cpu_load_info_t)(info + i * CPU_STATE_MAX);
        out->user += cpu->cpu_ticks[CPU_STATE_USER];
        out->system += cpu->cpu_ticks[CPU_STATE_SYSTEM];
        out->idle += cpu->cpu_ticks[CPU_STATE_IDLE];
        out->nice += cpu->cpu_ticks[CPU_STATE_NICE];
        if (i < *core_count) {
            cores[i] = (CpuTicks){
                .user = cpu->cpu_ticks[CPU_STATE_USER],
                .system = cpu->cpu_ticks[CPU_STATE_SYSTEM],
                .idle = cpu->cpu_ticks[CPU_STATE_IDLE],
                .nice = cpu->cpu_ticks[CPU_STATE_NICE],
            };
        }
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)info_count * sizeof(integer_t));
    return true;
}

static bool get_memory(unsigned long long *total, unsigned long long *used,
                       unsigned long long *pressure) {
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    size_t length = sizeof(*total);
    if (sysctl(mib, 2, total, &length, NULL, 0) != 0) return false;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        return false;
    }
    vm_size_t page_size = 0;
    if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS || !page_size) return false;
    unsigned long long available_pages = (unsigned long long)vm.free_count + vm.inactive_count;
    unsigned long long wired_pages = vm.wire_count;
    *used = *total > available_pages * page_size ? *total - available_pages * page_size : 0;
    *pressure = (wired_pages + vm.compressor_page_count) * (unsigned long long)page_size;
    return true;
}

double platform_uptime(void) {
    struct timeval boot;
    size_t size = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &size, NULL, 0) == 0)
        return difftime(time(NULL), boot.tv_sec);
    /* Hardened/sandboxed processes may be denied kern.boottime. Monotonic
     * time remains a useful uptime approximation and is available on Linux. */
    struct timespec monotonic;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0)
        return (double)monotonic.tv_sec + (double)monotonic.tv_nsec / 1e9;
    return -1;
}

static bool cf_number_to_int(CFTypeRef value, int *out) {
    return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, out);
}

static bool cf_boolean_value(CFTypeRef value) {
    return value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
}

static bool cf_string_equal(CFTypeRef value, const char *expected) {
    char text[64];
    return value && CFGetTypeID(value) == CFStringGetTypeID() &&
           CFStringGetCString((CFStringRef)value, text, sizeof(text), kCFStringEncodingUTF8) &&
           strcmp(text, expected) == 0;
}

bool platform_battery(BatteryInfo *out) {
    *out = (BatteryInfo){ .time_remaining_minutes = -1 };
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return false;
    CFArrayRef sources = IOPSCopyPowerSourcesList(info);
    if (!sources) {
        CFRelease(info);
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(sources); i++) {
        CFTypeRef source = CFArrayGetValueAtIndex(sources, i);
        CFDictionaryRef description = IOPSGetPowerSourceDescription(info, source);
        if (!description || !cf_string_equal(CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey)),
                                             kIOPSInternalBatteryType)) {
            continue;
        }

        int current = 0, maximum = 0;
        if (!cf_number_to_int(CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey)), &current) ||
            !cf_number_to_int(CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)), &maximum) ||
            current < 0 || maximum <= 0) {
            continue;
        }
        out->available = true;
        out->percent = (int)(100.0 * current / maximum + 0.5);
        if (out->percent > 100) out->percent = 100;
        out->charging = cf_boolean_value(CFDictionaryGetValue(description, CFSTR(kIOPSIsChargingKey)));
        out->charged = cf_boolean_value(CFDictionaryGetValue(description, CFSTR(kIOPSIsChargedKey)));

        CFTypeRef power_state = CFDictionaryGetValue(description, CFSTR(kIOPSPowerSourceStateKey));
        if (out->charged) snprintf(out->state, sizeof(out->state), "charged");
        else if (out->charging) snprintf(out->state, sizeof(out->state), "charging");
        else if (cf_string_equal(power_state, kIOPSBatteryPowerValue)) snprintf(out->state, sizeof(out->state), "discharging");
        else if (cf_string_equal(power_state, kIOPSACPowerValue)) snprintf(out->state, sizeof(out->state), "on power");
        else snprintf(out->state, sizeof(out->state), "unknown");

        if (out->charging || cf_string_equal(power_state, kIOPSBatteryPowerValue)) {
            CFStringRef time_key = out->charging ? CFSTR(kIOPSTimeToFullChargeKey) : CFSTR(kIOPSTimeToEmptyKey);
            cf_number_to_int(CFDictionaryGetValue(description, time_key), &out->time_remaining_minutes);
        }
        break;
    }
    CFRelease(sources);
    CFRelease(info);
    return out->available;
}

MetricStatus platform_cpu(Snapshot *out) {
    out->ticks = (CpuTicks){0};
    memset(out->core_ticks, 0, sizeof(out->core_ticks));
    memset(out->core_ids, 0, sizeof(out->core_ids));
    out->core_count = 0;
    out->cpu_status = read_cpu_ticks(&out->ticks, out->core_ticks, &out->core_count) ? METRIC_OK : METRIC_UNAVAILABLE;
    for (size_t i = 0; i < out->core_count; i++) out->core_ids[i] = (unsigned)i;
    return out->cpu_status;
}

MetricStatus platform_memory(SystemMetrics *out) {
    out->memory_total = out->memory_used = out->pressure = 0;
    out->pressure_available = out->pressure_percent_available = false;
    out->pressure_percent = 0;
    out->pressure_source = NULL;
    out->memory_status = get_memory(&out->memory_total, &out->memory_used, &out->pressure) ? METRIC_OK : METRIC_UNAVAILABLE;
    if (out->memory_status == METRIC_OK) {
        out->pressure_available = true;
        out->pressure_source = "wired_compressed";
    }
    return out->memory_status;
}
