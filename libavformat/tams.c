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
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"

#include <string.h>

/* ====================================================================
 * JSON parsing primitives (cursor-based, operating on const char *)
 * ==================================================================== */

void ff_tams_json_skip_ws(const char **p)
{

    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

int ff_tams_json_expect(const char **p, char c)
{
    ff_tams_json_skip_ws(p);
    if (**p != c)
        return AVERROR_INVALIDDATA;
    (*p)++;
    return 0;
}

/**
 * Parse a JSON string value. Handles escape sequences.
 * If out is NULL, the string is skipped without storing.
 */
static int json_parse_string(const char **p, char *out, size_t out_size)
{
    size_t len = 0;
    int ret;

    ret = ff_tams_json_expect(p, '"');
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

static int json_parse_int(const char **p, int64_t *out)
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

static int json_parse_bool(const char **p, int *out)
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
int ff_tams_json_skip_value(const char **p)
{
    int depth;

    ff_tams_json_skip_ws(p);

    if (**p == '"')
        return json_parse_string(p, NULL, 0);

    if (**p == '{' || **p == '[') {
        char open = **p;
        char close = (open == '{') ? '}' : ']';
        depth = 1;
        (*p)++;

        while (**p && depth > 0) {
            if (**p == '"') {
                int ret = json_parse_string(p, NULL, 0);
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
 * Parse a tag value: a JSON string, or an array of strings flattened with ','.
 * Any other type is skipped and buf is set to empty string.
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
                ret = json_parse_string(p, elem, sizeof(elem));
                if (ret < 0)
                    return ret;
                elen = strlen(elem);
                copy = elen < size - len - 1 ? elen : (size - len - 1);
                if (copy > 0) {
                    memcpy(buf + len, elem, copy);
                    len += copy;
                }
            } else {
                ret = ff_tams_json_skip_value(p);
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
    return json_parse_string(p, buf, size);
}

/**
 * Parse a JSON key: "key" followed by ':'.
 */
static int json_parse_key(const char **p, char *out, size_t out_size)
{
    int ret;

    ff_tams_json_skip_ws(p);
    ret = json_parse_string(p, out, out_size);
    if (ret < 0)
        return ret;

    return ff_tams_json_expect(p, ':');
}

/**
 * Parse a JSON object of the form {"numerator": N, "denominator": D}.
 * denominator defaults to 1 if not present.
 */
static int json_parse_rational(const char **p, AVRational *out)
{
    char key[32];
    int64_t val;
    int ret;

    out->num = 0;
    out->den = 1;

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;


    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "numerator")) {
            ret = json_parse_int(p, &val);
            if (ret < 0)
                return ret;
            out->num = (int)val;
        } else if (!strcmp(key, "denominator")) {
            ret = json_parse_int(p, &val);
            if (ret < 0)
                return ret;
            out->den = (int)val;
        } else {
            ret = ff_tams_json_skip_value(p);
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
int ff_tams_parse_timestamp(const char *str, int64_t *ts)
{
    return parse_timestamp_internal(str, NULL, ts);
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
int ff_tams_parse_timerange(const char *str, TAMSTimeRange *tr)
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

/* ====================================================================
 * Essence parameter parsing
 * ==================================================================== */

static int parse_data_essence(const char **p, TAMSFlow *flow)
{
    char key[64];
    int ret;

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "data_type")) {
            ret = json_parse_string(p, flow->data_type, sizeof(flow->data_type));
            if (ret < 0) return ret;
        } else {
            ret = ff_tams_json_skip_value(p);
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

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "frame_width")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->frame_width = (int)val;
        } else if (!strcmp(key, "frame_height")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->frame_height = (int)val;
        } else if (!strcmp(key, "frame_rate")) {
            ret = json_parse_rational(p, &flow->frame_rate);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "bit_depth")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->bit_depth = (int)val;
        } else if (!strcmp(key, "interlace_mode")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            if (!strcmp(str_val, "progressive"))
                flow->interlace_mode = TAMS_INTERLACE_PROGRESSIVE;
            else if (!strcmp(str_val, "interlaced_tff"))
                flow->interlace_mode = TAMS_INTERLACE_TFF;
            else if (!strcmp(str_val, "interlaced_bff"))
                flow->interlace_mode = TAMS_INTERLACE_BFF;
            else if (!strcmp(str_val, "interlaced_psf"))
                flow->interlace_mode = TAMS_INTERLACE_PSF;
        } else if (!strcmp(key, "colorspace")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            if (!strcmp(str_val, "BT601"))
                flow->colorspace = TAMS_COLORSPACE_BT601;
            else if (!strcmp(str_val, "BT709"))
                flow->colorspace = TAMS_COLORSPACE_BT709;
            else if (!strcmp(str_val, "BT2020"))
                flow->colorspace = TAMS_COLORSPACE_BT2020;
            else if (!strcmp(str_val, "BT2100"))
                flow->colorspace = TAMS_COLORSPACE_BT2100;
        } else if (!strcmp(key, "transfer_characteristic")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            if (!strcmp(str_val, "SDR"))
                flow->transfer_characteristic = TAMS_TRANSFER_SDR;
            else if (!strcmp(str_val, "HLG"))
                flow->transfer_characteristic = TAMS_TRANSFER_HLG;
            else if (!strcmp(str_val, "PQ"))
                flow->transfer_characteristic = TAMS_TRANSFER_PQ;
        } else if (!strcmp(key, "aspect_ratio")) {
            ret = json_parse_rational(p, &flow->aspect_ratio);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "pixel_aspect_ratio")) {
            ret = json_parse_rational(p, &flow->pixel_aspect_ratio);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "component_type")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret < 0) return ret;
            if (!strcmp(str_val, "YCbCr"))
                flow->component_type = TAMS_COMPONENT_YCBCR;
            else if (!strcmp(str_val, "RGB"))
                flow->component_type = TAMS_COMPONENT_RGB;
        } else if (!strcmp(key, "horiz_chroma_subs")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->horiz_chroma_subs = (int)val;
        } else if (!strcmp(key, "vert_chroma_subs")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->vert_chroma_subs = (int)val;
        } else if (!strcmp(key, "vfr")) {
            ret = json_parse_bool(p, &flow->vfr);
            if (ret < 0) return ret;
        } else if (!strcmp(key, "unc_parameters")) {
            char subkey[64];
            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_parse_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "unc_type")) {
                    ret = json_parse_string(p, str_val, sizeof(str_val));
                    if (ret < 0) return ret;
                    if (!strcmp(str_val, "planar"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_PLANAR;
                    else if (!strcmp(str_val, "YUYV"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_YUYV;
                    else if (!strcmp(str_val, "UYVY"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_UYVY;
                    else if (!strcmp(str_val, "AYUV"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_AYUV;
                    else if (!strcmp(str_val, "v210"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_V210;
                    else if (!strcmp(str_val, "v216"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_V216;
                    else if (!strcmp(str_val, "RGB"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_RGB;
                    else if (!strcmp(str_val, "RGBx"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_RGBX;
                    else if (!strcmp(str_val, "xRGB"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_XRGB;
                    else if (!strcmp(str_val, "BGRx"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_BGRX;
                    else if (!strcmp(str_val, "xBGR"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_XBGR;
                    else if (!strcmp(str_val, "RGBA"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_RGBA;
                    else if (!strcmp(str_val, "ARGB"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_ARGB;
                    else if (!strcmp(str_val, "BGRA"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_BGRA;
                    else if (!strcmp(str_val, "ABGR"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_ABGR;
                    else if (!strcmp(str_val, "alpha"))
                        flow->video_unc_type = TAMS_VIDEO_UNC_ALPHA;
                } else {
                    ret = ff_tams_json_skip_value(p);
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
            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_parse_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "profile")) {
                    ret = json_parse_int(p, &val);
                    if (ret < 0) return ret;
                    flow->avc_parameters.profile = (int)val;
                } else if (!strcmp(subkey, "level")) {
                    ret = json_parse_int(p, &val);
                    if (ret < 0) return ret;
                    flow->avc_parameters.level = (int)val;
                } else if (!strcmp(subkey, "flags")) {
                    ret = json_parse_int(p, &val);
                    if (ret < 0) return ret;
                    flow->avc_parameters.flags = (int)val;
                } else {
                    ret = ff_tams_json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
            flow->has_avc_parameters = 1;
        } else {
            ret = ff_tams_json_skip_value(p);
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

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "sample_rate")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->sample_rate = (int)val;
        } else if (!strcmp(key, "channels")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->channels = (int)val;
        } else if (!strcmp(key, "bit_depth")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            flow->bit_depth = (int)val;
        } else if (!strcmp(key, "codec_parameters")) {
            /* Parse nested codec_parameters object */
            char subkey[64];
            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;

            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_parse_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "coded_frame_size")) {
                    ret = json_parse_int(p, &val);
                    if (ret < 0) return ret;
                    flow->coded_frame_size = (int)val;
                } else if (!strcmp(subkey, "mp4_oti")) {
                    ret = json_parse_int(p, &val);
                    if (ret < 0) return ret;
                    flow->mp4_oti = (int)val;
                } else {
                    ret = ff_tams_json_skip_value(p);
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
            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;

            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_parse_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "unc_type")) {
                    ret = json_parse_string(p, str_val, sizeof(str_val));
                    if (ret < 0) return ret;
                    if (!strcmp(str_val, "interleaved"))
                        flow->audio_unc_type = TAMS_AUDIO_UNC_INTERLEAVED;
                    else if (!strcmp(str_val, "planar"))
                        flow->audio_unc_type = TAMS_AUDIO_UNC_PLANAR;
                    else if (!strcmp(str_val, "pairs"))
                        flow->audio_unc_type = TAMS_AUDIO_UNC_PAIRS;
                } else {
                    ret = ff_tams_json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
        } else {
            ret = ff_tams_json_skip_value(p);
            if (ret < 0) return ret;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++; /* skip '}' */
    return 0;
}

/* ====================================================================
 * Container mapping and flow collection parsing
 * ==================================================================== */

static int parse_mp2ts_container(const char **p, TAMSContainerMapping *mapping)
{
    char key[64];
    int64_t val;
    int ret;

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "pid")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            mapping->mp2ts_pid = (int)val;
            mapping->has_mp2ts_pid = 1;
        } else {
            ret = ff_tams_json_skip_value(p);
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

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "track_id")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            mapping->isobmff_track_id = (int)val;
            mapping->has_isobmff_track_id = 1;
        } else {
            ret = ff_tams_json_skip_value(p);
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

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "package_uid")) {
            ret = json_parse_string(p, mapping->mxf_package_uid,
                                    sizeof(mapping->mxf_package_uid));
            if (ret < 0) return ret;
        } else if (!strcmp(key, "track_id")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            mapping->mxf_track_id = (int)val;
            mapping->has_mxf_track_id = 1;
        } else {
            ret = ff_tams_json_skip_value(p);
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

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (!strcmp(key, "track_index")) {
            ret = json_parse_int(p, &val);
            if (ret < 0) return ret;
            mapping->track_index = (int)val;
            mapping->has_track_index = 1;
        } else if (!strcmp(key, "format_track_index")) {
            ret = json_parse_int(p, &val);
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
            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                ret = json_parse_key(p, subkey, sizeof(subkey));
                if (ret < 0) return ret;
                if (!strcmp(subkey, "channel_range")) {
                    ret = json_parse_string(p, mapping->audio_channel_range,
                                            sizeof(mapping->audio_channel_range));
                    if (ret < 0) return ret;
                } else {
                    ret = ff_tams_json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++;
        } else {
            ret = ff_tams_json_skip_value(p);
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

    ret = ff_tams_json_expect(p, '[');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == ']')
            break;

        if (flow->nb_flow_collection >= TAMS_MAX_COLLECTION_ITEMS) {
            ret = ff_tams_json_skip_value(p);
            if (ret < 0) return ret;
        } else {
            TAMSFlowCollectionItem *item =
                &flow->flow_collection[flow->nb_flow_collection];
            memset(item, 0, sizeof(*item));

            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;

            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;

                ret = json_parse_key(p, key, sizeof(key));
                if (ret < 0) return ret;

                if (!strcmp(key, "id")) {
                    ret = json_parse_string(p, item->id, sizeof(item->id));
                } else if (!strcmp(key, "role")) {
                    ret = json_parse_string(p, item->role, sizeof(item->role));
                } else if (!strcmp(key, "container_mapping")) {
                    ret = parse_container_mapping(p, &item->container_mapping);
                    if (ret == 0)
                        item->has_container_mapping = 1;
                } else {
                    ret = ff_tams_json_skip_value(p);
                }

                if (ret < 0) return ret;

                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++;
            flow->nb_flow_collection++;
        }

        ff_tams_json_skip_ws(p);
        if (**p == ',')
            (*p)++;
    }

    (*p)++;
    return 0;
}

/* ====================================================================
 * Flow parsing
 * ==================================================================== */

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

int ff_tams_parse_flow(const char **p, TAMSFlow *flow)
{
    char key[64], str_val[1024];
    int64_t val;
    int ret;

    memset(flow, 0, sizeof(*flow));
    flow->segment_duration = (AVRational){0, 1};
    flow->pixel_aspect_ratio = (AVRational){1, 1};

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(p)) {
            /* null value: skip */
        } else if (!strcmp(key, "id")) {
            ret = json_parse_string(p, flow->id, sizeof(flow->id));
        } else if (!strcmp(key, "source_id")) {
            ret = json_parse_string(p, flow->source_id, sizeof(flow->source_id));
        } else if (!strcmp(key, "label")) {
            ret = json_parse_string(p, flow->label, sizeof(flow->label));
        } else if (!strcmp(key, "description")) {
            ret = json_parse_string(p, flow->description, sizeof(flow->description));
        } else if (!strcmp(key, "format")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                flow->format = parse_format_urn(str_val);
        } else if (!strcmp(key, "codec")) {
            ret = json_parse_string(p, flow->codec, sizeof(flow->codec));
        } else if (!strcmp(key, "container")) {
            ret = json_parse_string(p, flow->container, sizeof(flow->container));
        } else if (!strcmp(key, "generation")) {
            ret = json_parse_int(p, &val);
            if (ret == 0) flow->generation = (int)val;
        } else if (!strcmp(key, "avg_bit_rate")) {
            ret = json_parse_int(p, &val);
            if (ret == 0) flow->avg_bit_rate = (int)val;
        } else if (!strcmp(key, "max_bit_rate")) {
            ret = json_parse_int(p, &val);
            if (ret == 0) flow->max_bit_rate = (int)val;
        } else if (!strcmp(key, "read_only")) {
            ret = json_parse_bool(p, &flow->read_only);
        } else if (!strcmp(key, "timerange")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                ret = ff_tams_parse_timerange(str_val, &flow->timerange);
        } else if (!strcmp(key, "segment_duration")) {
            ret = json_parse_rational(p, &flow->segment_duration);
        } else if (!strcmp(key, "tags")) {
            /* Parse tags object: {"key1": "val1", "key2": "val2", ...} */
            ret = ff_tams_json_expect(p, '{');
            if (ret < 0) return ret;
            while (1) {
                ff_tams_json_skip_ws(p);
                if (**p == '}')
                    break;
                if (flow->nb_tags < TAMS_MAX_TAGS) {
                    TAMSTag *tag = &flow->tags[flow->nb_tags];
                    ret = json_parse_string(p, tag->key, sizeof(tag->key));
                    if (ret < 0) return ret;
                    ret = ff_tams_json_expect(p, ':');
                    if (ret < 0) return ret;
                    ret = json_parse_tag_value(p, tag->value, sizeof(tag->value));
                    if (ret < 0) return ret;
                    flow->nb_tags++;
                } else {
                    /* Skip key */
                    ret = json_parse_string(p, NULL, 0);
                    if (ret < 0) return ret;
                    ret = ff_tams_json_expect(p, ':');
                    if (ret < 0) return ret;
                    ret = ff_tams_json_skip_value(p);
                    if (ret < 0) return ret;
                }
                ff_tams_json_skip_ws(p);
                if (**p == ',')
                    (*p)++;
            }
            (*p)++; /* skip '}' */
        } else if (!strcmp(key, "created_by")) {
            ret = json_parse_string(p, flow->created_by, sizeof(flow->created_by));
        } else if (!strcmp(key, "updated_by")) {
            ret = json_parse_string(p, flow->updated_by, sizeof(flow->updated_by));
        } else if (!strcmp(key, "metadata_version")) {
            ret = json_parse_string(p, flow->metadata_version, sizeof(flow->metadata_version));
        } else if (!strcmp(key, "created")) {
            ret = json_parse_string(p, flow->created, sizeof(flow->created));
        } else if (!strcmp(key, "metadata_updated")) {
            ret = json_parse_string(p, flow->metadata_updated, sizeof(flow->metadata_updated));
        } else if (!strcmp(key, "segments_updated")) {
            ret = json_parse_string(p, flow->segments_updated, sizeof(flow->segments_updated));
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
                ret = ff_tams_json_skip_value(p);
        } else {
            ret = ff_tams_json_skip_value(p);
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

/* ====================================================================
 * Flow Segment parsing
 * ==================================================================== */

/**
 * Parse the get_urls array and extract the best URL.
 * Prefers entries with "presigned":true; falls back to the first URL.
 */
static int parse_get_urls(const char **p, char *url, size_t url_size)
{
    char key[32], candidate[2048];
    int ret, found_presigned = 0;

    ret = ff_tams_json_expect(p, '[');
    if (ret < 0)
        return ret;

    while (1) {
        int is_presigned = 0;

        ff_tams_json_skip_ws(p);
        if (**p == ']')
            break;

        candidate[0] = '\0';
        ret = ff_tams_json_expect(p, '{');
        if (ret < 0)
            return ret;

        while (1) {
            ff_tams_json_skip_ws(p);
            if (**p == '}')
                break;
            ret = json_parse_key(p, key, sizeof(key));
            if (ret < 0)
                return ret;
            if (!strcmp(key, "url")) {
                ret = json_parse_string(p, candidate, sizeof(candidate));
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
                    ret = ff_tams_json_skip_value(p);
                    if (ret < 0)
                        return ret;
                }
            } else {
                ret = ff_tams_json_skip_value(p);
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
                ret = ff_tams_json_skip_value(p);
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

int ff_tams_parse_flow_segment(const char **p, TAMSFlowSegment *seg)
{
    char key[64], str_val[256];
    int ret;

    memset(seg, 0, sizeof(*seg));

    ret = ff_tams_json_expect(p, '{');
    if (ret < 0)
        return ret;

    while (1) {
        ff_tams_json_skip_ws(p);
        if (**p == '}')
            break;

        ret = json_parse_key(p, key, sizeof(key));
        if (ret < 0)
            return ret;

        if (json_is_null(p)) {
            /* null value: skip */
        } else if (!strcmp(key, "object_id")) {
            ret = json_parse_string(p, seg->object_id, sizeof(seg->object_id));
        } else if (!strcmp(key, "timerange")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret == 0)
                ret = ff_tams_parse_timerange(str_val, &seg->timerange);
        } else if (!strcmp(key, "ts_offset")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret == 0) {
                ret = ff_tams_parse_timestamp(str_val, &seg->ts_offset);
                if (ret == 0)
                    seg->has_ts_offset = 1;
            }
        } else if (!strcmp(key, "last_duration")) {
            ret = json_parse_string(p, str_val, sizeof(str_val));
            if (ret == 0) {
                ret = ff_tams_parse_timestamp(str_val, &seg->last_duration);
                if (ret == 0)
                    seg->has_last_duration = 1;
            }
        } else if (!strcmp(key, "get_urls")) {
            ret = parse_get_urls(p, seg->get_url, sizeof(seg->get_url));
        } else {
            ret = ff_tams_json_skip_value(p);
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

/* ====================================================================
 * Utility functions
 * ==================================================================== */

int64_t ff_tams_parse_iso8601(const char *str)
{
    int64_t t;
    if (!str || !str[0])
        return 0;
    if (av_parse_time(&t, str, 0) < 0)
        return 0;
    return t;
}

int ff_tams_parse_flows_json(const char *json,
                              TAMSFlow **flows_out, int *nb_flows_out)
{
    const char *cursor = json;
    TAMSFlow *flows = NULL;
    int nb_flows = 0;
    int is_array, ret;

    ff_tams_json_skip_ws(&cursor);
    is_array = (*cursor == '[');

    if (is_array) {
        cursor++; /* skip '[' */
        while (1) {
            TAMSFlow *tmp;

            ff_tams_json_skip_ws(&cursor);
            if (*cursor == ']')
                break;

            tmp = av_realloc_array(flows, nb_flows + 1, sizeof(*flows));
            if (!tmp) {
                av_free(flows);
                return AVERROR(ENOMEM);
            }
            flows = tmp;

            ret = ff_tams_parse_flow(&cursor, &flows[nb_flows]);
            if (ret < 0) {
                av_free(flows);
                return ret;
            }
            nb_flows++;

            ff_tams_json_skip_ws(&cursor);
            if (*cursor == ',')
                cursor++;
        }
    } else {
        flows = av_mallocz(sizeof(*flows));
        if (!flows)
            return AVERROR(ENOMEM);

        ret = ff_tams_parse_flow(&cursor, &flows[0]);
        if (ret < 0) {
            av_free(flows);
            return ret;
        }
        nb_flows = 1;
    }

    *flows_out    = flows;
    *nb_flows_out = nb_flows;
    return 0;
}
