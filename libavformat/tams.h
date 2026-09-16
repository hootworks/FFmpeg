/*
 * Copyright (c) 2026 Nick Ryan <nick.ryan@hoot.works>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * Public header file for the processing of Time-Addressable Media Store (TAMS)
 * Flows.
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#ifndef AVFORMAT_TAMS_H
#define AVFORMAT_TAMS_H

#include "libavcodec/codec_id.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/rational.h"
#include <stdint.h>

struct AVFormatContext;

#define TAMS_TIMEBASE INT64_C(1000000000)
#define TAMS_UUID_SIZE 37
#define TAMS_MAX_TAGS 32
#define TAMS_TAG_KEY_SIZE 128
#define TAMS_TAG_VALUE_SIZE 256
#define TAMS_MAX_COLLECTION_ITEMS 16
#define TAMS_ROLE_SIZE 64

/**
 * The TAMS API version this implementation was written against. Both the
 * muxer and demuxer compare it to a fetched Service's api_version and warn
 * (but do not fail) on a mismatch.
 */
#define TAMS_API_VERSION "8.1"

#define AVRATIONAL_FORMAT "%d/%d"
#define AVRATIONAL_ARG(rational) rational.num, rational.den

/**
 * TAMS format URN strings, shared between probe and parsing.
 */
static const char *const tams_format_urns[] = {
    "urn:x-nmos:format:video",
    "urn:x-nmos:format:audio",
    "urn:x-nmos:format:data",
    "urn:x-nmos:format:multi",
    "urn:x-tam:format:image",
};

enum TAMSFlowFormat {
    TAMS_FORMAT_UNKNOWN = 0,
    TAMS_FORMAT_VIDEO,
    TAMS_FORMAT_AUDIO,
    TAMS_FORMAT_DATA,
    TAMS_FORMAT_MULTI,
    TAMS_FORMAT_IMAGE,
};

enum TAMSInterlaceMode {
    TAMS_INTERLACE_UNKNOWN = 0,
    TAMS_INTERLACE_PROGRESSIVE,
    TAMS_INTERLACE_TFF,
    TAMS_INTERLACE_BFF,
    TAMS_INTERLACE_PSF,
};

enum TAMSColorspace {
    TAMS_COLORSPACE_UNKNOWN = 0,
    TAMS_COLORSPACE_BT601,
    TAMS_COLORSPACE_BT709,
    TAMS_COLORSPACE_BT2020,
    TAMS_COLORSPACE_BT2100,
};

enum TAMSTransferCharacteristic {
    TAMS_TRANSFER_UNKNOWN = 0,
    TAMS_TRANSFER_SDR,
    TAMS_TRANSFER_HLG,
    TAMS_TRANSFER_PQ,
};

enum TAMSComponentType {
    TAMS_COMPONENT_UNKNOWN = 0,
    TAMS_COMPONENT_YCBCR,
    TAMS_COMPONENT_RGB,
};

enum TAMSAudioUncType {
    TAMS_AUDIO_UNC_UNKNOWN = 0,
    TAMS_AUDIO_UNC_INTERLEAVED,
    TAMS_AUDIO_UNC_PLANAR,
    TAMS_AUDIO_UNC_PAIRS,
};

enum TAMSVideoUncType {
    TAMS_VIDEO_UNC_UNKNOWN = 0,
    TAMS_VIDEO_UNC_PLANAR,
    TAMS_VIDEO_UNC_YUYV,
    TAMS_VIDEO_UNC_UYVY,
    TAMS_VIDEO_UNC_AYUV,
    TAMS_VIDEO_UNC_V210,
    TAMS_VIDEO_UNC_V216,
    TAMS_VIDEO_UNC_RGB,
    TAMS_VIDEO_UNC_RGBX,
    TAMS_VIDEO_UNC_XRGB,
    TAMS_VIDEO_UNC_BGRX,
    TAMS_VIDEO_UNC_XBGR,
    TAMS_VIDEO_UNC_RGBA,
    TAMS_VIDEO_UNC_ARGB,
    TAMS_VIDEO_UNC_BGRA,
    TAMS_VIDEO_UNC_ABGR,
    TAMS_VIDEO_UNC_ALPHA,
};

