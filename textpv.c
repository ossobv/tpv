/* vim: set ts=8 sw=4 sts=4 et ai: */
#if defined(USE_SPLICE)
# define _GNU_SOURCE /* splice() and F_SETPIPE_SZ */
#endif

#include <assert.h>
#include <errno.h>
#include <poll.h>
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

#define CSI_EL "\x1b[K" /* erase from cursor to end of line */

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

enum print_format {
    P_SHORT,
    P_FULL
};

static void on_alarm(int);
static void on_pipe(int);

static void setup_signals() {
    struct itimerval tv;

    if (signal(SIGPIPE, on_pipe) != 0) {
        perror(CSI_EL "textpv: signal(SIGPIPE)");
        exit(1);
    }

    if (signal(SIGALRM, on_alarm) != 0) {
        perror(CSI_EL "textpv: signal(SIGALRM)");
        exit(1);
    }
    tv.it_value.tv_sec = tv.it_interval.tv_sec = 0;
    tv.it_value.tv_usec = tv.it_interval.tv_usec = 999999;
    if (setitimer(ITIMER_REAL, &tv, NULL) != 0) {
        perror(CSI_EL "textpv: setitimer");
        exit(1);
    }
}

static void setup_state() {
    for (unsigned buf_idx = 0; buf_idx < 2; ++buf_idx) {
#ifdef USE_SPLICE
        int pipes[2];
        if (isatty(STDOUT_FILENO)) {
            fprintf(
                stderr, CSI_EL "textpv: splice() will not work on stdout; "
                "please pipe this to something\n");
            exit(1);
        }
        if (pipe(pipes) != 0) {
            perror(CSI_EL "textpv: pipe");
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
            perror(CSI_EL "textpv: fcntl(F_SETPIPE_SZ)");
            exit(1);
        }
#endif
        state.buffers[buf_idx].size = BUFFER_SIZE;
    }
    state.bytes_written = 0;
    state.read_idx = 0;
    state.write_idx = 0;
}

#ifdef USE_SPLICE
static unsigned safe_read(int fd, char *dest, ssize_t size) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    unsigned off = 0;
    while (size) {
        ssize_t ret;
        int pollret;

        /* This MUST succeed if there is data in the pipe. If we're only
         * reading a partial pipe, then we would be reordering data. And
         * that would be bad. */
        do {
            pollret = poll(&pfd, 1, 0);
            if (unlikely(pollret < 0) && errno != EINTR) {
                perror(CSI_EL "textpv: poll");
                exit(1);
            }
        } while (pollret < 0);
        if (pollret == 0) {
            /* Aborting sooner than expected. Maybe someone has read the
             * pipe already. The worst thing we can do is block at this
             * point. Return now. */
            return off;
        }

        ret = read(fd, dest, size);
        if (likely(size == ret)) {
            off += (unsigned)ret;
            return off;
        }
        if (unlikely(ret <= 0)) {
            perror(CSI_EL "textpv: safe_read");
            exit(1);
        }
        size -= ret;
        off += (unsigned)ret;
    }
    return off;
}

static void safe_write(int fd, const char *source, ssize_t size) {
    while (size) {
        ssize_t ret = write(fd, source, size);
        if (likely(size == ret)) {
            return;
        }
        if (unlikely(ret <= 0)) {
            perror(CSI_EL "textpv: safe_write");
            exit(1);
        }
        size -= ret;
    }
}

/**
 * pipe_peek copies the data into a buffer and feeds it back into the
 * pipe, making it a convoluted "peek" operation.
 *
 * This only works if 'size' is equal to the entire buffer in the size.
 * If there was more data in the pipe, we would now have reordered it.
 */
static unsigned pipe_peek(int pipe_r, int pipe_w, char *buf, unsigned size) {
    unsigned read = safe_read(pipe_r, buf, size);
    if (read) {
        safe_write(pipe_w, buf, read);
    }
    return read;
}
#endif

