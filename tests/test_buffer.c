#include "buffer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char text[5];
    assert(buffer_copy(text, sizeof(text), "test"));
    assert(!buffer_copy(text, sizeof(text), "tests"));
    assert(!strcmp(text, "test"));

    int *items = NULL;
    size_t capacity = 0;
    assert(buffer_reserve((void **)&items, &capacity, 3, sizeof(*items), 2, 4));
    assert(capacity == 4);
    assert(!buffer_reserve((void **)&items, &capacity, 5, sizeof(*items), 2, 4));
    free(items);

    char output[5] = "";
    size_t used = 0;
    assert(buffer_append(output, sizeof(output), &used, "test", 4) == BUFFER_OK);
    assert(buffer_append(output, sizeof(output), &used, "x", 1) == BUFFER_LIMIT);
    assert(!strcmp(output, "test"));

    int pipefd[2];
    assert(pipe(pipefd) == 0);
    assert(write(pipefd[1], "buf", 3) == 3);
    close(pipefd[1]);
    assert(buffer_read_fd(pipefd[0], output, sizeof(output), &used) == BUFFER_OK);
    close(pipefd[0]);
    assert(used == 3 && !strcmp(output, "buf"));
    puts("Bounded buffer tests passed");
    return 0;
}
