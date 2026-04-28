#define _GNU_SOURCE

#include "proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct receiver_config {
    const char *bind_ip;
    uint16_t bind_port;
    const char *log_dir;
    const char *log_file_override;
    int rcvbuf;
    uint32_t idle_timeout_ms;
    uint32_t deadline_ms;
    uint32_t playout_fps;
};

#define MAX_FEEDBACK_RECORDS 128u
#define POLL_INTERVAL_MS 10
#define MAX_LOSS_REPORT_ENTRIES 1024u
#define DEFAULT_FEEDBACK_INTERVAL_PKTS 20u
#define AV1_MAX_TEMPLATE_COUNT 8u
#define AV1_MAX_DECODE_TARGET_COUNT 8u
#define AV1_MAX_CHAIN_COUNT 8u
#define AV1_MAX_FDIFF_COUNT 8u
#define PLAYBACK_MAX_TEMPORAL_UNITS 512u
#define PLAYBACK_PACKET_DEDUP_SLOTS 4096u

struct layer_packet_dedup {
    bool valid[PLAYBACK_PACKET_DEDUP_SLOTS];
    uint32_t packet_id[PLAYBACK_PACKET_DEDUP_SLOTS];
};

struct playback_layer_frame_state {
    bool seen;
    bool complete;
    uint32_t frame_packet_count;
    uint32_t received_packets;
    uint64_t received_bytes;
    uint64_t estimated_total_bytes;
    struct layer_packet_dedup packet_dedup;
};

struct playback_temporal_unit_state {
    bool valid;
    uint32_t temporal_unit_id;
    uint64_t first_recv_ts_us;
    uint64_t deadline_ts_us;
    struct playback_layer_frame_state layers[2];
};

struct playback_stats {
    uint64_t frame_interval_us;
    uint64_t stall_total_us;
    uint64_t played_units;
    uint64_t stalled_units;
};

struct av1_template_dependency_structure_template {
    uint8_t spatial_id;
    uint8_t temporal_id;
    uint8_t fdiff_count;
    uint8_t fdiffs[AV1_MAX_FDIFF_COUNT];
    uint8_t dtis[AV1_MAX_DECODE_TARGET_COUNT];
    uint8_t chain_fdiffs[AV1_MAX_CHAIN_COUNT];
};

struct av1_template_dependency_structure {
    bool valid;
    uint8_t template_id_offset;
    uint8_t decode_target_count;
    uint8_t chain_count;
    uint8_t template_count;
    struct av1_template_dependency_structure_template templates[AV1_MAX_TEMPLATE_COUNT];
};

struct av1_dependency_descriptor {
    bool parsed;
    uint8_t start_of_frame;
    uint8_t end_of_frame;
    uint8_t frame_dependency_template_id;
    uint16_t frame_number;

    uint8_t template_dependency_structure_present_flag;
    uint8_t active_decode_targets_present_flag;
    uint8_t custom_dtis_flag;
    uint8_t custom_fdiffs_flag;
    uint8_t custom_chains_flag;
    uint8_t spatial_id;
    uint8_t temporal_id;
    bool template_layer_info_available;
    uint8_t decode_target_count;
};

struct av1_rtp_analysis {
    bool found_av1_dependency_descriptor;
    bool is_udp;
    bool rtp_valid;
    bool extension_present;
    uint8_t rtp_version;
    uint16_t extension_profile;
    size_t extension_length_bytes;
    size_t prefix_len;
    struct av1_dependency_descriptor av1;
};

struct receiver_state {
    uint64_t last_seq_received;
    bool has_last_seq;
    uint64_t *lost_seqs;
    size_t lost_count;
    size_t lost_cap;
    struct rtcp_feedback_record feedback_records[MAX_FEEDBACK_RECORDS];
    uint32_t feedback_count;
    bool has_last_packet;
    uint64_t last_frame_id;
    uint32_t last_packet_id;
    uint64_t last_recv_ts_us;
    bool has_last_peer;
    struct sockaddr_in last_peer;
    struct av1_template_dependency_structure av1_templates;
};

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
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

static uint64_t timespec_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * NS_PER_SEC + (uint64_t)ts->tv_nsec;
}

static uint64_t ns_to_us(uint64_t ns)
{
    return ns / 1000ull;
}

static uint64_t monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ns(&ts);
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

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --bind-ip IP      default: %s\n"
            "  --bind-port PORT  default: %u\n"
            "  --log-dir PATH    default: .\n"
            "  --log-file PATH   optional fixed log path; use '-' for stdout\n"
            "  --idle-timeout MS default: 2000\n"
            "  --rcvbuf BYTES    default: 8388608\n"
            "  --deadline-ms MS  playback deadline per temporal unit, default: 100\n"
            "  --playout-fps FPS playout rate for stall stats, default: 30\n"
            "  --help            show this help\n"
            "Note: also accepts single-dash long style like -bind-port.\n",
            prog,
            DEFAULT_BIND_IP,
            DEFAULT_DST_PORT);
}