typedef struct TAMSAvcParameters {
    int profile;
    int level;
    int flags;
} TAMSAvcParameters;

typedef struct TAMSTag {
    char key[TAMS_TAG_KEY_SIZE];
    char value[TAMS_TAG_VALUE_SIZE];
} TAMSTag;

typedef struct TAMSTimeRange {
    int64_t start;
    int64_t end;
    int start_inclusive;
    int end_inclusive;
    int has_start;
    int has_end;
} TAMSTimeRange;

typedef struct TAMSContainerMapping {
    int track_index;
    int has_track_index;
    int format_track_index;
    int has_format_track_index;
    int mp2ts_pid;
    int has_mp2ts_pid;
    int isobmff_track_id;
    int has_isobmff_track_id;
    char mxf_package_uid[128];
    int mxf_track_id;
    int has_mxf_track_id;
    char audio_channel_range[32];
} TAMSContainerMapping;

typedef struct TAMSFlowCollectionItem {
    char id[TAMS_UUID_SIZE];
    char role[TAMS_ROLE_SIZE];
    TAMSContainerMapping container_mapping;
    int has_container_mapping;
} TAMSFlowCollectionItem;

typedef struct TAMSFlow {
    char id[TAMS_UUID_SIZE];
    char source_id[TAMS_UUID_SIZE];

    char label[256];
    char description[1024];
    enum TAMSFlowFormat format;
    char codec[128];
    char container[128];
    int generation;
    int avg_bit_rate;
    int max_bit_rate;
    int read_only;
    TAMSTimeRange timerange;
    AVRational segment_duration;
    TAMSTag tags[TAMS_MAX_TAGS];
    int nb_tags;
    char created_by[256];
    char updated_by[256];
    char metadata_version[256];
    char created[64];
    char metadata_updated[64];
    char segments_updated[64];

    /* Video/Image essence parameters */
    int frame_width;
    int frame_height;
    AVRational frame_rate;
    int bit_depth;
    enum TAMSInterlaceMode interlace_mode;
    enum TAMSColorspace colorspace;
    enum TAMSTransferCharacteristic transfer_characteristic;
    AVRational aspect_ratio;
    AVRational pixel_aspect_ratio;
    enum TAMSComponentType component_type;
    int horiz_chroma_subs;
    int vert_chroma_subs;
    int vfr;

    /* Audio essence parameters */
    int sample_rate;
    int channels;
    /* bit_depth is shared with video */
    int coded_frame_size;
    int mp4_oti;

    enum TAMSAudioUncType audio_unc_type;
    enum TAMSVideoUncType video_unc_type;
    TAMSAvcParameters avc_parameters;
    int has_avc_parameters;

    /* Data essence parameters */
    char data_type[256];

    /* Multi-flow collection */
    TAMSFlowCollectionItem flow_collection_items[TAMS_MAX_COLLECTION_ITEMS];
    int nb_flow_collection_items;
} TAMSFlow;

typedef struct TAMSFlowSegment {
    char object_id[512];
    TAMSTimeRange timerange;
    int64_t ts_offset;       /* defaults to 0 if absent from JSON */
    int64_t last_duration;
    int has_last_duration;
    char get_url[2048];
} TAMSFlowSegment;

typedef struct TAMSService {
    char type[64];
    char api_version[32];
    int64_t min_object_timeout;        /* whole seconds */
    int64_t min_presigned_url_timeout; /* whole seconds */
} TAMSService;

/**
 * One "http-request.json" object: an HTTP request a client should perform
 * (here, always the PUT of one Flow Segment's bytes). Only the fields the
 * muxer actually needs to issue that PUT are parsed; the spec's optional
 * "body" and arbitrary "headers" are not (this implementation only ever
 * PUTs presigned URLs that need no such extras).
 */
typedef struct TAMSHttpRequest {
    char url[2048];
    char content_type[128]; /* the JSON key is "content-type" */
} TAMSHttpRequest;

