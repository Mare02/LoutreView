#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CoreServices/CoreServices.h>
#pragma clang diagnostic pop
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <math.h>
#include "sampler.h"

static const char *path_leaf(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool path_contains_bundle(const char *bundle, const char *process_path) {
    size_t length = strlen(bundle);
    return length > 4 && strcmp(bundle + length - 4, ".app") == 0 &&
           strncmp(bundle, process_path, length) == 0 && process_path[length] == '/';
}

static void startup_owner(const char *path, char *owner, size_t size) {
    struct stat info;
    if (path && *path && stat(path, &info) == 0) {
        struct passwd *password = getpwuid(info.st_uid);
        if (password) {
            snprintf(owner, size, "%s", password->pw_name);
            return;
        }
        snprintf(owner, size, "%u", info.st_uid);
        return;
    }
    snprintf(owner, size, "unknown");
}

static void cf_string_copy(CFTypeRef value, char *out, size_t size) {
    out[0] = '\0';
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return;
    CFStringGetCString((CFStringRef)value, out, size, kCFStringEncodingUTF8);
}

static bool startup_add(StartupList *list, StartupKind kind, const char *name,
                        const char *path, const char *owner) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 256;
        StartupItem *items = realloc(list->items, capacity * sizeof(*items));
        if (!items) { list->partial = true; return false; }
        list->items = items;
        list->capacity = capacity;
    }
    StartupItem *item = &list->items[list->count++];
    *item = (StartupItem){ .kind = kind, .pid = 0, .cpu_percent = NAN };
    snprintf(item->name, sizeof(item->name), "%s", name && *name ? name : "(unnamed)");
    snprintf(item->path, sizeof(item->path), "%s", path ? path : "");
    if (owner && *owner) snprintf(item->owner, sizeof(item->owner), "%s", owner);
    else startup_owner(item->path, item->owner, sizeof(item->owner));
    item->path_missing = item->path[0] == '\0' || access(item->path, F_OK) != 0;
    return true;
}

static void launchctl_statuses(StartupList *list) {
    /* Startup-only command; normal resource sampling never invokes a shell. */
    FILE *stream = popen("/bin/launchctl list 2>/dev/null", "r");
    if (!stream) { list->partial = true; return; }
    char line[512];
    while (fgets(line, sizeof(line), stream)) {
        int status = 0;
        char label[256], pid_text[32];
        if (sscanf(line, "%31s %d %255s", pid_text, &status, label) != 3) continue;
        char *end = NULL;
        long pid = strtol(pid_text, &end, 10);
        if (strcmp(pid_text, "-") && (!end || *end || pid <= 0)) continue;
        (void)status;
        for (size_t i = 0; i < list->count; i++) {
            StartupItem *item = &list->items[i];
            if (item->kind != STARTUP_AGENT || strcmp(item->name, label) != 0) continue;
            item->pid = (pid > 0) ? (pid_t)pid : 0;
            item->running_known = true;
            item->running = pid > 0;
            snprintf(item->state, sizeof(item->state), "%s", item->running ? "running" : "inactive");
            break;
        }
    }
    if (pclose(stream) != 0) list->partial = true;
}

static void launch_plist(StartupList *list, StartupKind kind, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { list->partial = true; return; }
    if (fseek(file, 0, SEEK_END) != 0) { list->partial = true; fclose(file); return; }
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        list->partial = true;
        fclose(file);
        return;
    }
    UInt8 *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        list->partial = true;
        free(data); fclose(file); return;
    }
    fclose(file);
    CFDataRef cf_data = CFDataCreate(kCFAllocatorDefault, data, (CFIndex)length);
    free(data);
    if (!cf_data) { list->partial = true; return; }
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, cf_data,
                                                            kCFPropertyListImmutable, NULL, NULL);
    CFRelease(cf_data);
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        list->partial = true;
        if (plist) CFRelease(plist);
        return;
    }

    CFDictionaryRef dict = (CFDictionaryRef)plist;
    char name[256] = {0};
    cf_string_copy(CFDictionaryGetValue(dict, CFSTR("Label")), name, sizeof(name));
    if (!name[0]) snprintf(name, sizeof(name), "%s", path_leaf(path));
    char executable[LOUTRE_PATH_MAX] = {0};
    cf_string_copy(CFDictionaryGetValue(dict, CFSTR("Program")), executable, sizeof(executable));
    if (!executable[0]) {
        CFArrayRef arguments = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("ProgramArguments"));
        if (arguments && CFGetTypeID(arguments) == CFArrayGetTypeID() && CFArrayGetCount(arguments) > 0) {
            cf_string_copy(CFArrayGetValueAtIndex(arguments, 0), executable, sizeof(executable));
        }
    }
    char owner[64];
    startup_owner(executable[0] ? executable : path, owner, sizeof(owner));
    startup_add(list, kind, name, executable, owner);
    CFRelease(plist);
}