static int parse_args(int argc, char **argv, struct receiver_config *cfg)
{
    cfg->bind_ip = DEFAULT_BIND_IP;
    cfg->bind_port = DEFAULT_DST_PORT;
    cfg->log_dir = ".";
    cfg->log_file_override = NULL;
    cfg->rcvbuf = 8 * 1024 * 1024;
    cfg->idle_timeout_ms = 2000;
    cfg->deadline_ms = 100;
    cfg->playout_fps = DEFAULT_FPS;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (option_match(arg, "--help")) {
            print_usage(argv[0]);
            exit(0);
        } else if (option_match(arg, "--bind-ip")) {
            if (!val) return -1;
            cfg->bind_ip = val;
            i++;
        } else if (option_match(arg, "--bind-port")) {
            uint64_t p = 0;
            if (!val || parse_u64(val, &p) != 0 || p > 65535) return -1;
            cfg->bind_port = (uint16_t)p;
            i++;
        } else if (option_match(arg, "--log-dir")) {
            if (!val) return -1;
            cfg->log_dir = val;
            i++;
        } else if (option_match(arg, "--log-file")) {
            if (!val) return -1;
            cfg->log_file_override = val;
            i++;
        } else if (option_match(arg, "--idle-timeout")) {
            uint64_t t = 0;
            if (!val || parse_u64(val, &t) != 0 || t == 0 || t > 600000) return -1;
            cfg->idle_timeout_ms = (uint32_t)t;
            i++;
        } else if (option_match(arg, "--rcvbuf")) {
            uint64_t b = 0;
            if (!val || parse_u64(val, &b) != 0 || b > (uint64_t)INT32_MAX) return -1;
            cfg->rcvbuf = (int)b;
            i++;
        } else if (option_match(arg, "--deadline-ms")) {
            uint64_t d = 0;
            if (!val || parse_u64(val, &d) != 0 || d == 0 || d > 5000) return -1;
            cfg->deadline_ms = (uint32_t)d;
            i++;
        } else if (option_match(arg, "--playout-fps")) {
            uint64_t f = 0;
            if (!val || parse_u64(val, &f) != 0 || f == 0 || f > 1000) return -1;
            cfg->playout_fps = (uint32_t)f;
            i++;
        } else {
            return -1;
        }
    }
    return 0;
}

static FILE *open_auto_log_file(const char *log_dir, uint64_t first_ts_ns, char *path_buf, size_t path_buf_sz)
{
    uint64_t sec = first_ts_ns / NS_PER_SEC;
    int n = snprintf(path_buf, path_buf_sz, "%s/recv_%" PRIu64 ".csv", log_dir, sec);
    if (n <= 0 || (size_t)n >= path_buf_sz) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return fopen(path_buf, "w");
}

static FILE *open_auto_playback_log_file(const char *log_dir, uint64_t first_ts_ns, char *path_buf, size_t path_buf_sz)
{
    uint64_t sec = first_ts_ns / NS_PER_SEC;
    int n = snprintf(path_buf, path_buf_sz, "%s/playback_%" PRIu64 ".csv", log_dir, sec);
    if (n <= 0 || (size_t)n >= path_buf_sz) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return fopen(path_buf, "w");
}

static bool dedup_register_packet(struct layer_packet_dedup *dedup, uint32_t packet_id)
{
    if (!dedup) {
        return false;
    }
    const uint32_t idx = packet_id % PLAYBACK_PACKET_DEDUP_SLOTS;
    if (dedup->valid[idx] && dedup->packet_id[idx] == packet_id) {
        return false;
    }
    dedup->valid[idx] = true;
    dedup->packet_id[idx] = packet_id;
    return true;
}

static struct playback_temporal_unit_state *playback_get_or_create_temporal_unit(
    struct playback_temporal_unit_state *units,
    uint32_t temporal_unit_id,
    uint64_t now_us,
    uint32_t deadline_ms)
{
    if (!units) {
        return NULL;
    }

    size_t free_idx = PLAYBACK_MAX_TEMPORAL_UNITS;
    size_t evict_idx = 0u;
    uint64_t oldest_deadline = UINT64_MAX;
    for (size_t i = 0; i < PLAYBACK_MAX_TEMPORAL_UNITS; ++i) {
        if (units[i].valid && units[i].temporal_unit_id == temporal_unit_id) {
            return &units[i];
        }
        if (!units[i].valid && free_idx == PLAYBACK_MAX_TEMPORAL_UNITS) {
            free_idx = i;
        }
        if (units[i].valid && units[i].deadline_ts_us < oldest_deadline) {
            oldest_deadline = units[i].deadline_ts_us;
            evict_idx = i;
        }
    }

    size_t idx = (free_idx != PLAYBACK_MAX_TEMPORAL_UNITS) ? free_idx : evict_idx;
    memset(&units[idx], 0, sizeof(units[idx]));
    units[idx].valid = true;
    units[idx].temporal_unit_id = temporal_unit_id;
    units[idx].first_recv_ts_us = now_us;
    units[idx].deadline_ts_us = now_us + (uint64_t)deadline_ms * 1000ull;
    return &units[idx];
}

