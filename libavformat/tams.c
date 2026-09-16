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
 * Implements TAMS Flow processing: JSON parsing, timestamp/timerange
 * manipulation, and Flow/Segment deserialization.
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#include "tams.h"
#include "avformat.h"
#include "avio.h"
#include "internal.h"
#include "url.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"
#include "libavutil/time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ====================================================================
 * Enum <-> JSON string tables, used by both the parsers above and the
 * serializers below so the supported values are listed exactly once.
 * Internal to this file: nothing outside tams.c needs enum<->string
 * conversion directly, callers just use ff_tams_flows_from_json()/
 * ff_tams_flow_to_json() and the enum values in TAMSFlow.
 * ==================================================================== */

static const char *const tams_interlace_mode_strs[] = {
    NULL, /* TAMS_INTERLACE_UNKNOWN */
    "progressive",
    "interlaced_tff",
    "interlaced_bff",
    "interlaced_psf",
};

static const char *const tams_colorspace_strs[] = {
    NULL, /* TAMS_COLORSPACE_UNKNOWN */
    "BT601",
    "BT709",
    "BT2020",
    "BT2100",
};

static const char *const tams_transfer_characteristic_strs[] = {
    NULL, /* TAMS_TRANSFER_UNKNOWN */
    "SDR",
    "HLG",
    "PQ",
};

static const char *const tams_component_type_strs[] = {
    NULL, /* TAMS_COMPONENT_UNKNOWN */
    "YCbCr",
    "RGB",
};

static const char *const tams_audio_unc_type_strs[] = {
    NULL, /* TAMS_AUDIO_UNC_UNKNOWN */
    "interleaved",
    "planar",
    "pairs",
};

static const char *const tams_video_unc_type_strs[] = {
    NULL, /* TAMS_VIDEO_UNC_UNKNOWN */
    "planar",
    "YUYV",
    "UYVY",
    "AYUV",
    "v210",
    "v216",
    "RGB",
    "RGBx",
    "xRGB",
    "BGRx",
    "xBGR",
    "RGBA",
    "ARGB",
    "BGRA",
    "ABGR",
    "alpha",
};

/**
 * Look up str in an index-matched enum string table (as defined above,
 * index 0 reserved for the "unknown"/NULL entry).
 * @return the matching index, or 0 if not found.
 */
static int tams_str_to_enum(const char *const *table, int nb, const char *str)
{
    for (int i = 1; i < nb; i++)
        if (table[i] && !strcmp(str, table[i]))
            return i;
    return 0;
}

#define TAMS_STR_TO_ENUM(table, str) tams_str_to_enum(table, FF_ARRAY_ELEMS(table), str)

/* ====================================================================
 * Timestamp and TimeRange parsing
 * ==================================================================== */

/**
 * Parse a timestamp from a string, advancing *end past the parsed portion
 * (if end is not NULL).
 * Format: {sign?}{seconds}:{nanoseconds}
 */
static int parse_timestamp_internal(const char *str, const char **end,
                                    int64_t *ts)
{
    const char *p = str;
    int negative = 0;
    int64_t seconds = 0, nanoseconds = 0;

    if (*p == '-') {
        negative = 1;
        p++;
    }

    if (*p < '0' || *p > '9')
        return AVERROR_INVALIDDATA;

    while (*p >= '0' && *p <= '9') {
        seconds = seconds * 10 + (*p - '0');
        p++;
    }

    if (*p != ':')
        return AVERROR_INVALIDDATA;
    p++;

    if (*p < '0' || *p > '9')
        return AVERROR_INVALIDDATA;

    while (*p >= '0' && *p <= '9') {
        nanoseconds = nanoseconds * 10 + (*p - '0');
        p++;
    }

    *ts = seconds * TAMS_TIMEBASE + nanoseconds;
    if (negative)
        *ts = -(*ts);

    if (end)
        *end = p;
    return 0;
}

/*
 * Parse a TAMS timestamp string into nanoseconds.
 * Format: "{sign?}{seconds}:{nanoseconds}"
 * Examples: "0:0", "10:500000000", "-5:0"
 */
int ff_tams_timestamp_from_str(const char *str, int64_t *ts)
{
    return parse_timestamp_internal(str, NULL, ts);
}

int ff_tams_timestamp_to_str(int64_t ts, char *out, size_t out_size)
{
    int64_t abs_ts = ts < 0 ? -ts : ts;
    int64_t seconds = abs_ts / TAMS_TIMEBASE;
    int64_t nanoseconds = abs_ts % TAMS_TIMEBASE;
    int len = snprintf(out, out_size, "%s%"PRId64":%"PRId64,
                        ts < 0 ? "-" : "", seconds, nanoseconds);

    if (len < 0 || (size_t)len >= out_size)
        return AVERROR(EINVAL);
    return 0;
}

/*
 * Parse a TAMS timerange string into a TAMSTimeRange struct.
 *
 * Format: "{bracket}{start_ts}_{end_ts}{bracket}"
 *   - '[' or '(' for inclusive/exclusive start
 *   - ']' or ')' for inclusive/exclusive end
 *   - Either timestamp may be omitted for an unbounded range
 *
 * Examples: "[0:0_10:0)", "(5:0_)", "[_20:0]"
 */
int ff_tams_timerange_from_str(const char *str, TAMSTimeRange *tr)
{
    const char *p = str;

    memset(tr, 0, sizeof(*tr));

    /* Parse start bracket */
    if (*p == '[') {
        tr->start_inclusive = 1;
        p++;
    } else if (*p == '(') {
        p++;
    }

    /* Parse start timestamp if present */
    if (*p != '_' && *p != ']' && *p != ')' && *p != '\0') {
        int ret = parse_timestamp_internal(p, &p, &tr->start);
        if (ret < 0)
            return ret;
        tr->has_start = 1;
    }

    /* Check for underscore separator */
    if (*p == '_') {
        p++;
        /* Parse end timestamp if present */
        if (*p != ']' && *p != ')' && *p != '\0') {
            int ret = parse_timestamp_internal(p, &p, &tr->end);
            if (ret < 0)
                return ret;
            tr->has_end = 1;
        }
    } else if (tr->has_start) {
        /* Instantaneous short form: [ts] means [ts_ts] */
        tr->end = tr->start;
        tr->has_end = 1;
    }

    /* Parse end bracket */
    if (*p == ']') {
        tr->end_inclusive = 1;
        p++;
    } else if (*p == ')') {
        p++;
    }

    return 0;
}

int ff_tams_timerange_to_str(const TAMSTimeRange *tr, char *out, size_t out_size)
{
    char start_buf[32] = "";
    char end_buf[32] = "";
    int len, ret;

    if (tr->has_start) {
        ret = ff_tams_timestamp_to_str(tr->start, start_buf, sizeof(start_buf));
        if (ret < 0)
            return ret;
    }
    if (tr->has_end) {
        ret = ff_tams_timestamp_to_str(tr->end, end_buf, sizeof(end_buf));
        if (ret < 0)
            return ret;
    }

    len = snprintf(out, out_size, "%c%s_%s%c",
                   tr->start_inclusive ? '[' : '(',
                   start_buf, end_buf,
                   tr->end_inclusive ? ']' : ')');

    if (len < 0 || (size_t)len >= out_size)
        return AVERROR(EINVAL);
    return 0;
}

int64_t ff_tams_iso8601_from_str(const char *str)
{
    int64_t t;
    if (!str || !str[0])
        return 0;
    if (av_parse_time(&t, str, 0) < 0)
        return 0;
    return t;
}

/* ====================================================================
 * Generic JSON utilities (read + write). Cursor-based on read,
 * AVBPrint-based on write. Not TAMS-specific -- these are shared
 * value-type primitives (string, int, bool, null, key, rational) used
 * throughout the TAMS-specific parsing/serialization further down.
 * ==================================================================== */

