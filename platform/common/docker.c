#define _POSIX_C_SOURCE 200809L
#include "buffer.h"
#include "docker.h"
#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DOCKER_RESPONSE_LIMIT (2u * 1024u * 1024u)
#define DOCKER_TOKEN_LIMIT 65536u
#define DOCKER_TOTAL_TIMEOUT_MS 1200
#define DOCKER_STATS_TIMEOUT_MS 3500
#define DOCKER_STATS_WORKERS 8u

typedef enum { JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE } JsonType;
typedef struct {
    size_t start, end, next, children;
    JsonType type;
} JsonToken;
typedef struct {
    const char *input;
    size_t length, position, count, capacity;
    JsonToken *tokens;
    bool failed;
} JsonParser;

static double monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int remaining_ms(double deadline) {
    double remaining = deadline - monotonic_seconds();
    if (remaining <= 0) return 0;
    if (remaining > 1200) remaining = 1200;
    return (int)(remaining * 1000.0 + 0.5);
}

static bool wait_socket(int fd, short events, double deadline) {
    for (;;) {
        int timeout = remaining_ms(deadline);
        if (timeout <= 0) return false;
        struct pollfd descriptor = { .fd = fd, .events = events };
        int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && !(descriptor.revents & (POLLERR | POLLNVAL));
    }
}

static bool send_all(int fd, const char *data, size_t length, double deadline) {
    size_t sent = 0;
    while (sent < length) {
        if (!wait_socket(fd, POLLOUT, deadline)) return false;
#ifdef MSG_NOSIGNAL
        ssize_t count = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
#else
        ssize_t count = send(fd, data + sent, length - sent, 0);
#endif
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        sent += (size_t)count;
    }
    return true;
}

static bool parse_content_length(const char *headers, size_t length, size_t *out) {
    const char *line = headers;
    const char *end = headers + length;
    while (line < end) {
        const char *next = strstr(line, "\r\n");
        if (!next || next > end) next = end;
        if ((size_t)(next - line) > 15 && !strncasecmp(line, "Content-Length:", 15)) {
            const char *value = line + 15;
            while (value < next && isspace((unsigned char)*value)) value++;
            char *parsed = NULL;
            errno = 0;
            unsigned long long n = strtoull(value, &parsed, 10);
            if (errno || parsed == value || parsed > next || n > DOCKER_RESPONSE_LIMIT) return false;
            *out = (size_t)n;
            return true;
        }
        if (next == end) break;
        line = next + 2;
    }
    return false;
}

static bool response_is_chunked(const char *headers, size_t length) {
    const char *end = headers + length;
    for (const char *p = headers; p + 18 < end; p++)
        if (!strncasecmp(p, "Transfer-Encoding:", 18)) {
            const char *line_end = strstr(p, "\r\n");
            if (!line_end || line_end > end) line_end = end;
            for (const char *q = p + 18; q + 7 <= line_end; q++)
                if (!strncasecmp(q, "chunked", 7)) return true;
        }
    return false;
}

static bool dechunk(char *body, size_t *length) {
    size_t read_at = 0, write_at = 0, total = *length;
    for (;;) {
        size_t line_end = read_at;
        while (line_end + 1 < total && !(body[line_end] == '\r' && body[line_end + 1] == '\n'))
            line_end++;
        if (line_end + 1 >= total || line_end - read_at >= 32) return false;
        char size_text[32];
        size_t size_len = line_end - read_at;
        memcpy(size_text, body + read_at, size_len);
        size_text[size_len] = '\0';
        char *extension = strchr(size_text, ';');
        if (extension) *extension = '\0';
        char *parsed = NULL;
        errno = 0;
        unsigned long chunk = strtoul(size_text, &parsed, 16);
        if (errno || parsed == size_text || *parsed || chunk > total) return false;
        read_at = line_end + 2;
        if (chunk == 0) {
            *length = write_at;
            body[write_at] = '\0';
            return true;
        }
        if ((size_t)chunk > total - read_at || total - read_at - (size_t)chunk < 2) return false;
        memmove(body + write_at, body + read_at, chunk);
        write_at += chunk;
        read_at += chunk;
        if (body[read_at] != '\r' || body[read_at + 1] != '\n') return false;
        read_at += 2;
    }
}