static void playback_finalize_temporal_unit(struct playback_temporal_unit_state *unit,
                                            struct playback_stats *stats,
                                            FILE *playback_out)
{
    if (!unit || !unit->valid || !stats || !playback_out) {
        return;
    }

    for (size_t sid = 0; sid < 2u; ++sid) {
        struct playback_layer_frame_state *layer = &unit->layers[sid];
        if (layer->seen &&
            layer->frame_packet_count > 0u &&
            layer->received_packets >= layer->frame_packet_count) {
            layer->complete = true;
        }
    }

    const bool s0_decodable = unit->layers[0].complete;
    const bool s1_decodable = unit->layers[1].complete && s0_decodable;

    int chosen_sid = -1;
    uint64_t decodable_bytes = 0u;
    if (s1_decodable) {
        chosen_sid = 1;
        decodable_bytes = unit->layers[1].received_bytes;
    } else if (s0_decodable) {
        chosen_sid = 0;
        decodable_bytes = unit->layers[0].received_bytes;
    }

    uint64_t total_bytes = unit->layers[1].estimated_total_bytes;
    if (total_bytes == 0u) {
        total_bytes = unit->layers[0].estimated_total_bytes;
    }
    if (total_bytes == 0u) {
        total_bytes = 1u;
    }

    uint64_t stall_us = 0u;
    if (chosen_sid < 0) {
        stall_us = stats->frame_interval_us;
        stats->stall_total_us += stall_us;
        stats->stalled_units++;
    } else {
        stats->played_units++;
    }
    const double quality = (double)decodable_bytes / (double)total_bytes;

    fprintf(playback_out,
            "%" PRIu64 ",%" PRIu32 ",%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
            unit->deadline_ts_us,
            unit->temporal_unit_id,
            chosen_sid,
            stall_us,
            decodable_bytes,
            total_bytes,
            quality);

    unit->valid = false;
}

static int send_rtcp_packet(int fd,
                            const struct sockaddr_in *peer,
                            const void *data,
                            size_t data_len)
{
    if (!peer || !data || data_len == 0) {
        return -1;
    }
    ssize_t sent = sendto(fd, data, data_len, 0, (const struct sockaddr *)peer, sizeof(*peer));
    if (sent < 0 || (size_t)sent != data_len) {
        return -1;
    }
    return 0;
}

static int send_feedback_batch(int fd,
                               const struct sockaddr_in *peer,
                               const struct rtcp_feedback_record *records,
                               uint16_t record_count,
                               struct receiver_state *state)
{
    if (!records || record_count == 0 || record_count > MAX_FEEDBACK_RECORDS) {
        return -1;
    }

    const size_t payload_len = sizeof(uint16_t) + ((size_t)record_count * sizeof(struct rtcp_feedback_record));
    const size_t packet_len = sizeof(struct rtcp_header) + payload_len;
    uint8_t *packet = (uint8_t *)malloc(packet_len);
    if (!packet) {
        return -1;
    }

    struct rtcp_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = RTCP_PKT_FEEDBACK;
    hdr.length_be = to_be16((uint16_t)packet_len);
    memcpy(packet, &hdr, sizeof(hdr));

    const uint16_t cnt_be = to_be16(record_count);
    memcpy(packet + sizeof(hdr), &cnt_be, sizeof(cnt_be));
    memcpy(packet + sizeof(hdr) + sizeof(cnt_be),
           records,
           (size_t)record_count * sizeof(struct rtcp_feedback_record));

    int rc = send_rtcp_packet(fd, peer, packet, packet_len);
    (void)state;
    free(packet);
    return rc;
}

static int append_feedback_record(struct receiver_state *state,
                                  uint64_t send_seq,
                                  uint64_t frame_id,
                                  uint32_t packet_id,
                                  uint32_t frame_packet_count,
                                  uint64_t recv_ts_us)
{
    if (state->feedback_count >= MAX_FEEDBACK_RECORDS) {
        return -1;
    }
    struct rtcp_feedback_record *r = &state->feedback_records[state->feedback_count++];
    r->send_seq_be = to_be64(send_seq);
    r->frame_id_be = to_be64(frame_id);
    r->packet_id_be = to_be32(packet_id);
    r->frame_packet_count_be = to_be32(frame_packet_count);
    r->recv_ts_us_be = to_be64(recv_ts_us);
    return 0;
}

static bool lost_list_has(const struct receiver_state *state, uint64_t seq)
{
    for (size_t i = 0; i < state->lost_count; ++i) {
        if (state->lost_seqs[i] == seq) {
            return true;
        }
    }
    return false;
}

static int lost_list_append(struct receiver_state *state, uint64_t seq)
{
    if (lost_list_has(state, seq)) {
        return 0;
    }
    if (state->lost_count == state->lost_cap) {
        size_t new_cap = state->lost_cap == 0 ? 64 : state->lost_cap * 2;
        uint64_t *new_buf = (uint64_t *)realloc(state->lost_seqs, new_cap * sizeof(uint64_t));
        if (!new_buf) {
            return -1;
        }
        state->lost_seqs = new_buf;
        state->lost_cap = new_cap;
    }
    state->lost_seqs[state->lost_count++] = seq;
    return 0;
}

static void lost_list_remove(struct receiver_state *state, uint64_t seq)
{
    for (size_t i = 0; i < state->lost_count; ++i) {
        if (state->lost_seqs[i] == seq) {
            if (i + 1 < state->lost_count) {
                memmove(&state->lost_seqs[i],
                        &state->lost_seqs[i + 1],
                        (state->lost_count - i - 1) * sizeof(uint64_t));
            }
            state->lost_count--;
            return;
        }
    }
}

static int send_loss_report(int fd, const struct sockaddr_in *peer, struct receiver_state *state)
{
    if (!peer || state->lost_count == 0) {
        return 0;
    }

    size_t send_count = state->lost_count;
    if (send_count > MAX_LOSS_REPORT_ENTRIES) {
        send_count = MAX_LOSS_REPORT_ENTRIES;
    }

    size_t payload_len = sizeof(uint16_t) + send_count * sizeof(uint64_t);
    size_t packet_len = sizeof(struct rtcp_header) + payload_len;
    uint8_t *packet = (uint8_t *)malloc(packet_len);
    if (!packet) {
        return -1;
    }

    struct rtcp_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = RTCP_PKT_LOSS;
    hdr.length_be = to_be16((uint16_t)packet_len);
    memcpy(packet, &hdr, sizeof(hdr));

    uint16_t cnt_be = to_be16((uint16_t)send_count);
    memcpy(packet + sizeof(hdr), &cnt_be, sizeof(cnt_be));

    uint64_t *seq_out = (uint64_t *)(packet + sizeof(hdr) + sizeof(cnt_be));
    for (size_t i = 0; i < send_count; ++i) {
        seq_out[i] = to_be64(state->lost_seqs[i]);
    }

    int rc = send_rtcp_packet(fd, peer, packet, packet_len);
    free(packet);
    return rc;
}

