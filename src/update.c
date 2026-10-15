#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include "version.h"
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
#define LATEST_RELEASE_URL \
    "https://api.github.com/repos/Mare02/LoutreView/releases/latest"
#define RELEASE_INFO_CAPACITY 65536

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

static int download_url(const char *url, const char *path) {
    pid_t child = fork();
    if (child < 0) {
        perror("loutre-view update: fork");
        return -1;
    }
    if (child == 0) {
        execlp("curl", "curl", "--fail", "--silent", "--show-error", "--location",
               url, "--output", path, (char *)NULL);
        perror("loutre-view update: curl");
        _exit(127);
    }
    return wait_for_child(child);
}

static bool read_release_tag(const char *path, char *version, size_t capacity) {
    char json[RELEASE_INFO_CAPACITY];
    FILE *file = fopen(path, "r");
    if (!file) return false;
    size_t length = fread(json, 1, sizeof(json) - 1, file);
    bool read_ok = !ferror(file) && feof(file);
    fclose(file);
    if (!read_ok) return false;
    json[length] = '\0';

    const char *tag = strstr(json, "\"tag_name\"");
    if (!tag || !(tag = strchr(tag, ':'))) return false;
    tag++;
    while (*tag == ' ' || *tag == '\t' || *tag == '\r' || *tag == '\n') tag++;
    if (*tag++ != '"' || *tag++ != 'v') return false;
    const char *end = strchr(tag, '"');
    if (!end || end == tag) return false;
    size_t version_length = (size_t)(end - tag);
    if (version_length >= capacity) return false;
    memcpy(version, tag, version_length);
    version[version_length] = '\0';
    return true;
}

static bool parse_version(const char *version, unsigned long parts[3]) {
    const char *cursor = version;
    for (size_t i = 0; i < 3; i++) {
        if (*cursor < '0' || *cursor > '9') return false;
        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(cursor, &end, 10);
        if (errno || end == cursor) return false;
        parts[i] = value;
        if (i < 2) {
            if (*end != '.') return false;
            cursor = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    return true;
}

static int compare_versions(const char *left, const char *right, bool *valid) {
    unsigned long left_parts[3], right_parts[3];
    if (!parse_version(left, left_parts) || !parse_version(right, right_parts)) {
        *valid = false;
        return 0;
    }
    *valid = true;
    for (size_t i = 0; i < 3; i++) {
        if (left_parts[i] < right_parts[i]) return -1;
        if (left_parts[i] > right_parts[i]) return 1;
    }
    return 0;
}

static bool create_temp_file(const char *directory, const char *name,
                             char *path, size_t capacity) {
    int written = snprintf(path, capacity, "%s/%s.XXXXXX", directory, name);
    if (written <= 0 || (size_t)written >= capacity) return false;
    int fd = mkstemp(path);
    if (fd < 0) return false;
    if (close(fd) == 0) return true;
    unlink(path);
    return false;
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
    char release_info_path[4096], installer_path[4096];
    if (!create_temp_file(temporary_directory, "loutre-view-release", release_info_path,
                          sizeof(release_info_path))) {
        perror("loutre-view update: create temporary release info");
        return 1;
    }
    if (!create_temp_file(temporary_directory, "loutre-view-installer", installer_path,
                          sizeof(installer_path))) {
        perror("loutre-view update: create temporary installer");
        unlink(release_info_path);
        return 1;
    }

    fprintf(stderr, "Checking the latest release...\n");
    int status = download_url(LATEST_RELEASE_URL, release_info_path);
    if (status != 0) {
        if (status >= 0) fprintf(stderr, "loutre-view update: release check failed (curl exit %d)\n", status);
        unlink(release_info_path);
        unlink(installer_path);
        return 1;
    }

    char latest_version[64];
    if (!read_release_tag(release_info_path, latest_version, sizeof(latest_version))) {
        fprintf(stderr, "loutre-view update: could not read the latest release version\n");
        unlink(release_info_path);
        unlink(installer_path);
        return 1;
    }
    unlink(release_info_path);

    bool versions_valid;
    int version_order = compare_versions(VERSION, latest_version, &versions_valid);
    if (!versions_valid) {
        fprintf(stderr, "loutre-view update: unsupported version format\n");
        unlink(installer_path);
        return 1;
    }
    if (version_order == 0) {
        printf("loutre-view %s is already up to date.\n", VERSION);
        unlink(installer_path);
        return 0;
    }
    if (version_order > 0) {
        printf("Installed loutre-view %s is newer than latest release %s.\n",
               VERSION, latest_version);
        unlink(installer_path);
        return 0;
    }

    fprintf(stderr, "Downloading the release installer...\n");
    status = download_url(UPDATE_INSTALLER_URL, installer_path);
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
