#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STREAM_MAGIC 0x56494430u
#define STREAM_VERSION 1u

#define DEFAULT_DST_IP "192.168.1.10"
#define DEFAULT_DST_PORT 5201u
#define DEFAULT_BIND_IP "0.0.0.0"
#define DEFAULT_FPS 30u
#define DEFAULT_PKT_SIZE 1400u

#define BITRATE_MIN_BPS 250000ull
#define BITRATE_MAX_BPS 100000000ull
#define BITRATE_INIT_BPS 20000000ull

#define NS_PER_SEC 1000000000ull
#define NS_PER_MS 1000000ull

enum fill_mode {
    FILL_ZERO = 0,
    FILL_RANDOM = 1,
};

enum rtcp_packet_type {
    RTCP_PKT_LOSS = 1,
    RTCP_PKT_FEEDBACK = 2,
};

struct rtcp_header {
    uint8_t type;
    uint8_t reserved;
    uint16_t length_be;
} __attribute__((packed));

struct rtcp_feedback_record {
    uint64_t send_seq_be;
    uint64_t frame_id_be;
    uint32_t packet_id_be;
    uint32_t frame_packet_count_be;
    uint64_t recv_ts_us_be;
} __attribute__((packed));

struct stream_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint64_t frame_id;
    uint32_t packet_id;
    uint32_t frame_packet_count;
    uint64_t send_seq;
    uint64_t send_time_us;
} __attribute__((packed));

/* -------------------------------------------------------------------------- */
/* RTP / AV1 test definitions                                                 */
/* -------------------------------------------------------------------------- */

#define RTP_VERSION 2u
#define RTP_DEFAULT_PT 96u

/* V=2, P=0, X=1, CC=0 */
#define RTP_V2_X1_CC0 0x90u

#define RTP_PT96 0x60u
#define RTP_MARKER_BIT 0x80u

/* RFC5285 one-byte header extension profile */
#define RTP_EXT_PROFILE_ONE_BYTE 0xBEDEu

/* Parsed in pfcp_switch.cpp as AV1 dependency descriptor extension id */
#define RTP_EXT_ID_AV1_DEPENDENCY_DESCRIPTOR 7u

#define AV1_DD_MIN_BYTES 4u
#define AV1_DD_MAX_BYTES 16u
#define RTP_AV1_MIN_EXT_PAYLOAD_BYTES 8u
#define RTP_AV1_MAX_EXT_PAYLOAD_BYTES 20u
#define RTP_AV1_MIN_PREFIX_BYTES \
    (sizeof(struct rtp_fixed_header) + sizeof(struct rtp_extension_header) + RTP_AV1_MIN_EXT_PAYLOAD_BYTES)
#define RTP_AV1_MAX_PREFIX_BYTES \
    (sizeof(struct rtp_fixed_header) + sizeof(struct rtp_extension_header) + RTP_AV1_MAX_EXT_PAYLOAD_BYTES)

enum av1_frame_kind {
    AV1_FRAME_KIND_I = 0,
    AV1_FRAME_KIND_P = 1,
};

struct rtp_fixed_header {
    uint8_t vpxcc;
    uint8_t mpt;
    uint16_t sequence_number_be;
    uint32_t timestamp_be;
    uint32_t ssrc_be;
} __attribute__((packed));

struct rtp_extension_header {
    uint16_t profile_be;
    uint16_t length_words_be;
} __attribute__((packed));

/* RFC5285 one-byte extension element header:
 * high 4 bits: id
 * low 4 bits : len-1
 */
struct rtp_onebyte_ext_element {
    uint8_t id_len;
} __attribute__((packed));

/* Semantic model for AV1 dependency descriptor fixed fields.
 * Do not serialize this struct directly as wire format bit layout.
 */
struct av1_dd_fixed_fields {
    uint8_t start_of_frame;
    uint8_t end_of_frame;
    uint8_t frame_dependency_template_id;
    uint16_t frame_number;
};

struct av1_dd_flags {
    uint8_t template_dependency_structure_present_flag;
    uint8_t active_decode_targets_present_flag;
    uint8_t custom_dtis_flag;
    uint8_t custom_fdiffs_flag;
    uint8_t custom_chains_flag;
};

struct av1_layer_info {
    uint8_t spatial_id;
    uint8_t temporal_id;
};

struct rtp_prefix_info {
    bool rtp_valid;
    bool extension_present;
    uint8_t version;
    uint8_t csrc_count;
    uint16_t extension_profile;
    size_t fixed_header_len;
    size_t extension_length_bytes;
    size_t prefix_len;
};

_Static_assert(sizeof(struct rtcp_header) == 4, "unexpected rtcp_header size");
_Static_assert(sizeof(struct rtcp_feedback_record) == 32, "unexpected rtcp_feedback_record size");
_Static_assert(sizeof(struct stream_payload_header) == 40, "unexpected payload header size");
_Static_assert(sizeof(struct rtp_fixed_header) == 12, "unexpected rtp_fixed_header size");
_Static_assert(sizeof(struct rtp_extension_header) == 4, "unexpected rtp_extension_header size");
_Static_assert(sizeof(struct rtp_onebyte_ext_element) == 1, "unexpected rtp_onebyte_ext_element size");

static inline bool rtp_parse_prefix_info(const uint8_t *buffer,
                                         size_t num_bytes,
                                         struct rtp_prefix_info *out)
{
    if (!buffer || !out || num_bytes < sizeof(struct rtp_fixed_header)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const uint8_t b0 = buffer[0];
    out->version = (uint8_t)(b0 >> 6);
    if (out->version != RTP_VERSION) {
        return false;
    }

    out->rtp_valid = true;
    out->extension_present = (b0 & 0x10u) != 0;
    out->csrc_count = (uint8_t)(b0 & 0x0Fu);
    out->fixed_header_len = sizeof(struct rtp_fixed_header) + ((size_t)out->csrc_count * 4u);
    if (num_bytes < out->fixed_header_len) {
        return false;
    }

    if (!out->extension_present) {
        out->prefix_len = out->fixed_header_len;
        return true;
    }

    if (num_bytes < out->fixed_header_len + sizeof(struct rtp_extension_header)) {
        return false;
    }

    out->extension_profile =
        ((uint16_t)buffer[out->fixed_header_len] << 8) |
        (uint16_t)buffer[out->fixed_header_len + 1u];

    const uint16_t ext_len_words =
        ((uint16_t)buffer[out->fixed_header_len + 2u] << 8) |
        (uint16_t)buffer[out->fixed_header_len + 3u];

    out->extension_length_bytes = (size_t)ext_len_words * 4u;
    out->prefix_len = out->fixed_header_len + sizeof(struct rtp_extension_header) + out->extension_length_bytes;
    if (num_bytes < out->prefix_len) {
        return false;
    }

    return true;
}

static inline bool stream_payload_header_read(const uint8_t *packet,
                                              size_t packet_len,
                                              size_t offset,
                                              struct stream_payload_header *out,
                                              size_t *header_len_out)
{
    if (!packet || !out || offset > packet_len || packet_len - offset < sizeof(*out)) {
        return false;
    }

    memcpy(out, packet + offset, sizeof(*out));

    const uint16_t wire_header_len =
        ((uint16_t)packet[offset + 6u] << 8) | (uint16_t)packet[offset + 7u];
    if (wire_header_len < sizeof(*out) || packet_len - offset < wire_header_len) {
        return false;
    }

    if (header_len_out) {
        *header_len_out = wire_header_len;
    }
    return true;
}

#endif