struct bit_reader {
    const uint8_t *data;
    size_t len;
    size_t bitpos;
};

static size_t bit_reader_bits_left(const struct bit_reader *br)
{
    return br->len * 8u - br->bitpos;
}

static int bit_reader_read_bits(struct bit_reader *br, int n, uint64_t *out)
{
    if (!br || !out || n <= 0) {
        return -1;
    }
    if (bit_reader_bits_left(br) < (size_t)n) {
        return -1;
    }
    uint64_t result = 0;
    for (int i = 0; i < n; ++i) {
        size_t byte_index = br->bitpos / 8u;
        int bit_index = 7 - (int)(br->bitpos % 8u);
        result = (result << 1u) | (uint64_t)((br->data[byte_index] >> bit_index) & 0x1u);
        ++br->bitpos;
    }
    *out = result;
    return 0;
}

static int bit_reader_read_bool(struct bit_reader *br, bool *out)
{
    uint64_t v = 0;
    if (bit_reader_read_bits(br, 1, &v) != 0) {
        return -1;
    }
    *out = (v == 1u);
    return 0;
}

static int bit_reader_read_ns(struct bit_reader *br, uint32_t n, uint64_t *out)
{
    if (!br || !out || n == 0u) {
        return -1;
    }
    if (n == 1u) {
        *out = 0u;
        return 0;
    }

    uint32_t w = 0u;
    for (uint32_t x = n; x > 0u; x >>= 1u) {
        w++;
    }

    const uint32_t m = (1u << w) - n;
    uint64_t v = 0;
    if (bit_reader_read_bits(br, (int)(w - 1u), &v) != 0) {
        return -1;
    }
    if (v < m) {
        *out = v;
        return 0;
    }

    uint64_t extra = 0;
    if (bit_reader_read_bits(br, 1, &extra) != 0) {
        return -1;
    }
    v = (v << 1u) | extra;
    v -= m;
    if (v >= n) {
        return -1;
    }
    *out = v;
    return 0;
}

static bool av1_template_structure_lookup(const struct av1_template_dependency_structure *structure,
                                          uint8_t template_id,
                                          uint8_t *spatial_id_out,
                                          uint8_t *temporal_id_out)
{
    if (!structure || !structure->valid) {
        return false;
    }
    if (template_id < structure->template_id_offset) {
        return false;
    }

    const uint8_t template_index = (uint8_t)(template_id - structure->template_id_offset);
    if (template_index >= structure->template_count) {
        return false;
    }

    if (spatial_id_out) {
        *spatial_id_out = structure->templates[template_index].spatial_id;
    }
    if (temporal_id_out) {
        *temporal_id_out = structure->templates[template_index].temporal_id;
    }
    return true;
}

static bool parse_av1_template_dependency_structure(
    struct bit_reader *br,
    struct av1_template_dependency_structure *structure)
{
    if (!br || !structure) {
        return false;
    }

    struct av1_template_dependency_structure parsed;
    memset(&parsed, 0, sizeof(parsed));

    uint64_t v = 0;
    if (bit_reader_read_bits(br, 6, &v) != 0) {
        return false;
    }
    parsed.template_id_offset = (uint8_t)v;

    if (bit_reader_read_bits(br, 5, &v) != 0) {
        return false;
    }
    parsed.decode_target_count = (uint8_t)(v + 1u);
    if (parsed.decode_target_count == 0u || parsed.decode_target_count > AV1_MAX_DECODE_TARGET_COUNT) {
        return false;
    }

    uint8_t spatial_id = 0u;
    uint8_t temporal_id = 0u;
    for (size_t i = 0; i < AV1_MAX_TEMPLATE_COUNT; ++i) {
        struct av1_template_dependency_structure_template *tmpl = &parsed.templates[parsed.template_count];
        tmpl->spatial_id = spatial_id;
        tmpl->temporal_id = temporal_id;
        parsed.template_count++;

        if (bit_reader_read_bits(br, 2, &v) != 0) {
            return false;
        }
        if (v == 1u) {
            temporal_id++;
        } else if (v == 2u) {
            temporal_id = 0u;
            spatial_id++;
        } else if (v == 3u) {
            break;
        }

        if (parsed.template_count >= AV1_MAX_TEMPLATE_COUNT && v != 3u) {
            return false;
        }
    }

    for (uint8_t template_idx = 0; template_idx < parsed.template_count; ++template_idx) {
        for (uint8_t dt_idx = 0; dt_idx < parsed.decode_target_count; ++dt_idx) {
            if (bit_reader_read_bits(br, 2, &v) != 0) {
                return false;
            }
            parsed.templates[template_idx].dtis[dt_idx] = (uint8_t)v;
        }
    }

