#define _GNU_SOURCE

#include "proto.h"
#include "rate_estimator.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct sender_config {
    const char *dst_ip;
    uint16_t dst_port;
    uint64_t bitrate_bps;
    uint32_t fps;
    size_t pkt_size;
    enum fill_mode fill_mode;
    size_t batch_size;
    const char *log_dir;
    const char *log_file_override;
    enum rate_estimator_algo estimator_algo;
    bool debug_estimator;
    bool debug_naive_ewma_monitor;
    bool pacer_enabled;
    uint64_t pacer_window_ms;
};

struct retransmit_slot {
    bool valid;
    uint64_t send_seq;
    uint16_t pkt_len;
};

struct feedback_entry {
    uint64_t send_seq;
    uint64_t rtcp_batch_id;
    uint64_t frame_id;
    uint32_t packet_id;
    uint32_t frame_packet_count;
    uint32_t pkt_len;
    uint64_t send_time_us;
    uint64_t recv_ts_us;
    uint64_t local_record_ts_us;
};

#define DEFAULT_RTCP_RING_SLOTS 8192u
#define RTCP_BUF_MAX 65536u
#define RTCP_POLL_SLEEP_NS (2ull * NS_PER_MS)
#define ACK_DEDUP_SLOTS 16384u

struct rtcp_runtime {
    int fd;
    size_t pkt_size;
    _Atomic bool running;
    pthread_t listener_thread;
    bool enable_retransmit;
    bool debug_loss_events;
    _Atomic uint64_t loss_events_total;

    uint8_t *retransmit_packet_ring;
    struct retransmit_slot *retransmit_meta_ring;
    uint32_t retransmit_ring_slots;
    pthread_mutex_t retransmit_lock;

    struct feedback_entry *feedback_ring;
    uint32_t feedback_ring_slots;
    uint32_t feedback_write_idx;
    uint32_t feedback_read_idx;
    uint32_t feedback_count;
    uint64_t feedback_batch_id;
    pthread_mutex_t feedback_lock;
};

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static inline uint16_t to_be16(uint16_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(x);
#else
    return x;
#endif
}

static inline uint32_t to_be32(uint32_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(x);
#else
    return x;
#endif
}

static inline uint64_t to_be64(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(x);
#else
    return x;
#endif
}

static inline uint16_t from_be16(uint16_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(x);
#else
    return x;
#endif
}

static inline uint32_t from_be32(uint32_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(x);
#else
    return x;
#endif
}

static inline uint64_t from_be64(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(x);
#else
    return x;
#endif
}

static uint64_t timespec_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * NS_PER_SEC + (uint64_t)ts->tv_nsec;
}

static uint64_t ns_to_us(uint64_t ns)
{
    return ns / 1000ull;
}

static void ns_to_timespec(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec = (time_t)(ns / NS_PER_SEC);
    ts->tv_nsec = (long)(ns % NS_PER_SEC);
}

static FILE *open_auto_log_file(const char *log_dir, uint64_t first_ts_ns, char *path_buf, size_t path_buf_sz)
{
    uint64_t sec = first_ts_ns / NS_PER_SEC;
    int n = snprintf(path_buf, path_buf_sz, "%s/send_%" PRIu64 ".csv", log_dir, sec);
    if (n <= 0 || (size_t)n >= path_buf_sz) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return fopen(path_buf, "w");
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --dst-ip IP      default: %s\n"
            "  --dst-port PORT  default: %u\n"
            "  --bitrate RATE   default: 10M, range: [250k,100M]\n"
            "  --fps FPS        default: 30\n"
            "  --pkt-size SIZE  default: 1400\n"
            "  --fill MODE      zero|random, default: zero\n"
            "  --batch N        sendmmsg batch size, default: 64\n"
            "  --pacer on|off   enable global pacer, default: off\n"
            "  --pacer-window-ms N  pacer tick window in ms, default: 5\n"
            "  --estimator MODE fix|naive-ewma|camel|gcc-remb, default: naive-ewma\n"
            "  --debug-estimator  print estimator debug logs\n"
            "  --debug-naive-ewma-monitor  log naive-ewma monitor state every 200ms (naive-ewma mode only)\n"
            "  --log-dir PATH   default: .\n"
            "  --log-file PATH  optional fixed log path (use '-' for stdout)\n"
            "  --help           show this help\n"
            "Note: also accepts single-dash long style like -dst-port.\n",
            prog,
            DEFAULT_DST_IP,
            DEFAULT_DST_PORT);
}

static bool option_match(const char *arg, const char *name)
{
    if (!arg || !name) {
        return false;
    }
    if (strcmp(arg, name) == 0) {
        return true;
    }
    if (arg[0] == '-' && arg[1] != '-' && strcmp(arg + 1, name + 2) == 0) {
        return true;
    }
    return false;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!s[0] || (end && *end != '\0')) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int parse_bitrate(const char *s, uint64_t *out_bps)
{
    char *end = NULL;
    double value = strtod(s, &end);
    double scale = 1.0;
    if (!s[0] || value <= 0.0) {
        return -1;
    }
    if (end && *end) {
        char c = (char)(*end | 0x20);
        if (c == 'k') {
            scale = 1e3;
            end++;
        } else if (c == 'm') {
            scale = 1e6;
            end++;
        } else if (c == 'g') {
            scale = 1e9;
            end++;
        }
        if (end && *end) {
            if ((end[0] | 0x20) == 'b' && (end[1] | 0x20) == 'p' && (end[2] | 0x20) == 's' && end[3] == '\0') {
                end += 3;
            }
        }
    }
    if (end && *end != '\0') {
        return -1;
    }
    *out_bps = (uint64_t)llround(value * scale);
    return 0;
}

