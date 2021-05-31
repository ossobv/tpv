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
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef USE_RUSAGE
# include <sys/resource.h>
#endif

#ifdef USE_SPLICE
/* sendfile() does not work on stdin/stdout, splice() does. */
# include <fcntl.h>
#endif

#define CSI_EL "\x1b[K"             /* erase from cursor to end of line */
#define U_LEFTSHIFT "\xc2\xab"      /* U+00AB */
#define U_ELLIPSIS "\xe2\x80\xa6"   /* U+2026 */

#ifdef USE_SPLICE
# define BUFFER_SIZE (128L * 1024L)
# define BUFFERS 256 /* 256 * 8K == 2M, but we prefer 64+K buffers */
#else
/* TODO: backlog buffer so we can print what we've been doing */
# define BUFFER_SIZE (128L * 1024L)
# define BUFFERS 16 /* 16 * 128K == 2M */
#endif

#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)


struct buffer_t {
    off_t pos;
    unsigned size;
#ifdef USE_SPLICE
    int pipe_r;
    int pipe_w;
#else
    unsigned oldsize;
    char data[BUFFER_SIZE];
#endif
};

struct state_t {
    struct buffer_t buffers[BUFFERS];
    off_t bytes_read;
    off_t bytes_written;
    unsigned rdwr_idx;
};

struct state_t state;

enum print_format {
    P_SHORT,
    P_FULL
};

static void on_alarm(int);
static void on_pipe(int);

static unsigned gettermcols() {
    struct winsize ws;
    unsigned cols = 80; /* safe default */
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0
            && (unsigned)ws.ws_col >= 72) {
        cols = (unsigned)ws.ws_col;
    }
    return cols; /* 72.. */
}

static const char *human_bufsize(unsigned size) {
    static char buf[20];
    unsigned bits = 0;
    while (((size >> 1) << 1) == size) {
        size >>= 1;
        ++bits;
    }
    snprintf(buf, 20, "%03x" U_LEFTSHIFT "%x", size, bits);
    return buf;
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
        snprintf(buf, 20, "%d B/s", (unsigned)bps);
    }
    return buf;
}


static void setup_signals() {
    struct sigaction setup_action;
    sigset_t block_signals;
    struct itimerval tv;

    /* Do we want to block other signals? Or just PIPE and ALRM? */
#if 0
    sigfillset(&block_signals);
#else
    sigaddset(&block_signals, SIGALRM);
    sigaddset(&block_signals, SIGPIPE);
#endif

    /* Block other signals while handler runs. */
    setup_action.sa_mask = block_signals;
    setup_action.sa_flags = SA_RESTART; /* handle EINTR in kernel */

    setup_action.sa_handler = on_pipe;
    if (sigaction(SIGPIPE, &setup_action, NULL) != 0) {
        perror("\ntextpv: sigaction(SIGPIPE)");
        exit(1);
    }

    setup_action.sa_handler = on_alarm;
    if (sigaction(SIGALRM, &setup_action, NULL) != 0) {
        perror("\ntextpv: sigaction(SIGALRM)");
        exit(1);
    }

    tv.it_value.tv_sec = tv.it_interval.tv_sec = 0;
    tv.it_value.tv_usec = tv.it_interval.tv_usec = 999999;
    if (setitimer(ITIMER_REAL, &tv, NULL) != 0) {
        perror("\ntextpv: setitimer");
        exit(1);
    }
}