    for (uint8_t template_idx = 0; template_idx < parsed.template_count; ++template_idx) {
        struct av1_template_dependency_structure_template *tmpl = &parsed.templates[template_idx];
        while (tmpl->fdiff_count < AV1_MAX_FDIFF_COUNT) {
            if (bit_reader_read_bits(br, 1, &v) != 0) {
                return false;
            }
            if (v == 0u) {
                break;
            }
            if (bit_reader_read_bits(br, 4, &v) != 0) {
                return false;
            }
            tmpl->fdiffs[tmpl->fdiff_count++] = (uint8_t)(v + 1u);
        }
        if (tmpl->fdiff_count >= AV1_MAX_FDIFF_COUNT) {
            return false;
        }
    }

    if (bit_reader_read_ns(br, parsed.decode_target_count + 1u, &v) != 0) {
        return false;
    }
    parsed.chain_count = (uint8_t)v;
    if (parsed.chain_count > AV1_MAX_CHAIN_COUNT) {
        return false;
    }

    for (uint8_t dt_idx = 0; dt_idx < parsed.decode_target_count; ++dt_idx) {
        if (parsed.chain_count == 0u) {
            break;
        }
        if (bit_reader_read_ns(br, parsed.chain_count, &v) != 0) {
            return false;
        }
    }

    for (uint8_t template_idx = 0; template_idx < parsed.template_count; ++template_idx) {
        for (uint8_t chain_idx = 0; chain_idx < parsed.chain_count; ++chain_idx) {
            if (bit_reader_read_bits(br, 4, &v) != 0) {
                return false;
            }
            parsed.templates[template_idx].chain_fdiffs[chain_idx] = (uint8_t)v;
        }
    }

    if (bit_reader_read_bits(br, 1, &v) != 0) {
        return false;
    }
    if (v != 0u) {
        return false;
    }

    parsed.valid = true;
    *structure = parsed;
    return true;
}

static bool parse_av1_dependency_descriptor(const uint8_t *data,
                                            size_t data_len,
                                            struct av1_template_dependency_structure *structure_cache,
                                            struct av1_dependency_descriptor *out)
{
    if (!data || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    struct bit_reader br;
    br.data = data;
    br.len = data_len;
    br.bitpos = 0;

    if (bit_reader_bits_left(&br) < 24u) {
        return false;
    }

    {
        bool b = false;
        uint64_t v = 0;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->start_of_frame = b ? 1u : 0u;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->end_of_frame = b ? 1u : 0u;

        if (bit_reader_read_bits(&br, 6, &v) != 0) return false;
        out->frame_dependency_template_id = (uint8_t)v;

        if (bit_reader_read_bits(&br, 16, &v) != 0) return false;
        out->frame_number = (uint16_t)v;
    }

    if (bit_reader_bits_left(&br) >= 5u) {
        bool b = false;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->template_dependency_structure_present_flag = b ? 1u : 0u;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->active_decode_targets_present_flag = b ? 1u : 0u;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->custom_dtis_flag = b ? 1u : 0u;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->custom_fdiffs_flag = b ? 1u : 0u;

        if (bit_reader_read_bool(&br, &b) != 0) return false;
        out->custom_chains_flag = b ? 1u : 0u;
    }

    if (out->template_dependency_structure_present_flag) {
        if (!structure_cache || !parse_av1_template_dependency_structure(&br, structure_cache)) {
            return false;
        }
    }

    if (structure_cache &&
        av1_template_structure_lookup(structure_cache,
                                      out->frame_dependency_template_id,
                                      &out->spatial_id,
                                      &out->temporal_id)) {
        out->template_layer_info_available = true;
        out->decode_target_count = structure_cache->decode_target_count;
    }

    out->parsed = true;
    return true;
}

static bool look_up_pack_av1_in_core(const uint8_t *buffer,
                                     size_t num_bytes,
                                     struct av1_template_dependency_structure *structure_cache,
                                     struct av1_rtp_analysis *res)
{
    if (!buffer || !res) {
        return false;
    }

    memset(res, 0, sizeof(*res));

    struct rtp_prefix_info prefix;
    if (!rtp_parse_prefix_info(buffer, num_bytes, &prefix)) {
        return false;
    }

    res->rtp_valid = prefix.rtp_valid;
    res->rtp_version = prefix.version;
    res->extension_present = prefix.extension_present;
    res->extension_profile = prefix.extension_profile;
    res->extension_length_bytes = prefix.extension_length_bytes;
    res->prefix_len = prefix.prefix_len;

    if (!prefix.extension_present) {
        return true;
    }

    const uint8_t *rtp = buffer;
    const size_t rtp_fixed_hdr_len = prefix.fixed_header_len;
    const size_t ext_bytes = prefix.extension_length_bytes;

    if (prefix.extension_profile == RTP_EXT_PROFILE_ONE_BYTE) {
        const uint8_t *ext_ptr = rtp + rtp_fixed_hdr_len + 4u;
        size_t bytes_rem = ext_bytes;

        while (bytes_rem > 0u) {
            const uint8_t b = *ext_ptr;
            if (b == 0x00u) {
                ext_ptr += 1u;
                bytes_rem -= 1u;
                continue;
            }

            const uint8_t ext_id = (uint8_t)(b >> 4);
            const uint8_t ext_len_minus1 = (uint8_t)(b & 0x0Fu);

            if (ext_id == 15u) {
                const size_t skip_len = (size_t)ext_len_minus1 + 1u;
                if (bytes_rem < 1u + skip_len) {
                    return false;
                }
                ext_ptr += 1u + skip_len;
                bytes_rem -= 1u + skip_len;
                continue;
            }

            const size_t data_len = (size_t)ext_len_minus1 + 1u;
            if (bytes_rem < 1u + data_len) {
                return false;
            }

            const uint8_t *data = ext_ptr + 1u;

            if (ext_id == RTP_EXT_ID_AV1_DEPENDENCY_DESCRIPTOR) {
                res->found_av1_dependency_descriptor = true;
                if (!parse_av1_dependency_descriptor(data, data_len, structure_cache, &res->av1)) {
                    return false;
                }
            }

            ext_ptr += 1u + data_len;
            bytes_rem -= 1u + data_len;
        }
        return true;
    }