static int parse_args(int argc, char **argv, struct sender_config *cfg)
{
    cfg->dst_ip = DEFAULT_DST_IP;
    cfg->dst_port = DEFAULT_DST_PORT;
    cfg->bitrate_bps = BITRATE_INIT_BPS;
    cfg->fps = DEFAULT_FPS;
    cfg->pkt_size = DEFAULT_PKT_SIZE;
    cfg->fill_mode = FILL_ZERO;
    cfg->batch_size = 64;
    cfg->log_dir = ".";
    cfg->log_file_override = NULL;
    cfg->estimator_algo = RATE_ESTIMATOR_NAIVE_EWMA;
    cfg->debug_estimator = false;
    cfg->debug_naive_ewma_monitor = false;
    cfg->pacer_enabled = false;
    cfg->pacer_window_ms = 5;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (option_match(arg, "--help")) {
            print_usage(argv[0]);
            exit(0);
        } else if (option_match(arg, "--dst-ip")) {
            if (!val) return -1;
            cfg->dst_ip = val;
            i++;
        } else if (option_match(arg, "--dst-port")) {
            uint64_t p = 0;
            if (!val || parse_u64(val, &p) != 0 || p > 65535) return -1;
            cfg->dst_port = (uint16_t)p;
            i++;
        } else if (option_match(arg, "--bitrate")) {
            uint64_t b = 0;
            if (!val || parse_bitrate(val, &b) != 0) return -1;
            cfg->bitrate_bps = b;
            i++;
        } else if (option_match(arg, "--fps")) {
            uint64_t f = 0;
            if (!val || parse_u64(val, &f) != 0 || f == 0 || f > 1000) return -1;
            cfg->fps = (uint32_t)f;
            i++;
        } else if (option_match(arg, "--pkt-size")) {
            uint64_t s = 0;
            if (!val || parse_u64(val, &s) != 0) return -1;
            cfg->pkt_size = (size_t)s;
            i++;
        } else if (option_match(arg, "--fill")) {
            if (!val) return -1;
            if (strcmp(val, "zero") == 0) {
                cfg->fill_mode = FILL_ZERO;
            } else if (strcmp(val, "random") == 0) {
                cfg->fill_mode = FILL_RANDOM;
            } else {
                return -1;
            }
            i++;
        } else if (option_match(arg, "--batch")) {
            uint64_t n = 0;
            if (!val || parse_u64(val, &n) != 0 || n == 0 || n > 2048) return -1;
            cfg->batch_size = (size_t)n;
            i++;
        } else if (option_match(arg, "--pacer")) {
            if (!val) return -1;
            if (strcmp(val, "on") == 0) {
                cfg->pacer_enabled = true;
            } else if (strcmp(val, "off") == 0) {
                cfg->pacer_enabled = false;
            } else {
                return -1;
            }
            i++;
        } else if (option_match(arg, "--pacer-window-ms")) {
            uint64_t n = 0;
            if (!val || parse_u64(val, &n) != 0 || n == 0 || n > 1000) return -1;
            cfg->pacer_window_ms = n;
            i++;
        } else if (option_match(arg, "--estimator")) {
            if (!val) return -1;
            if (strcmp(val, "fix") == 0) {
                cfg->estimator_algo = RATE_ESTIMATOR_FIX;
            } else if (strcmp(val, "naive-ewma") == 0 || strcmp(val, "mortise") == 0) {
                cfg->estimator_algo = RATE_ESTIMATOR_NAIVE_EWMA;
            } else if (strcmp(val, "camel") == 0) {
                cfg->estimator_algo = RATE_ESTIMATOR_CAMEL;
            } else if (strcmp(val, "gcc-remb") == 0) {
                cfg->estimator_algo = RATE_ESTIMATOR_GCC_REMB;
            } else {
                return -1;
            }
            i++;
        } else if (option_match(arg, "--debug-estimator")) {
            cfg->debug_estimator = true;
        } else if (option_match(arg, "--debug-naive-ewma-monitor")
                   || option_match(arg, "--debug-mortise-monitor")) {
            cfg->debug_naive_ewma_monitor = true;
        } else if (option_match(arg, "--log-dir")) {
            if (!val) return -1;
            cfg->log_dir = val;
            i++;
        } else if (option_match(arg, "--log-file")) {
            if (!val) return -1;
            cfg->log_file_override = val;
            i++;
        } else {
            return -1;
        }
    }

    if (cfg->bitrate_bps < BITRATE_MIN_BPS || cfg->bitrate_bps > BITRATE_MAX_BPS) {
        fprintf(stderr, "bitrate out of range: %" PRIu64 " bps\n", cfg->bitrate_bps);
        return -1;
    }
    if (cfg->pkt_size < sizeof(struct stream_payload_header) || cfg->pkt_size > 65507u) {
        fprintf(stderr, "pkt-size must be in [%zu,65507]\n", sizeof(struct stream_payload_header));
        return -1;
    }
    return 0;
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void fill_payload(uint8_t *buf, size_t len, enum fill_mode mode, uint64_t *rng)
{
    if (mode == FILL_ZERO) {
        memset(buf, 0, len);
        return;
    }
    size_t i = 0;
    while (i < len) {
        uint64_t v = xorshift64(rng);
        size_t n = (len - i < sizeof(v)) ? (len - i) : sizeof(v);
        memcpy(buf + i, &v, n);
        i += n;
    }
}