static MetricStatus errno_status(int value) {
    if (value == EACCES || value == EPERM) return METRIC_PERMISSION;
    if (value == ENOENT || value == ENOTDIR || value == ECONNREFUSED) return METRIC_UNAVAILABLE;
    return METRIC_ERROR;
}

static bool http_get(const char *socket_path, const char *route, double deadline,
                     char **body_out, size_t *body_length, int *status_out,
                     MetricStatus *failure_status) {
    *body_out = NULL;
    *body_length = 0;
    *status_out = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *failure_status = errno_status(errno); return false; }
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *failure_status = METRIC_ERROR; close(fd); return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(socket_path);
    if (!path_length || path_length >= sizeof(address.sun_path)) {
        *failure_status = METRIC_ERROR; close(fd); return false;
    }
#ifdef __APPLE__
    address.sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1);
#endif
    memcpy(address.sun_path, socket_path, path_length + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            *failure_status = errno_status(errno); close(fd); return false;
        }
        if (!wait_socket(fd, POLLOUT, deadline)) {
            *failure_status = METRIC_UNAVAILABLE; close(fd); return false;
        }
        int error = 0;
        socklen_t error_length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error) {
            *failure_status = errno_status(error ? error : errno); close(fd); return false;
        }
    }
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
        route);
    if (request_length < 0 || (size_t)request_length >= sizeof(request) ||
        !send_all(fd, request, (size_t)request_length, deadline)) {
        *failure_status = METRIC_UNAVAILABLE; close(fd); return false;
    }
    size_t capacity = DOCKER_RESPONSE_LIMIT + 16384;
    char *response = malloc(capacity);
    if (!response) { *failure_status = METRIC_ERROR; close(fd); return false; }
    size_t used = 0, header_end = 0, content_length = 0;
    bool have_length = false, chunked = false;
    while (used < capacity - 1) {
        if (header_end && have_length && used - header_end >= content_length) break;
        if (!wait_socket(fd, POLLIN, deadline)) {
            *failure_status = METRIC_UNAVAILABLE; free(response); close(fd); return false;
        }
        ssize_t got = recv(fd, response + used, capacity - used - 1, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (got < 0) { *failure_status = errno_status(errno); free(response); close(fd); return false; }
        if (got == 0) break;
        used += (size_t)got;
        response[used] = '\0';
        if (!header_end) {
            char *separator = strstr(response, "\r\n\r\n");
            if (separator) {
                header_end = (size_t)(separator - response) + 4;
                have_length = parse_content_length(response, header_end, &content_length);
                chunked = response_is_chunked(response, header_end);
                if (!have_length && !chunked && header_end > 16384) {
                    *failure_status = METRIC_ERROR; free(response); close(fd); return false;
                }
            } else if (used > 16384) {
                *failure_status = METRIC_ERROR; free(response); close(fd); return false;
            }
        }
        if (header_end && !chunked && have_length && used - header_end > content_length) {
            used = header_end + content_length;
            break;
        }
    }
    close(fd);
    if (!header_end || used < header_end || used == capacity - 1) {
        *failure_status = METRIC_ERROR; free(response); return false;
    }
    if (have_length && used - header_end < content_length) {
        *failure_status = METRIC_UNAVAILABLE; free(response); return false;
    }
    char *first_line_end = strstr(response, "\r\n");
    if (!first_line_end || strncmp(response, "HTTP/", 5)) {
        *failure_status = METRIC_ERROR; free(response); return false;
    }
    char *space = memchr(response, ' ', (size_t)(first_line_end - response));
    if (!space) { *failure_status = METRIC_ERROR; free(response); return false; }
    *status_out = atoi(space + 1);
    size_t response_body_length = used - header_end;
    memmove(response, response + header_end, response_body_length);
    response[response_body_length] = '\0';
    if (chunked && !dechunk(response, &response_body_length)) {
        *failure_status = METRIC_ERROR; free(response); return false;
    }
    *body_out = response;
    *body_length = response_body_length;
    return true;
}

static void skip_space(JsonParser *parser) {
    while (parser->position < parser->length &&
           isspace((unsigned char)parser->input[parser->position])) parser->position++;
}