void ff_tams_json_skip_ws(const char **p)
{

    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

/**
 * Expect and consume a specific character (after skipping whitespace).
 * @return 0 on success, AVERROR_INVALIDDATA if the character does not match
 */
static int json_expect(const char **p, char c)
{
    ff_tams_json_skip_ws(p);
    if (**p != c)
        return AVERROR_INVALIDDATA;
    (*p)++;
    return 0;
}

/**
 * Read a JSON string value. Handles escape sequences.
 * If out is NULL, the string is skipped without storing.
 */
static int json_read_string(const char **p, char *out, size_t out_size)
{
    size_t len = 0;
    int ret;

    ret = json_expect(p, '"');
    if (ret < 0)
        return ret;


    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\') {
            (*p)++;
            if (!**p)
                return AVERROR_INVALIDDATA;
            c = **p;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                /* Skip 4 hex digits, replace with '?' */
                for (int i = 0; i < 4; i++) {
                    (*p)++;
                    if (!**p)
                        return AVERROR_INVALIDDATA;
                }
                c = '?';
                break;
            default:
                return AVERROR_INVALIDDATA;
            }
        }
        if (out && len < out_size - 1)
            out[len] = c;
        len++;
        (*p)++;
    }

    if (**p != '"')
        return AVERROR_INVALIDDATA;
    (*p)++;

    if (out) {
        if (len >= out_size)
            len = out_size - 1;
        out[len] = '\0';
    }

    return 0;
}

static int json_read_int(const char **p, int64_t *out)
{
    int64_t val = 0;
    int negative = 0;

    ff_tams_json_skip_ws(p);

    if (**p == '-') {
        negative = 1;
        (*p)++;
    }

    if (**p < '0' || **p > '9')
        return AVERROR_INVALIDDATA;


    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }

    /* Skip fractional part if present */
    if (**p == '.') {
        (*p)++;

        while (**p >= '0' && **p <= '9')
            (*p)++;
    }

    /* Skip exponent if present */
    if (**p == 'e' || **p == 'E') {
        (*p)++;
        if (**p == '+' || **p == '-')
            (*p)++;

        while (**p >= '0' && **p <= '9')
            (*p)++;
    }

    if (negative)
        val = -val;

    *out = val;
    return 0;
}

static int json_read_bool(const char **p, int *out)
{
    ff_tams_json_skip_ws(p);
    if (!strncmp(*p, "true", 4)) {
        *p += 4;
        *out = 1;
        return 0;
    }
    if (!strncmp(*p, "false", 5)) {
        *p += 5;
        *out = 0;
        return 0;
    }
    return AVERROR_INVALIDDATA;
}

/*
 * Skip over a complete JSON value (string, number, boolean, null,
 * object, or array) advancing the cursor past it.
 */
static int json_skip_value(const char **p)
{
    int depth;

    ff_tams_json_skip_ws(p);

    if (**p == '"')
        return json_read_string(p, NULL, 0);

    if (**p == '{' || **p == '[') {
        char open = **p;
        char close = (open == '{') ? '}' : ']';
        depth = 1;
        (*p)++;

        while (**p && depth > 0) {
            if (**p == '"') {
                int ret = json_read_string(p, NULL, 0);
                if (ret < 0)
                    return ret;
                continue;
            }
            if (**p == open)
                depth++;
            else if (**p == close)
                depth--;
            (*p)++;
        }
        return depth == 0 ? 0 : AVERROR_INVALIDDATA;
    }

    /* number, bool, null: skip until delimiter */
    while (**p && **p != ',' && **p != '}' && **p != ']' &&
           **p != ' ' && **p != '\t' && **p != '\r' && **p != '\n')
        (*p)++;
    return 0;
}

static int json_is_null(const char **p)
{
    const char *save = *p;
    ff_tams_json_skip_ws(p);
    if (!strncmp(*p, "null", 4)) {
        *p += 4;
        return 1;
    }
    *p = save;
    return 0;
}

/**
 * Read a JSON key: "key" followed by ':'.
 */
static int json_read_key(const char **p, char *out, size_t out_size)
{
    int ret;

    ff_tams_json_skip_ws(p);
    ret = json_read_string(p, out, out_size);
    if (ret < 0)
        return ret;

    return json_expect(p, ':');
}

/**
 * Read a JSON object of the form {"numerator": N, "denominator": D}.
 * denominator defaults to 1 if not present.
 */
static int json_read_rational(const char **p, AVRational *out)
{
    char key[32];
    int64_t val;
    int ret;

    out->num = 0;
    out->den = 1;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;


    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "numerator")) {
            ret = json_read_int(p, &val);
            if (ret < 0)
                return ret;
            out->num = (int)val;
        } else if (!strcmp(key, "denominator")) {
            ret = json_read_int(p, &val);
            if (ret < 0)
                return ret;
            out->den = (int)val;
        } else {
            ret = json_skip_value(p);
            if (ret < 0)
                return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */
    return 0;
}

/**
 * Write a JSON string literal, escaping quotes/backslash/control characters.
 * Bytes >= 0x20 are copied through unescaped (including multi-byte UTF-8
 * sequences), matching what json_read_string() accepts unescaped on read.
 */
static void json_write_string(AVBPrint *buf, const char *val)
{
    const unsigned char *c;

    av_bprint_chars(buf, '"', 1);
    for (c = (const unsigned char *)val; *c; c++) {
        switch (*c) {
        case '"':  av_bprintf(buf, "\\\""); break;
        case '\\': av_bprintf(buf, "\\\\"); break;
        case '\b': av_bprintf(buf, "\\b");  break;
        case '\f': av_bprintf(buf, "\\f");  break;
        case '\n': av_bprintf(buf, "\\n");  break;
        case '\r': av_bprintf(buf, "\\r");  break;
        case '\t': av_bprintf(buf, "\\t");  break;
        default:
            if (*c < 0x20)
                av_bprintf(buf, "\\u%04x", *c);
            else
                av_bprint_chars(buf, *c, 1);
        }
    }
    av_bprint_chars(buf, '"', 1);
}

/**
 * Write a JSON "key": value pair, where value is a string literal.
 * Pairs with json_read_key(), which reads the key half of the same shape.
 */
static void json_write_key_string(AVBPrint *buf, const char *key, const char *val)
{
    av_bprintf(buf, "\"%s\":", key);
    json_write_string(buf, val);
}

/**
 * Write a JSON "key": {"numerator": N, "denominator": D} pair.
 * Pairs with json_read_rational().
 */
static void json_write_rational(AVBPrint *buf, const char *key, AVRational r)
{
    av_bprintf(buf, "\"%s\":{\"numerator\":%d,\"denominator\":%d}", key, r.num, r.den);
}

/**
 * Write a field separator (',') before every field after the first in a
 * JSON object/array being built incrementally. Call once before each field,
 * before writing that field's key/value.
 */
static void json_write_sep(AVBPrint *buf, int *first)
{
    if (!*first)
        av_bprintf(buf, ",");
    *first = 0;
}

/* ====================================================================
 * TAMS specific serialization
 * ==================================================================== */