static void debug_all_data(
        enum print_format pfmt, const char *data, unsigned size) {
    unsigned lfs = 0;
    unsigned first = 1;
    unsigned trimmed;

    const char *p = data;
    const char *pe = data + size;
    while (p != pe) {
        lfs += (*p++ == '\n');
    }
    p = data;
    while (p != pe) {
        const char *lf = p;
        unsigned linelen;
        while (lf != pe) {
            if (*lf++ == '\n')
                break;
        }
        --lf; /* at LF or at (pe - 1) */
        linelen = (lf - p);
        // TODO: no printing of control chars..
        // TODO: print special if lf == pe
        if (linelen > 72) {
            trimmed = 1;
            linelen = 72;
        } else {
            trimmed = 0;
        }
        // TODO: only print if first??
        // TODO: this is problematic, as a mysql dump INSERT line can
        // easily span >1MB. either we need more buffer, or we'll want
        // to print some values in between anyway.
        if (!first || pfmt == P_FULL) {
            fprintf(
                stderr, CSI_EL "textpv: > %s%.*s%s\n", (first ? "... " : ""),
                linelen, p, (trimmed ? " ..." : ""));
        }
        p = lf + 1;
        first = 0;
    }
}

static const char *human_speed(off_t bps) {
    static char buf[20];
    if (bps > (1024L * 1024L * 1024L)) {
        snprintf(buf, 20, "%.1f GiB/s", ((double)bps) / 1024 / 1024 / 1024);
    } else if (bps > (1024L * 1024L)) {
        snprintf(buf, 20, "%.1f MiB/s", ((double)bps) / 1024 / 1024);
    } else if (bps > (1024L)) {
        snprintf(buf, 20, "%.1f KiB/s", ((double)bps) / 1024);
    } else {
        snprintf(buf, 20, "%zd B/s", bps);
    }
    return buf;
}

static void print_read_write_state(enum print_format pfmt) {
    static off_t last_written;
    static struct timeval tprev;

    const struct buffer_t *buf0, *buf1; /* but the FDs are mutable */
    char data[BUFFER_SIZE * 2];
    unsigned size;

    if (tprev.tv_sec == 0 && tprev.tv_usec == 0) {
        gettimeofday(&tprev, NULL); /* assume this cannot fail */
        assert(tprev.tv_sec != 0 || tprev.tv_usec != 0);
    }

    if (unlikely(state.buffers[0].bytes_read == state.buffers[1].bytes_read)) {
        fprintf(stderr, CSI_EL "textpv: nothing written yet\n");
        return;
    }
    if (state.buffers[0].bytes_read > state.buffers[1].bytes_read) {
        buf0 = &state.buffers[1];
        buf1 = &state.buffers[0];
    } else {
        buf0 = &state.buffers[0];
        buf1 = &state.buffers[1];
    }
#ifdef USE_SPLICE
    size = pipe_peek(buf0->pipe_r, buf0->pipe_w, data, buf0->size);
    size += pipe_peek(buf1->pipe_r, buf1->pipe_w, data + size, buf1->size);
#else
    memcpy(data, buf0->data, buf0->size);
    memcpy(data + buf0->size, buf1->data, buf1->size);
    size = buf0->size + buf1->size;
#endif
    /* We cannot tell here whether we've written buf0 and buf1 already.
     * If we have two full buffers, buf0 may or may not have been
     * written already. But buf1 should still be ours alone. (Except
     * when we're at the end and we're flushing both buffers.) */
    {
        off_t bytes_read = buf1->bytes_read;
        off_t bytes_written = state.bytes_written;
        off_t bytes_written_d = (bytes_written - last_written);
        unsigned time_d;
        off_t speed;
        struct timeval tnow;
        gettimeofday(&tnow, NULL); /* assume this cannot fail */
        assert(tnow.tv_sec != 0 || tnow.tv_usec != 0);
        time_d = (
            (tnow.tv_sec - tprev.tv_sec) * 1000 +
            (tnow.tv_usec - tprev.tv_usec) / 1000);
        if (time_d) {
            speed = (bytes_written_d * 1000 / time_d);
        } else {
            speed = 0;
        }

        assert(bytes_written <= bytes_read);
        // fprintf(stderr, "[[[ %.*s ]]]\n", size, data);
        // lfs = count_lfs(buf1)
        // fprintf(
        //     stderr, "we have buf0 %zu size %u and buf1 %zu size %u\n",
        //     buf0->bytes_read, buf0->size, buf1->bytes_read, buf1->size);
        debug_all_data(pfmt, data, size);
        fprintf(
            stderr,
            CSI_EL "textpv: %zu+ (0x%zx) bytes written, "
            "%zu+ (0x%zx) bytes read (delta 0x%zx), curbuf 0x%x, %s\r",
            bytes_written, bytes_written, bytes_read, bytes_read,
            (bytes_read - bytes_written), size, human_speed(speed));
    }
}

