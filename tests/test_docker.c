#define _POSIX_C_SOURCE 200809L
#include "docker.h"

#include <assert.h>
#include <stddef.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *list_json =
    "[{\"Id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"Names\":[\"/w\\u0065b\"],\"Image\":\"example/web:dev\",\"State\":\"running\"},"
    "{\"Id\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
    "\"Names\":[\"/api\"],\"Image\":\"example/api:dev\",\"State\":\"running\"},"
    "{\"Id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"Names\":[\"/stopped\"],\"Image\":\"example/old\",\"State\":\"exited\"}]";

static const char *stats_one =
    "{\"cpu_stats\":{\"cpu_usage\":{\"total_usage\":100,\"percpu_usage\":[1,2]},"
    "\"system_cpu_usage\":1000,\"online_cpus\":2},"
    "\"memory_stats\":{\"usage\":8192,\"limit\":65536},"
    "\"networks\":{\"eth0\":{\"rx_bytes\":1000,\"tx_bytes\":500}}}";
static const char *stats_two =
    "{\"cpu_stats\":{\"cpu_usage\":{\"total_usage\":150,\"percpu_usage\":[1,2]},"
    "\"system_cpu_usage\":2000,\"online_cpus\":2},"
    "\"memory_stats\":{\"usage\":12288,\"limit\":65536},"
    "\"networks\":{\"eth0\":{\"rx_bytes\":5000,\"tx_bytes\":1000}}}";

static void serve_response(int fd, const char *body) {
    char response[8192];
    int length = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s", strlen(body), body);
    assert(length > 0 && (size_t)length < sizeof(response));
    size_t sent = 0;
    while (sent < (size_t)length) {
        ssize_t count = write(fd, response + sent, (size_t)length - sent);
        if (count <= 0) _exit(4);
        sent += (size_t)count;
    }
}

static void serve_not_found(int fd) {
    static const char response[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    assert(write(fd, response, sizeof(response) - 1) == (ssize_t)(sizeof(response) - 1));
}

static void serve_chunked_response(int fd, const char *body) {
    char response[8192];
    int length = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n%zx\r\n%s\r\n0\r\n\r\n",
        strlen(body), body);
    assert(length > 0 && (size_t)length < sizeof(response));
    assert(write(fd, response, (size_t)length) == length);
}

static void run_server(int listener) {
    unsigned stats_requests = 0;
    for (unsigned request = 0; request < 8; request++) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) _exit(2);
        char line[1024] = {0};
        ssize_t count = read(client, line, sizeof(line) - 1);
        if (count <= 0) _exit(3);
        const char *body;
        if (strstr(line, "GET /version ")) body = "{\"ApiVersion\":\"1.41\"}";
        else if (strstr(line, "/containers/json")) body = list_json;
        else if (strstr(line, "/containers/cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc/stats")) {
            serve_not_found(client);
            close(client);
            continue;
        } else if (strstr(line, "/stats?stream=false")) {
            body = ++stats_requests == 1 ? stats_one : stats_two;
            if (stats_requests == 1) {
                serve_chunked_response(client, body);
                close(client);
                continue;
            }
        } else _exit(5);
        serve_response(client, body);
        close(client);
    }
    close(listener);
    _exit(0);
}

int main(void) {
    char path[DOCKER_SOCKET_PATH_MAX];
    snprintf(path, sizeof(path), "/tmp/loutre-view-docker-%ld.sock", (long)getpid());
    unlink(path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
#ifdef __APPLE__
    address.sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
#endif
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind test Docker socket");
        abort();
    }
    assert(listen(listener, 8) == 0);
    pid_t server = fork();
    assert(server >= 0);
    if (server == 0) run_server(listener);
    close(listener);

    char host[DOCKER_SOCKET_PATH_MAX + 8];
    snprintf(host, sizeof(host), "unix://%s", path);
    assert(setenv("DOCKER_HOST", host, 1) == 0);
    DockerSnapshot first = collect_docker_snapshot();
    assert(first.status == METRIC_OK && first.count == 2);
    assert(strcmp(first.items[0].id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
    assert(strcmp(first.items[0].name, "web") == 0);
    assert(strcmp(first.items[0].image, "example/web:dev") == 0);
    assert(first.items[0].stats_status == METRIC_OK);
    assert(first.items[0].memory_used == 8192 && first.items[0].memory_limit == 65536);
    assert(first.items[0].network_received == 1000 && first.items[0].network_transmitted == 500);
    assert(!isfinite(first.items[0].cpu_percent));
    assert(strcmp(first.items[1].name, "api") == 0);
    assert(first.items[1].stats_status == METRIC_ERROR);

    DockerSnapshot second = collect_docker_snapshot();
    assert(second.status == METRIC_OK && second.count == 2);
    sample_docker_usage(&second, &first, 2.0);
    assert(fabs(second.items[0].cpu_percent - 10.0) < 0.001);
    assert(fabs(second.items[0].receive_rate - 2000.0) < 0.001);
    assert(fabs(second.items[0].transmit_rate - 250.0) < 0.001);

    assert(setenv("DOCKER_HOST", "tcp://127.0.0.1:2375", 1) == 0);
    DockerSnapshot remote = collect_docker_snapshot();
    assert(remote.status == METRIC_UNAVAILABLE && remote.message[0]);
    unsetenv("DOCKER_HOST");
    unlink(path);
    int child_status = 0;
    assert(waitpid(server, &child_status, 0) == server);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    puts("Docker collector tests passed");
    return 0;
}