static void json_write_container_mapping(AVBPrint *buf, const TAMSContainerMapping *m)
{
    int first = 1;

    av_bprintf(buf, "{");
    if (m->has_track_index) {
        av_bprintf(buf, "\"track_index\":%d", m->track_index);
        first = 0;
    }
    if (m->has_format_track_index) {
        av_bprintf(buf, "%s\"format_track_index\":%d", first ? "" : ",", m->format_track_index);
        first = 0;
    }
    if (m->has_mp2ts_pid) {
        av_bprintf(buf, "%s\"mp2ts_container\":{\"pid\":%d}", first ? "" : ",", m->mp2ts_pid);
        first = 0;
    }
    if (m->has_isobmff_track_id) {
        av_bprintf(buf, "%s\"isobmff_container\":{\"track_id\":%d}", first ? "" : ",", m->isobmff_track_id);
        first = 0;
    }
    if (m->mxf_package_uid[0] || m->has_mxf_track_id) {
        av_bprintf(buf, "%s\"mxf_container\":{", first ? "" : ",");
        first = 0;
        if (m->mxf_package_uid[0]) {
            json_write_key_string(buf, "package_uid", m->mxf_package_uid);
            if (m->has_mxf_track_id)
                av_bprintf(buf, ",");
        }
        if (m->has_mxf_track_id)
            av_bprintf(buf, "\"track_id\":%d", m->mxf_track_id);
        av_bprintf(buf, "}");
    }
    if (m->audio_channel_range[0]) {
        av_bprintf(buf, "%s\"audio_track\":{", first ? "" : ",");
        json_write_key_string(buf, "channel_range", m->audio_channel_range);
        av_bprintf(buf, "}");
        first = 0;
    }
    av_bprintf(buf, "}");
}

static void json_write_video_essence(AVBPrint *buf, const TAMSFlow *flow)
{
    const char *s;
    int first = 1;

    av_bprintf(buf, "\"essence_parameters\":{");

    json_write_sep(buf, &first); av_bprintf(buf, "\"frame_width\":%d", flow->frame_width);
    json_write_sep(buf, &first); av_bprintf(buf, "\"frame_height\":%d", flow->frame_height);
    if (flow->frame_rate.num) {
        json_write_sep(buf, &first); json_write_rational(buf, "frame_rate", flow->frame_rate);
    }
    if (flow->bit_depth) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"bit_depth\":%d", flow->bit_depth);
    }
    if ((s = tams_interlace_mode_strs[flow->interlace_mode])) {
        json_write_sep(buf, &first); json_write_key_string(buf, "interlace_mode", s);
    }
    if ((s = tams_colorspace_strs[flow->colorspace])) {
        json_write_sep(buf, &first); json_write_key_string(buf, "colorspace", s);
    }
    if ((s = tams_transfer_characteristic_strs[flow->transfer_characteristic])) {
        json_write_sep(buf, &first); json_write_key_string(buf, "transfer_characteristic", s);
    }
    if (flow->aspect_ratio.num) {
        json_write_sep(buf, &first); json_write_rational(buf, "aspect_ratio", flow->aspect_ratio);
    }
    json_write_sep(buf, &first); json_write_rational(buf, "pixel_aspect_ratio", flow->pixel_aspect_ratio);
    if ((s = tams_component_type_strs[flow->component_type])) {
        json_write_sep(buf, &first); json_write_key_string(buf, "component_type", s);
    }
    if (flow->horiz_chroma_subs) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"horiz_chroma_subs\":%d", flow->horiz_chroma_subs);
    }
    if (flow->vert_chroma_subs) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"vert_chroma_subs\":%d", flow->vert_chroma_subs);
    }
    json_write_sep(buf, &first); av_bprintf(buf, "\"vfr\":%s", flow->vfr ? "true" : "false");
    if ((s = tams_video_unc_type_strs[flow->video_unc_type])) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"unc_parameters\":{");
        json_write_key_string(buf, "unc_type", s);
        av_bprintf(buf, "}");
    }
    if (flow->has_avc_parameters) {
        json_write_sep(buf, &first);
        av_bprintf(buf, "\"avc_parameters\":{\"profile\":%d,\"level\":%d,\"flags\":%d}",
                   flow->avc_parameters.profile, flow->avc_parameters.level,
                   flow->avc_parameters.flags);
    }

    av_bprintf(buf, "}");
}

static void json_write_audio_essence(AVBPrint *buf, const TAMSFlow *flow)
{
    const char *s;
    int first = 1;

    av_bprintf(buf, "\"essence_parameters\":{");

    json_write_sep(buf, &first); av_bprintf(buf, "\"sample_rate\":%d", flow->sample_rate);
    json_write_sep(buf, &first); av_bprintf(buf, "\"channels\":%d", flow->channels);
    if (flow->bit_depth) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"bit_depth\":%d", flow->bit_depth);
    }
    if (flow->coded_frame_size || flow->mp4_oti) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"codec_parameters\":{");
        if (flow->coded_frame_size)
            av_bprintf(buf, "\"coded_frame_size\":%d%s", flow->coded_frame_size,
                       flow->mp4_oti ? "," : "");
        if (flow->mp4_oti)
            av_bprintf(buf, "\"mp4_oti\":%d", flow->mp4_oti);
        av_bprintf(buf, "}");
    }
    if ((s = tams_audio_unc_type_strs[flow->audio_unc_type])) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"unc_parameters\":{");
        json_write_key_string(buf, "unc_type", s);
        av_bprintf(buf, "}");
    }

    av_bprintf(buf, "}");
}

static void json_write_flow_collection(AVBPrint *buf, const TAMSFlow *flow)
{
    av_bprintf(buf, "\"flow_collection\":[");
    for (int i = 0; i < flow->nb_flow_collection_items; i++) {
        const TAMSFlowCollectionItem *item = &flow->flow_collection_items[i];

        if (i)
            av_bprintf(buf, ",");
        av_bprintf(buf, "{");
        json_write_key_string(buf, "id", item->id);
        av_bprintf(buf, ",");
        json_write_key_string(buf, "role", item->role);
        if (item->has_container_mapping) {
            av_bprintf(buf, ",\"container_mapping\":");
            json_write_container_mapping(buf, &item->container_mapping);
        }
        av_bprintf(buf, "}");
    }
    av_bprintf(buf, "]");
}

/* ====================================================================
 * TAMS specific parsing
 * ==================================================================== */

/**
 * Parse a tag value: a JSON string, or an array of strings flattened with ','.
 * Any other type is skipped and buf is set to empty string.
 * TAMS-specific quirk (tags), not a generic JSON value type -- kept next to
 * the Flow parsing below where it's used, rather than in the generic
 * JSON utilities section.
 */
static int json_parse_tag_value(const char **p, char *buf, size_t size)
{
    ff_tams_json_skip_ws(p);
    if (**p == '[') {
        size_t len = 0;
        int first = 1;
        (*p)++; /* skip '[' */
        while (1) {
            int ret;
            ff_tams_json_skip_ws(p);
            if (**p == ']')
                break;
            if (!first && len < size - 1)
                buf[len++] = ',';
            first = 0;
            if (**p == '"') {
                char elem[TAMS_TAG_VALUE_SIZE];
                size_t elen, copy;
                ret = json_read_string(p, elem, sizeof(elem));
                if (ret < 0)
                    return ret;
                elen = strlen(elem);
                copy = elen < size - len - 1 ? elen : (size - len - 1);
                if (copy > 0) {
                    memcpy(buf + len, elem, copy);
                    len += copy;
                }
            } else {
                ret = json_skip_value(p);
                if (ret < 0)
                    return ret;
            }
            ff_tams_json_skip_ws(p);
            if (**p == ',')
                (*p)++;
        }
        if (**p != ']')
            return AVERROR_INVALIDDATA;
        (*p)++; /* skip ']' */
        buf[len < size ? len : size - 1] = '\0';
        return 0;
    }
    return json_read_string(p, buf, size);
}

static int parse_data_essence(const char **p, TAMSFlow *flow)
{
    char key[64];
    int ret;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "data_type")) {
            ret = json_read_string(p, flow->data_type, sizeof(flow->data_type));
            if (ret < 0) return ret;
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */
    return 0;
}

