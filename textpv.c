/* vim: set ts=8 sw=4 sts=4 et ai: */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_RUSAGE
# include <sys/resource.h>
#endif

#define BUFFER_SIZE (128L * 1024L)

#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

struct buffer_t {
    char data[BUFFER_SIZE];
    unsigned size;
};

struct state_t {
    struct buffer_t buffers[2];
    off_t bytes;
    unsigned read_buffer;
    unsigned write_buffer;
};

struct state_t state;

static void setup_signals() {
    // something involving SIGPIPE so we can write buffers to a temp file
    // something involving SIGALRM so we can display periodic statuses
}

static void setup_state() {
    for (unsigned buf_idx = 0; buf_idx < 2; ++buf_idx) {
        state.buffers[buf_idx].size = BUFFER_SIZE;
    }
}

static void read_abort() {
    perror("read?");
    exit(4);
}

static void write_abort() {
    perror("write?");
    exit(2);
}

static int fill_one_buffer(struct buffer_t *buf) {
    unsigned off = 0;
    do {
        ssize_t size = read(
            STDIN_FILENO, buf->data + off, BUFFER_SIZE - off);
        if (unlikely(size < 0)) {
            read_abort();
            return 0;
        }
        if (unlikely(size == 0)) {
            buf->size = off;
            return 0;
        }
        off += size;
        if (likely(off == BUFFER_SIZE)) {
            return 1;
        }
    } while (1);
}

static void empty_one_buffer(struct buffer_t *buf, off_t *bytes) {
    unsigned off = 0;
    unsigned to_write = buf->size;
    assert(to_write != 0);
    do {
        ssize_t size = write(
            STDOUT_FILENO, buf->data + off, to_write - off);
        if (unlikely(size <= 0)) {
            write_abort();
            return;
        }
        off += size;
        if (likely(off == to_write)) {
            *bytes += to_write;
            return;
        }
    } while (1);
}

static void passthrough() {
    state.write_buffer = 0;
    state.read_buffer = 0;
    if (likely(fill_one_buffer(&state.buffers[state.read_buffer]))) {
        state.read_buffer = 1;
        while (likely(fill_one_buffer(&state.buffers[state.read_buffer]))) {
            empty_one_buffer(&state.buffers[state.write_buffer], &state.bytes);
            state.read_buffer = !state.read_buffer;
            state.write_buffer = !state.write_buffer;
        }
        empty_one_buffer(&state.buffers[state.write_buffer], &state.bytes);
        state.write_buffer = !state.write_buffer;
    }
    if (state.buffers[state.write_buffer].size != 0) {
        empty_one_buffer(&state.buffers[state.write_buffer], &state.bytes);
    }
}

static void finish() {
    /* Close STDIN/STDOUT so we don't fail just because we're doing
     * stuff at summary time. */
    if (close(STDOUT_FILENO) != 0) {
        perror("close(STDOUT_FILENO)");
    }
    if (close(STDIN_FILENO) != 0) {
        perror("close(STDIN_FILENO)");
    }
}

static void show_summary() {
#ifdef USE_RUSAGE
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        fprintf(stderr,
                "textpv: %zu bytes, %zu.%03zu utime, %zu.%03zu stime\n",
                state.bytes,
                ru.ru_utime.tv_sec, ru.ru_utime.tv_usec / 1000,
                ru.ru_stime.tv_sec, ru.ru_stime.tv_usec / 1000);
    } else {
        perror("getrusage");
        fprintf(stderr, "textpv: %zu bytes\n", state.bytes);
    }
#else
    fprintf(stderr, "textpv: %zu bytes\n", state.bytes);
#endif
}


int main() {
    setup_signals();
    setup_state();

    passthrough();
    finish();

    // TODO: write_last_bytes/buffers to some tempfile..
    show_summary();
    return 0;
}