static uint32_t ring_index_for_seq(uint64_t send_seq, uint32_t slots)
{
    return (uint32_t)(send_seq % slots);
}

static void cache_packet_for_retransmit(struct rtcp_runtime *rt, uint64_t send_seq, const uint8_t *pkt, size_t len)
{
    if (!rt || !pkt || len == 0 || len > UINT16_MAX) {
        return;
    }
    uint32_t idx = ring_index_for_seq(send_seq, rt->retransmit_ring_slots);
    pthread_mutex_lock(&rt->retransmit_lock);
    memcpy(rt->retransmit_packet_ring + (size_t)idx * rt->pkt_size, pkt, len);
    rt->retransmit_meta_ring[idx].send_seq = send_seq;
    rt->retransmit_meta_ring[idx].pkt_len = (uint16_t)len;
    rt->retransmit_meta_ring[idx].valid = true;
    pthread_mutex_unlock(&rt->retransmit_lock);
}

static int retransmit_packet_by_seq(struct rtcp_runtime *rt, uint64_t send_seq)
{
    uint8_t local_pkt[65536];
    uint16_t local_len = 0;
    uint32_t idx = ring_index_for_seq(send_seq, rt->retransmit_ring_slots);

    pthread_mutex_lock(&rt->retransmit_lock);
    if (rt->retransmit_meta_ring[idx].valid && rt->retransmit_meta_ring[idx].send_seq == send_seq) {
        local_len = rt->retransmit_meta_ring[idx].pkt_len;
        if (local_len > 0) {
            memcpy(local_pkt, rt->retransmit_packet_ring + (size_t)idx * rt->pkt_size, local_len);
        }
    }
    pthread_mutex_unlock(&rt->retransmit_lock);

    if (local_len == 0) {
        return -1;
    }

    ssize_t n = send(rt->fd, local_pkt, local_len, 0);
    if (n < 0 || (size_t)n != local_len) {
        return -1;
    }
    return 0;
}

static bool lookup_feedback_metadata_by_seq(struct rtcp_runtime *rt,
                                            uint64_t send_seq,
                                            uint64_t *send_time_us_out,
                                            uint32_t *pkt_len_out,
                                            uint64_t *frame_id_out,
                                            uint32_t *packet_id_out,
                                            uint32_t *frame_packet_count_out)
{
    uint64_t send_time_us = 0;
    uint32_t pkt_len = 0;
    uint64_t frame_id = 0;
    uint32_t packet_id = 0;
    uint32_t frame_packet_count = 0;
    bool found = false;
    uint32_t idx = ring_index_for_seq(send_seq, rt->retransmit_ring_slots);

    pthread_mutex_lock(&rt->retransmit_lock);
    if (rt->retransmit_meta_ring[idx].valid && rt->retransmit_meta_ring[idx].send_seq == send_seq) {
        const uint8_t *pkt = rt->retransmit_packet_ring + (size_t)idx * rt->pkt_size;
        const struct stream_payload_header *hdr = (const struct stream_payload_header *)pkt;
        send_time_us = from_be64(hdr->send_time_us);
        frame_id = from_be64(hdr->frame_id);
        packet_id = from_be32(hdr->packet_id);
        frame_packet_count = from_be32(hdr->frame_packet_count);
        pkt_len = rt->retransmit_meta_ring[idx].pkt_len;
        found = true;
    }
    pthread_mutex_unlock(&rt->retransmit_lock);
    if (send_time_us_out) {
        *send_time_us_out = send_time_us;
    }
    if (pkt_len_out) {
        *pkt_len_out = pkt_len;
    }
    if (frame_id_out) {
        *frame_id_out = frame_id;
    }
    if (packet_id_out) {
        *packet_id_out = packet_id;
    }
    if (frame_packet_count_out) {
        *frame_packet_count_out = frame_packet_count;
    }
    return found;
}

static void push_feedback_entry(struct rtcp_runtime *rt,
                                uint64_t send_seq,
                                uint64_t frame_id,
                                uint32_t packet_id,
                                uint32_t frame_packet_count,
                                uint64_t recv_ts_us,
                                uint64_t rtcp_batch_id)
{
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    struct feedback_entry e;
    memset(&e, 0, sizeof(e));
    e.send_seq = send_seq;
    e.rtcp_batch_id = rtcp_batch_id;
    e.frame_id = frame_id;
    e.packet_id = packet_id;
    e.frame_packet_count = frame_packet_count;
    (void)lookup_feedback_metadata_by_seq(rt,
                                          send_seq,
                                          &e.send_time_us,
                                          &e.pkt_len,
                                          &e.frame_id,
                                          &e.packet_id,
                                          &e.frame_packet_count);
    e.recv_ts_us = recv_ts_us;
    e.local_record_ts_us = ns_to_us(timespec_to_ns(&mono));

    pthread_mutex_lock(&rt->feedback_lock);
    if (rt->feedback_count == rt->feedback_ring_slots) {
        rt->feedback_read_idx = (rt->feedback_read_idx + 1u) % rt->feedback_ring_slots;
    } else {
        rt->feedback_count++;
    }
    rt->feedback_ring[rt->feedback_write_idx] = e;
    rt->feedback_write_idx = (rt->feedback_write_idx + 1u) % rt->feedback_ring_slots;
    pthread_mutex_unlock(&rt->feedback_lock);
}

