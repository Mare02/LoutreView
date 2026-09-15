#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) || defined(LOUTRE_TEST_LINUX_STARTUP)
#include "../platform/linux/startup_internal.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Standalone test, link with platform/linux/startup.c and -Iinclude.
 * Define LOUTRE_TEST_LINUX_STARTUP to run these POSIX fixtures on macOS too.
 * Every collector test injects the runner. Real subprocess tests exec this
 * binary's helper modes; neither the host systemctl nor desktop Exec is run. */
typedef struct { int mode, calls, shows; } Fixture;

static bool argument(const char *const argv[], const char *value) {
    for (size_t i = 0; argv[i]; i++) if (!strcmp(argv[i], value)) return true;
    return false;
}

static LinuxStartupCommandResult fake_systemctl(
    const char *const argv[], char *out, size_t cap, unsigned ms, void *context) {
    Fixture *f = context;
    f->calls++;
    assert(!strcmp(argv[0], "/fixture/systemctl"));
    assert(ms > 0 && ms <= 1000);
    assert(argument(argv, "--no-pager"));
    if (f->mode == 1) return LINUX_STARTUP_COMMAND_UNAVAILABLE;
    bool user = argument(argv, "--user");
    if (f->mode == 2 && user) return LINUX_STARTUP_COMMAND_ERROR;
    const char *response = "";
    if (argument(argv, "list-unit-files")) {
        assert(argument(argv, "--type=service"));
        response = user ? "user.service enabled enabled\n" :
            "inactive.service enabled enabled\n"
            "busy.service disabled enabled\n"
            "oneshot.service enabled enabled\n"
            "static.service static -\n"
            "masked.service masked disabled\n";
    } else if (argument(argv, "list-units")) {
        assert(argument(argv, "--all"));
        response = user ? "user.service loaded inactive dead User service\n" :
            "busy.service loaded active running Busy service\n"
            "transient.service loaded active running Transient service\n";
    } else {
        assert(argument(argv, "show"));
        assert(argument(argv, "--"));
        f->shows++;
        if (f->mode == 3) return LINUX_STARTUP_COMMAND_TIMEOUT;
        if (f->mode == 4) return LINUX_STARTUP_COMMAND_LIMIT;
        response = user ?
            "Id=user.service\nActiveState=inactive\nSubState=dead\nMainPID=0\nUnitFileState=enabled\n\n" :
            "MainPID=0\nUnitFileState=enabled\nActiveState=inactive\nSubState=dead\nId=inactive.service\n\n"
            "Id=busy.service\nUnitFileState=disabled\nMainPID=432\nActiveState=active\nSubState=running\nMemoryCurrent=8192\nFragmentPath=/fixture/busy.service\n\n"
            "Id=oneshot.service\nUnitFileState=enabled\nMainPID=0\nActiveState=active\nSubState=exited\nMemoryCurrent=18446744073709551615\n\n"
            "Id=static.service\nUnitFileState=static\nMainPID=0\nActiveState=inactive\nSubState=dead\n\n"
            "Id=masked.service\nUnitFileState=masked\nActiveState=inactive\nSubState=dead\n\n"
            "Id=transient.service\nUnitFileState=transient\nMainPID=901\nActiveState=active\nSubState=running\n";
        if (f->mode == 5 && !user) response =
            "Id=busy.service\nActiveState=future-state\nMainPID=22\n\n"
            "Id=oneshot.service\nActiveState=active\nSubState=future\nMainPID=0\n";
    }
    assert(strlen(response) < cap);
    strcpy(out, response);
    return LINUX_STARTUP_COMMAND_OK;
}

static LinuxStartupOptions options(Fixture *f) {
    return (LinuxStartupOptions){ .systemctl = "/fixture/systemctl",
        .config_home = "", .config_dirs = "", .desktop = "ubuntu:GNOME",
        .search_path = "", .runner = fake_systemctl, .context = f };
}

static const StartupItem *find(const StartupList *list, const char *name) {
    const StartupItem *found = NULL;
    for (size_t i = 0; i < list->count; i++) if (!strcmp(list->items[i].name, name)) {
        assert(!found); found = &list->items[i];
    }
    assert(found);
    return found;
}

