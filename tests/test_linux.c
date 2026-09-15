#ifdef __linux__
#include "../platform/linux/linux.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void battery_file(const char *root, const char *key, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/BAT0/%s", root, key);
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static void test_battery(void) {
    char root[] = "/tmp/loutre-battery-XXXXXX";
    assert(mkdtemp(root));
    char dir[512]; snprintf(dir, sizeof(dir), "%s/BAT0", root);
    assert(mkdir(dir, 0700) == 0);
    battery_file(root, "type", "Battery\n");
    battery_file(root, "energy_now", "20000000\n");
    battery_file(root, "energy_full", "40000000\n");
    battery_file(root, "power_now", "10000000\n");
    battery_file(root, "status", "Discharging\n");
    BatteryInfo out;
    assert(linux_read_battery(root, &out));
    assert(out.percent == 50 && out.time_remaining_minutes == 120);
    assert(!strcmp(out.state, "discharging"));
    battery_file(root, "status", "Charging\n");
    assert(linux_read_battery(root, &out) && out.charging && out.time_remaining_minutes == 120);
    battery_file(root, "power_now", "0\n");
    assert(linux_read_battery(root, &out) && out.time_remaining_minutes == -1);
    /* Force charge/current units and verify signed discharge current. */
    battery_file(root, "energy_full", "0\n");
    battery_file(root, "charge_now", "2000000\n");
    battery_file(root, "charge_full", "4000000\n");
    battery_file(root, "current_now", "-1000000\n");
    battery_file(root, "status", "Discharging\n");
    assert(linux_read_battery(root, &out) && out.percent == 50 && out.time_remaining_minutes == 120);
    battery_file(root, "present", "0\n");
    assert(!linux_read_battery(root, &out));
    const char *keys[] = {"type", "energy_now", "energy_full", "power_now", "status", "present",
                         "charge_now", "charge_full", "current_now"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        char path[512]; snprintf(path, sizeof(path), "%s/BAT0/%s", root, keys[i]);
        assert(unlink(path) == 0);
    }
    assert(rmdir(dir) == 0 && rmdir(root) == 0);
}

static void test_cpu(void) {
    CpuTicks ticks;
    unsigned id;
    bool aggregate;
    assert(linux_parse_cpu("cpu 100 20 30 400 50 6 7 8 9 10\n", &ticks, &id, &aggregate));
    assert(aggregate && ticks.user == 100 && ticks.nice == 20);
    assert(ticks.system == 51 && ticks.idle == 450);
    assert(linux_parse_cpu("cpu135 10 0 5 100\n", &ticks, &id, &aggregate));
    assert(!aggregate && id == 135 && ticks.system == 5);
    assert(!linux_parse_cpu("cpu 1 2\n", &ticks, &id, &aggregate));
    assert(!linux_parse_cpu("cpuX 1 2 3 4\n", &ticks, &id, &aggregate));
    assert(!linux_parse_cpu("cpu 1 2 -3 4\n", &ticks, &id, &aggregate));
    assert(!linux_parse_cpu("cpu 1 2 18446744073709551615 4 0 1\n", &ticks, &id, &aggregate));
}

static void test_memory(void) {
    FILE *file = tmpfile();
    assert(file);
    fputs("MemTotal: 1000 kB\nMemFree: 1 kB\nCached: 20 kB\nMemAvailable: 400 kB\n", file);
    rewind(file);
    SystemMetrics metrics = {0};
    assert(linux_parse_memory(file, &metrics));
    assert(metrics.memory_total == 1024000 && metrics.memory_used == 614400);
    fclose(file);
    file = tmpfile(); assert(file);
    fputs("MemTotal: 1000 kB\nMemFree: 10 kB\n", file); rewind(file);
    assert(!linux_parse_memory(file, &metrics));
    fclose(file);
    file = tmpfile(); assert(file);
    fputs("MemTotal: 10 kB\nMemAvailable: 100 kB\n", file); rewind(file);
    assert(!linux_parse_memory(file, &metrics));
    fclose(file);
}

static void test_process(void) {
    Process p;
    /* comm can contain spaces and closing parentheses; signed ignored fields
     * include negative priority/nice. Fields 14/15 are user/system ticks. */
    const char *stat = "42 (worker ) with spaces) S 1 2 3 0 -1 0 0 0 0 0 150 50 0 0 20 -5 4 0 12345 8192 3 0 0\n";
    assert(linux_parse_process(stat, 100, 4096, 1000, &p));
    assert(p.pid == 42 && !strcmp(p.name, "worker ) with spaces"));
    assert(p.cpu_time == 2000000000ULL && p.threads == 4);
    assert(p.start_id == 12345 && p.start_time == 1123);
    assert(p.resident == 12288 && p.virtual_size == 8192);
    assert(!linux_parse_process(stat, 0, 4096, 0, &p));
    assert(!linux_parse_process("42 (broken) S 1 2 3", 100, 4096, 0, &p));
    assert(!linux_parse_process("42 broken S 1", 100, 4096, 0, &p));
    assert(linux_parse_process("43 (x) S 1 2 3 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 1 0 0", 128, 4096, 0, &p));
    assert(p.cpu_time == 15625000ULL && p.start_time == 0);
    assert(!linux_parse_process("43 (x) S 1 2 3 0 -1 0 0 0 0 0 1844674407371 0 0 0 20 0 1 0 1 0 0", 100, 4096, 0, &p));
}

static void test_network(void) {
    NetworkInterface net;
    assert(linux_parse_network(" eth0: 123 1 0 0 0 0 0 0 456 2 0 0 0 0 0 0\n", &net));
    assert(!strcmp(net.name, "eth0") && net.received == 123 && net.transmitted == 456);
    assert(!linux_parse_network(" face |bytes packets", &net));
    assert(!linux_parse_network("eth0: 1 2 3", &net));
    assert(!linux_parse_network("eth0: -1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0", &net));
}

int main(void) {
    test_cpu(); test_memory(); test_process(); test_network(); test_battery();
    BatteryInfo battery;
    assert(!linux_read_battery("/nonexistent-loutre-test-battery", &battery));
    assert(!battery.available && battery.time_remaining_minutes == -1);
    puts("Linux parser tests passed");
    return 0;
}
#else
#include <stdio.h>
int main(void) { puts("Linux parser tests skipped on non-Linux platform"); return 0; }
#endif