static size_t drain_feedback_batch(struct rtcp_runtime *rt,
                                   struct rate_estimator_packet_feedback *out,
                                   size_t out_cap)
{
    if (!rt || !out || out_cap == 0) {
        return 0;
    }

    size_t n = 0;
    pthread_mutex_lock(&rt->feedback_lock);
    while (rt->feedback_count > 0 && n < out_cap) {
        const struct feedback_entry *e = &rt->feedback_ring[rt->feedback_read_idx];
        out[n].send_seq = e->send_seq;
        out[n].rtcp_batch_id = e->rtcp_batch_id;
        out[n].frame_id = e->frame_id;
        out[n].packet_id = e->packet_id;
        out[n].frame_packet_count = e->frame_packet_count;
        out[n].pkt_len = e->pkt_len;
        out[n].send_time_us = e->send_time_us;
        out[n].recv_ts_us = e->recv_ts_us;
        out[n].local_record_ts_us = e->local_record_ts_us;

        rt->feedback_read_idx = (rt->feedback_read_idx + 1u) % rt->feedback_ring_slots;
        rt->feedback_count--;
        n++;
    }
    pthread_mutex_unlock(&rt->feedback_lock);
    return n;
}

static void handle_rtcp_loss_packet(struct rtcp_runtime *rt, const uint8_t *payload, size_t payload_len)
{
    if (payload_len < sizeof(uint16_t)) {
        return;
    }
    uint16_t cnt = from_be16(*(const uint16_t *)payload);
    size_t need = sizeof(uint16_t) + (size_t)cnt * sizeof(uint64_t);
    if (payload_len < need) {
        return;
    }

    const uint64_t *seqs = (const uint64_t *)(payload + sizeof(uint16_t));
    uint64_t total_loss = atomic_fetch_add(&rt->loss_events_total, cnt) + cnt;
    if (rt->debug_loss_events) {
        uint64_t first_seq = from_be64(seqs[0]);
        uint64_t last_seq = from_be64(seqs[cnt - 1]);
        fprintf(stderr,
                "[TWCC] loss report: cnt=%" PRIu16 " first_seq=%" PRIu64 " last_seq=%" PRIu64 " total_loss_events=%" PRIu64 "\n",
                cnt,
                first_seq,
                last_seq,
                total_loss);
    }
    for (uint16_t i = 0; i < cnt; ++i) {
        if (rt->enable_retransmit) {
            (void)retransmit_packet_by_seq(rt, from_be64(seqs[i]));
        }
    }
}

static void handle_rtcp_feedback_packet(struct rtcp_runtime *rt, const uint8_t *payload, size_t payload_len)
{
    if (payload_len < sizeof(uint16_t)) {
        return;
    }
    uint16_t cnt = from_be16(*(const uint16_t *)payload);
    size_t need = sizeof(uint16_t) + (size_t)cnt * sizeof(struct rtcp_feedback_record);
    if (payload_len < need) {
        return;
    }

    const struct rtcp_feedback_record *records =
        (const struct rtcp_feedback_record *)(payload + sizeof(uint16_t));
    uint64_t rtcp_batch_id = ++rt->feedback_batch_id;
    for (uint16_t i = 0; i < cnt; ++i) {
        push_feedback_entry(rt,
                            from_be64(records[i].send_seq_be),
                            from_be64(records[i].frame_id_be),
                            from_be32(records[i].packet_id_be),
                            from_be32(records[i].frame_packet_count_be),
                            from_be64(records[i].recv_ts_us_be),
                            rtcp_batch_id);
    }
}

static void *rtcp_listener_thread_fn(void *arg)
{
    struct rtcp_runtime *rt = (struct rtcp_runtime *)arg;
    uint8_t buf[RTCP_BUF_MAX];

    while (atomic_load(&rt->running)) {
        ssize_t n = recv(rt->fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                struct timespec nap;
                ns_to_timespec(RTCP_POLL_SLEEP_NS, &nap);
                clock_nanosleep(CLOCK_MONOTONIC, 0, &nap, NULL);
                continue;
            }
            break;
        }
        if ((size_t)n < sizeof(struct rtcp_header)) {
            continue;
        }

        const struct rtcp_header *hdr = (const struct rtcp_header *)buf;
        uint16_t wire_len = from_be16(hdr->length_be);
        if (wire_len < sizeof(struct rtcp_header) || wire_len > (uint16_t)n) {
            continue;
        }

        const uint8_t *payload = buf + sizeof(struct rtcp_header);
        size_t payload_len = (size_t)wire_len - sizeof(struct rtcp_header);
        if (hdr->type == RTCP_PKT_LOSS) {
            handle_rtcp_loss_packet(rt, payload, payload_len);
        } else if (hdr->type == RTCP_PKT_FEEDBACK) {
            handle_rtcp_feedback_packet(rt, payload, payload_len);
        }
    }
    return NULL;
}