static void test_services(void) {
    Fixture f = {0};
    LinuxStartupOptions o = options(&f);
    StartupList list = linux_startup_collect(&o);
    assert(list.status == METRIC_OK && !list.partial && list.count == 7);
    assert(list.items && list.capacity >= list.count);
    const StartupItem *item = find(&list, "inactive.service");
    assert(item->enabled_known && item->enabled && item->running_known && !item->running);
    item = find(&list, "busy.service");
    assert(item->enabled_known && !item->enabled && item->running_known && item->running);
    assert(item->pid == 432 && item->resident == 8192);
    assert(!strcmp(item->path, "/fixture/busy.service"));
    assert(item->kind == STARTUP_SYSTEM_SERVICE);
    item = find(&list, "oneshot.service");
    assert(item->enabled && item->running_known && !item->running && item->resident == 0);
    item = find(&list, "static.service");
    assert(!item->enabled_known && item->running_known && !item->running);
    item = find(&list, "masked.service");
    assert(item->enabled_known && !item->enabled);
    item = find(&list, "transient.service");
    assert(item->running_known && item->running && !item->enabled_known);
    assert(find(&list, "user.service")->kind == STARTUP_USER_SERVICE);
    assert(f.calls == 6 && f.shows == 2);
    free(list.items);

    for (int mode = 1; mode <= 5; mode++) {
        f = (Fixture){ .mode = mode };
        list = linux_startup_collect(&o);
        assert(list.partial && list.status != METRIC_OK);
        if (mode == 1) assert(list.count == 0);
        if (mode == 2) assert(list.count == 6 && find(&list, "busy.service")->running);
        if (mode >= 3) {
            item = find(&list, "inactive.service");
            assert(item->enabled_known && item->enabled && !item->running_known);
            assert(!find(&list, "oneshot.service")->running_known);
        }
        free(list.items);
    }
}

static LinuxStartupCommandResult batch_systemctl(
    const char *const argv[], char *out, size_t cap, unsigned ms, void *context) {
    (void)ms;
    Fixture *f = context;
    out[0] = '\0';
    if (argument(argv, "--user") || argument(argv, "list-units")) return LINUX_STARTUP_COMMAND_OK;
    size_t used = 0;
    if (argument(argv, "list-unit-files")) {
        for (int i = 0; i < 130; i++)
            used += (size_t)snprintf(out + used, cap - used, "fixture-%d.service enabled -\n", i);
    } else {
        f->shows++;
        size_t i = 0;
        while (argv[i] && strcmp(argv[i], "--")) i++;
        assert(argv[i]); i++;
        size_t count = 0;
        for (; argv[i]; i++, count++) {
            used += (size_t)snprintf(out + used, cap - used,
                "Id=%s\nActiveState=inactive\nSubState=dead\n\n", argv[i]);
        }
        assert(count > 0 && count <= 64);
    }
    assert(used < cap);
    return LINUX_STARTUP_COMMAND_OK;
}

static void test_batches(void) {
    Fixture f = {0};
    LinuxStartupOptions o = options(&f);
    o.runner = batch_systemctl;
    StartupList list = linux_startup_collect(&o);
    assert(list.count == 130 && f.shows == 3 && !list.partial);
    assert(find(&list, "fixture-129.service")->running_known);
    free(list.items);
}

static void test_exec_parser(void) {
    char out[4096];
    assert(linux_startup_exec_token("/usr/bin/app --flag %U", out, sizeof(out)));
    assert(!strcmp(out, "/usr/bin/app"));
    assert(linux_startup_exec_token("\"/opt/My App/bin\" %f", out, sizeof(out)));
    assert(!strcmp(out, "/opt/My App/bin"));
    assert(linux_startup_exec_token("app%%name --arg", out, sizeof(out)));
    assert(!strcmp(out, "app%name"));
    assert(linux_startup_exec_token("\"/opt/a\\\\$b\"", out, sizeof(out)));
    assert(!strcmp(out, "/opt/a$b"));
    const char *bad[] = {"", "\"unclosed", "'shell quoted'", "$HOME/bin/app", "a;b",
                         "FOO=x app", "%f", "\"app\"suffix", "a\\q", "\"$HOME/bin\""};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
        assert(!linux_startup_exec_token(bad[i], out, sizeof(out)));
    assert(!linux_startup_exec_token("tool", out, 3));
}

static char files[40][LOUTRE_PATH_MAX];
static size_t file_count;

static void fixture_file(const char *dir, const char *name, const char *contents, mode_t mode) {
    assert(file_count < 40);
    char *path = files[file_count++];
    int n = snprintf(path, LOUTRE_PATH_MAX, "%s/%s", dir, name);
    assert(n > 0 && n < LOUTRE_PATH_MAX);
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(contents, f) >= 0); assert(fclose(f) == 0);
    assert(chmod(path, mode) == 0);
}