static int parse_video_essence(const char **p, TAMSFlow *flow)
{
    char key[64], str_val[64];
    int64_t val;
    int ret;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "frame_width")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->frame_width = (int)val;
        } else if (!strcmp(key, "frame_height")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->frame_height = (int)val;
        } else if (!strcmp(key, "frame_rate")) {
            ret = json_read_rational(p, &flow->frame_rate);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "bit_depth")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->bit_depth = (int)val;
        } else if (!strcmp(key, "interlace_mode")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            flow->interlace_mode = TAMS_STR_TO_ENUM(tams_interlace_mode_strs, str_val);
        } else if (!strcmp(key, "colorspace")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            flow->colorspace = TAMS_STR_TO_ENUM(tams_colorspace_strs, str_val);
        } else if (!strcmp(key, "transfer_characteristic")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            flow->transfer_characteristic = TAMS_STR_TO_ENUM(tams_transfer_characteristic_strs, str_val);
        } else if (!strcmp(key, "aspect_ratio")) {
            ret = json_read_rational(p, &flow->aspect_ratio);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "pixel_aspect_ratio")) {
            ret = json_read_rational(p, &flow->pixel_aspect_ratio);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "component_type")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            flow->component_type = TAMS_STR_TO_ENUM(tams_component_type_strs, str_val);
        } else if (!strcmp(key, "horiz_chroma_subs")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->horiz_chroma_subs = (int)val;
        } else if (!strcmp(key, "vert_chroma_subs")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->vert_chroma_subs = (int)val;
        } else if (!strcmp(key, "vfr")) {
            ret = json_read_bool(p, &flow->vfr);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "unc_parameters")) {
            char subkey[64];
            ret = json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_read_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "unc_type")) {
                    ret = json_read_string(p, str_val, sizeof(str_val));
                    if (ret < 0) return ret;
                    flow->video_unc_type = TAMS_STR_TO_ENUM(tams_video_unc_type_strs, str_val);
                } else {
                    ret = json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
        } else if (!strcmp(key, "avc_parameters")) {
            char subkey[64];
            int64_t val;
            ret = json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_read_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "profile")) {
                    ret = json_read_int(p, &val);
                    if (ret < 0) return ret;
                    flow->avc_parameters.profile = (int)val;
                } else if (!strcmp(subkey, "level")) {
                    ret = json_read_int(p, &val);
                    if (ret < 0) return ret;
                    flow->avc_parameters.level = (int)val;
                } else if (!strcmp(subkey, "flags")) {
                    ret = json_read_int(p, &val);
                    if (ret < 0) return ret;
                    flow->avc_parameters.flags = (int)val;
                } else {
                    ret = json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
            flow->has_avc_parameters = 1;
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */
    return 0;
}

static int parse_audio_essence(const char **p, TAMSFlow *flow)
{
    char key[64];
    int64_t val;
    int ret;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "sample_rate")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->sample_rate = (int)val;
        } else if (!strcmp(key, "channels")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->channels = (int)val;
        } else if (!strcmp(key, "bit_depth")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            flow->bit_depth = (int)val;
        } else if (!strcmp(key, "codec_parameters")) {
            /* Parse nested codec_parameters object */
            char subkey[64];
            ret = json_expect(p, '{');
            if (ret < 0) return ret;

            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_read_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "coded_frame_size")) {
                    ret = json_read_int(p, &val);
                    if (ret < 0) return ret;
                    flow->coded_frame_size = (int)val;
                } else if (!strcmp(subkey, "mp4_oti")) {
                    ret = json_read_int(p, &val);
                    if (ret < 0) return ret;
                    flow->mp4_oti = (int)val;
                } else {
                    ret = json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
        } else if (!strcmp(key, "unc_parameters")) {
            /* Parse nested unc_parameters object */
            char subkey[64], str_val[64];
            ret = json_expect(p, '{');
            if (ret < 0) return ret;

            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_read_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "unc_type")) {
                    ret = json_read_string(p, str_val, sizeof(str_val));
                    if (ret < 0) return ret;
                    flow->audio_unc_type = TAMS_STR_TO_ENUM(tams_audio_unc_type_strs, str_val);
                } else {
                    ret = json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */
    return 0;
}

static int parse_mp2ts_container(const char **p, TAMSContainerMapping *mapping)
{
    char key[64];
    int64_t val;
    int ret;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "pid")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            mapping->mp2ts_pid = (int)val;
            mapping->has_mp2ts_pid = 1;
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

static int parse_isobmff_container(const char **p, TAMSContainerMapping *mapping)
{
    char key[64];
    int64_t val;
    int ret;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "track_id")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            mapping->isobmff_track_id = (int)val;
            mapping->has_isobmff_track_id = 1;
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

