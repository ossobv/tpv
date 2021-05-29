/* vim: set ts=8 sw=4 sts=4 et ai: */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE (32L * 1024L)

#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

struct buffer_t {
    char data[BUFFER_SIZE];
    unsigned size;
};

struct state_t {
    struct buffer_t buffers[2];
};

struct state_t state;

static void setup_signals() {
    // something involving SIGPIPE so we can write buffers to a temp file
    // something involving SIGALRM so we can display periodic statuses
}

static void setup_state() {
    state.buffers[0].size = BUFFER_SIZE;
    state.buffers[1].size = BUFFER_SIZE;
}

static void read_abort() {
    fprintf(stderr, "read error\n");
    exit(4);
}

static void write_abort() {
    fprintf(stderr, "write error\n");
    exit(2);
}

static int fill_one_buffer(unsigned buf_idx) {
    unsigned off = 0;
    do {
        ssize_t size = read(
            STDIN_FILENO, state.buffers[buf_idx].data + off,
            BUFFER_SIZE - off);
        if (unlikely(size < 0)) {
            read_abort();
            return 0;
        }
        if (unlikely(size == 0)) {
            state.buffers[buf_idx].size = off;
            return 0;
        }
        off += size;
        if (likely(off == BUFFER_SIZE)) {
            return 1;
        }
    } while (1);
}

static void empty_one_buffer(unsigned buf_idx) {
    unsigned off = 0;
    unsigned to_write = state.buffers[buf_idx].size;
    assert(to_write != 0);
    do {
        ssize_t size = write(
            STDOUT_FILENO, state.buffers[buf_idx].data + off,
            to_write - off);
        if (unlikely(size <= 0)) {
            write_abort();
            return;
        }
        off += size;
        if (likely(off == to_write)) {
            return;
        }
    } while (1);
}

static void feed() {
    unsigned write_buffer = 0;
    if (likely(fill_one_buffer(0))) {
        unsigned read_buffer = 1;
        while (likely(fill_one_buffer(read_buffer))) {
            empty_one_buffer(write_buffer);
            read_buffer = !read_buffer;
            write_buffer = !write_buffer;
        }
        empty_one_buffer(write_buffer);
        write_buffer = !write_buffer;
    }
    if (state.buffers[write_buffer].size != 0) {
        empty_one_buffer(write_buffer);
    }
}

int main() {
    setup_signals();
    setup_state();
    feed();
    // write_last_bytes/buffers to some tempfile..
    // write rusage
    return 0;
}
