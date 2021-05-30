/* vim: set ts=8 sw=4 sts=4 et ai: */
#if defined(USE_SPLICE)
# define _GNU_SOURCE /* splice() and F_SETPIPE_SZ */
#endif

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef USE_RUSAGE
# include <sys/resource.h>
#endif

#ifdef USE_SPLICE
/* sendfile() does not work on stdin/stdout, splice() does. */
# include <fcntl.h>
#endif

#define BUFFER_SIZE (128L * 1024L)

#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

struct buffer_t {
    off_t bytes_read;
    unsigned size;
#ifdef USE_SPLICE
    int pipe_r;
    int pipe_w;
#else
    char data[BUFFER_SIZE];
#endif
};

struct state_t {
    struct buffer_t buffers[2];
    off_t bytes_read;
    off_t bytes_written;
    unsigned read_idx;
    unsigned write_idx;
};

struct state_t state;

static void on_alarm(int);
static void on_pipe(int);

static void setup_signals() {
    struct itimerval tv;

    if (signal(SIGPIPE, on_pipe) != 0) {
        perror("textpv: signal(SIGPIPE)");
        exit(1);
    }

    if (signal(SIGALRM, on_alarm) != 0) {
        perror("textpv: signal(SIGALRM)");
        exit(1);
    }
    tv.it_value.tv_sec = tv.it_interval.tv_sec = 0;
    tv.it_value.tv_usec = tv.it_interval.tv_usec = 250000;
    if (setitimer(ITIMER_REAL, &tv, NULL) != 0) {
        perror("textpv: setitimer");
        exit(1);
    }
}

static void setup_state() {
    for (unsigned buf_idx = 0; buf_idx < 2; ++buf_idx) {
#ifdef USE_SPLICE
        int pipes[2];
        if (pipe(pipes) != 0) {
            perror("textpv: pipe");
            exit(1);
        }
        state.buffers[buf_idx].pipe_r = pipes[0];
        state.buffers[buf_idx].pipe_w = pipes[1];

        /* Note that because of the way the pages of the pipe buffer
         * are employed when data is written to the pipe, the number
         * of bytes that can be written may be less than the nominal
         * size, depending on the size of the writes. */
        /* XXX: do we need more bytes because of that? */
        if (fcntl(
                state.buffers[buf_idx].pipe_w, F_SETPIPE_SZ,
                BUFFER_SIZE) < BUFFER_SIZE) {
            perror("textpv: fcntl(F_SETPIPE_SZ)");
            exit(1);
        }
#endif
        state.buffers[buf_idx].size = BUFFER_SIZE;
    }
    state.bytes_written = 0;
    state.read_idx = 0;
    state.write_idx = 0;
}

static void read_abort() {
    int error = errno;
    if (close(STDOUT_FILENO) != 0) {
        perror("textpv: close(STDOUT_FILENO)");
    }

    errno = error;
    perror("textpv: read error");
    exit(4);
}

static void write_abort() {
    int error = errno;
    if (close(STDIN_FILENO) != 0) {
        perror("textpv: close(STDIN_FILENO)");
    }

    errno = error;
    perror("textpv: write error");
    exit(2);
}

static int fill_one_buffer(struct buffer_t *buf, off_t *bytes) {
    unsigned off = 0;
    do {
#ifdef USE_SPLICE
        ssize_t size = splice(STDIN_FILENO, NULL, buf->pipe_w, NULL,
            BUFFER_SIZE - off, SPLICE_F_MORE | SPLICE_F_MOVE);
#else
        ssize_t size = read(
            STDIN_FILENO, buf->data + off, BUFFER_SIZE - off);
#endif
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
#ifdef USE_SPLICE
        ssize_t size = splice(buf->pipe_r, NULL, STDOUT_FILENO, NULL,
            BUFFER_SIZE - off, SPLICE_F_MORE | SPLICE_F_MOVE);
#else
        ssize_t size = write(
            STDOUT_FILENO, buf->data + off, to_write - off);
#endif
        if (unlikely(size <= 0)) {
            *bytes += off;
            write_abort();
            return;
        }
        off += size;
        if (likely(off == to_write)) {
            *bytes += off;
            return;
        }
    } while (1);
}

static void passthrough() {
    if (likely(fill_one_buffer(
            &state.buffers[state.read_idx], &state.bytes_read))) {
        state.read_idx = 1;
        while (likely(fill_one_buffer(
                &state.buffers[state.read_idx], &state.bytes_read))) {
            empty_one_buffer(
                &state.buffers[state.write_idx], &state.bytes_written);
            state.read_idx = !state.read_idx;
            state.write_idx = !state.write_idx;
        }
        empty_one_buffer(
            &state.buffers[state.write_idx], &state.bytes_written);
        state.write_idx = !state.write_idx;
    }
    if (state.buffers[state.write_idx].size != 0) {
        empty_one_buffer(
            &state.buffers[state.write_idx], &state.bytes_written);
    }
}

static void finish() {
    /* Close STDIN/STDOUT so we don't fail just because we're doing
     * stuff at summary time. */
    if (close(STDOUT_FILENO) != 0) {
        perror("textpv: close(STDOUT_FILENO)");
    }
    if (close(STDIN_FILENO) != 0) {
        perror("textpv: close(STDIN_FILENO)");
    }
}

static void show_summary() {
#ifdef USE_RUSAGE
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        fprintf(
            stderr,
            "textpv: %zu bytes, %zu.%02zu utime, %zu.%02zu stime\n",
            state.bytes_written,
            ru.ru_utime.tv_sec, (ru.ru_utime.tv_usec + 5000) / 10000,
            ru.ru_stime.tv_sec, (ru.ru_stime.tv_usec + 5000) / 10000);
    } else {
        perror("textpv: getrusage");
        fprintf(stderr, "textpv: %zu bytes\n", state.bytes_written);
    }
#else
    fprintf(stderr, "textpv: %zu bytes\n", state.bytes);
#endif
}

static void on_alarm(int signum) {
    fprintf(stderr, "textpv: %zu bytes\n", state.bytes_written);
#ifdef USE_SPLICE
    assert(0); /* broken for splice; may hang */
#endif
}

static void on_pipe(int signum) {
    errno = EPIPE;
    write_abort();
    assert(0);
}


int main() {
    setup_state();
    setup_signals();

    passthrough();
    finish();

    // TODO: write_last_bytes/buffers to some tempfile..
    show_summary();
    return 0;
}
