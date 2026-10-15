#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define UPDATE_INSTALLER_URL \
    "https://raw.githubusercontent.com/Mare02/LoutreView/main/install.sh"

static bool executable_file(const char *path) {
    struct stat info;
    return access(path, X_OK) == 0 && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool resolve_executable(const char *argv0, char *output, size_t capacity) {
    if (!argv0 || !*argv0) return false;
    if (strchr(argv0, '/')) {
        int written = snprintf(output, capacity, "%s", argv0);
        return written > 0 && (size_t)written < capacity && executable_file(output);
    }

    const char *path = getenv("PATH");
    if (!path) return false;
    while (true) {
        const char *separator = strchr(path, ':');
        size_t directory_length = separator ? (size_t)(separator - path) : strlen(path);
        const char *directory = path;
        if (directory_length == 0) {
            directory = ".";
            directory_length = 1;
        }
        int written = snprintf(output, capacity, "%.*s/%s", (int)directory_length,
                               directory, argv0);
        if (written > 0 && (size_t)written < capacity && executable_file(output)) return true;
        if (!separator) break;
        path = separator + 1;
    }
    return false;
}

static int wait_for_child(pid_t child) {
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("loutre-view update: wait for curl");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128;
}

static int download_installer(const char *path) {
    pid_t child = fork();
    if (child < 0) {
        perror("loutre-view update: fork");
        return -1;
    }
    if (child == 0) {
        execlp("curl", "curl", "--fail", "--silent", "--show-error", "--location",
               UPDATE_INSTALLER_URL, "--output", path, (char *)NULL);
        perror("loutre-view update: curl");
        _exit(127);
    }
    return wait_for_child(child);
}

int update_cli(const char *argv0) {
    char executable[4096];
    if (!resolve_executable(argv0, executable, sizeof(executable))) {
        fprintf(stderr, "loutre-view update: could not locate the running executable\n");
        return 1;
    }

    char *separator = strrchr(executable, '/');
    const char *install_directory = ".";
    if (separator) {
        if (separator == executable) separator[1] = '\0';
        else *separator = '\0';
        install_directory = executable;
    }

    const char *temporary_directory = getenv("TMPDIR");
    if (!temporary_directory || !*temporary_directory) temporary_directory = "/tmp";
    char installer_path[4096];
    int written = snprintf(installer_path, sizeof(installer_path),
                           "%s/%s", temporary_directory, "loutre-view-update.XXXXXX");
    if (written <= 0 || (size_t)written >= sizeof(installer_path)) {
        fprintf(stderr, "loutre-view update: temporary path is too long\n");
        return 1;
    }
    int installer_fd = mkstemp(installer_path);
    if (installer_fd < 0) {
        perror("loutre-view update: create temporary installer");
        return 1;
    }
    if (close(installer_fd) != 0) {
        perror("loutre-view update: close temporary installer");
        unlink(installer_path);
        return 1;
    }

    fprintf(stderr, "Downloading the release installer...\n");
    int status = download_installer(installer_path);
    if (status != 0) {
        if (status >= 0) fprintf(stderr, "loutre-view update: installer download failed (curl exit %d)\n", status);
        unlink(installer_path);
        return 1;
    }
    if (setenv("LOUTREVIEW_INSTALL_DIR", install_directory, 1) != 0) {
        perror("loutre-view update: set install directory");
        unlink(installer_path);
        return 1;
    }

    fprintf(stderr, "Updating loutre-view in %s...\n", install_directory);
    execl("/bin/sh", "sh", "-c",
          "trap 'rm -f -- \"$1\"' EXIT HUP INT TERM; sh \"$1\"; status=$?; exit \"$status\"",
          "loutre-view-update", installer_path, (char *)NULL);
    perror("loutre-view update: start installer");
    unlink(installer_path);
    return 1;
}