static size_t json_add_token(JsonParser *parser, JsonType type, size_t start) {
    if (parser->count >= DOCKER_TOKEN_LIMIT) { parser->failed = true; return 0; }
    if (parser->count == parser->capacity) {
        size_t next = parser->capacity ? parser->capacity * 2 : 256;
        if (next > DOCKER_TOKEN_LIMIT) next = DOCKER_TOKEN_LIMIT;
        JsonToken *grown = realloc(parser->tokens, next * sizeof(*grown));
        if (!grown) { parser->failed = true; return 0; }
        parser->tokens = grown;
        parser->capacity = next;
    }
    size_t index = parser->count++;
    parser->tokens[index] = (JsonToken){ .start = start, .end = start, .next = index + 1,
                                         .type = type };
    return index;
}

static bool json_parse_value(JsonParser *parser, unsigned depth, size_t *index_out);

static bool json_parse_string(JsonParser *parser, size_t *index_out) {
    size_t start = parser->position++;
    size_t index = json_add_token(parser, JSON_STRING, start);
    if (parser->failed) return false;
    while (parser->position < parser->length) {
        unsigned char c = (unsigned char)parser->input[parser->position++];
        if (c == '"') {
            parser->tokens[index].end = parser->position;
            parser->tokens[index].next = parser->count;
            *index_out = index;
            return true;
        }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (parser->position >= parser->length) return false;
            char escape = parser->input[parser->position++];
            if (strchr("\"\\/bfnrt", escape)) continue;
            if (escape != 'u' || parser->length - parser->position < 4) return false;
            for (int i = 0; i < 4; i++)
                if (!isxdigit((unsigned char)parser->input[parser->position++])) return false;
        }
    }
    return false;
}

static bool json_parse_value(JsonParser *parser, unsigned depth, size_t *index_out) {
    if (depth > 64) return false;
    skip_space(parser);
    if (parser->position >= parser->length) return false;
    char c = parser->input[parser->position];
    if (c == '"') return json_parse_string(parser, index_out);
    if (c == '{' || c == '[') {
        size_t start = parser->position++;
        JsonType type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
        size_t index = json_add_token(parser, type, start);
        if (parser->failed) return false;
        skip_space(parser);
        char close = type == JSON_OBJECT ? '}' : ']';
        if (parser->position < parser->length && parser->input[parser->position] == close) {
            parser->position++;
            parser->tokens[index].end = parser->position;
            parser->tokens[index].next = parser->count;
            *index_out = index;
            return true;
        }
        for (;;) {
            size_t child;
            if (type == JSON_OBJECT) {
                skip_space(parser);
                if (parser->position >= parser->length || parser->input[parser->position] != '"' ||
                    !json_parse_string(parser, &child)) return false;
                parser->tokens[index].children++;
                skip_space(parser);
                if (parser->position >= parser->length || parser->input[parser->position++] != ':')
                    return false;
            }
            if (!json_parse_value(parser, depth + 1, &child)) return false;
            parser->tokens[index].children++;
            skip_space(parser);
            if (parser->position >= parser->length) return false;
            if (parser->input[parser->position] == close) {
                parser->position++;
                parser->tokens[index].end = parser->position;
                parser->tokens[index].next = parser->count;
                *index_out = index;
                return true;
            }
            if (parser->input[parser->position++] != ',') return false;
        }
    }
    size_t start = parser->position;
    while (parser->position < parser->length) {
        c = parser->input[parser->position];
        if (c == ',' || c == ']' || c == '}' || isspace((unsigned char)c)) break;
        parser->position++;
    }
    if (parser->position == start) return false;
    size_t index = json_add_token(parser, JSON_PRIMITIVE, start);
    if (parser->failed) return false;
    parser->tokens[index].end = parser->position;
    parser->tokens[index].next = parser->count;
    *index_out = index;
    return true;
}

static bool json_parse(const char *input, size_t length, JsonParser *parser, size_t *root) {
    *parser = (JsonParser){ .input = input, .length = length };
    bool result = json_parse_value(parser, 0, root);
    skip_space(parser);
    if (!result || parser->failed || parser->position != parser->length) {
        free(parser->tokens);
        parser->tokens = NULL;
        return false;
    }
    return true;
}