struct sender_context {
    int fd;
    struct iovec *iov;
    struct mmsghdr *msgs;
    struct rtcp_runtime *rtcp;
    uint64_t *send_seq;
    uint64_t *rng;
    const struct sender_config *cfg;
};

static int send_burst(struct sender_context *ctx, uint64_t frame_id, uint32_t frame_packet_count, uint32_t start_packet_id, uint32_t count)
{
    uint32_t sent = 0;
    while (sent < count && !g_stop) {
        uint32_t chunk = count - sent;
        if (chunk > ctx->cfg->batch_size) {
            chunk = (uint32_t)ctx->cfg->batch_size;
        }

        struct timespec now;
        for (uint32_t i = 0; i < chunk; ++i) {
            uint8_t *pkt = (uint8_t *)ctx->iov[i].iov_base;
            struct stream_payload_header hdr;
            memset(&hdr, 0, sizeof(hdr));
            uint64_t this_seq = (*ctx->send_seq)++;

            clock_gettime(CLOCK_MONOTONIC, &now);
            hdr.magic = to_be32(STREAM_MAGIC);
            hdr.version = to_be16(STREAM_VERSION);
            hdr.header_len = to_be16((uint16_t)sizeof(hdr));
            hdr.frame_id = to_be64(frame_id);
            hdr.packet_id = to_be32(start_packet_id + sent + i);
            hdr.frame_packet_count = to_be32(frame_packet_count);
            hdr.send_seq = to_be64(this_seq);
            hdr.send_time_us = to_be64(ns_to_us(timespec_to_ns(&now)));
            memcpy(pkt, &hdr, sizeof(hdr));

            fill_payload(pkt + sizeof(hdr), ctx->cfg->pkt_size - sizeof(hdr), ctx->cfg->fill_mode, ctx->rng);
            cache_packet_for_retransmit(ctx->rtcp, this_seq, pkt, ctx->cfg->pkt_size);
        }

        int rc = sendmmsg(ctx->fd, ctx->msgs, chunk, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("sendmmsg");
            g_stop = 1;
            return -1;
        }
        sent += (uint32_t)rc;
    }
    return (int)sent;
}

struct pending_frame {
    uint64_t frame_id;
    uint32_t frame_packet_count;
    uint32_t next_packet_id;
};

#define PACER_QUEUE_SIZE 1024

struct pacer_state {
    bool enabled;
    uint64_t window_ns;
    
    pthread_mutex_t lock;
    pthread_cond_t cond;
    
    uint64_t target_bitrate_bps;
    
    struct pending_frame queue[PACER_QUEUE_SIZE];
    uint32_t q_head;
    uint32_t q_tail;
    uint32_t q_size;
    
    pthread_t thread;
    _Atomic bool running;
    
    struct sender_context *send_ctx;
};

static void pacer_enqueue(struct pacer_state *pacer, uint64_t frame_id, uint32_t frame_packet_count)
{
    if (frame_packet_count == 0) return;

    pthread_mutex_lock(&pacer->lock);
    if (pacer->q_size < PACER_QUEUE_SIZE) {
        pacer->queue[pacer->q_tail].frame_id = frame_id;
        pacer->queue[pacer->q_tail].frame_packet_count = frame_packet_count;
        pacer->queue[pacer->q_tail].next_packet_id = 0;
        pacer->q_tail = (pacer->q_tail + 1) % PACER_QUEUE_SIZE;
        pacer->q_size++;
    } else {
        fprintf(stderr, "[Pacer] queue full, dropping frame %" PRIu64 "\n", frame_id);
    }
    pthread_cond_signal(&pacer->cond);
    pthread_mutex_unlock(&pacer->lock);
}

static void *pacer_thread_fn(void *arg)
{
    struct pacer_state *pacer = (struct pacer_state *)arg;
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t next_tick_ns = timespec_to_ns(&now) + pacer->window_ns;
    
    double packet_credit = 0.0;
    
    while (atomic_load(&pacer->running) && !g_stop) {
        ns_to_timespec(next_tick_ns, &now);
        int sleep_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL);
        if (sleep_rc != 0 && sleep_rc != EINTR) {
            break;
        }
        
        uint64_t current_bps = 0;
        bool is_empty = false;
        
        pthread_mutex_lock(&pacer->lock);
        current_bps = pacer->target_bitrate_bps;
        is_empty = (pacer->q_size == 0);
        pthread_mutex_unlock(&pacer->lock);
        
        double added_credit = (double)current_bps * (double)pacer->window_ns / 8.0 / 1e9 / (double)pacer->send_ctx->cfg->pkt_size;
        
        if (is_empty) {
            packet_credit = added_credit;
        } else {
            packet_credit += added_credit;
        }
        
        uint32_t to_send = (uint32_t)floor(packet_credit);
        uint32_t actually_sent = 0;
        
        while (to_send > 0 && !g_stop) {
            pthread_mutex_lock(&pacer->lock);
            if (pacer->q_size == 0) {
                pthread_mutex_unlock(&pacer->lock);
                break;
            }
            
            struct pending_frame *f = &pacer->queue[pacer->q_head];
            uint32_t frame_rem = f->frame_packet_count - f->next_packet_id;
            uint32_t chunk = (to_send < frame_rem) ? to_send : frame_rem;
            
            uint64_t frame_id = f->frame_id;
            uint32_t frame_total = f->frame_packet_count;
            uint32_t start_id = f->next_packet_id;
            
            f->next_packet_id += chunk;
            bool frame_done = (f->next_packet_id == f->frame_packet_count);
            if (frame_done) {
                pacer->q_head = (pacer->q_head + 1) % PACER_QUEUE_SIZE;
                pacer->q_size--;
            }
            pthread_mutex_unlock(&pacer->lock);
            
            int rc = send_burst(pacer->send_ctx, frame_id, frame_total, start_id, chunk);
            if (rc < 0) {
                break;
            }
            to_send -= rc;
            actually_sent += rc;
        }
        
        packet_credit -= actually_sent;
        next_tick_ns += pacer->window_ns;
    }
    
    return NULL;
}

