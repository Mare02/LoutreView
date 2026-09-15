#ifndef LOUTRE_BUFFER_H
#define LOUTRE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { BUFFER_OK, BUFFER_LIMIT, BUFFER_ERROR } BufferResult;

/* Copy a string into a NUL-terminated buffer. False means it was truncated. */
bool buffer_copy(char *destination, size_t capacity, const char *source);

/* Allocate zeroed storage, rejecting multiplication overflow. */
bool buffer_calloc(void **storage, size_t count, size_t item_size);

/* Grow an array geometrically up to a fixed item limit. */
bool buffer_reserve(void **storage, size_t *capacity, size_t required,
                    size_t item_size, size_t initial_capacity, size_t limit);

/* Append bytes while retaining one byte for a trailing NUL. */
BufferResult buffer_append(char *destination, size_t capacity, size_t *used,
                           const void *source, size_t length);

/* Read a complete file descriptor into a NUL-terminated fixed-size buffer. */
BufferResult buffer_read_fd(int fd, char *destination, size_t capacity, size_t *used);

#endif