static void collect_launch_plists(StartupList *list, StartupKind kind, const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) { if (errno != ENOENT) list->partial = true; return; }
    list->status = METRIC_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 6 || strcmp(entry->d_name + length - 6, ".plist") != 0) continue;
        char path[LOUTRE_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        launch_plist(list, kind, path);
    }
    closedir(dir);
}

static void collect_login_items(StartupList *list) {
    /* The legacy public API does not enumerate every modern background item. */
    list->partial = true;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    LSSharedFileListRef shared = LSSharedFileListCreate(kCFAllocatorDefault,
                                                         kLSSharedFileListSessionLoginItems, NULL);
    if (!shared) return;
    UInt32 seed = 0;
    CFArrayRef snapshot = LSSharedFileListCopySnapshot(shared, &seed);
    if (!snapshot) { CFRelease(shared); return; }
    list->status = METRIC_OK;
    for (CFIndex i = 0; i < CFArrayGetCount(snapshot); i++) {
        LSSharedFileListItemRef item = (LSSharedFileListItemRef)CFArrayGetValueAtIndex(snapshot, i);
        CFURLRef url = NULL;
        if (LSSharedFileListItemResolve(item, 0, &url, NULL) != noErr || !url) continue;
        char path[LOUTRE_PATH_MAX] = {0};
        char name[256] = {0};
        CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof(path));
        CFStringRef last = CFURLCopyLastPathComponent(url);
        cf_string_copy(last, name, sizeof(name));
        if (last) CFRelease(last);
        startup_add(list, STARTUP_LOGIN_ITEM, name[0] ? name : path_leaf(path), path, NULL);
        CFRelease(url);
    }
    CFRelease(snapshot);
    CFRelease(shared);
#pragma clang diagnostic pop
}

static void startup_match_processes(StartupList *list, const ProcessList *processes) {
    for (size_t i = 0; i < list->count; i++) {
        StartupItem *item = &list->items[i];
        if (item->running_known && !item->running) continue;
        for (size_t j = 0; j < processes->count; j++) {
            const Process *process = &processes->items[j];
            bool same = item->pid > 0 && item->pid == process->pid;
            if (!same && item->pid <= 0 && item->path[0] && process->path[0]) same = strcmp(item->path, process->path) == 0;
            if (!same && item->kind == STARTUP_LOGIN_ITEM && item->path[0] && process->path[0]) {
                same = path_contains_bundle(item->path, process->path);
            }
            if (!same && item->kind == STARTUP_LOGIN_ITEM) {
                same = strcasecmp(item->name, process->name) == 0 ||
                       strcasecmp(path_leaf(item->path), process->name) == 0;
            }
            if (!same) continue;
            item->pid = process->pid;
            item->running_known = item->running = true;
            snprintf(item->state, sizeof(item->state), "running");
            item->resident = process->resident;
            item->memory_known = true;
            item->cpu_percent = process->cpu_percent;
            item->start_time = process->start_time;
            break;
        }
    }
}

static int compare_startup_items(const void *left_value, const void *right_value) {
    const StartupItem *left = left_value, *right = right_value;
    if (left->kind != right->kind) return (int)left->kind - (int)right->kind;
    return strcasecmp(left->name, right->name);
}


StartupList platform_startup(void) {
    StartupList list = { .status = METRIC_UNAVAILABLE };
    const char *home = getenv("HOME");
    char user_agents[LOUTRE_PATH_MAX];
    if (home) {
        snprintf(user_agents, sizeof(user_agents), "%s/Library/LaunchAgents", home);
        collect_launch_plists(&list, STARTUP_AGENT, user_agents);
    }
    collect_launch_plists(&list, STARTUP_AGENT, "/Library/LaunchAgents");
    collect_launch_plists(&list, STARTUP_AGENT, "/System/Library/LaunchAgents");
    collect_launch_plists(&list, STARTUP_DAEMON, "/Library/LaunchDaemons");
    collect_launch_plists(&list, STARTUP_DAEMON, "/System/Library/LaunchDaemons");
    collect_login_items(&list);
    launchctl_statuses(&list);

    double before_time = now_seconds();
    ProcessList before = platform_processes();
    struct timespec delay = { .tv_nsec = 100000000L };
    nanosleep(&delay, NULL);
    ProcessList processes = platform_processes();
    sample_process_usage(&processes, &before, now_seconds() - before_time);
    startup_match_processes(&list, &processes);
    if (processes.status != METRIC_OK) list.partial = true;
    free(before.items);
    free(processes.items);
    if (list.count > 1) qsort(list.items, list.count, sizeof(list.items[0]), compare_startup_items);
    return list;
}