int main(int argc, char **argv)
{
    struct sender_config cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.dst_port);
    if (inet_pton(AF_INET, cfg.dst_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "invalid dst ip: %s\n", cfg.dst_ip);
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    uint8_t *packet_buf = calloc(cfg.batch_size, cfg.pkt_size);
    struct iovec *iov = calloc(cfg.batch_size, sizeof(*iov));
    struct mmsghdr *msgs = calloc(cfg.batch_size, sizeof(*msgs));
    struct rate_estimator_packet_feedback *feedback_batch = NULL;
    struct rtcp_runtime rtcp;
    memset(&rtcp, 0, sizeof(rtcp));
    rtcp.fd = fd;
    rtcp.pkt_size = cfg.pkt_size;
    rtcp.enable_retransmit = false;
    rtcp.debug_loss_events = cfg.debug_estimator;
    rtcp.retransmit_ring_slots = DEFAULT_RTCP_RING_SLOTS;
    rtcp.feedback_ring_slots = DEFAULT_RTCP_RING_SLOTS;
    rtcp.retransmit_packet_ring = calloc(rtcp.retransmit_ring_slots, cfg.pkt_size);
    rtcp.retransmit_meta_ring = calloc(rtcp.retransmit_ring_slots, sizeof(*rtcp.retransmit_meta_ring));
    rtcp.feedback_ring = calloc(rtcp.feedback_ring_slots, sizeof(*rtcp.feedback_ring));
    feedback_batch = calloc(rtcp.feedback_ring_slots, sizeof(*feedback_batch));
    if (!packet_buf || !iov || !msgs ||
        !rtcp.retransmit_packet_ring || !rtcp.retransmit_meta_ring || !rtcp.feedback_ring ||
        !feedback_batch) {
        fprintf(stderr, "oom\n");
        close(fd);
        free(packet_buf);
        free(iov);
        free(msgs);
        free(rtcp.retransmit_packet_ring);
        free(rtcp.retransmit_meta_ring);
        free(rtcp.feedback_ring);
        free(feedback_batch);
        return 1;
    }
    if (pthread_mutex_init(&rtcp.retransmit_lock, NULL) != 0 ||
        pthread_mutex_init(&rtcp.feedback_lock, NULL) != 0) {
        fprintf(stderr, "pthread_mutex_init failed\n");
        close(fd);
        free(packet_buf);
        free(iov);
        free(msgs);
        free(rtcp.retransmit_packet_ring);
        free(rtcp.retransmit_meta_ring);
        free(rtcp.feedback_ring);
        free(feedback_batch);
        return 1;
    }
    atomic_store(&rtcp.running, true);
    if (pthread_create(&rtcp.listener_thread, NULL, rtcp_listener_thread_fn, &rtcp) != 0) {
        fprintf(stderr, "pthread_create rtcp listener failed\n");
        pthread_mutex_destroy(&rtcp.retransmit_lock);
        pthread_mutex_destroy(&rtcp.feedback_lock);
        close(fd);
        free(packet_buf);
        free(iov);
        free(msgs);
        free(rtcp.retransmit_packet_ring);
        free(rtcp.retransmit_meta_ring);
        free(rtcp.feedback_ring);
        free(feedback_batch);
        return 1;
    }

    for (size_t i = 0; i < cfg.batch_size; ++i) {
        iov[i].iov_base = packet_buf + i * cfg.pkt_size;
        iov[i].iov_len = cfg.pkt_size;
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    double packet_budget = 0.0;
    uint64_t frame_id = 0;
    uint64_t send_seq = 0;
    uint64_t rng = (uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ull;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t next_frame_ns = timespec_to_ns(&now);
    const uint64_t frame_ns = NS_PER_SEC / cfg.fps;
    FILE *log_out = NULL;
    char auto_log_path[512] = {0};

    fprintf(stderr,
            "sender dst=%s:%u bitrate=%" PRIu64 " fps=%u pkt=%zu fill=%s batch=%zu\n",
            cfg.dst_ip,
            cfg.dst_port,
            cfg.bitrate_bps,
            cfg.fps,
            cfg.pkt_size,
            cfg.fill_mode == FILL_ZERO ? "zero" : "random",
            cfg.batch_size);
    fprintf(stderr, "sender rtcp listener enabled, feedback ring=%u\n", rtcp.feedback_ring_slots);
    fprintf(stderr, "sender retransmit mode: %s\n", rtcp.enable_retransmit ? "enabled" : "disabled");
    struct rate_estimator_config estimator_cfg = {
        .algo = cfg.estimator_algo,
        .debug_enabled = cfg.debug_estimator,
        .debug_naive_ewma_monitor = cfg.debug_naive_ewma_monitor,
        .log_dir = cfg.log_dir,
    };
    if (rate_estimator_init(&estimator_cfg) != 0) {
        fprintf(stderr, "rate estimator init failed\n");
        atomic_store(&rtcp.running, false);
        pthread_join(rtcp.listener_thread, NULL);
        pthread_mutex_destroy(&rtcp.retransmit_lock);
        pthread_mutex_destroy(&rtcp.feedback_lock);
        free(rtcp.retransmit_packet_ring);
        free(rtcp.retransmit_meta_ring);
        free(rtcp.feedback_ring);
        free(feedback_batch);
        free(packet_buf);
        free(iov);
        free(msgs);
        close(fd);
        return 1;
    }
    if (cfg.debug_estimator) {
        fprintf(stderr, "sender estimator debug: enabled (algo=%s)\n",
                cfg.estimator_algo == RATE_ESTIMATOR_NAIVE_EWMA ? "naive-ewma"
                : (cfg.estimator_algo == RATE_ESTIMATOR_CAMEL
                       ? "camel"
                       : (cfg.estimator_algo == RATE_ESTIMATOR_GCC_REMB ? "gcc-remb" : "fix")));
    }
    if (cfg.log_file_override && strcmp(cfg.log_file_override, "-") == 0) {
        log_out = stdout;
    } else if (cfg.log_file_override) {
        log_out = fopen(cfg.log_file_override, "w");
    } else {
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        log_out = open_auto_log_file(cfg.log_dir, timespec_to_ns(&rt), auto_log_path, sizeof(auto_log_path));
    }
    if (!log_out) {
        perror("open sender log file");
        atomic_store(&rtcp.running, false);
        pthread_join(rtcp.listener_thread, NULL);
        pthread_mutex_destroy(&rtcp.retransmit_lock);
        pthread_mutex_destroy(&rtcp.feedback_lock);
        free(rtcp.retransmit_packet_ring);
        free(rtcp.retransmit_meta_ring);
        free(rtcp.feedback_ring);
        free(feedback_batch);
        free(packet_buf);
        free(iov);
        free(msgs);
        close(fd);
        rate_estimator_shutdown();
        return 1;
    }
    setvbuf(log_out, NULL, _IOLBF, 0);
    fprintf(log_out, "timestamp_us,frame_id,target_bitrate_bps,send_this_frame,planned_frame_packets,sent_frame_packets\n");
    if (log_out == stdout) {
        fprintf(stderr, "sender frame log: stdout\n");
    } else if (cfg.log_file_override) {
        fprintf(stderr, "sender fixed log file: %s\n", cfg.log_file_override);
    } else {
        fprintf(stderr, "sender log naming: %s/send_<first_pkt_sec>.csv\n", cfg.log_dir);
        fprintf(stderr, "sender frame log file: %s\n", auto_log_path);
    }

    if (rate_estimator_start() != 0) {
        fprintf(stderr, "rate estimator start failed\n");
        atomic_store(&rtcp.running, false);
        pthread_join(rtcp.listener_thread, NULL);
        pthread_mutex_destroy(&rtcp.retransmit_lock);
        pthread_mutex_destroy(&rtcp.feedback_lock);
        free(rtcp.retransmit_packet_ring);
        free(rtcp.retransmit_meta_ring);
        free(rtcp.feedback_ring);
        free(feedback_batch);
        free(packet_buf);
        free(iov);
        free(msgs);
        if (log_out && log_out != stdout) {
            fclose(log_out);
        }
        close(fd);
        rate_estimator_shutdown();
        return 1;
    }

    struct sender_context send_ctx = {
        .fd = fd,
        .iov = iov,
        .msgs = msgs,
        .rtcp = &rtcp,
        .send_seq = &send_seq,
        .rng = &rng,
        .cfg = &cfg,
    };

    struct pacer_state pacer;
    memset(&pacer, 0, sizeof(pacer));
    pacer.enabled = cfg.pacer_enabled;
    pacer.window_ns = cfg.pacer_window_ms * NS_PER_MS;
    pacer.send_ctx = &send_ctx;
    pthread_mutex_init(&pacer.lock, NULL);
    pthread_cond_init(&pacer.cond, NULL);

    if (pacer.enabled) {
        atomic_store(&pacer.running, true);
        if (pthread_create(&pacer.thread, NULL, pacer_thread_fn, &pacer) != 0) {
            fprintf(stderr, "pthread_create pacer failed\n");
            g_stop = 1;
        }
    }

    uint64_t acked_bytes_total = 0;
    uint64_t last_loss_total = 0;
    uint64_t acked_seq_ring[ACK_DEDUP_SLOTS];
    bool acked_seq_valid[ACK_DEDUP_SLOTS];
    memset(acked_seq_ring, 0, sizeof(acked_seq_ring));
    memset(acked_seq_valid, 0, sizeof(acked_seq_valid));

    while (!g_stop) {
        struct rate_estimator_input est_in;
        struct rate_estimator_output est_out;
        memset(&est_in, 0, sizeof(est_in));
        memset(&est_out, 0, sizeof(est_out));

        clock_gettime(CLOCK_MONOTONIC, &now);
        est_in.feedbacks = feedback_batch;
        est_in.feedback_count = drain_feedback_batch(&rtcp, feedback_batch, rtcp.feedback_ring_slots);
        for (size_t i = 0; i < est_in.feedback_count; ++i) {
            const struct rate_estimator_packet_feedback *f = &feedback_batch[i];
            uint32_t idx = (uint32_t)(f->send_seq % ACK_DEDUP_SLOTS);
            if (acked_seq_valid[idx] && acked_seq_ring[idx] == f->send_seq) {
                continue;
            }
            acked_seq_valid[idx] = true;
            acked_seq_ring[idx] = f->send_seq;
            acked_bytes_total += (uint64_t)f->pkt_len;
        }
        uint64_t sent_bytes_total = send_seq * (uint64_t)cfg.pkt_size;
        est_in.inflight_bytes = sent_bytes_total > acked_bytes_total ? (sent_bytes_total - acked_bytes_total) : 0ull;
        uint64_t loss_total = atomic_load(&rtcp.loss_events_total);
        est_in.loss_events_delta = loss_total >= last_loss_total ? (uint32_t)(loss_total - last_loss_total) : 0u;
        last_loss_total = loss_total;
        est_in.has_latest_sent_seq = (send_seq > 0);
        est_in.latest_sent_seq = est_in.has_latest_sent_seq ? (send_seq - 1u) : 0;
        est_in.frame_id = frame_id;
        est_in.now_us = ns_to_us(timespec_to_ns(&now));
        est_in.frame_interval_ns = frame_ns;
        est_in.init_bitrate_bps = cfg.bitrate_bps;
        if (rate_estimator_get_target(&est_in, &est_out) != 0) {
            fprintf(stderr, "rate estimator failed at frame=%" PRIu64 "\n", frame_id);
            break;
        }

        if (est_out.send_this_frame) {
            const double packets_per_frame =
                ((double)est_out.target_bitrate_bps / 8.0 / (double)cfg.pkt_size) / (double)cfg.fps;
            packet_budget += packets_per_frame;
            if (cfg.debug_estimator && ((frame_id % 30u) == 0u)) {
                fprintf(stderr,
                        "[SND] frame=%" PRIu64 " packets_per_frame=%.3f budget=%.3f\n",
                        frame_id,
                        packets_per_frame,
                        packet_budget);
            }
        }

        uint32_t frame_packets = 0;
        if (est_out.send_this_frame) {
            frame_packets = (uint32_t)floor(packet_budget);
            packet_budget -= (double)frame_packets;
        }
        if (est_out.burst_budget_packets > 0 && frame_packets > est_out.burst_budget_packets) {
            packet_budget += (double)(frame_packets - est_out.burst_budget_packets);
            frame_packets = est_out.burst_budget_packets;
        }

        uint32_t sent_in_frame = 0;
        if (pacer.enabled) {
            pthread_mutex_lock(&pacer.lock);
            pacer.target_bitrate_bps = est_out.target_bitrate_bps;
            pthread_mutex_unlock(&pacer.lock);

            if (frame_packets > 0) {
                pacer_enqueue(&pacer, frame_id, frame_packets);
            }
            sent_in_frame = frame_packets;
        } else {
            int rc = send_burst(&send_ctx, frame_id, frame_packets, 0, frame_packets);
            if (rc > 0) {
                sent_in_frame = (uint32_t)rc;
            }
        }

        struct timespec rt_now;
        clock_gettime(CLOCK_REALTIME, &rt_now);
        fprintf(log_out,
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu32 ",%" PRIu32 "\n",
                ns_to_us(timespec_to_ns(&rt_now)),
                frame_id,
                est_out.target_bitrate_bps,
                est_out.send_this_frame ? 1u : 0u,
                frame_packets,
                sent_in_frame);

        frame_id++;
        next_frame_ns += frame_ns;
        ns_to_timespec(next_frame_ns, &now);
        int sleep_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL);
        if (sleep_rc != 0 && sleep_rc != EINTR) {
            errno = sleep_rc;
            perror("clock_nanosleep");
            break;
        }
    }

    rate_estimator_shutdown();

    if (pacer.enabled) {
        atomic_store(&pacer.running, false);
        pthread_mutex_lock(&pacer.lock);
        pthread_cond_signal(&pacer.cond);
        pthread_mutex_unlock(&pacer.lock);
        pthread_join(pacer.thread, NULL);
    }
    pthread_mutex_destroy(&pacer.lock);
    pthread_cond_destroy(&pacer.cond);

    atomic_store(&rtcp.running, false);
    pthread_join(rtcp.listener_thread, NULL);
    pthread_mutex_destroy(&rtcp.retransmit_lock);
    pthread_mutex_destroy(&rtcp.feedback_lock);
    free(rtcp.retransmit_packet_ring);
    free(rtcp.retransmit_meta_ring);
    free(rtcp.feedback_ring);
    free(feedback_batch);
    free(packet_buf);
    free(iov);
    free(msgs);
    if (log_out && log_out != stdout) {
        fclose(log_out);
    }
    close(fd);
    return 0;
}