static void test_desktops(const char *self) {
    char root[] = "/tmp/loutre-startup-XXXXXX";
    assert(mkdtemp(root));
    char user[4096], first[4096], second[4096], bin[4096];
    assert(snprintf(user, sizeof(user), "%s/user", root) > 0);
    assert(snprintf(first, sizeof(first), "%s/first", root) > 0);
    assert(snprintf(second, sizeof(second), "%s/second", root) > 0);
    assert(snprintf(bin, sizeof(bin), "%s/bin", root) > 0);
    assert(mkdir(user, 0700) == 0 && mkdir(first, 0700) == 0);
    assert(mkdir(second, 0700) == 0 && mkdir(bin, 0700) == 0);
    char ua[4096], fa[4096], sa[4096];
    assert(snprintf(ua, sizeof(ua), "%s/autostart", user) > 0);
    assert(snprintf(fa, sizeof(fa), "%s/autostart", first) > 0);
    assert(snprintf(sa, sizeof(sa), "%s/autostart", second) > 0);
    assert(mkdir(ua, 0700) == 0 && mkdir(fa, 0700) == 0 && mkdir(sa, 0700) == 0);
    fixture_file(bin, "tool", "this file must never be executed\n", 0700);
    fixture_file(bin, "spaced tool", "this file must never be executed\n", 0700);
    fixture_file(bin, "not-executable", "fixture\n", 0600);
    fixture_file(ua, "hidden.desktop", "[Desktop Entry]\nHidden=true\n", 0600);
    fixture_file(fa, "hidden.desktop", "[Desktop Entry]\nType=Application\nName=wrong-hidden\nExec=tool\n", 0600);
    fixture_file(ua, "override.desktop", "[Desktop Entry]\nType=Application\nName=user-choice\nExec=tool\n", 0600);
    fixture_file(fa, "override.desktop", "[Desktop Entry]\nType=Application\nName=wrong-system\nExec=tool\n", 0600);
    fixture_file(fa, "order.desktop", "[Desktop Entry]\nType=Application\nName=first-choice\nExec=tool\n", 0600);
    fixture_file(sa, "order.desktop", "[Desktop Entry]\nType=Application\nName=wrong-second\nExec=tool\n", 0600);
    fixture_file(fa, "only.desktop", "[Desktop Entry]\nType=Application\nExec=tool\nOnlyShowIn=KDE;GNOME;\n", 0600);
    fixture_file(fa, "excluded.desktop", "[Desktop Entry]\nType=Application\nExec=tool\nNotShowIn=GNOME;\n", 0600);
    fixture_file(fa, "other.desktop", "[Desktop Entry]\nType=Application\nExec=tool\nOnlyShowIn=KDE;\n", 0600);
    fixture_file(fa, "missing.desktop", "[Desktop Entry]\nType=Application\nExec=tool\nTryExec=not-executable\n", 0600);
    fixture_file(fa, "try.desktop", "[Desktop Entry]\nType=Application\nExec=tool --literal ; never-run\nTryExec=tool\n", 0600);
    fixture_file(fa, "bad.desktop", "[Desktop Entry]\nType=Application\nExec=\"unterminated\n", 0600);
    fixture_file(fa, "groups.desktop", "[Desktop Entry]\nType=Application\nExec=tool\n[Desktop Action Other]\nHidden=true\nExec=missing\n", 0600);
    char contents[16384];
    assert(snprintf(contents, sizeof(contents), "[Desktop Entry]\nType=Application\nExec=\"%s/spaced tool\" %%U\n", bin) > 0);
    fixture_file(fa, "quoted.desktop", contents, 0600);
    assert(snprintf(contents, sizeof(contents), "[Desktop Entry]\nType=Application\nExec=tool\nTryExec=%s\n", bin) > 0);
    fixture_file(fa, "directory.desktop", contents, 0600);
    fixture_file(fa, "absent.desktop", "[Desktop Entry]\nType=Application\nExec=no-such-fixture-executable\n", 0600);
    fixture_file(fa, "ignored.txt", "[Desktop Entry]\nExec=tool\n", 0600);
    char marker[4096];
    assert(snprintf(marker, sizeof(marker), "%s/must-not-exist", root) > 0);
    assert(snprintf(contents, sizeof(contents),
        "[Desktop Entry]\nType=Application\nExec=\"%s\" --helper-marker \"%s\"\n", self, marker) > 0);
    fixture_file(fa, "never-run.desktop", contents, 0600);
    char dirs[12288];
    assert(snprintf(dirs, sizeof(dirs), "relative-ignored:%s:%s", first, second) > 0);
    Fixture f = { .mode = 1 };
    LinuxStartupOptions o = options(&f);
    o.config_home = user; o.config_dirs = dirs; o.search_path = bin;
    StartupList list = linux_startup_collect(&o);
    assert(list.partial && list.status != METRIC_OK && list.count == 14);
    assert(find(&list, "never-run.desktop")->enabled);
    assert(access(marker, F_OK) != 0 && errno == ENOENT);
    for (size_t i = 0; i < list.count; i++) {
        assert(list.items[i].kind == STARTUP_DESKTOP_AUTOSTART);
        assert(!list.items[i].running_known && !list.items[i].running);
    }
    assert(!find(&list, "hidden.desktop")->enabled);
    assert(!strcmp(find(&list, "hidden.desktop")->state, "hidden"));
    assert(find(&list, "user-choice")->enabled);
    assert(find(&list, "first-choice")->enabled);
    assert(find(&list, "only.desktop")->enabled);
    assert(!find(&list, "excluded.desktop")->enabled);
    assert(!find(&list, "other.desktop")->enabled);
    assert(!find(&list, "missing.desktop")->enabled);
    assert(find(&list, "missing.desktop")->path_missing);
    assert(find(&list, "try.desktop")->enabled);
    assert(!find(&list, "bad.desktop")->enabled_known);
    assert(find(&list, "groups.desktop")->enabled);
    assert(find(&list, "quoted.desktop")->enabled && !find(&list, "quoted.desktop")->path_missing);
    assert(!find(&list, "directory.desktop")->enabled);
    assert(find(&list, "absent.desktop")->enabled && find(&list, "absent.desktop")->path_missing);
    free(list.items);
    o.desktop = "";
    list = linux_startup_collect(&o);
    assert(!find(&list, "only.desktop")->enabled);
    assert(find(&list, "excluded.desktop")->enabled);
    assert(access(marker, F_OK) != 0 && errno == ENOENT);
    free(list.items);
    for (size_t i = 0; i < file_count; i++) assert(unlink(files[i]) == 0);
    assert(rmdir(ua) == 0 && rmdir(fa) == 0 && rmdir(sa) == 0);
    assert(rmdir(user) == 0 && rmdir(first) == 0 && rmdir(second) == 0);
    assert(rmdir(bin) == 0 && rmdir(root) == 0);
}