static int parse_mxf_container(const char **p, TAMSContainerMapping *mapping)
{
    char key[64];
    int64_t val;
    int ret;

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "package_uid")) {
            ret = json_read_string(p, mapping->mxf_package_uid,
                                    sizeof(mapping->mxf_package_uid));
            if (ret < 0) return ret;
        } else if (!strcmp(key, "track_id")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            mapping->mxf_track_id = (int)val;
            mapping->has_mxf_track_id = 1;
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

static int parse_container_mapping(const char **p, TAMSContainerMapping *mapping)
{
    char key[64];
    int64_t val;
    int ret;

    memset(mapping, 0, sizeof(*mapping));

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "track_index")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            mapping->track_index = (int)val;
            mapping->has_track_index = 1;
        } else if (!strcmp(key, "format_track_index")) {
            ret = json_read_int(p, &val);
            if (ret < 0) return ret;
            mapping->format_track_index = (int)val;
            mapping->has_format_track_index = 1;
        } else if (!strcmp(key, "mp2ts_container")) {
            ret = parse_mp2ts_container(p, mapping);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "isobmff_container")) {
            ret = parse_isobmff_container(p, mapping);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "mxf_container")) {
            ret = parse_mxf_container(p, mapping);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "audio_track")) {
            char subkey[64];
            ret = json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_read_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "channel_range")) {
                    ret = json_read_string(p, mapping->audio_channel_range,
                                            sizeof(mapping->audio_channel_range));
                    if (ret < 0) return ret;
                } else {
                    ret = json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++;
        } else {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

static int parse_flow_collection(const char **p, TAMSFlow *flow)
{
    char key[64];
    int ret;

    ret = json_expect(p, '[');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == ']')
            break;

        if (flow->nb_flow_collection_items >= TAMS_MAX_COLLECTION_ITEMS) {
            ret = json_skip_value(p);
            if (ret < 0) return ret;
        } else {
            TAMSFlowCollectionItem *item =
                &flow->flow_collection_items[flow->nb_flow_collection_items];
            memset(item, 0, sizeof(*item));

            ret = json_expect(p, '{');
            if (ret < 0) return ret;

            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;

                ret = json_read_key(p, key, sizeof(key));
                if (ret < 0) return ret;

                if (!strcmp(key, "id")) {
                    ret = json_read_string(p, item->id, sizeof(item->id));
                } else if (!strcmp(key, "role")) {
                    ret = json_read_string(p, item->role, sizeof(item->role));
                } else if (!strcmp(key, "container_mapping")) {
                    ret = parse_container_mapping(p, &item->container_mapping);
                    if (ret == 0)
                        item->has_container_mapping = 1;
                } else {
                    ret = json_skip_value(p);
                }

                if (ret < 0) return ret;

                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++;
            flow->nb_flow_collection_items++;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

static enum TAMSFlowFormat parse_format_urn(const char *format)
{
    if (!strcmp(format, "urn:x-nmos:format:video"))
        return TAMS_FORMAT_VIDEO;
    if (!strcmp(format, "urn:x-nmos:format:audio"))
        return TAMS_FORMAT_AUDIO;
    if (!strcmp(format, "urn:x-nmos:format:data"))
        return TAMS_FORMAT_DATA;
    if (!strcmp(format, "urn:x-nmos:format:multi"))
        return TAMS_FORMAT_MULTI;
    if (!strcmp(format, "urn:x-tam:format:image"))
        return TAMS_FORMAT_IMAGE;
    return TAMS_FORMAT_UNKNOWN;
}

/**
 * Parse the get_urls array and extract the best URL.
 * Prefers entries with "presigned":true; falls back to the first URL.
 */
static int parse_get_urls(const char **p, char *url, size_t url_size)
{
    char key[32], candidate[2048];
    int ret, found_presigned = 0;

    ret = json_expect(p, '[');
    if (ret < 0)
        return ret;

    while (1) {
        int is_presigned = 0;

        ff_tams_json_skip_ws(p);
        if (**p == ']')
            break;

        candidate[0] = '\0';
        ret = json_expect(p, '{');
        if (ret < 0)
            return ret;

        while (1) {
            ff_tams_json_skip_ws(p);
            if (**p == '}')
                break;
            ret = json_read_key(p, key, sizeof(key));
            if (ret < 0)
                return ret;
            if (!strcmp(key, "url")) {
                ret = json_read_string(p, candidate, sizeof(candidate));
                if (ret < 0)
                    return ret;
            } else if (!strcmp(key, "presigned")) {
                ff_tams_json_skip_ws(p);
                if (!strncmp(*p, "true", 4)) {
                    is_presigned = 1;
                    *p += 4;
                } else if (!strncmp(*p, "false", 5)) {
                    *p += 5;
                } else {
                    ret = json_skip_value(p);
                    if (ret < 0)
                        return ret;
                }
            } else {
                ret = json_skip_value(p);
                if (ret < 0)
                    return ret;
            }
            ff_tams_json_skip_ws(p);
            if (**p == ',')
                (*p)++;
        }
        (*p)++;

        if (candidate[0] && (is_presigned || !url[0])) {
            av_strlcpy(url, candidate, url_size);
            if (is_presigned)
                found_presigned = 1;
        }

        if (found_presigned) {
            ff_tams_json_skip_ws(p);
            while (**p == ',') {
                (*p)++;
                ff_tams_json_skip_ws(p);
                if (**p == ']')
                    break;
                ret = json_skip_value(p);
                if (ret < 0)
                    return ret;
                ff_tams_json_skip_ws(p);
            }
            break;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

/**
 * Parse a single TAMS Flow JSON object from a buffer, advancing the cursor
 * past it. Internal building block for ff_tams_flows_from_json(); not part
 * of the public API since every caller needs the array-or-single-object
 * handling that function provides.
 */
static int tams_flow_from_json(const char **p, TAMSFlow *flow)
{
    char key[64], str_val[1024];
    int64_t val;
    int ret;

    memset(flow, 0, sizeof(*flow));
    flow->segment_duration = (AVRational){0, 1};
    flow->pixel_aspect_ratio = (AVRational){1, 1};

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(p)) {
            /* null value: skip */
        } else if (!strcmp(key, "id")) {
            ret = json_read_string(p, flow->id, sizeof(flow->id));
        } else if (!strcmp(key, "source_id")) {
            ret = json_read_string(p, flow->source_id, sizeof(flow->source_id));
        } else if (!strcmp(key, "label")) {
            ret = json_read_string(p, flow->label, sizeof(flow->label));
        } else if (!strcmp(key, "description")) {
            ret = json_read_string(p, flow->description, sizeof(flow->description));
        } else if (!strcmp(key, "format")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                flow->format = parse_format_urn(str_val);
        } else if (!strcmp(key, "codec")) {
            ret = json_read_string(p, flow->codec, sizeof(flow->codec));
        } else if (!strcmp(key, "container")) {
            ret = json_read_string(p, flow->container, sizeof(flow->container));
        } else if (!strcmp(key, "generation")) {
            ret = json_read_int(p, &val);
            if (ret == 0) flow->generation = (int)val;
        } else if (!strcmp(key, "avg_bit_rate")) {
            ret = json_read_int(p, &val);
            if (ret == 0) flow->avg_bit_rate = (int)val;
        } else if (!strcmp(key, "max_bit_rate")) {
            ret = json_read_int(p, &val);
            if (ret == 0) flow->max_bit_rate = (int)val;
        } else if (!strcmp(key, "read_only")) {
            ret = json_read_bool(p, &flow->read_only);
        } else if (!strcmp(key, "timerange")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                ret = ff_tams_timerange_from_str(str_val, &flow->timerange);
        } else if (!strcmp(key, "segment_duration")) {
            ret = json_read_rational(p, &flow->segment_duration);
        } else if (!strcmp(key, "tags")) {
            /* Parse tags object: {"key1": "val1", "key2": "val2", ...} */
            ret = json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                if (flow->nb_tags < TAMS_MAX_TAGS) {
                    TAMSTag *tag = &flow->tags[flow->nb_tags];
                    ret = json_read_string(p, tag->key, sizeof(tag->key));
                    if (ret < 0) return ret;
                    ret = json_expect(p, ':');
                    if (ret < 0) return ret;
                    ret = json_parse_tag_value(p, tag->value, sizeof(tag->value));
                    if (ret < 0) return ret;
                    flow->nb_tags++;
                } else {
                    /* Skip key */
                    ret = json_read_string(p, NULL, 0);
                    if (ret < 0) return ret;
                    ret = json_expect(p, ':');
                    if (ret < 0) return ret;
                    ret = json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
        } else if (!strcmp(key, "created_by")) {
            ret = json_read_string(p, flow->created_by, sizeof(flow->created_by));
        } else if (!strcmp(key, "updated_by")) {
            ret = json_read_string(p, flow->updated_by, sizeof(flow->updated_by));
        } else if (!strcmp(key, "metadata_version")) {
            ret = json_read_string(p, flow->metadata_version, sizeof(flow->metadata_version));
        } else if (!strcmp(key, "created")) {
            ret = json_read_string(p, flow->created, sizeof(flow->created));
        } else if (!strcmp(key, "metadata_updated")) {
            ret = json_read_string(p, flow->metadata_updated, sizeof(flow->metadata_updated));
        } else if (!strcmp(key, "segments_updated")) {
            ret = json_read_string(p, flow->segments_updated, sizeof(flow->segments_updated));
        } else if (!strcmp(key, "flow_collection")) {
            ret = parse_flow_collection(p, flow);
        } else if (!strcmp(key, "essence_parameters")) {
            if (flow->format == TAMS_FORMAT_VIDEO ||
                flow->format == TAMS_FORMAT_IMAGE)
                ret = parse_video_essence(p, flow);
            else if (flow->format == TAMS_FORMAT_AUDIO)
                ret = parse_audio_essence(p, flow);
            else if (flow->format == TAMS_FORMAT_DATA)
                ret = parse_data_essence(p, flow);
            else
                ret = json_skip_value(p);
        } else {
            ret = json_skip_value(p);
        }

        if (ret < 0)
            return ret;

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */
    return 0;
}

/**
 * Parse a single TAMS Flow Segment JSON object from a buffer, advancing the
 * cursor past it. Internal building block for
 * ff_tams_flow_segments_from_json(); not part of the public API.
 */
static int tams_flow_segment_from_json(const char **p, TAMSFlowSegment *seg)
{
    char key[64], str_val[256];
    int ret;

    memset(seg, 0, sizeof(*seg));

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(p)) {
            /* null value: skip */
        } else if (!strcmp(key, "object_id")) {
            ret = json_read_string(p, seg->object_id, sizeof(seg->object_id));
        } else if (!strcmp(key, "timerange")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                ret = ff_tams_timerange_from_str(str_val, &seg->timerange);
        } else if (!strcmp(key, "ts_offset")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                ret = ff_tams_timestamp_from_str(str_val, &seg->ts_offset);
        } else if (!strcmp(key, "last_duration")) {
            ret = json_read_string(p, str_val, sizeof(str_val));
            if (ret == 0) {
                ret = ff_tams_timestamp_from_str(str_val, &seg->last_duration);
                if (ret == 0)
                    seg->has_last_duration = 1;
            }
        } else if (!strcmp(key, "get_urls")) {
            ret = parse_get_urls(p, seg->get_url, sizeof(seg->get_url));
        } else {
            ret = json_skip_value(p);
        }

        if (ret < 0)
            return ret;

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */

    if (!seg->timerange.has_start || !seg->timerange.has_end)
        return AVERROR_INVALIDDATA;

    return 0;
}

/* ====================================================================
 * Main functions for converting TAMS Flow and Flow Segment structures to/from JSON.
 * ==================================================================== */

int ff_tams_flow_to_json(AVBPrint *buf, const TAMSFlow *flow)
{
    int first = 1;

    if (flow->format == TAMS_FORMAT_UNKNOWN || flow->format > TAMS_FORMAT_IMAGE)
        return AVERROR(EINVAL);

    av_bprintf(buf, "{");

    if (flow->id[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "id", flow->id);
    }
    if (flow->source_id[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "source_id", flow->source_id);
    }
    if (flow->label[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "label", flow->label);
    }
    if (flow->description[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "description", flow->description);
    }
    json_write_sep(buf, &first); json_write_key_string(buf, "format", tams_format_urns[flow->format - 1]);
    if (flow->codec[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "codec", flow->codec);
    }
    if (flow->container[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "container", flow->container);
    }
    json_write_sep(buf, &first); av_bprintf(buf, "\"generation\":%d", flow->generation);
    if (flow->avg_bit_rate) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"avg_bit_rate\":%d", flow->avg_bit_rate);
    }
    if (flow->max_bit_rate) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"max_bit_rate\":%d", flow->max_bit_rate);
    }
    json_write_sep(buf, &first); av_bprintf(buf, "\"read_only\":%s", flow->read_only ? "true" : "false");
    if (flow->segment_duration.num) {
        json_write_sep(buf, &first); json_write_rational(buf, "segment_duration", flow->segment_duration);
    }
    if (flow->nb_tags) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"tags\":{");
        for (int i = 0; i < flow->nb_tags; i++) {
            if (i)
                av_bprintf(buf, ",");
            json_write_key_string(buf, flow->tags[i].key, flow->tags[i].value);
        }
        av_bprintf(buf, "}");
    }
    if (flow->created_by[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "created_by", flow->created_by);
    }
    if (flow->updated_by[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "updated_by", flow->updated_by);
    }
    if (flow->metadata_version[0]) {
        json_write_sep(buf, &first); json_write_key_string(buf, "metadata_version", flow->metadata_version);
    }

    if (flow->format == TAMS_FORMAT_VIDEO || flow->format == TAMS_FORMAT_IMAGE) {
        json_write_sep(buf, &first); json_write_video_essence(buf, flow);
    } else if (flow->format == TAMS_FORMAT_AUDIO) {
        json_write_sep(buf, &first); json_write_audio_essence(buf, flow);
    } else if (flow->format == TAMS_FORMAT_DATA) {
        json_write_sep(buf, &first); av_bprintf(buf, "\"essence_parameters\":{");
        json_write_key_string(buf, "data_type", flow->data_type);
        av_bprintf(buf, "}");
    } else if (flow->format == TAMS_FORMAT_MULTI && flow->nb_flow_collection_items) {
        json_write_sep(buf, &first); json_write_flow_collection(buf, flow);
    }

    av_bprintf(buf, "}");

    return av_bprint_is_complete(buf) ? 0 : AVERROR(ENOMEM);
}

