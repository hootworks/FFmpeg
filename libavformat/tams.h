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

#include "libavutil/bprint.h"
#include "libavutil/rational.h"
#include <stdint.h>

#define TAMS_TIMEBASE INT64_C(1000000000)
#define TAMS_UUID_SIZE 37
#define TAMS_MAX_TAGS 32
#define TAMS_TAG_KEY_SIZE 128
#define TAMS_TAG_VALUE_SIZE 256
#define TAMS_MAX_COLLECTION_ITEMS 16
#define TAMS_ROLE_SIZE 64

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

#endif /* AVFORMAT_TAMS_H */