static void setup_state() {
    for (unsigned rdwr_idx = 0; rdwr_idx < BUFFERS; ++rdwr_idx) {
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
        state.buffers[rdwr_idx].pipe_r = pipes[0];
        state.buffers[rdwr_idx].pipe_w = pipes[1];

        /* Note that because of the way the pages of the pipe buffer
         * are employed when data is written to the pipe, the number
         * of bytes that can be written may be less than the nominal
         * size, depending on the size of the writes. */
        /* XXX: do we need more bytes because of that? */
        if (fcntl(
                state.buffers[rdwr_idx].pipe_w, F_SETPIPE_SZ,
                BUFFER_SIZE) < BUFFER_SIZE) {
            perror(CSI_EL "textpv: fcntl(F_SETPIPE_SZ)");
            exit(1);
        }
#else
        state.buffers[rdwr_idx].oldsize = 0;
#endif
        state.buffers[rdwr_idx].size = 0;
    }
    state.bytes_read = 0;
    state.bytes_written = 0;
    state.rdwr_idx = 0;
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

        if (unlikely(pollret == 0)) {
            /* Because empty_one_buffer() sets the size to 0 first
             * _after_ reading, we may end up here after that read, but
             * before we see a 0-size. In that case there is nothing to
             * read. Moving on. */
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
        enum print_format pfmt, char *data, unsigned size, off_t pos) {
    unsigned lfs = 0;
    unsigned first = 1;
    unsigned trimmed;

    char *p = data;
    const char *pe = data + size;

    /* Prefix: "textpv: [02f786bc+055«e] " = 26, "... " = 4 */
    unsigned cols = gettermcols() - 36;

    while (p != pe) {
        char *lf = p;
        unsigned i;
        unsigned linelen;
        while (lf != pe) {
            if (*lf++ == '\n') {
                lfs++;
                break;
            }
        }
        --lf; /* at LF or at (pe - 1) */
        linelen = (lf - p);

        /* Limit width */
        if (linelen > cols) {
            trimmed = 1;
            linelen = cols;
        } else {
            trimmed = 0;
        }

        /* No control characters (except TAB). */
        for (i = 0; i < linelen; ++i) {
            if (unlikely((p[i] < 0x20 || p[i] >= 0x7f) && p[i] != '\t')) {
                p[i] = '?';
            }
        }

        /* Because MySQL SQL files can span up and above 1MB in length,
         * we need a big enough buffer to only print lines starting
         * after an LF. With the current config we do. */
        if (!first || pfmt == P_FULL) {
            fprintf(
                stderr, CSI_EL "textpv: [%08zx+%s] %s%.*s%s\n",
                pos + (p - data), human_bufsize(size),
                (first ? U_ELLIPSIS : ""), linelen, p,
                (trimmed ? U_ELLIPSIS : ""));
        }

        p = lf + 1;
        first = 0;

        /* Don't print more than N lines. */
        if (lfs >= 80) {
            fprintf(
                stderr, CSI_EL "textpv: [%08zx+%s] " U_ELLIPSIS "\n",
                pos + (p - data), human_bufsize(size));
            break;
        }
    }
}

static void print_read_write_state(enum print_format pfmt) {
    static struct timeval t0;
    struct timeval tnow;
    unsigned i;

    off_t pos;
    char data[3L * 1024L * 2024L]; /* 2M + some extra room */
    unsigned size;

    unsigned time_delta_ms;
    off_t speed;

    if (t0.tv_sec == 0 && t0.tv_usec == 0) {
        gettimeofday(&t0, NULL); /* assume this cannot fail */
        assert(t0.tv_sec != 0 || t0.tv_usec != 0);
    }

    /* We're always at state.rdwr_idx first. Either it was _just_
     * read from (first of the buffers), or it was just written to (last
     * of the buffers). We've ensured that if it was just written to,
     * the size is still 0 until rdwr_idx is incremented. So we won't
     * read a newest read buffer in the wrong order. */
    // TODO: move this to a function
    pos = (off_t)-1;
    size = 0;
    for (i = 0; i < BUFFERS; ++i) {
        const struct buffer_t *buf; /* although fds are mutable */
        unsigned idx = (state.rdwr_idx + i) % BUFFERS;
        unsigned read;
        buf = &state.buffers[idx];
        assert(buf->size < (1L * 1024L * 1024L)); /* no overflowing please */
#ifdef USE_SPLICE
        read = pipe_peek(buf->pipe_r, buf->pipe_w, data + size, buf->size);
#else
        read = buf->size;
        memcpy(data + size, buf->data, read);
#endif
        size += read;
        if (unlikely(pos == (off_t)-1 && read != 0)) {
            pos = buf->pos;
        }
        if (unlikely(size >= (2L * 1024L * 1024L))) {
            size = (2L * 1024L * 1024L);
            break;
        }
    }

    /* Verbose printing of at least one line. */
    debug_all_data(pfmt, data, size, pos);

    gettimeofday(&tnow, NULL); /* assume this cannot fail */
    assert(tnow.tv_sec != 0 || tnow.tv_usec != 0);
    time_delta_ms = (
        (tnow.tv_sec - t0.tv_sec) * 1000 +
        (tnow.tv_usec - t0.tv_usec) / 1000);
    if (time_delta_ms) {
        speed = (state.bytes_written * 1000 / time_delta_ms);
    } else {
        speed = 0;
    }

    fprintf(
        stderr,
        CSI_EL "textpv: %zu..%zu (0x%zx) bytes, %s\r",
        state.bytes_written, state.bytes_read, state.bytes_written,
        human_speed(speed));
}

static void read_abort() {
    int error = errno;
    fprintf(stderr, "\n");
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
    fprintf(stderr, "\n");
    if (close(STDIN_FILENO) != 0) {
        perror(CSI_EL "textpv: close(STDIN_FILENO)");
    }

    print_read_write_state(P_FULL);

    errno = error;
    perror(CSI_EL "textpv: write error");
    exit(2);
}

static unsigned fill_one_buffer(struct buffer_t *buf, off_t *bytes) {
#ifndef USE_SPLICE
    off_t off = 0;
#endif
    ssize_t size;

    buf->pos = *bytes;

#ifdef USE_SPLICE
    /* We'll splice() once and don't attempt to fill the entire buffer.
     * This failed when reading ( cat FILE1 FILE2 ) from stdin: splice()
     * blocked. */
    size = splice(STDIN_FILENO, NULL, buf->pipe_w, NULL,
        BUFFER_SIZE, SPLICE_F_MORE | SPLICE_F_MOVE);

    if (unlikely(size < 0)) {
        read_abort();
        return 0;
    }
    if (unlikely(size == 0)) {
        return 0;
    }
    *bytes += size;
#else
    /* For non-splice data, we now mark that this data is overwritten. */
    buf->oldsize = 0;
    /* We'll do a "safe read" and fill the buffer to the brim. Otherwise
     * will get possibly tiny chunks fed to us, depending on the source
     * application. */
    while (1) {
        size = read(STDIN_FILENO, buf->data + off, BUFFER_SIZE - off);
        if (unlikely(size < 0)) {
            read_abort();
            return 0;
        }
        off += size;
        *bytes += size;
        if (unlikely(size == 0 || off == BUFFER_SIZE)) {
            /* We can keep the buffer for a short while before we're filling it
             * anew. Use this to find the last data that we pushed before an
             * EPIPE. */
            buf->oldsize = off;
            return off;
        }
    }
#endif
    return size;
}

static void empty_one_buffer(struct buffer_t *buf, off_t *bytes) {
    unsigned off = 0;
    unsigned to_write = buf->size;
    assert(to_write != 0);
    while (1) {
#ifdef USE_SPLICE
        ssize_t size = splice(buf->pipe_r, NULL, STDOUT_FILENO, NULL,
            BUFFER_SIZE - off, SPLICE_F_MORE | SPLICE_F_MOVE);
#else
        ssize_t size = write(
            STDOUT_FILENO, buf->data + off, to_write - off);
#endif
        if (unlikely(size <= 0)) {
            write_abort();
            return;
        }
        buf->size -= size; /* makes buf->size 0 when done */
        off += size;
        *bytes += size;
        if (likely(off == to_write)) {
            assert(buf->size == 0);
            return;
        }
    }
}

static void passthrough() {
    unsigned read;
    unsigned i;

    /* Step 1: fill all buffers */
    for (i = 0; i < BUFFERS; ++i) {
        read = fill_one_buffer(&state.buffers[i], &state.bytes_read);
        state.buffers[i].size = read;
        if (unlikely(read == 0)) {
            goto empty_all_buffers;
        }
    }

    /* Step 2: empty one buffer, fill that, select next */
    while (1) {
        unsigned cur_idx = state.rdwr_idx;
        empty_one_buffer(&state.buffers[cur_idx], &state.bytes_written);
        read = fill_one_buffer(&state.buffers[cur_idx], &state.bytes_read);

        /* We update the rdwr_idx _before_ setting the size. That means
         * that rdwr always points to size 0 or the first data to read. */
        state.rdwr_idx = (state.rdwr_idx + 1) % BUFFERS;
        state.buffers[cur_idx].size = read;
        if (unlikely(read == 0)) {
            break;
        }
    }

empty_all_buffers:
    /* Step 3: empty all leftover buffers */
    while (state.buffers[state.rdwr_idx].size) {
        empty_one_buffer(
            &state.buffers[state.rdwr_idx], &state.bytes_written);
        state.rdwr_idx = (state.rdwr_idx + 1) % BUFFERS;
    }
    assert(state.bytes_written == state.bytes_read);
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
    fprintf(stderr, CSI_EL "textpv: %zu bytes\n", state.bytes_written);
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
    fprintf(stderr, "\n");

    // TODO: write_last_bytes/buffers to some tempfile..
    // TODO: we can mem/peek the buffers as soon as we detect EOF
    print_read_write_state(P_FULL);
    show_summary();
    return 0;
}