int ff_tams_flows_from_json(const char *json,
                            TAMSFlow **flows_inout, int *nb_flows_inout)
{
    const char *cursor = json;
    int ret;

    ff_tams_json_skip_ws(&cursor);

    if (*cursor != '[') {
        /* single Flow object (e.g. a GET /flows/{id} response) */
        TAMSFlow *tmp = av_realloc_array(*flows_inout, *nb_flows_inout + 1,
                                         sizeof(**flows_inout));
        if (!tmp)
            return AVERROR(ENOMEM);
        *flows_inout = tmp;
        memset(&(*flows_inout)[*nb_flows_inout], 0, sizeof(**flows_inout));

        ret = tams_flow_from_json(&cursor, &(*flows_inout)[*nb_flows_inout]);
        if (ret < 0)
            return ret;
        (*nb_flows_inout)++;
        return 0;
    }

    cursor++; /* skip '[' */
    while (1) {
        TAMSFlow *tmp;

        ff_tams_json_skip_ws(&cursor);
        if (*cursor == ']')
            break;

        tmp = av_realloc_array(*flows_inout, *nb_flows_inout + 1,
                               sizeof(**flows_inout));
        if (!tmp)
            return AVERROR(ENOMEM);
        *flows_inout = tmp;

        ret = tams_flow_from_json(&cursor, &(*flows_inout)[*nb_flows_inout]);
        if (ret < 0)
            return ret;
        (*nb_flows_inout)++;

        ff_tams_json_skip_ws(&cursor);
        if (*cursor == ',')
            cursor++;
    }

    return 0;
}

int ff_tams_flow_segment_to_json(AVBPrint *buf, const TAMSFlowSegment *seg)
{
    char tr_buf[64], ts_buf[32];
    int ret;

    ret = ff_tams_timerange_to_str(&seg->timerange, tr_buf, sizeof(tr_buf));
    if (ret < 0)
        return ret;

    av_bprintf(buf, "{");
    json_write_key_string(buf, "object_id", seg->object_id);
    av_bprintf(buf, ",");
    json_write_key_string(buf, "timerange", tr_buf);

    ret = ff_tams_timestamp_to_str(seg->ts_offset, ts_buf, sizeof(ts_buf));
    if (ret < 0)
        return ret;
    av_bprintf(buf, ",");
    json_write_key_string(buf, "ts_offset", ts_buf);

    if (seg->has_last_duration) {
        char ld_buf[32];
        ret = ff_tams_timestamp_to_str(seg->last_duration, ld_buf, sizeof(ld_buf));
        if (ret < 0)
            return ret;
        av_bprintf(buf, ",");
        json_write_key_string(buf, "last_duration", ld_buf);
    }

    av_bprintf(buf, "}");

    return av_bprint_is_complete(buf) ? 0 : AVERROR(ENOMEM);
}

int ff_tams_flow_segments_from_json(const char *json,
                                    TAMSFlowSegment **segments_inout,
                                    int *nb_segments_inout)
{
    const char *cursor = json;
    int ret = 0;

    ff_tams_json_skip_ws(&cursor);
    if (*cursor != '[')
        return 0;
    cursor++;

    while (1) {
        TAMSFlowSegment *tmp;

        ff_tams_json_skip_ws(&cursor);
        if (*cursor == ']')
            break;

        tmp = av_realloc_array(*segments_inout, *nb_segments_inout + 1,
                               sizeof(**segments_inout));
        if (!tmp)
            return AVERROR(ENOMEM);
        *segments_inout = tmp;

        ret = tams_flow_segment_from_json(&cursor,
                                         &(*segments_inout)[*nb_segments_inout]);
        if (ret < 0)
            return ret;
        (*nb_segments_inout)++;

        ff_tams_json_skip_ws(&cursor);
        if (*cursor == ',')
            cursor++;
    }

    return 0;
}

