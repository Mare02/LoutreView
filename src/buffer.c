#include "buffer.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool buffer_copy(char *destination, size_t capacity, const char *source) {
    if (!destination || !capacity || !source) return false;
    size_t length = strlen(source);
    bool complete = length < capacity;
    if (!complete) length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
    return complete;
}

bool buffer_calloc(void **storage, size_t count, size_t item_size) {
    if (!storage || !item_size || count > SIZE_MAX / item_size) return false;
    void *allocated = calloc(count, item_size);
    if (!allocated && count) return false;
    *storage = allocated;
    return true;
}

bool buffer_reserve(void **storage, size_t *capacity, size_t required,
                    size_t item_size, size_t initial_capacity, size_t limit) {
    if (!storage || !capacity || !item_size || !initial_capacity || required > limit)
        return false;
    if (required <= *capacity) return true;
    size_t next = *capacity ? *capacity : initial_capacity;
    while (next < required) {
        if (next >= limit) return false;
        next = next > limit / 2 ? limit : next * 2;
    }
    if (next > SIZE_MAX / item_size) return false;
    void *grown = realloc(*storage, next * item_size);
    if (!grown) return false;
    *storage = grown;
    *capacity = next;
    return true;
}

BufferResult buffer_append(char *destination, size_t capacity, size_t *used,
                           const void *source, size_t length) {
    if (!destination || !used || !source || !capacity || *used >= capacity ||
        length >= capacity - *used) return BUFFER_LIMIT;
    memcpy(destination + *used, source, length);
    *used += length;
    destination[*used] = '\0';
    return BUFFER_OK;
}

BufferResult buffer_read_fd(int fd, char *destination, size_t capacity, size_t *used) {
    if (fd < 0 || !destination || !used || capacity < 2) return BUFFER_ERROR;
    *used = 0;
    destination[0] = '\0';
    while (*used < capacity - 1) {
        ssize_t read_count = read(fd, destination + *used, capacity - 1 - *used);
        if (read_count == 0) return BUFFER_OK;
        if (read_count < 0) {
            if (errno == EINTR) continue;
            return BUFFER_ERROR;
        }
        *used += (size_t)read_count;
        destination[*used] = '\0';
    }
    return BUFFER_LIMIT;
}