/**
 * One "flow-storage.json" media_objects[] entry, as returned by
 * POST /flows/{id}/storage.
 */
typedef struct TAMSMediaObject {
    char object_id[512];
    TAMSHttpRequest put_url;
} TAMSMediaObject;

/**
 * Parse a TAMS timestamp string into nanoseconds.
 * Format: "{sign?}{seconds}:{nanoseconds}"
 * @return 0 on success, AVERROR_INVALIDDATA on parse error
 */
int ff_tams_timestamp_from_str(const char *str, int64_t *ts);

/**
 * Format nanoseconds as a TAMS timestamp string ("{sign?}{seconds}:{nanoseconds}").
 * Inverse of ff_tams_timestamp_from_str().
 * @return 0 on success, AVERROR(EINVAL) if out_size is too small
 */
int ff_tams_timestamp_to_str(int64_t ts, char *out, size_t out_size);

/**
 * Parse a TAMS timerange string.
 * Format: "{bracket}{start_ts}_{end_ts}{bracket}"
 * @return 0 on success, AVERROR_INVALIDDATA on parse error
 */
int ff_tams_timerange_from_str(const char *str, TAMSTimeRange *tr);

/**
 * Format a TAMSTimeRange as a TAMS timerange string.
 * Inverse of ff_tams_timerange_from_str(): parsing the output of this function
 * always reproduces the same TAMSTimeRange, though not necessarily the exact
 * same string a server might have sent (e.g. the "[ts]" instantaneous
 * shorthand and eternity/never shorthands are never emitted, only their
 * equivalent explicit "{bracket}{start}_{end}{bracket}" form).
 * @return 0 on success, AVERROR(EINVAL) if out_size is too small
 */
int ff_tams_timerange_to_str(const TAMSTimeRange *tr, char *out, size_t out_size);

/**
 * Parse an ISO 8601 datetime string into microseconds since the Unix epoch.
 * Returns 0 if str is NULL, empty, or unparseable.
 */
int64_t ff_tams_iso8601_from_str(const char *str);

/**
 * Skip JSON whitespace. Useful for array iteration in the demuxer.
 */
void ff_tams_json_skip_ws(const char **p);

/**
 * Serialize a TAMSFlow to a JSON object suitable for a PUT /flows/{id} body.
 * Fields the TAMS spec documents as server-managed (timerange, created,
 * metadata_updated, segments_updated, collected_by) are intentionally never
 * emitted, since implementations MUST ignore them in a PUT request.
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_flow_to_json(AVBPrint *buf, const TAMSFlow *flow);

/**
 * Parse a TAMS /flows JSON response (array or single object, e.g. from
 * GET /flows or GET /flows/{id}), appending newly parsed Flows to an
 * existing dynamically-allocated array.
 *
 * *flows_inout must either be NULL or point to an av_realloc_array'd
 * buffer; it is grown as needed.  *nb_flows_inout is updated to reflect
 * the new count.  On failure the array and count are left in whatever
 * partial state they were in; the caller is responsible for freeing them.
 *
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_flows_from_json(const char *json,
                            TAMSFlow **flows_inout, int *nb_flows_inout);

/**
 * Serialize a TAMSFlowSegment to a JSON object suitable for a
 * POST /flows/{id}/segments body element.
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_flow_segment_to_json(AVBPrint *buf, const TAMSFlowSegment *seg);

/**
 * Parse a TAMS /flows/{id}/segments JSON array response, appending newly
 * parsed segments to an existing dynamically-allocated array.
 *
 * *segments_inout must either be NULL or point to an av_realloc_array'd
 * buffer; it is grown as needed.  *nb_segments_inout is updated to reflect
 * the new count.  On failure the array and count are left in whatever partial
 * state they were in; the caller is responsible for freeing them.
 *
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_flow_segments_from_json(const char *json,
                                    TAMSFlowSegment **segments_inout,
                                    int *nb_segments_inout);

/**
 * Parse a GET /service response.
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_service_from_json(const char *json, TAMSService *service);

/**
 * Parse a POST /flows/{id}/storage response ("flow-storage.json"),
 * appending its media_objects[] entries to an existing dynamically
 * allocated array.
 *
 * *objects_inout must either be NULL or point to an av_realloc_array'd
 * buffer; it is grown as needed. *nb_objects_inout is updated to reflect
 * the new count. On failure the array and count are left in whatever
 * partial state they were in; the caller is responsible for freeing them.
 *
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_storage_allocation_from_json(const char *json,
                                         TAMSMediaObject **objects_inout,
                                         int *nb_objects_inout);

/**
 * Look up the AVCodecID for a TAMS codec MIME string.
 * @return the codec id, or AV_CODEC_ID_NONE if unrecognized
 */