int ff_tams_service_from_json(const char *json, TAMSService *service)
{
    const char *p = json;
    char key[64];
    int ret;

    memset(service, 0, sizeof(*service));

    ret = json_expect(&p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        int64_t val;

        ff_tams_json_skip_ws(&p);
        if (*p == '}')
            break;

        ret = json_read_key(&p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(&p)) {
            /* null value: skip */
        } else if (!strcmp(key, "type")) {
            ret = json_read_string(&p, service->type, sizeof(service->type));
        } else if (!strcmp(key, "api_version")) {
            ret = json_read_string(&p, service->api_version, sizeof(service->api_version));
        } else if (!strcmp(key, "min_object_timeout")) {
            ret = json_read_int(&p, &val);
            if (ret == 0)
                service->min_object_timeout = val;
        } else if (!strcmp(key, "min_presigned_url_timeout")) {
            ret = json_read_int(&p, &val);
            if (ret == 0)
                service->min_presigned_url_timeout = val;
        } else {
            ret = json_skip_value(&p);
        }

        if (ret < 0)
            return ret;

        ff_tams_json_skip_ws(&p);
        if (*p == ',')
            p++;
    }

    return 0;
}

static int tams_http_request_from_json(const char **p, TAMSHttpRequest *req)
{
    char key[64];
    int ret;

    memset(req, 0, sizeof(*req));

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(p)) {
            /* null value: skip */
        } else if (!strcmp(key, "url")) {
            ret = json_read_string(p, req->url, sizeof(req->url));
        } else if (!strcmp(key, "content-type")) {
            ret = json_read_string(p, req->content_type, sizeof(req->content_type));
        } else {
            ret = json_skip_value(p);
        }

        if (ret < 0)
            return ret;

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */

    if (!req->url[0])
        return AVERROR_INVALIDDATA;

    return 0;
}

static int tams_media_object_from_json(const char **p, TAMSMediaObject *obj)
{
    char key[64];
    int ret;

    memset(obj, 0, sizeof(*obj));

    ret = json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_read_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(p)) {
            /* null value: skip */
        } else if (!strcmp(key, "object_id")) {
            ret = json_read_string(p, obj->object_id, sizeof(obj->object_id));
        } else if (!strcmp(key, "put_url")) {
            ret = tams_http_request_from_json(p, &obj->put_url);
        } else {
            ret = json_skip_value(p);
        }

        if (ret < 0)
            return ret;

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */

    if (!obj->object_id[0] || !obj->put_url.url[0])
        return AVERROR_INVALIDDATA;

    return 0;
}

int ff_tams_storage_allocation_from_json(const char *json,
                                         TAMSMediaObject **objects_inout,
                                         int *nb_objects_inout)
{
    const char *p = json;
    char key[64];
    int ret;

    ret = json_expect(&p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(&p);
        if (*p == '}')
            break;

        ret = json_read_key(&p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(&p)) {
            /* null value: skip */
        } else if (!strcmp(key, "media_objects")) {
            ret = json_expect(&p, '[');
            if (ret < 0)
                return ret;

            while (1) {
                TAMSMediaObject *tmp;

                ff_tams_json_skip_ws(&p);
                if (*p == ']')
                    break;

                tmp = av_realloc_array(*objects_inout, *nb_objects_inout + 1,
                                       sizeof(**objects_inout));
                if (!tmp)
                    return AVERROR(ENOMEM);
                *objects_inout = tmp;

                ret = tams_media_object_from_json(&p, &(*objects_inout)[*nb_objects_inout]);
                if (ret < 0)
                    return ret;
                (*nb_objects_inout)++;

                ff_tams_json_skip_ws(&p);
                if (*p == ',')
                    p++;
            }
            p++; /* skip ']' */
        } else {
            ret = json_skip_value(&p);
        }

        if (ret < 0)
            return ret;

        ff_tams_json_skip_ws(&p);
        if (*p == ',')
            p++;
    }

    return 0;
}

/* ====================================================================
 * Codec MIME type <-> AVCodecID: TAMS Flow.codec strings, one entry per
 * accepted MIME alias; is_preferred marks the alias ff_tams_mime_from_codec()
 * maps back to when several alternatives exist for one codec.
 * ==================================================================== */

static const struct {
    const char *mime;
    enum AVCodecID id;
    int is_preferred;
} tams_codec_mime_map[] = {
    { "video/h264",      AV_CODEC_ID_H264,       1 },
    { "video/hevc",      AV_CODEC_ID_HEVC,       1 },
    { "video/h265",      AV_CODEC_ID_HEVC,       0 },
    { "video/vp8",       AV_CODEC_ID_VP8,        1 },
    { "video/vp9",       AV_CODEC_ID_VP9,        1 },
    { "video/av1",       AV_CODEC_ID_AV1,        1 },
    { "video/mpeg2",     AV_CODEC_ID_MPEG2VIDEO, 1 },
    { "video/raw",       AV_CODEC_ID_RAWVIDEO,   1 },
    { "audio/aac",       AV_CODEC_ID_AAC,        1 },
    { "audio/mp4a-latm", AV_CODEC_ID_AAC,        0 },
    { "audio/opus",      AV_CODEC_ID_OPUS,       1 },
    { "audio/mp2",       AV_CODEC_ID_MP2,        1 },
    { "audio/mp3",       AV_CODEC_ID_MP3,        1 },
    { "audio/mpeg",      AV_CODEC_ID_MP3,        0 },
    { "audio/flac",      AV_CODEC_ID_FLAC,       1 },
    { "audio/vorbis",    AV_CODEC_ID_VORBIS,     1 },
    { "audio/ac3",       AV_CODEC_ID_AC3,        1 },
    { "audio/eac3",      AV_CODEC_ID_EAC3,       1 },
    { "audio/pcm",       AV_CODEC_ID_PCM_S24LE,  1 },
    { "text/vtt",        AV_CODEC_ID_WEBVTT,     1 },
    { "text/srt",        AV_CODEC_ID_SUBRIP,     1 },
};

enum AVCodecID ff_tams_codec_from_mime(const char *mime)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(tams_codec_mime_map); i++)
        if (!strcmp(mime, tams_codec_mime_map[i].mime))
            return tams_codec_mime_map[i].id;
    return AV_CODEC_ID_NONE;
}

const char *ff_tams_mime_from_codec(enum AVCodecID codec_id)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(tams_codec_mime_map); i++)
        if (tams_codec_mime_map[i].id == codec_id && tams_codec_mime_map[i].is_preferred)
            return tams_codec_mime_map[i].mime;
    return NULL;
}

/* ====================================================================
 * Utilities
 * ==================================================================== */

int ff_tams_same_host(const char *url1, const char *url2)
{
    char host1[256] = "", host2[256] = "";
    av_url_split(NULL, 0, NULL, 0, host1, sizeof(host1), NULL, NULL, 0, url1);
    av_url_split(NULL, 0, NULL, 0, host2, sizeof(host2), NULL, NULL, 0, url2);
    return !av_strcasecmp(host1, host2);
}

