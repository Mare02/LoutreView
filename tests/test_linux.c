#ifdef __linux__
#include "../platform/linux/linux.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fixture_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static void fixture_path(char *out, size_t size, const char *base, const char *leaf) {
    int length = snprintf(out, size, "%s/%s", base, leaf);
    assert(length > 0 && (size_t)length < size);
}

static void battery_file(const char *root, const char *battery, const char *key, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", root, battery, key);
    fixture_file(path, text);
}

static void test_battery(void) {
    char root[] = "/tmp/loutre-battery-XXXXXX";
    assert(mkdtemp(root));
    char dir[512]; snprintf(dir, sizeof(dir), "%s/BAT0", root);
    assert(mkdir(dir, 0700) == 0);
    battery_file(root, "BAT0", "type", "Battery\n");
    battery_file(root, "BAT0", "energy_now", "20000000\n");
    battery_file(root, "BAT0", "energy_full", "40000000\n");
    battery_file(root, "BAT0", "power_now", "10000000\n");
    battery_file(root, "BAT0", "status", "Discharging\n");
    BatteryInfo out;
    assert(linux_read_battery(root, &out));
    assert(out.percent == 50 && out.time_remaining_minutes == 120);
    assert(!strcmp(out.state, "discharging"));
    battery_file(root, "BAT0", "status", "Charging\n");
    assert(linux_read_battery(root, &out) && out.charging && out.time_remaining_minutes == 120);
    battery_file(root, "BAT0", "power_now", "0\n");
    assert(linux_read_battery(root, &out) && out.time_remaining_minutes == -1);
    /* Force charge/current units and verify signed discharge current. */
    battery_file(root, "BAT0", "energy_full", "0\n");
    battery_file(root, "BAT0", "charge_now", "2000000\n");
    battery_file(root, "BAT0", "charge_full", "4000000\n");
    battery_file(root, "BAT0", "current_now", "-1000000\n");
    battery_file(root, "BAT0", "status", "Discharging\n");
    assert(linux_read_battery(root, &out) && out.percent == 50 && out.time_remaining_minutes == 120);
    char second[512]; snprintf(second, sizeof(second), "%s/BAT1", root);
    assert(mkdir(second, 0700) == 0);
    battery_file(root, "BAT1", "type", "Battery\n");
    battery_file(root, "BAT1", "capacity", "25\n");
    battery_file(root, "BAT0", "charge_full", "malformed\n");
    assert(linux_read_battery(root, &out) && out.percent == 25);
    battery_file(root, "BAT0", "charge_full", "4000000\n");
    assert(linux_read_battery(root, &out) && out.percent == 50); /* deterministic BAT0 preference */
    battery_file(root, "BAT0", "present", "0\n");
    battery_file(root, "BAT1", "present", "0\n");
    assert(!linux_read_battery(root, &out));
    const char *keys[] = {"type", "energy_now", "energy_full", "power_now", "status", "present",
                         "charge_now", "charge_full", "current_now"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        char path[512]; snprintf(path, sizeof(path), "%s/BAT0/%s", root, keys[i]);
        assert(unlink(path) == 0);
    }
    const char *second_keys[] = {"type", "capacity", "present"};
    for (size_t i = 0; i < sizeof(second_keys)/sizeof(second_keys[0]); i++) {
        char path[512]; snprintf(path, sizeof(path), "%s/BAT1/%s", root, second_keys[i]);
        assert(unlink(path) == 0);
    }
    assert(rmdir(dir) == 0 && rmdir(second) == 0 && rmdir(root) == 0);
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

static void test_collector_fixtures(void) {
    char root[] = "/tmp/loutre-linux-fixtures-XXXXXX";
    assert(mkdtemp(root));
    char path[512], proc[512], sysnet[512];
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(sysnet, sizeof(sysnet), "%s/sysnet", root);
    assert(mkdir(proc, 0700) == 0 && mkdir(sysnet, 0700) == 0);

    snprintf(path, sizeof(path), "%s/missing", root);
    Snapshot snapshot;
    assert(linux_collect_cpu(path, &snapshot) == METRIC_UNAVAILABLE);
    snprintf(path, sizeof(path), "%s/cpu", root);
    fixture_file(path, "cpu malformed\n");
    assert(linux_collect_cpu(path, &snapshot) == METRIC_ERROR);

    char meminfo[512], pressure[512];
    snprintf(meminfo, sizeof(meminfo), "%s/meminfo", root);
    snprintf(pressure, sizeof(pressure), "%s/pressure", root);
    SystemMetrics metrics;
    fixture_path(path, sizeof(path), root, "missing-meminfo");
    assert(linux_collect_memory(path, pressure, &metrics) == METRIC_UNAVAILABLE);
    fixture_file(meminfo, "MemTotal: bad kB\n");
    assert(linux_collect_memory(meminfo, pressure, &metrics) == METRIC_ERROR);
    fixture_file(meminfo, "MemTotal: 100 kB\nMemAvailable: 50 kB\n");
    fixture_file(pressure, "some avg10=not-a-number\n");
    assert(linux_collect_memory(meminfo, pressure, &metrics) == METRIC_OK && !metrics.pressure_percent_available);

    fixture_path(path, sizeof(path), proc, "stat");
    fixture_file(path, "btime 1000\n");
    char process[512]; fixture_path(process, sizeof(process), proc, "42");
    assert(mkdir(process, 0700) == 0);
    fixture_path(path, sizeof(path), process, "stat");
    fixture_file(path, "42 (fixture) S 1 2 3 0 -1 0 0 0 0 0 10 5 0 0 20 0 1 0 100 4096 2 0 0\n");
    fixture_path(process, sizeof(process), proc, "43");
    assert(mkdir(process, 0700) == 0); /* Process vanishes before its stat file can be opened. */
    ProcessList processes = linux_collect_processes(proc, 100, 4096);
    assert(processes.status == METRIC_OK && processes.count == 1 && processes.items[0].pid == 42);
    free(processes.items);

    char dev[512]; snprintf(dev, sizeof(dev), "%s/dev", root);
    fixture_file(dev, "Inter-| Receive | Transmit\n eth0: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n gone0: 1 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0\n");
    fixture_path(path, sizeof(path), sysnet, "eth0");
    assert(mkdir(path, 0700) == 0);
    fixture_path(path, sizeof(path), sysnet, "eth0/flags");
    fixture_file(path, "0x1\n");
    NetworkSnapshot networks = linux_collect_networks(dev, sysnet);
    assert(networks.status == METRIC_OK && networks.count == 2 && networks.items[0].up);
    snprintf(path, sizeof(path), "%s/no-dev", root);
    assert(linux_collect_networks(path, sysnet).status == METRIC_UNAVAILABLE);

    errno = EACCES;
    assert(linux_errno_status() == METRIC_PERMISSION);
    assert(unlink(dev) == 0 && unlink(meminfo) == 0 && unlink(pressure) == 0);
    char cpu[512]; snprintf(cpu, sizeof(cpu), "%s/cpu", root); assert(unlink(cpu) == 0);
    fixture_path(path, sizeof(path), sysnet, "eth0/flags"); assert(unlink(path) == 0);
    fixture_path(path, sizeof(path), sysnet, "eth0"); assert(rmdir(path) == 0);
    fixture_path(path, sizeof(path), proc, "42/stat"); assert(unlink(path) == 0);
    fixture_path(path, sizeof(path), proc, "42"); assert(rmdir(path) == 0);
    fixture_path(path, sizeof(path), proc, "43"); assert(rmdir(path) == 0);
    fixture_path(path, sizeof(path), proc, "stat"); assert(unlink(path) == 0);
    assert(rmdir(proc) == 0 && rmdir(sysnet) == 0 && rmdir(root) == 0);
}

int main(void) {
    test_cpu(); test_memory(); test_process(); test_network(); test_battery(); test_collector_fixtures();
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