enum AVCodecID ff_tams_codec_from_mime(const char *mime);

/**
 * Look up the TAMS codec MIME string for an AVCodecID, the inverse of
 * ff_tams_codec_from_mime(). Where the spec allows several MIME aliases for
 * one codec, this returns the same one ff_tams_codec_from_mime() maps back.
 * @return the MIME string, or NULL if codec_id has no TAMS mapping
 */
const char *ff_tams_mime_from_codec(enum AVCodecID codec_id);

/**
 * Log one Flow's identity/essence/timerange summary at the given level.
 * Shared by the demuxer's and muxer's mapping-summary logging.
 */
void ff_tams_log_flow_summary(void *avcl, int level, const TAMSFlow *flow);

/**
 * Compare the hosts of two URLs (case-insensitively).
 * @return non-zero if the hosts match
 */
int ff_tams_same_host(const char *url1, const char *url2);

/**
 * Derive the store's "/flows" collection base URL from a full URL that is
 * either that collection URL itself (optionally with a query string) or a
 * path below it (e.g. "/flows/{id}"). If full_url's path ends in "/flows",
 * base_url is set to that exactly and, if is_exact is non-NULL, *is_exact
 * is set to 1. Otherwise base_url is set to full_url's parent directory
 * (appropriate when full_url points at a specific sub-resource such as a
 * Flow id) and *is_exact is set to 0.
 *
 * The demuxer, which accepts either URL form, ignores is_exact. The muxer,
 * which has no specific sub-resource to derive from and so must never
 * guess, passes is_exact and treats 0 as a hard error.
 *
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_get_base_url(const char *full_url, char *base_url, size_t base_size,
                                 int *is_exact);

/**
 * Build the URL for a single Flow: "{flows_base_url}/{flow_id}".
 * @return 0 on success, AVERROR(ENAMETOOLONG) if out_size is too small
 */
int ff_tams_flow_url(const char *flows_base_url, const char *flow_id,
                     char *out, size_t out_size);

/**
 * Build the URL for a Flow subresource: "{flows_base_url}/{flow_id}/{sub}"
 * (e.g. sub="storage" or sub="segments").
 * @return 0 on success, AVERROR(ENAMETOOLONG) if out_size is too small
 */
int ff_tams_flow_subresource_url(const char *flows_base_url, const char *flow_id,
                                 const char *sub, char *out, size_t out_size);

/**
 * Build the store-wide "/service" URL from the "/flows" collection base URL.
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_service_url(const char *flows_base_url, char *out, size_t out_size);

/**
 * Issue one TAMS HTTP request with bounded retry and backoff on transient errors
 * (429, 5xx, and generic network errors). 4xx/malformed responses are not retried.
 *
 * method=NULL means GET. A non-NULL body opens for writing (method defaults
 * to POST unless overridden). avio_opts, when non-NULL, is copied and
 * passed to s->io_open() for every attempt against the same host as s->url
 * but it is never sent to a different host (e.g. a presigned storage URL).
 * If out is non-NULL, the response body is read into it on success (the
 * caller must av_bprint_finalize() it). Note that *out is only initialized if the
 * function returns success.
 * @return 0 on success, negative AVERROR on failure
 */
int ff_tams_request(struct AVFormatContext *s, AVDictionary *const *avio_opts,
                    const char *url, const char *method,
                    const uint8_t *body, int body_size, const char *content_type,
                    int retry_max, int64_t retry_backoff_us, AVBPrint *out);

#endif /* AVFORMAT_TAMS_H */