    if ((prefix.extension_profile & 0xF000u) == 0x1000u) {
        const uint8_t *ext_ptr = rtp + rtp_fixed_hdr_len + 4u;
        size_t bytes_rem = ext_bytes;

        while (bytes_rem > 0u) {
            if (bytes_rem < 2u) {
                return false;
            }

            const uint8_t ext_id = ext_ptr[0];
            const uint8_t ext_len = ext_ptr[1];
            ext_ptr += 2u;
            bytes_rem -= 2u;

            if (ext_id == 0u) {
                if (bytes_rem < ext_len) {
                    return false;
                }
                ext_ptr += ext_len;
                bytes_rem -= ext_len;
                continue;
            }

            if (bytes_rem < ext_len) {
                return false;
            }

            if (ext_id == RTP_EXT_ID_AV1_DEPENDENCY_DESCRIPTOR) {
                res->found_av1_dependency_descriptor = true;
                if (!parse_av1_dependency_descriptor(ext_ptr, (size_t)ext_len, structure_cache, &res->av1)) {
                    return false;
                }
            }

            ext_ptr += ext_len;
            bytes_rem -= ext_len;
        }
        return true;
    }

    return false;
}

int main(int argc, char **argv)
{
    struct receiver_config cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf)) != 0) {
        perror("setsockopt(SO_RCVBUF)");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.bind_port);
    if (inet_pton(AF_INET, cfg.bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind ip: %s\n", cfg.bind_ip);
        close(fd);
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint8_t buf[65536];
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);

    FILE *out = NULL;
    FILE *playback_out = NULL;
    char auto_log_path[512] = {0};
    char auto_playback_log_path[512] = {0};
    bool session_active = false;
    uint64_t last_recv_mono_ns = 0;
    const uint64_t idle_timeout_ns = (uint64_t)cfg.idle_timeout_ms * NS_PER_MS;
    struct receiver_state twcc;
    memset(&twcc, 0, sizeof(twcc));
    struct playback_temporal_unit_state *playback_units =
        (struct playback_temporal_unit_state *)calloc(PLAYBACK_MAX_TEMPORAL_UNITS, sizeof(*playback_units));
    if (!playback_units) {
        fprintf(stderr, "oom\n");
        close(fd);
        return 1;
    }
    struct playback_stats playback_stats = {
        .frame_interval_us = 1000000ull / cfg.playout_fps,
        .stall_total_us = 0u,
        .played_units = 0u,
        .stalled_units = 0u,
    };

    if (cfg.log_file_override && strcmp(cfg.log_file_override, "-") == 0) {
        out = stdout;
    }

    fprintf(stderr, "receiver listening on %s:%u (app_ts=CLOCK_MONOTONIC)\n",
            cfg.bind_ip,
            cfg.bind_port);
    fprintf(stderr, "receiver playback deadline=%ums fps=%u\n",
            cfg.deadline_ms,
            cfg.playout_fps);
    fprintf(stderr, "receiver twcc mode: batch=%u or frame-start flush\n",
            DEFAULT_FEEDBACK_INTERVAL_PKTS);
    if (cfg.log_file_override && strcmp(cfg.log_file_override, "-") == 0) {
        fprintf(stderr, "receiver logging to stdout (CSV + readable line)\n");
    } else if (cfg.log_file_override) {
        fprintf(stderr, "receiver fixed log file: %s\n", cfg.log_file_override);
    } else {
        fprintf(stderr, "receiver log naming: %s/recv_<first_pkt_sec>.csv\n", cfg.log_dir);
    }

    while (!g_stop) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int prc = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (prc == 0) {
            uint64_t now_us = ns_to_us(monotonic_now_ns());
            for (size_t i = 0; i < PLAYBACK_MAX_TEMPORAL_UNITS; ++i) {
                if (playback_units[i].valid &&
                    playback_units[i].deadline_ts_us <= now_us &&
                    playback_out) {
                    playback_finalize_temporal_unit(&playback_units[i], &playback_stats, playback_out);
                }
            }
            if (session_active && monotonic_now_ns() - last_recv_mono_ns >= idle_timeout_ns) {
                fprintf(stderr,
                        "receiver timeout: %u ms未收到数据包，请检查发送端，继续等待...\n",
                        cfg.idle_timeout_ms);
                session_active = false;
                if (out && out != stdout) {
                    fclose(out);
                }
                out = NULL;
                if (playback_out && playback_out != stdout) {
                    fclose(playback_out);
                }
                playback_out = NULL;
            }
            continue;
        }

        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = NULL;
        msg.msg_controllen = 0;

        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvmsg");
            break;
        }

        struct av1_rtp_analysis av1;
        if (!look_up_pack_av1_in_core(buf, (size_t)n, &twcc.av1_templates, &av1)) {
            continue;
        }
        if (!av1.rtp_valid || !av1.extension_present || !av1.found_av1_dependency_descriptor) {
            continue;
        }

        if ((size_t)n < av1.prefix_len + sizeof(struct stream_payload_header)) {
            continue;
        }

        struct timespec app_ts;
        clock_gettime(CLOCK_MONOTONIC, &app_ts);
        uint64_t ts_ns = timespec_to_ns(&app_ts);
        const char *ts_source = "app_mono";
        uint64_t ts_us = ns_to_us(ts_ns);

        struct stream_payload_header hdr;
        size_t header_len = 0;
        if (!stream_payload_header_read(buf, (size_t)n, av1.prefix_len, &hdr, &header_len)) {
            continue;
        }
        if (from_be32(hdr.magic) != STREAM_MAGIC) {
            continue;
        }
        if (from_be16(hdr.version) != STREAM_VERSION) {
            continue;
        }
        if (header_len < sizeof(struct stream_payload_header)) {
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

        if (!session_active) {
            if (!out) {
                if (cfg.log_file_override && strcmp(cfg.log_file_override, "-") != 0) {
                    out = fopen(cfg.log_file_override, "w");
                } else if (cfg.log_file_override && strcmp(cfg.log_file_override, "-") == 0) {
                    out = stdout;
                } else {
                    out = open_auto_log_file(cfg.log_dir, ts_ns, auto_log_path, sizeof(auto_log_path));
                }
                if (!out) {
                    perror("open log file");
                    break;
                }
                setvbuf(out, NULL, _IOLBF, 0);
                fprintf(out, "timestamp_us,ts_source,frame_id,packet_id,frame_packet_count,send_seq,pkt_len,src_ip,src_port,rtp_version,rtp_ext_profile,av1_sof,av1_eof,av1_template_id,av1_tds_flag,av1_spatial_id,av1_temporal_id,av1_frame_number,av1_prefix_len\n");
            }
            if (!playback_out) {
                if (cfg.log_file_override && strcmp(cfg.log_file_override, "-") == 0) {
                    playback_out = stdout;
                } else if (cfg.log_file_override) {
                    char playback_fixed_path[1024];
                    int n_play = snprintf(playback_fixed_path,
                                          sizeof(playback_fixed_path),
                                          "%s.playback.csv",
                                          cfg.log_file_override);
                    if (n_play <= 0 || (size_t)n_play >= sizeof(playback_fixed_path)) {
                        fprintf(stderr, "playback log path too long\n");
                        break;
                    }
                    playback_out = fopen(playback_fixed_path, "w");
                } else {
                    playback_out = open_auto_playback_log_file(cfg.log_dir, ts_ns, auto_playback_log_path, sizeof(auto_playback_log_path));
                }
                if (!playback_out) {
                    perror("open playback log file");
                    break;
                }
                setvbuf(playback_out, NULL, _IOLBF, 0);
                fprintf(playback_out, "deadline_us,temporal_unit_id,chosen_sid,stall_us,decodable_bytes,total_bytes,quality\n");
            }
            session_active = true;
            if (out == stdout) {
                fprintf(stderr,
                        "receiver session start: from %s:%u -> local_port=%u, logging=stdout\n",
                        ip, ntohs(peer.sin_port), cfg.bind_port);
            } else if (cfg.log_file_override) {
                fprintf(stderr,
                        "receiver session start: from %s:%u -> local_port=%u, logging=%s\n",
                        ip, ntohs(peer.sin_port), cfg.bind_port, cfg.log_file_override);
            } else {
                fprintf(stderr,
                        "receiver session start: from %s:%u -> local_port=%u, logging=%s\n",
                        ip, ntohs(peer.sin_port), cfg.bind_port, auto_log_path);
                fprintf(stderr, "receiver playback log file: %s\n", auto_playback_log_path);
            }
        }

        last_recv_mono_ns = monotonic_now_ns();
        twcc.last_peer = peer;
        twcc.has_last_peer = true;

        uint64_t frame_id = from_be64(hdr.frame_id);
        uint32_t packet_id = from_be32(hdr.packet_id);
        uint64_t send_seq = from_be64(hdr.send_seq);

        bool frame_start_flush = twcc.feedback_count > 0 &&
                                 twcc.has_last_peer &&
                                 twcc.has_last_packet &&
                                 packet_id == 0 &&
                                 frame_id != twcc.last_frame_id;
        if (frame_start_flush) {
            if (send_feedback_batch(fd,
                                    &twcc.last_peer,
                                    twcc.feedback_records,
                                    (uint16_t)twcc.feedback_count,
                                    &twcc) != 0) {
                perror("send frame-start feedback rtcp");
            } else {
                twcc.feedback_count = 0;
            }
        }

        if (!twcc.has_last_seq) {
            twcc.last_seq_received = send_seq;
            twcc.has_last_seq = true;
        } else if (send_seq > twcc.last_seq_received + 1) {
            for (uint64_t missing = twcc.last_seq_received + 1; missing < send_seq; ++missing) {
                if (lost_list_append(&twcc, missing) != 0) {
                    perror("append lost seq");
                    break;
                }
            }
            twcc.last_seq_received = send_seq;
            if (send_loss_report(fd, &peer, &twcc) != 0) {
                perror("send loss rtcp");
            }
        } else {
            if (send_seq <= twcc.last_seq_received) {
                lost_list_remove(&twcc, send_seq);
            } else {
                twcc.last_seq_received = send_seq;
            }
        }

        uint32_t frame_packet_count = from_be32(hdr.frame_packet_count);

        uint32_t temporal_unit_id = 0u;
        if ((size_t)n >= sizeof(struct rtp_fixed_header)) {
            const struct rtp_fixed_header *rtp = (const struct rtp_fixed_header *)buf;
            temporal_unit_id = from_be32(rtp->timestamp_be);
        }
        uint8_t spatial_id = av1.av1.template_layer_info_available ? av1.av1.spatial_id : 0u;
        if (spatial_id < 2u) {
            struct playback_temporal_unit_state *tu =
                playback_get_or_create_temporal_unit(playback_units, temporal_unit_id, ts_us, cfg.deadline_ms);
            if (tu) {
                struct playback_layer_frame_state *layer = &tu->layers[spatial_id];
                if (!layer->seen) {
                    layer->seen = true;
                    layer->frame_packet_count = frame_packet_count;
                    layer->estimated_total_bytes = (uint64_t)frame_packet_count * (uint64_t)n;
                } else if (layer->frame_packet_count == 0u && frame_packet_count > 0u) {
                    layer->frame_packet_count = frame_packet_count;
                    layer->estimated_total_bytes = (uint64_t)frame_packet_count * (uint64_t)n;
                }
                if (dedup_register_packet(&layer->packet_dedup, packet_id)) {
                    layer->received_packets++;
                    layer->received_bytes += (uint64_t)n;
                    if (layer->frame_packet_count > 0u &&
                        layer->received_packets >= layer->frame_packet_count) {
                        layer->complete = true;
                    }
                }
            }
        }

        if (append_feedback_record(&twcc, send_seq, frame_id, packet_id, frame_packet_count, ts_us) != 0) {
            if (twcc.feedback_count > 0 &&
                send_feedback_batch(fd, &peer, twcc.feedback_records, (uint16_t)twcc.feedback_count, &twcc) != 0) {
                perror("flush feedback rtcp");
            }
            twcc.feedback_count = 0;
            if (append_feedback_record(&twcc, send_seq, frame_id, packet_id, frame_packet_count, ts_us) != 0) {
                perror("append feedback record");
            }
        }

        if (twcc.feedback_count >= DEFAULT_FEEDBACK_INTERVAL_PKTS) {
            if (send_feedback_batch(fd, &peer, twcc.feedback_records, (uint16_t)twcc.feedback_count, &twcc) != 0) {
                perror("send feedback rtcp");
            }
            twcc.feedback_count = 0;
        }

        twcc.has_last_packet = true;
        twcc.last_frame_id = frame_id;
        twcc.last_packet_id = packet_id;
        twcc.last_recv_ts_us = ts_us;

        fprintf(out,
                "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%zd,%s,%u,%u,0x%04x,%u,%u,%u,%u,%u,%u,%u,%zu\n",
                ts_us,
                ts_source,
                frame_id,
                packet_id,
                frame_packet_count,
                send_seq,
                n,
                ip,
                ntohs(peer.sin_port),
                av1.rtp_version,
                av1.extension_profile,
                av1.av1.start_of_frame,
                av1.av1.end_of_frame,
                av1.av1.frame_dependency_template_id,
                av1.av1.template_dependency_structure_present_flag,
                av1.av1.template_layer_info_available ? av1.av1.spatial_id : 0u,
                av1.av1.template_layer_info_available ? av1.av1.temporal_id : 0u,
                av1.av1.frame_number,
                av1.prefix_len);

        if (out == stdout) {
            printf("[RECV] ts=%" PRIu64 "us frame=%" PRIu64 " pkt=%" PRIu32 "/%" PRIu32
                   " seq=%" PRIu64 " len=%zd src=%s:%u RTPv=%u ext=0x%04x "
                   "AV1{sof=%u eof=%u template_id=%u tds=%u sid=%u tid=%u fnum=%u prefix=%zu}\n",
                   ts_us,
                   frame_id,
                   packet_id,
                   frame_packet_count,
                   send_seq,
                   n,
                   ip,
                   ntohs(peer.sin_port),
                   av1.rtp_version,
                   av1.extension_profile,
                   av1.av1.start_of_frame,
                   av1.av1.end_of_frame,
                   av1.av1.frame_dependency_template_id,
                   av1.av1.template_dependency_structure_present_flag,
                   av1.av1.template_layer_info_available ? av1.av1.spatial_id : 0u,
                   av1.av1.template_layer_info_available ? av1.av1.temporal_id : 0u,
                   av1.av1.frame_number,
                   av1.prefix_len);
        }

        for (size_t i = 0; i < PLAYBACK_MAX_TEMPORAL_UNITS; ++i) {
            if (playback_units[i].valid &&
                playback_units[i].deadline_ts_us <= ts_us &&
                playback_out) {
                playback_finalize_temporal_unit(&playback_units[i], &playback_stats, playback_out);
            }
        }
    }

    uint64_t now_us = ns_to_us(monotonic_now_ns());
    for (size_t i = 0; i < PLAYBACK_MAX_TEMPORAL_UNITS; ++i) {
        if (playback_units[i].valid && playback_out) {
            if (playback_units[i].deadline_ts_us > now_us) {
                playback_units[i].deadline_ts_us = now_us;
            }
            playback_finalize_temporal_unit(&playback_units[i], &playback_stats, playback_out);
        }
    }

    fprintf(stderr,
            "receiver playback summary: played=%" PRIu64 " stalled=%" PRIu64 " total_stall_ms=%.2f\n",
            playback_stats.played_units,
            playback_stats.stalled_units,
            (double)playback_stats.stall_total_us / 1000.0);

    if (out && out != stdout) {
        fclose(out);
    }
    if (playback_out && playback_out != stdout) {
        fclose(playback_out);
    }
    free(playback_units);
    free(twcc.lost_seqs);
    close(fd);
    return 0;
}