static bool json_string_copy(const JsonParser *parser, size_t index,
                             char *out, size_t capacity) {
    if (index >= parser->count || parser->tokens[index].type != JSON_STRING || !capacity)
        return false;
    size_t read_at = parser->tokens[index].start + 1;
    size_t end = parser->tokens[index].end - 1;
    size_t used = 0;
    while (read_at < end) {
        unsigned char c = (unsigned char)parser->input[read_at++];
        if (c == '\\') {
            char escape = parser->input[read_at++];
            if (escape == 'u') {
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = parser->input[read_at++];
                    code = code * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' :
                        tolower((unsigned char)h) - 'a' + 10);
                }
                if (code >= 0xd800 && code <= 0xdbff && read_at + 6 <= end &&
                    parser->input[read_at] == '\\' && parser->input[read_at + 1] == 'u') {
                    read_at += 2;
                    unsigned low = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = parser->input[read_at++];
                        low = low * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' :
                            tolower((unsigned char)h) - 'a' + 10);
                    }
                    if (low >= 0xdc00 && low <= 0xdfff)
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    else code = 0xfffd;
                } else if (code >= 0xdc00 && code <= 0xdfff) code = 0xfffd;
                unsigned char bytes[4];
                size_t byte_count;
                if (code <= 0x7f) { bytes[0] = (unsigned char)code; byte_count = 1; }
                else if (code <= 0x7ff) {
                    bytes[0] = (unsigned char)(0xc0 | (code >> 6));
                    bytes[1] = (unsigned char)(0x80 | (code & 0x3f)); byte_count = 2;
                } else if (code <= 0xffff) {
                    bytes[0] = (unsigned char)(0xe0 | (code >> 12));
                    bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
                    bytes[2] = (unsigned char)(0x80 | (code & 0x3f)); byte_count = 3;
                } else {
                    bytes[0] = (unsigned char)(0xf0 | (code >> 18));
                    bytes[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
                    bytes[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
                    bytes[3] = (unsigned char)(0x80 | (code & 0x3f)); byte_count = 4;
                }
                if (byte_count >= capacity - used) return false;
                memcpy(out + used, bytes, byte_count); used += byte_count;
                continue;
            }
            if (escape == 'b') c = '\b'; else if (escape == 'f') c = '\f';
            else if (escape == 'n') c = '\n'; else if (escape == 'r') c = '\r';
            else if (escape == 't') c = '\t'; else c = (unsigned char)escape;
        }
        if (used + 1 >= capacity) return false;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return true;
}

static bool json_equal(const JsonParser *parser, size_t index, const char *value) {
    char text[128];
    return json_string_copy(parser, index, text, sizeof(text)) && strcmp(text, value) == 0;
}

static size_t json_object_get(const JsonParser *parser, size_t object, const char *key) {
    if (object >= parser->count || parser->tokens[object].type != JSON_OBJECT) return SIZE_MAX;
    size_t end = parser->tokens[object].next;
    for (size_t i = object + 1; i + 1 < end;) {
        size_t key_index = i, value_index = i + 1;
        if (json_equal(parser, key_index, key)) return value_index;
        i = parser->tokens[value_index].next;
    }
    return SIZE_MAX;
}

static bool json_number(const JsonParser *parser, size_t index, double *out) {
    if (index >= parser->count || parser->tokens[index].type != JSON_PRIMITIVE) return false;
    size_t length = parser->tokens[index].end - parser->tokens[index].start;
    if (!length || length >= 64) return false;
    char text[64];
    memcpy(text, parser->input + parser->tokens[index].start, length);
    text[length] = '\0';
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || end != text + length || !isfinite(value)) return false;
    *out = value;
    return true;
}

static bool json_u64(const JsonParser *parser, size_t index, unsigned long long *out) {
    double value;
    if (!json_number(parser, index, &value) || value < 0 || value > 18446744073709551615.0)
        return false;
    size_t length = parser->tokens[index].end - parser->tokens[index].start;
    char text[64];
    if (length >= sizeof(text)) return false;
    memcpy(text, parser->input + parser->tokens[index].start, length); text[length] = '\0';
    if (strchr(text, '.') || strchr(text, 'e') || strchr(text, 'E')) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno || end != text + length) return false;
    *out = number;
    return true;
}

static bool object_u64(const JsonParser *parser, size_t object, const char *key,
                       unsigned long long *out) {
    return json_u64(parser, json_object_get(parser, object, key), out);
}