static double seconds(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void no_children(void) {
    int status;
    errno = 0;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void test_runner(const char *self) {
    char out[128];
    const char *ok[] = {self, "--helper-ok", "literal;$(never-execute)", NULL};
    assert(linux_startup_run(ok, out, sizeof(out), 1000, NULL) == LINUX_STARTUP_COMMAND_OK);
    assert(!strcmp(out, "literal;$(never-execute)")); no_children();
    const char *fail[] = {self, "--helper-fail", NULL};
    assert(linux_startup_run(fail, out, sizeof(out), 1000, NULL) == LINUX_STARTUP_COMMAND_ERROR);
    assert(!*out); no_children();
    const char *flood[] = {self, "--helper-flood", NULL};
    assert(linux_startup_run(flood, out, sizeof(out), 1000, NULL) == LINUX_STARTUP_COMMAND_LIMIT);
    assert(!*out); no_children();
    const char *slow[] = {self, "--helper-slow", NULL};
    double start = seconds();
    assert(linux_startup_run(slow, out, sizeof(out), 80, NULL) == LINUX_STARTUP_COMMAND_TIMEOUT);
    assert(seconds() - start < 2.0 && !*out); no_children();
    const char *closed[] = {self, "--helper-closed", NULL};
    assert(linux_startup_run(closed, out, sizeof(out), 80, NULL) == LINUX_STARTUP_COMMAND_TIMEOUT);
    no_children();
    const char *missing[] = {"/no-such-loutre-fixture/systemctl", NULL};
    assert(linux_startup_run(missing, out, sizeof(out), 100, NULL) == LINUX_STARTUP_COMMAND_UNAVAILABLE);
    no_children();
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "--helper-marker")) {
            assert(argc == 3);
            FILE *marker = fopen(argv[2], "w"); assert(marker);
            assert(fclose(marker) == 0); return 0;
        }
        if (!strcmp(argv[1], "--helper-ok")) { assert(argc == 3); fputs(argv[2], stdout); return 0; }
        if (!strcmp(argv[1], "--helper-fail")) return 1;
        if (!strcmp(argv[1], "--helper-flood")) {
            for (;;) { if (write(STDOUT_FILENO, "0123456789", 10) < 0) return 1; }
        }
        if (!strcmp(argv[1], "--helper-closed")) close(STDOUT_FILENO);
        for (;;) pause();
    }
    char self[LOUTRE_PATH_MAX];
    if (argv[0][0] == '/') snprintf(self, sizeof(self), "%s", argv[0]);
    else {
        char cwd[LOUTRE_PATH_MAX]; assert(getcwd(cwd, sizeof(cwd)));
        int n = snprintf(self, sizeof(self), "%s/%s", cwd, argv[0]);
        assert(n > 0 && (size_t)n < sizeof(self));
    }
    test_services(); test_batches(); test_exec_parser(); test_desktops(self); test_runner(self);
    puts("startup fixtures: ok");
    return 0;
}
#else
#include <stdio.h>
int main(void) {
    puts("startup fixtures: skipped (Linux backend)");
    return 0;
}
#endif