int ff_tams_get_base_url(const char *full_url, char *base_url, size_t base_size,
                                 int *is_exact)
{
    const char *query = strchr(full_url, '?');
    int path_len = query ? (int)(query - full_url) : (int)strlen(full_url);
    const char *last_slash = NULL;

    /* If the path ends with "/flows", keep it so derived URLs are correct
     * for both "/flows/<id>" and "/flows?source_id=..." input forms. */
    if (path_len >= 6 && !strncmp(full_url + path_len - 6, "/flows", 6)) {
        if (path_len >= (int)base_size)
            return AVERROR(ENAMETOOLONG);
        memcpy(base_url, full_url, path_len);
        base_url[path_len] = '\0';
        if (is_exact)
            *is_exact = 1;
        return 0;
    }

    if (is_exact)
        *is_exact = 0;

    for (int i = path_len - 1; i >= 0; i--) {
        if (full_url[i] == '/') {
            last_slash = &full_url[i];
            break;
        }
    }

    if (last_slash) {
        int base_len = (int)(last_slash - full_url);
        if (base_len >= (int)base_size)
            return AVERROR(ENAMETOOLONG);
        memcpy(base_url, full_url, base_len);
        base_url[base_len] = '\0';
        return 0;
    }

    base_url[0] = '\0';
    return 0;
}

int ff_tams_flow_url(const char *flows_base_url, const char *flow_id, char *out, size_t out_size)
{
    if (snprintf(out, out_size, "%s/%s", flows_base_url, flow_id) >= (int)out_size)
        return AVERROR(ENAMETOOLONG);
    return 0;
}

int ff_tams_flow_subresource_url(const char *flows_base_url, const char *flow_id,
                                 const char *sub, char *out, size_t out_size)
{
    if (snprintf(out, out_size, "%s/%s/%s", flows_base_url, flow_id, sub) >= (int)out_size)
        return AVERROR(ENAMETOOLONG);
    return 0;
}

int ff_tams_service_url(const char *flows_base_url, char *out, size_t out_size)
{
    size_t base_len = strlen(flows_base_url);

    if (base_len < 6 || strcmp(flows_base_url + base_len - 6, "/flows"))
        return AVERROR(EINVAL);
    if (snprintf(out, out_size, "%.*s/service", (int)(base_len - 6), flows_base_url) >= (int)out_size)
        return AVERROR(ENAMETOOLONG);
    return 0;
}

static int tams_is_transient_error(int ret)
{
    switch (ret) {
    case AVERROR_HTTP_BAD_REQUEST:
    case AVERROR_HTTP_UNAUTHORIZED:
    case AVERROR_HTTP_FORBIDDEN:
    case AVERROR_HTTP_NOT_FOUND:
    case AVERROR_HTTP_OTHER_4XX:
        return 0;
    default:
        return 1; /* 429, 5xx, and generic network errors: retry */
    }
}

int ff_tams_request(AVFormatContext *s, AVDictionary *const *avio_opts,
                    const char *url, const char *method,
                    const uint8_t *body, int body_size, const char *content_type,
                    int retry_max, int64_t retry_backoff_us, AVBPrint *out)
{
    for (int attempt = 0; ; attempt++) {
        AVDictionary *opts = NULL;
        char headers[256];
        int ret;

        if (avio_opts && ff_tams_same_host(s->url, url)) {
            ret = av_dict_copy(&opts, *avio_opts, 0);
            if (ret < 0)
                return ret;
        }
        if (method)
            av_dict_set(&opts, "method", method, 0);
        if (body) {
            char *hex = av_malloc(2 * (size_t)body_size + 1);
            if (!hex) {
                av_dict_free(&opts);
                return AVERROR(ENOMEM);
            }
            ff_data_to_hex(hex, body, body_size, 0);
            av_dict_set(&opts, "post_data", hex, AV_DICT_DONT_STRDUP_VAL);
            if (content_type) {
                snprintf(headers, sizeof(headers), "Content-Type: %s\r\n", content_type);
                av_dict_set(&opts, "headers", headers, 0);
            }
        }

        if (body) {
            /* AVIOContext's buffering layer doesn't invoke the read
             * callback once write_flag is set (see fill_buffer() in
             * aviobuf.c). A POST/PUT response can never be read back 
             * through s->io_open()'s AVIOContext. Go one layer down
             * and read the URLContext. (the same pattern whip.c uses
             * for its SDP-offer POST) */
            URLContext *uc = NULL;
            uint8_t buf[4096];

            ret = ffurl_open_whitelist(&uc, url, AVIO_FLAG_READ_WRITE, &s->interrupt_callback,
                                       &opts, s->protocol_whitelist, s->protocol_blacklist, NULL);
            av_dict_free(&opts);

            if (ret >= 0) {
                if (out)
                    av_bprint_init(out, 0, INT_MAX);
                while (out) {
                    int n = ffurl_read(uc, buf, sizeof(buf));
                    if (n == AVERROR_EOF) {
                        ret = 0;
                        break;
                    }
                    if (n < 0) {
                        ret = n;
                        av_bprint_finalize(out, NULL);
                        break;
                    }
                    av_bprint_append_data(out, (const char *)buf, n);
                }
                ffurl_closep(&uc);
                if (ret >= 0)
                    return 0;
            }
        } else {
            AVIOContext *pb = NULL;

            ret = s->io_open(s, &pb, url, AVIO_FLAG_READ, &opts);
            av_dict_free(&opts);

            if (ret >= 0) {
                if (out) {
                    av_bprint_init(out, 0, INT_MAX);
                    ret = avio_read_to_bprint(pb, out, SIZE_MAX);
                    if (ret < 0)
                        av_bprint_finalize(out, NULL);
                }
                ff_format_io_close(s, &pb);
                if (ret >= 0)
                    return 0;
            }
        }

        if (!tams_is_transient_error(ret) || attempt >= retry_max)
            return ret;

        av_log(s, AV_LOG_WARNING, "TAMS request to %s failed (%s), retrying (%d/%d)\n",
               url, av_err2str(ret), attempt + 1, retry_max);

        if (ff_check_interrupt(&s->interrupt_callback))
            return AVERROR_EXIT;
        av_usleep(retry_backoff_us << attempt);
    }
}

void ff_tams_log_flow_summary(void *avcl, int level, const TAMSFlow *flow)
{
    static const char *const flow_type_names[] = {
        "UNKNOWN", "VIDEO", "AUDIO", "DATA", "MULTI", "IMAGE"
    };
    const char *type_name = (flow->format < FF_ARRAY_ELEMS(flow_type_names))
                           ? flow_type_names[flow->format] : "INVALID";

    av_log(avcl, level, "id=%s, format=%s(%d)", flow->id, type_name, flow->format);

    if (flow->format == TAMS_FORMAT_VIDEO || flow->format == TAMS_FORMAT_IMAGE) {
        if (flow->frame_rate.num > 0)
            av_log(avcl, level, ", frame_rate=" AVRATIONAL_FORMAT, AVRATIONAL_ARG(flow->frame_rate));
        if (flow->frame_width > 0 && flow->frame_height > 0)
            av_log(avcl, level, ", resolution=%dx%d", flow->frame_width, flow->frame_height);
    } else if (flow->format == TAMS_FORMAT_AUDIO) {
        if (flow->sample_rate > 0)
            av_log(avcl, level, ", sample_rate=%d", flow->sample_rate);
        if (flow->channels > 0)
            av_log(avcl, level, ", channels=%d", flow->channels);
    } else if (flow->format == TAMS_FORMAT_MULTI) {
        av_log(avcl, level, ", sub_flows=%d", flow->nb_flow_collection_items);
    }

    if (flow->timerange.has_start || flow->timerange.has_end) {
        av_log(avcl, level, ", timerange=[%s%"PRId64"_%"PRId64"%s)",
               flow->timerange.start_inclusive ? "[" : "(",
               flow->timerange.has_start ? flow->timerange.start : 0,
               flow->timerange.has_end ? flow->timerange.end : 0,
               flow->timerange.end_inclusive ? "]" : ")");
    }

    av_log(avcl, level, "\n");
}