static bool object_string(const JsonParser *parser, size_t object, const char *key,
                          char *out, size_t capacity) {
    return json_string_copy(parser, json_object_get(parser, object, key), out, capacity);
}

static size_t array_first(const JsonParser *parser, size_t array) {
    if (array >= parser->count || parser->tokens[array].type != JSON_ARRAY ||
        parser->tokens[array].children == 0) return SIZE_MAX;
    return array + 1;
}

static size_t array_count(const JsonParser *parser, size_t array) {
    if (array >= parser->count || parser->tokens[array].type != JSON_ARRAY) return 0;
    return parser->tokens[array].children;
}

static void json_parser_free(JsonParser *parser) {
    free(parser->tokens);
    parser->tokens = NULL;
}

static bool valid_container_id(const char *id) {
    size_t length = strlen(id);
    if (length < 12 || length > 64) return false;
    for (size_t i = 0; i < length; i++)
        if (!isxdigit((unsigned char)id[i])) return false;
    return true;
}

static bool parse_api_version(const char *body, size_t length, char *version, size_t capacity) {
    JsonParser parser;
    size_t root;
    if (!json_parse(body, length, &parser, &root)) return false;
    bool result = object_string(&parser, root, "ApiVersion", version, capacity);
    json_parser_free(&parser);
    if (!result) return false;
    bool dot = false;
    for (const char *p = version; *p; p++) {
        if (*p == '.') { if (dot) return false; dot = true; }
        else if (!isdigit((unsigned char)*p)) return false;
    }
    return dot;
}

static bool parse_container_list(const char *body, size_t length, DockerSnapshot *snapshot) {
    JsonParser parser;
    size_t root;
    if (!json_parse(body, length, &parser, &root) || parser.tokens[root].type != JSON_ARRAY) {
        if (parser.tokens) json_parser_free(&parser);
        return false;
    }
    for (size_t item = array_first(&parser, root); item != SIZE_MAX &&
         item < parser.tokens[root].next; item = parser.tokens[item].next) {
        if (parser.tokens[item].type != JSON_OBJECT) continue;
        char state[32] = "";
        (void)object_string(&parser, item, "State", state, sizeof(state));
        if (strcmp(state, "running") != 0) continue;
        if (snapshot->count >= MAX_DOCKER_CONTAINERS) { snapshot->truncated = true; break; }
        DockerContainer container = { .cpu_percent = NAN, .receive_rate = NAN,
                                      .transmit_rate = NAN, .stats_status = METRIC_UNAVAILABLE };
        if (!object_string(&parser, item, "Id", container.id, sizeof(container.id)) ||
            !valid_container_id(container.id)) continue;
        (void)object_string(&parser, item, "Image", container.image, sizeof(container.image));
        size_t names = json_object_get(&parser, item, "Names");
        size_t name = array_first(&parser, names);
        if (name != SIZE_MAX) (void)json_string_copy(&parser, name, container.name, sizeof(container.name));
        if (container.name[0] == '/') memmove(container.name, container.name + 1, strlen(container.name));
        if (!container.name[0]) snprintf(container.name, sizeof(container.name), "%.12s", container.id);
        snapshot->items[snapshot->count++] = container;
    }
    json_parser_free(&parser);
    return true;
}

static unsigned parse_networks(const JsonParser *parser, size_t networks,
                               unsigned long long *received, unsigned long long *transmitted) {
    if (networks >= parser->count || parser->tokens[networks].type != JSON_OBJECT) return 0;
    unsigned count = 0;
    size_t end = parser->tokens[networks].next;
    for (size_t key = networks + 1; key + 1 < end;) {
        size_t iface = key + 1;
        unsigned long long rx, tx;
        if (object_u64(parser, iface, "rx_bytes", &rx) &&
            object_u64(parser, iface, "tx_bytes", &tx)) {
            *received = ULLONG_MAX - *received < rx ? ULLONG_MAX : *received + rx;
            *transmitted = ULLONG_MAX - *transmitted < tx ? ULLONG_MAX : *transmitted + tx;
            count++;
        }
        key = parser->tokens[iface].next;
    }
    return count;
}