static void read_abort() {
    int error = errno;
    if (close(STDOUT_FILENO) != 0) {
        perror(CSI_EL "textpv: close(STDOUT_FILENO)");
    }

    print_read_write_state(P_FULL);

    errno = error;
    perror(CSI_EL "textpv: read error");
    exit(4);
}

static void write_abort() {
    int error = errno;
    if (close(STDIN_FILENO) != 0) {
        perror(CSI_EL "textpv: close(STDIN_FILENO)");
    }

    print_read_write_state(P_FULL);

    errno = error;
    perror(CSI_EL "textpv: write error");
    exit(2);
}

static int fill_one_buffer(struct buffer_t *buf, off_t *bytes) {
    unsigned off = 0;
    buf->size = 0; /* if size is zero; don't show */
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
#if 0
        /* This does not work for some inputs. For instance ( cat x y )
         * in stdin. That will keep restarting the splice()
         * (ERESTARTSYS) on the boundary of x and y while not proceeding. */
        if (likely(off == BUFFER_SIZE)) {
            *bytes += off;
            buf->bytes_read = *bytes;
            buf->size = off; /* make size non-zero _after_ setting bytes_read */
            return 1;
        }
        assert(0); /* this fails in some cases; do not use this code */
#else
        /* Normally, we'll get a decent sized chunk (64k), and for the
         * last few bytes of a file it might be less. But there may be
         * more to come for a new input pipe. (Relevant for splice()
         * only, but the read() code is not negatively impacted.) */
        *bytes += off;
        buf->bytes_read = *bytes;
        buf->size = off; /* make size non-zero _after_ setting bytes_read */
        return 1;
#endif
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
        if (likely(size >= 0)) {
            buf->size -= size; /* no point in reading from an empty pipe */
        }
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
        perror(CSI_EL "textpv: close(STDOUT_FILENO)");
    }
    if (close(STDIN_FILENO) != 0) {
        perror(CSI_EL "textpv: close(STDIN_FILENO)");
    }
}

static void show_summary() {
#ifdef USE_RUSAGE
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        fprintf(
            stderr,
            CSI_EL "textpv: %zu bytes, %zu.%02zu utime, %zu.%02zu stime\n",
            state.bytes_written,
            ru.ru_utime.tv_sec, (ru.ru_utime.tv_usec + 5000) / 10000,
            ru.ru_stime.tv_sec, (ru.ru_stime.tv_usec + 5000) / 10000);
    } else {
        perror(CSI_EL "textpv: getrusage");
        fprintf(stderr, CSI_EL "textpv: %zu bytes\n", state.bytes_written);
    }
#else
    fprintf(stderr, CSI_EL "textpv: %zu bytes\n", state.bytes);
#endif
}

static void on_alarm(int signum) {
    print_read_write_state(P_SHORT);
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
    print_read_write_state(P_FULL);
    show_summary();
    return 0;
}