static bool parse_stats(const char *body, size_t length, DockerContainer *container) {
    JsonParser parser;
    size_t root;
    if (!json_parse(body, length, &parser, &root) || parser.tokens[root].type != JSON_OBJECT) {
        if (parser.tokens) json_parser_free(&parser);
        return false;
    }
    size_t cpu = json_object_get(&parser, root, "cpu_stats");
    size_t usage = json_object_get(&parser, cpu, "cpu_usage");
    (void)object_u64(&parser, usage, "total_usage", &container->cpu_total);
    (void)object_u64(&parser, cpu, "system_cpu_usage", &container->cpu_system);
    unsigned long long online = 0;
    (void)object_u64(&parser, cpu, "online_cpus", &online);
    size_t per_cpu = json_object_get(&parser, usage, "percpu_usage");
    container->cpu_count = online > 0 && online <= UINT_MAX ? (unsigned)online :
                           (unsigned)array_count(&parser, per_cpu);

    size_t memory = json_object_get(&parser, root, "memory_stats");
    bool have_memory = object_u64(&parser, memory, "usage", &container->memory_used) &&
                       object_u64(&parser, memory, "limit", &container->memory_limit);
    size_t networks = json_object_get(&parser, root, "networks");
    unsigned network_count = parse_networks(&parser, networks, &container->network_received,
                                            &container->network_transmitted);
    (void)network_count; /* A container without a network reports zero traffic. */
    json_parser_free(&parser);
    return have_memory;
}

static bool api_get(const char *socket_path, const char *route, double deadline,
                    char **body, size_t *length, MetricStatus *status);

typedef struct {
    DockerContainer *container;
    const char *socket_path;
    const char *version;
    double deadline;
} DockerStatsJob;

static void *collect_container_stats(void *argument) {
    DockerStatsJob *job = argument;
    DockerContainer *container = job->container;
    char route[128];
    int written = snprintf(route, sizeof(route), "/v%s/containers/%s/stats?stream=false",
                           job->version, container->id);
    if (written < 0 || (size_t)written >= sizeof(route)) {
        container->stats_status = METRIC_ERROR;
        return NULL;
    }
    char *body = NULL;
    size_t length = 0;
    MetricStatus failure = METRIC_UNAVAILABLE;
    if (!api_get(job->socket_path, route, job->deadline, &body, &length, &failure)) {
        container->stats_status = failure;
        return NULL;
    }
    container->stats_status = parse_stats(body, length, container) ? METRIC_OK : METRIC_ERROR;
    free(body);
    return NULL;
}

static void collect_container_stats_batch(DockerSnapshot *snapshot, const char *socket_path,
                                          const char *version, size_t start, size_t count) {
    DockerStatsJob jobs[DOCKER_STATS_WORKERS];
    pthread_t threads[DOCKER_STATS_WORKERS];
    bool started[DOCKER_STATS_WORKERS] = {0};
    size_t batch_count = count < DOCKER_STATS_WORKERS ? count : DOCKER_STATS_WORKERS;
    double deadline = monotonic_seconds() + (double)DOCKER_STATS_TIMEOUT_MS / 1000.0;
    for (size_t i = 0; i < batch_count; i++) {
        jobs[i] = (DockerStatsJob){ .container = &snapshot->items[start + i],
                                    .socket_path = socket_path, .version = version,
                                    .deadline = deadline };
        if (pthread_create(&threads[i], NULL, collect_container_stats, &jobs[i]) == 0) {
            started[i] = true;
        } else {
            (void)collect_container_stats(&jobs[i]);
        }
    }
    for (size_t i = 0; i < batch_count; i++)
        if (started[i]) (void)pthread_join(threads[i], NULL);
}

static bool api_get(const char *socket_path, const char *route, double deadline,
                    char **body, size_t *length, MetricStatus *status) {
    int http_status = 0;
    if (!http_get(socket_path, route, deadline, body, length, &http_status, status)) return false;
    if (http_status < 200 || http_status >= 300) {
        free(*body); *body = NULL; *length = 0;
        *status = http_status == 401 || http_status == 403 ? METRIC_PERMISSION : METRIC_ERROR;
        return false;
    }
    return true;
}

static bool resolve_socket(char *path, size_t capacity, MetricStatus *status) {
    const char *host = getenv("DOCKER_HOST");
    if (host && *host) {
        if (strncmp(host, "unix://", 7) != 0) {
            *status = METRIC_UNAVAILABLE;
            return false;
        }
        host += 7;
        if (!*host || strlen(host) >= capacity) { *status = METRIC_ERROR; return false; }
        snprintf(path, capacity, "%s", host);
        return true;
    }
    char candidates[8][DOCKER_SOCKET_PATH_MAX];
    size_t count = platform_docker_socket_paths(candidates, 8);
    MetricStatus best = METRIC_UNAVAILABLE;
    for (size_t i = 0; i < count; i++) {
        double deadline = monotonic_seconds() + 0.2;
        char *body = NULL; size_t length = 0; int http_status = 0;
        MetricStatus failed = METRIC_UNAVAILABLE;
        if (http_get(candidates[i], "/version", deadline, &body, &length,
                     &http_status, &failed) && http_status >= 200 && http_status < 300) {
            free(body);
            if (buffer_copy(path, capacity, candidates[i])) return true;
            best = METRIC_ERROR;
        }
        if (failed == METRIC_PERMISSION) best = METRIC_PERMISSION;
        free(body);
    }
    *status = best;
    return false;
}

DockerSnapshot collect_docker_snapshot(void) {
    DockerSnapshot snapshot = { .status = METRIC_UNAVAILABLE };
    const char *configured_host = getenv("DOCKER_HOST");
    if (configured_host && *configured_host && strncmp(configured_host, "unix://", 7) != 0) {
        snprintf(snapshot.message, sizeof(snapshot.message),
                 "Remote Docker endpoints are not supported by this view.");
        return snapshot;
    }
    char socket_path[DOCKER_SOCKET_PATH_MAX];
    MetricStatus failure = METRIC_UNAVAILABLE;
    if (!resolve_socket(socket_path, sizeof(socket_path), &failure)) {
        snapshot.status = failure;
        return snapshot;
    }
    double deadline = monotonic_seconds() + (double)DOCKER_TOTAL_TIMEOUT_MS / 1000.0;
    char *body = NULL; size_t length = 0;
    if (!api_get(socket_path, "/version", deadline, &body, &length, &failure)) {
        snapshot.status = failure; return snapshot;
    }
    char version[16];
    bool version_ok = parse_api_version(body, length, version, sizeof(version));
    free(body); body = NULL;
    if (!version_ok) { snapshot.status = METRIC_ERROR; return snapshot; }
    char route[128];
    snprintf(route, sizeof(route), "/v%s/containers/json?all=0", version);
    if (!api_get(socket_path, route, deadline, &body, &length, &failure)) {
        snapshot.status = failure; return snapshot;
    }
    bool list_ok = parse_container_list(body, length, &snapshot);
    free(body);
    if (!list_ok) { snapshot.status = METRIC_ERROR; return snapshot; }
    snapshot.status = METRIC_OK;
    for (size_t start = 0; start < snapshot.count; start += DOCKER_STATS_WORKERS) {
        size_t remaining = snapshot.count - start;
        collect_container_stats_batch(&snapshot, socket_path, version, start, remaining);
    }
    return snapshot;
}

void sample_docker_usage(DockerSnapshot *current, const DockerSnapshot *previous,
                         double elapsed_seconds) {
    if (!current || !previous || elapsed_seconds <= 0 || !isfinite(elapsed_seconds)) return;
    for (size_t i = 0; i < current->count; i++) {
        DockerContainer *now = &current->items[i];
        if (now->stats_status != METRIC_OK) continue;
        for (size_t j = 0; j < previous->count; j++) {
            const DockerContainer *before = &previous->items[j];
            if (before->stats_status != METRIC_OK || strcmp(now->id, before->id) != 0) continue;
            if (now->cpu_total >= before->cpu_total && now->cpu_system >= before->cpu_system) {
                unsigned long long cpu_delta = now->cpu_total - before->cpu_total;
                unsigned long long system_delta = now->cpu_system - before->cpu_system;
                if (system_delta && now->cpu_count)
                    now->cpu_percent = 100.0 * (double)cpu_delta / (double)system_delta *
                                       (double)now->cpu_count;
            }
            if (now->network_received >= before->network_received)
                now->receive_rate = (double)(now->network_received - before->network_received) /
                                    elapsed_seconds;
            if (now->network_transmitted >= before->network_transmitted)
                now->transmit_rate = (double)(now->network_transmitted - before->network_transmitted) /
                                     elapsed_seconds;
            break;
        }
    }
}
