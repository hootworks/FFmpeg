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
 * TAMS (Time-Addressable Media Store) muxer.
 *
 * Muxes one or more TAMS Flows: creates/updates Flows, segments encoded output
 * on keyframe boundaries, uploads segment bytes to presigned storage, and
 * registers segments via the TAMS write API.
 *
 * References
 * https://bbc.github.io/tams/main/index.html
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "tams.h"

#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include <string.h>

typedef struct TAMSFlowContext {
    int stream_index;    /* index into the owning container's sub_ctx->streams[] */
    char flow_id[TAMS_UUID_SIZE];
    int  has_flow_id;    /* came from -flow_map id= on this token */
    char source_id[TAMS_UUID_SIZE];
    int  has_source_id;  /* came from -flow_map source_id= on this token */
    TAMSFlow flow;
} TAMSFlowContext;

/* one physical container / nested muxer -- one 'flow=' token from -flow_map */
typedef struct TAMSContainerContext {
    int *stream_indices; /* indices into the parent AVFormatContext's streams[] */
    int  nb_streams;

    const AVOutputFormat *oformat;
    AVFormatContext *sub_ctx; /* nested muxer for this container's physical output */

    TAMSFlowContext *flow_ctxs; /* one entry per stream_indices[], sized nb_streams */

    int multi_flow_index; /* the 'multi_flow=N' correlation key, if any */
    int has_multi_flow;   /* true iff this container belongs to some multi-Flow */

    int reference_stream_index; /* sub_ctx stream index used for boundary decisions */
    int64_t segment_start_pts;  /* TAMS-ns, relative to c->start_tai_ns */
    int64_t next_boundary_ns;
    int has_segment_data;
    int64_t segment_index;

    /* eager pre-allocated storage for the *next* segment, requested at this
     * segment's start so it's already available when this one flushes */
    char **next_put_urls; /* one per TAMSFlowContext, parallel to flow_ctxs[] */
    char next_object_id[TAMS_UUID_SIZE];
} TAMSContainerContext;

/* one independent multi-Flow -- one distinct 'multi_flow=N' correlation key
 * referenced by some flow= token(s) */
typedef struct TAMSMultiFlowContext {
    int index; /* the N value: an arbitrary correlation key, need not be dense */
    char flow_id[TAMS_UUID_SIZE];
    int  has_flow_id; /* came from a 'multi_flow=N,id=...' token */
    char source_id[TAMS_UUID_SIZE];
    int  has_source_id; /* came from a 'multi_flow=N,source_id=...' token */
    TAMSFlow flow;

    int *container_indices; /* indices into TAMSMuxContext.container_ctxs[] belonging here */
    int  nb_container_indices;
} TAMSMultiFlowContext;

typedef struct TAMSMuxContext {
    const AVClass *class;

    /* AVOptions */
    char *flow_map_str;
    char *container_name;
    char *label;
    char *description;
    char *tags_str;
    char *start_timestamp_str;
    int64_t segment_duration; /* seconds; -1 = auto */
    int retry_max;
    int64_t retry_backoff_us;

    TAMSContainerContext *container_ctxs;
    int nb_container_ctxs;

    TAMSMultiFlowContext *multi_flow_ctxs;
    int nb_multi_flow_ctxs;

    int *stream_to_container;
    int *stream_to_subindex;

    int64_t start_tai_ns;
    AVDictionary *avio_opts;
} TAMSMuxContext;

static void tams_free_container_ctxs(TAMSContainerContext *container_ctxs, int nb_container_ctxs)
{
    if (!container_ctxs)
        return;
    for (int i = 0; i < nb_container_ctxs; i++) {
        av_freep(&container_ctxs[i].stream_indices);
        av_freep(&container_ctxs[i].flow_ctxs);
        avformat_free_context(container_ctxs[i].sub_ctx);
    }
    av_free(container_ctxs);
}

static void tams_free_multi_flow_ctxs(TAMSMultiFlowContext *multi_flow_ctxs, int nb_multi_flow_ctxs)
{
    if (!multi_flow_ctxs)
        return;
    for (int i = 0; i < nb_multi_flow_ctxs; i++)
        av_freep(&multi_flow_ctxs[i].container_indices);
    av_free(multi_flow_ctxs);
}

/* per-codec default raw elementary-stream muxer, tried when -container isn't
 * given and the container has exactly one stream. AV_CODEC_ID_RAWVIDEO and
 * AV_CODEC_ID_PCM_S24LE are deliberately absent: their raw forms have no
 * header/sync bytes at all and can't be auto-probed, so they fall through
 * to the mp4 fallback below. */
static const struct {
    enum AVCodecID codec_id;
    const char *muxer_name;
} tams_default_muxers[] = {
    { AV_CODEC_ID_H264,       "h264"       },
    { AV_CODEC_ID_HEVC,       "hevc"       },
    { AV_CODEC_ID_VP8,        "ivf"        },
    { AV_CODEC_ID_VP9,        "ivf"        },
    { AV_CODEC_ID_AV1,        "obu"        },
    { AV_CODEC_ID_MPEG2VIDEO, "mpeg2video" },
    { AV_CODEC_ID_AAC,        "adts"       },
    { AV_CODEC_ID_OPUS,       "opus"       },
    { AV_CODEC_ID_MP2,        "mp2"        },
    { AV_CODEC_ID_MP3,        "mp3"        },
    { AV_CODEC_ID_FLAC,       "flac"       },
    { AV_CODEC_ID_VORBIS,     "oga"        },
    { AV_CODEC_ID_AC3,        "ac3"        },
    { AV_CODEC_ID_EAC3,       "eac3"       },
    { AV_CODEC_ID_WEBVTT,     "webvtt"     },
    { AV_CODEC_ID_SUBRIP,     "srt"        },
};

static const AVOutputFormat *tams_default_raw_oformat(enum AVCodecID codec_id)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(tams_default_muxers); i++)
        if (tams_default_muxers[i].codec_id == codec_id)
            return av_guess_format(tams_default_muxers[i].muxer_name, NULL, NULL);
    return NULL;
}

/* resolve cc->oformat: -container if given, else a per-codec raw-ES default
 * for a single-stream container, else fragmented mp4 */
static int tams_resolve_oformat(AVFormatContext *s, TAMSContainerContext *cc)
{
    TAMSMuxContext *c = s->priv_data;
    const AVOutputFormat *oformat;

    if (c->container_name) {
        oformat = av_guess_format(c->container_name, NULL, NULL);
        if (!oformat) {
            av_log(s, AV_LOG_ERROR, "Unknown -container format '%s'\n", c->container_name);
            return AVERROR_MUXER_NOT_FOUND;
        }
        cc->oformat = oformat;
        return 0;
    }

    if (cc->nb_streams == 1) {
        oformat = tams_default_raw_oformat(s->streams[cc->stream_indices[0]]->codecpar->codec_id);
        if (oformat) {
            cc->oformat = oformat;
            return 0;
        }
    }

    oformat = av_guess_format("mp4", NULL, NULL);
    if (!oformat) {
        av_log(s, AV_LOG_ERROR, "mp4 muxer not available for fragmented-MP4 fallback\n");
        return AVERROR_MUXER_NOT_FOUND;
    }
    cc->oformat = oformat;
    return 0;
}

/* allocate cc->sub_ctx and clone this container's mapped streams into it,
 * following segment_mux_init()'s pattern in segment.c */
static int tams_build_container_mux(AVFormatContext *s, TAMSContainerContext *cc)
{
    AVFormatContext *oc;
    int ret;

    ret = avformat_alloc_output_context2(&cc->sub_ctx, cc->oformat, NULL, NULL);
    if (ret < 0)
        return ret;
    oc = cc->sub_ctx;

    oc->interrupt_callback = s->interrupt_callback;
    oc->max_delay          = s->max_delay;
    oc->opaque             = s->opaque;
    oc->io_close2          = s->io_close2;
    oc->io_open            = s->io_open;
    oc->flags              = s->flags;

    for (int i = 0; i < cc->nb_streams; i++) {
        AVStream *ist = s->streams[cc->stream_indices[i]];
        AVCodecParameters *ipar = ist->codecpar, *opar;
        AVStream *ost = ff_stream_clone(oc, ist);

        if (!ost)
            return AVERROR(ENOMEM);

        opar = ost->codecpar;
        if (!oc->oformat->codec_tag ||
            av_codec_get_id (oc->oformat->codec_tag, ipar->codec_tag) == opar->codec_id ||
            av_codec_get_tag(oc->oformat->codec_tag, ipar->codec_id) <= 0) {
            opar->codec_tag = ipar->codec_tag;
        } else {
            opar->codec_tag = 0;
        }
    }

    return 0;
}

/* resolve oformat and build the nested muxer for every container */
static int tams_build_containers(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];
        int ret = tams_resolve_oformat(s, cc);

        if (ret < 0)
            return ret;

        ret = tams_build_container_mux(s, cc);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int tams_is_valid_uuid(const char *s)
{
    static const char pattern[] = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";
    size_t i;

    if (strlen(s) != strlen(pattern))
        return 0;
    for (i = 0; pattern[i]; i++) {
        if (pattern[i] == '-') {
            if (s[i] != '-')
                return 0;
        } else if (!av_isxdigit(s[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Parse one 'flow=streams=...' token (the 'flow=' prefix already consumed
 * by the caller; p points at 'streams=...').
 */
static int tams_parse_flow_token(const char *p, const char *tok_end, int nb_streams,
                                  uint8_t *assigned,
                                  TAMSContainerContext **container_ctxs_inout, int *nb_container_ctxs_inout)
{
    int *indices = NULL, nb_indices = 0;
    char (*ids)[TAMS_UUID_SIZE] = NULL;
    int nb_ids = 0;
    char source_id[TAMS_UUID_SIZE];
    int has_source_id = 0;
    int multi_flow_index = -1, has_multi_flow = 0;
    TAMSContainerContext *tmp_container_ctxs, *container_ctx;
    int ret;

    source_id[0] = '\0';

    if (strncmp(p, "streams=", 8)) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    p += 8;

    /* stream index list */
    for (;;) {
        long idx;
        char *end;
        int *tmp;

        idx = strtol(p, &end, 10);
        if (end == p || idx < 0 || idx >= nb_streams) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (assigned[idx]) {
            ret = AVERROR(EINVAL); /* duplicate assignment */
            goto fail;
        }
        assigned[idx] = 1;

        tmp = av_realloc_array(indices, nb_indices + 1, sizeof(*indices));
        if (!tmp) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        indices = tmp;
        indices[nb_indices++] = (int)idx;

        p = end;
        if (p >= tok_end || *p != ',')
            break;
        p++; /* skip ',' */
        if (p >= tok_end || !av_isdigit(*p))
            break; /* next is an attr, not another index; comma already consumed */
    }

    /* attrs: id=, source_id=, multi_flow= (any order, comma-separated) */
    while (p < tok_end) {
        if (!strncmp(p, "id=", 3)) {
            const char *v = p + 3;
            const char *v_end = memchr(v, ',', tok_end - v);
            char (*tmp)[TAMS_UUID_SIZE];

            if (!v_end)
                v_end = tok_end;
            if (nb_ids >= nb_indices) {
                ret = AVERROR(EINVAL); /* more ids than streams */
                goto fail;
            }
            if ((size_t)(v_end - v) >= TAMS_UUID_SIZE) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            tmp = av_realloc_array(ids, nb_ids + 1, sizeof(*ids));
            if (!tmp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            ids = tmp;
            memcpy(ids[nb_ids], v, v_end - v);
            ids[nb_ids][v_end - v] = '\0';
            if (!tams_is_valid_uuid(ids[nb_ids])) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            nb_ids++;
            p = v_end;
        } else if (!strncmp(p, "source_id=", 10)) {
            const char *v = p + 10;
            const char *v_end = memchr(v, ',', tok_end - v);

            if (!v_end)
                v_end = tok_end;
            if (has_source_id) {
                ret = AVERROR(EINVAL); /* duplicate key */
                goto fail;
            }
            if ((size_t)(v_end - v) >= sizeof(source_id)) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            memcpy(source_id, v, v_end - v);
            source_id[v_end - v] = '\0';
            if (!tams_is_valid_uuid(source_id)) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            has_source_id = 1;
            p = v_end;
        } else if (!strncmp(p, "multi_flow=", 11)) {
            const char *v = p + 11;
            long n;
            char *end;

            if (has_multi_flow) {
                ret = AVERROR(EINVAL); /* duplicate key */
                goto fail;
            }
            n = strtol(v, &end, 10);
            if (end == v || n < 0 || (end < tok_end && *end != ',')) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            multi_flow_index = (int)n;
            has_multi_flow = 1;
            p = end;
        } else {
            ret = AVERROR(EINVAL); /* unknown attr */
            goto fail;
        }

        if (p < tok_end) {
            if (*p != ',') {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            p++;
        }
    }

    /* container_mapping (needed whenever >1 stream shares this container)
     * only has meaning inside a multi-Flow's flow_collection */
    if (nb_indices > 1 && !has_multi_flow) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    tmp_container_ctxs = av_realloc_array(*container_ctxs_inout, *nb_container_ctxs_inout + 1,
                                          sizeof(**container_ctxs_inout));
    if (!tmp_container_ctxs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    *container_ctxs_inout = tmp_container_ctxs;
    container_ctx = &(*container_ctxs_inout)[*nb_container_ctxs_inout];
    memset(container_ctx, 0, sizeof(*container_ctx));
    container_ctx->stream_indices   = indices; /* ownership transferred */
    container_ctx->nb_streams       = nb_indices;
    container_ctx->multi_flow_index = multi_flow_index;
    container_ctx->has_multi_flow   = has_multi_flow;

    container_ctx->flow_ctxs = av_calloc(nb_indices, sizeof(*container_ctx->flow_ctxs));
    if (!container_ctx->flow_ctxs) {
        av_freep(&container_ctx->stream_indices);
        ret = AVERROR(ENOMEM);
        goto fail_no_indices;
    }
    (*nb_container_ctxs_inout)++;
    for (int i = 0; i < nb_indices; i++) {
        container_ctx->flow_ctxs[i].stream_index = indices[i];
        if (i < nb_ids) {
            av_strlcpy(container_ctx->flow_ctxs[i].flow_id, ids[i],
                      sizeof(container_ctx->flow_ctxs[i].flow_id));
            container_ctx->flow_ctxs[i].has_flow_id = 1;
        }
        if (has_source_id) {
            av_strlcpy(container_ctx->flow_ctxs[i].source_id, source_id,
                      sizeof(container_ctx->flow_ctxs[i].source_id));
            container_ctx->flow_ctxs[i].has_source_id = 1;
        }
    }

    av_free(ids);
    return 0;

fail:
    av_free(indices);
fail_no_indices:
    av_free(ids);
    return ret;
}

/**
 * Parse one standalone 'multi_flow=N,...' property token (the 'multi_flow='
 * prefix already consumed by the caller; p points at 'N,...').
 */
static int tams_parse_multi_flow_token(const char *p, const char *tok_end,
                                        TAMSMultiFlowContext **multi_flow_ctxs_inout,
                                        int *nb_multi_flow_ctxs_inout)
{
    long n;
    char *end;
    char id[TAMS_UUID_SIZE];
    int has_id = 0;
    char source_id[TAMS_UUID_SIZE];
    int has_source_id = 0;
    TAMSMultiFlowContext *tmp_multi_flow_ctxs, *multi_flow_ctx;

    id[0] = source_id[0] = '\0';

    n = strtol(p, &end, 10);
    if (end == p || n < 0 || (end < tok_end && *end != ','))
        return AVERROR(EINVAL);
    p = end;
    if (p >= tok_end || *p != ',')
        return AVERROR(EINVAL); /* must have at least one attr */
    p++;

    while (p < tok_end) {
        if (!strncmp(p, "id=", 3)) {
            const char *v = p + 3;
            const char *v_end = memchr(v, ',', tok_end - v);

            if (!v_end)
                v_end = tok_end;
            if (has_id || (size_t)(v_end - v) >= sizeof(id))
                return AVERROR(EINVAL);
            memcpy(id, v, v_end - v);
            id[v_end - v] = '\0';
            if (!tams_is_valid_uuid(id))
                return AVERROR(EINVAL);
            has_id = 1;
            p = v_end;
        } else if (!strncmp(p, "source_id=", 10)) {
            const char *v = p + 10;
            const char *v_end = memchr(v, ',', tok_end - v);

            if (!v_end)
                v_end = tok_end;
            if (has_source_id || (size_t)(v_end - v) >= sizeof(source_id))
                return AVERROR(EINVAL);
            memcpy(source_id, v, v_end - v);
            source_id[v_end - v] = '\0';
            if (!tams_is_valid_uuid(source_id))
                return AVERROR(EINVAL);
            has_source_id = 1;
            p = v_end;
        } else {
            return AVERROR(EINVAL); /* unknown attr */
        }

        if (p < tok_end) {
            if (*p != ',')
                return AVERROR(EINVAL);
            p++;
        }
    }

    for (int i = 0; i < *nb_multi_flow_ctxs_inout; i++) {
        if ((*multi_flow_ctxs_inout)[i].index == (int)n)
            return AVERROR(EINVAL); /* duplicate multi_flow index */
    }

    tmp_multi_flow_ctxs = av_realloc_array(*multi_flow_ctxs_inout, *nb_multi_flow_ctxs_inout + 1,
                                           sizeof(**multi_flow_ctxs_inout));
    if (!tmp_multi_flow_ctxs)
        return AVERROR(ENOMEM);
    *multi_flow_ctxs_inout = tmp_multi_flow_ctxs;
    multi_flow_ctx = &(*multi_flow_ctxs_inout)[(*nb_multi_flow_ctxs_inout)++];
    memset(multi_flow_ctx, 0, sizeof(*multi_flow_ctx));
    multi_flow_ctx->index = (int)n;
    if (has_id) {
        av_strlcpy(multi_flow_ctx->flow_id, id, sizeof(multi_flow_ctx->flow_id));
        multi_flow_ctx->has_flow_id = 1;
    }
    if (has_source_id) {
        av_strlcpy(multi_flow_ctx->source_id, source_id, sizeof(multi_flow_ctx->source_id));
        multi_flow_ctx->has_source_id = 1;
    }

    return 0;
}

/**
 * Append a fresh standalone single-stream TAMSContainerContext (no multi_flow, no
 * pinned id) for stream_index. Used both when no -flow_map is provided
 * or for any stream -flow_map leaves unmentioned.
 */
static int tams_append_mono_container(int stream_index,
                                       TAMSContainerContext **container_ctxs_inout,
                                       int *nb_container_ctxs_inout)
{
    TAMSContainerContext *tmp_container_ctxs, *container_ctx;

    tmp_container_ctxs = av_realloc_array(*container_ctxs_inout, *nb_container_ctxs_inout + 1,
                                          sizeof(**container_ctxs_inout));
    if (!tmp_container_ctxs)
        return AVERROR(ENOMEM);
    *container_ctxs_inout = tmp_container_ctxs;
    container_ctx = &(*container_ctxs_inout)[*nb_container_ctxs_inout];
    memset(container_ctx, 0, sizeof(*container_ctx));

    container_ctx->stream_indices = av_malloc(sizeof(int));
    if (!container_ctx->stream_indices)
        return AVERROR(ENOMEM);
    container_ctx->stream_indices[0] = stream_index;
    container_ctx->nb_streams        = 1;
    container_ctx->multi_flow_index  = -1;
    container_ctx->has_multi_flow    = 0;

    container_ctx->flow_ctxs = av_calloc(1, sizeof(*container_ctx->flow_ctxs));
    if (!container_ctx->flow_ctxs) {
        av_freep(&container_ctx->stream_indices);
        return AVERROR(ENOMEM);
    }
    container_ctx->flow_ctxs[0].stream_index = stream_index;

    (*nb_container_ctxs_inout)++;
    return 0;
}

/*
 * Grammar (see doc/muxers.texi for worked examples):
 *
 *   flow_map         := token (' ' token)*
 *   token            := flow_token | multi_flow_token
 *
 *   flow_token       := 'flow=streams=' index (',' index)* (',' flow_attr)*
 *   flow_attr        := 'id=' uuid | 'source_id=' uuid | 'multi_flow=' integer
 *
 *   multi_flow_token := 'multi_flow=' integer ',' multi_attr (',' multi_attr)*
 *   multi_attr       := 'id=' uuid | 'source_id=' uuid
 *
 * An AVStream index may appear in at most one flow_token (hard error on
 * duplicate assignment); it is never required to appear in any. Any stream
 * -flow_map doesn't mention defaults to its own standalone mono Flow, same
 * as when -flow_map is omitted entirely. A flow_token with more than one
 * stream requires multi_flow= (container mapping only has meaning inside a
 * multi-Flow's flow_collection). id= may repeat within one flow_token,
 * positionally paired with the streams= list; source_id=/multi_flow= may
 * each appear at most once per token.
 */
static int tams_parse_flow_map(const char *str, int nb_streams,
                                TAMSContainerContext **container_ctxs_out, int *nb_container_ctxs_out,
                                TAMSMultiFlowContext **multi_flow_ctxs_out, int *nb_multi_flow_ctxs_out)
{
    TAMSContainerContext *container_ctxs = NULL;
    int nb_container_ctxs = 0;
    TAMSMultiFlowContext *multi_flow_ctxs = NULL;
    int nb_multi_flow_ctxs = 0;
    uint8_t *assigned = NULL;
    const char *p;
    int ret;

    *container_ctxs_out    = NULL;
    *nb_container_ctxs_out = 0;
    *multi_flow_ctxs_out    = NULL;
    *nb_multi_flow_ctxs_out = 0;

    if (nb_streams <= 0)
        return AVERROR(EINVAL);

    if (!str || !str[0]) {
        for (int i = 0; i < nb_streams; i++) {
            ret = tams_append_mono_container(i, &container_ctxs, &nb_container_ctxs);
            if (ret < 0) {
                tams_free_container_ctxs(container_ctxs, nb_container_ctxs);
                return ret;
            }
        }
        *container_ctxs_out    = container_ctxs;
        *nb_container_ctxs_out = nb_container_ctxs;
        return 0;
    }

    assigned = av_calloc(nb_streams, sizeof(*assigned));
    if (!assigned)
        return AVERROR(ENOMEM);

    p = str;
    while (*p) {
        const char *tok_end;

        while (*p == ' ')
            p++;
        if (!*p)
            break;

        tok_end = strchr(p, ' ');
        if (!tok_end)
            tok_end = p + strlen(p);

        if (!strncmp(p, "flow=", 5)) {
            ret = tams_parse_flow_token(p + 5, tok_end, nb_streams, assigned,
                                        &container_ctxs, &nb_container_ctxs);
        } else if (!strncmp(p, "multi_flow=", 11)) {
            ret = tams_parse_multi_flow_token(p + 11, tok_end, &multi_flow_ctxs, &nb_multi_flow_ctxs);
        } else {
            ret = AVERROR(EINVAL); /* unknown token */
        }
        if (ret < 0)
            goto fail;

        p = tok_end;
    }

    /* any stream not explicitly mentioned in a flow= token defaults to its
     * own standalone mono Flow, same as when -flow_map is omitted entirely */
    for (int i = 0; i < nb_streams; i++) {
        if (assigned[i])
            continue;
        ret = tams_append_mono_container(i, &container_ctxs, &nb_container_ctxs);
        if (ret < 0)
            goto fail;
        assigned[i] = 1;
    }

    /* auto-create a (property-less) TAMSMultiFlowContext for any multi_flow_index
     * referenced by a container that has no standalone 'multi_flow=N,...'
     * token of its own -- e.g. a brand-new multi with no id=/source_id= to set */
    for (int j = 0; j < nb_container_ctxs; j++) {
        TAMSMultiFlowContext *tmp_multi_flow_ctxs;
        int found = 0;

        if (!container_ctxs[j].has_multi_flow)
            continue;
        for (int i = 0; i < nb_multi_flow_ctxs; i++) {
            if (multi_flow_ctxs[i].index == container_ctxs[j].multi_flow_index) {
                found = 1;
                break;
            }
        }
        if (found)
            continue;

        tmp_multi_flow_ctxs = av_realloc_array(multi_flow_ctxs, nb_multi_flow_ctxs + 1,
                                               sizeof(*multi_flow_ctxs));
        if (!tmp_multi_flow_ctxs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        multi_flow_ctxs = tmp_multi_flow_ctxs;
        memset(&multi_flow_ctxs[nb_multi_flow_ctxs], 0, sizeof(*multi_flow_ctxs));
        multi_flow_ctxs[nb_multi_flow_ctxs].index = container_ctxs[j].multi_flow_index;
        nb_multi_flow_ctxs++;
    }

    /* cross-link each multi to its member containers; error on orphan
     * multi_flow= tokens (index never referenced by any flow= token) */
    for (int i = 0; i < nb_multi_flow_ctxs; i++) {
        int *gidx = NULL, ngidx = 0;

        for (int j = 0; j < nb_container_ctxs; j++) {
            if (container_ctxs[j].has_multi_flow &&
                container_ctxs[j].multi_flow_index == multi_flow_ctxs[i].index) {
                int *tmp = av_realloc_array(gidx, ngidx + 1, sizeof(*gidx));
                if (!tmp) {
                    av_free(gidx);
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                gidx = tmp;
                gidx[ngidx++] = j;
            }
        }
        if (ngidx == 0) {
            av_free(gidx);
            ret = AVERROR(EINVAL); /* orphan multi_flow= token */
            goto fail;
        }
        multi_flow_ctxs[i].container_indices    = gidx;
        multi_flow_ctxs[i].nb_container_indices = ngidx;
    }

    av_free(assigned);
    *container_ctxs_out    = container_ctxs;
    *nb_container_ctxs_out = nb_container_ctxs;
    *multi_flow_ctxs_out    = multi_flow_ctxs;
    *nb_multi_flow_ctxs_out = nb_multi_flow_ctxs;
    return 0;

fail:
    av_free(assigned);
    tams_free_container_ctxs(container_ctxs, nb_container_ctxs);
    tams_free_multi_flow_ctxs(multi_flow_ctxs, nb_multi_flow_ctxs);
    return ret;
}

static av_cold int tams_init(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;
    int ret;

    ret = tams_parse_flow_map(c->flow_map_str, s->nb_streams,
                              &c->container_ctxs, &c->nb_container_ctxs,
                              &c->multi_flow_ctxs, &c->nb_multi_flow_ctxs);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid -flow_map: %s\n",
               c->flow_map_str ? c->flow_map_str : "(default)");
        return ret;
    }

    return 0;
}

static av_cold void tams_deinit(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    tams_free_container_ctxs(c->container_ctxs, c->nb_container_ctxs);
    c->container_ctxs    = NULL;
    c->nb_container_ctxs = 0;

    tams_free_multi_flow_ctxs(c->multi_flow_ctxs, c->nb_multi_flow_ctxs);
    c->multi_flow_ctxs    = NULL;
    c->nb_multi_flow_ctxs = 0;

    av_freep(&c->stream_to_container);
    av_freep(&c->stream_to_subindex);
    av_dict_free(&c->avio_opts);
}

static int tams_write_header(AVFormatContext *s)
{
    int ret = tams_build_containers(s);
    if (ret < 0)
        return ret;

    av_log(s, AV_LOG_ERROR, "TAMS muxer write_header not yet fully implemented\n");
    return AVERROR(ENOSYS);
}

static int tams_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    return AVERROR(ENOSYS);
}

static int tams_write_trailer(AVFormatContext *s)
{
    return 0;
}

#define OFFSET(x) offsetof(TAMSMuxContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM

static const AVOption tams_options[] = {
    { "flow_map", "map AVStreams to Flows/multi-Flows: space-separated "
        "\"flow=streams=0,id=<uuid>,source_id=<uuid>,multi_flow=N\" and "
        "\"multi_flow=N,id=<uuid>,source_id=<uuid>\" tokens "
        "(default, and for any stream not mentioned: its own standalone mono Flow)",
        OFFSET(flow_map_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "container", "container format (default: codec-specific raw ES for a single-stream "
        "container, fragmented mp4 for a shared multi-stream container)",
        OFFSET(container_name), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "label", "label for the multi-Flow (or sole Flow)",
        OFFSET(label), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "description", "description for the multi-Flow (or sole Flow)",
        OFFSET(description), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "tags", "tags for the multi-Flow (or sole Flow)",
        OFFSET(tags_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "start_timestamp", "TAMS timestamp string overriding the wall-clock timeline origin",
        OFFSET(start_timestamp_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "segment_duration", "target segment duration in seconds (-1=auto: existing flow's segment_duration, else 2)",
        OFFSET(segment_duration), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT64_MAX, FLAGS },
    { "retry_max", "maximum retry attempts for transient HTTP errors",
        OFFSET(retry_max), AV_OPT_TYPE_INT, {.i64 = 3}, 0, INT_MAX, FLAGS },
    { "retry_backoff", "initial retry backoff in microseconds, doubling on each attempt",
        OFFSET(retry_backoff_us), AV_OPT_TYPE_INT64, {.i64 = 500000}, 0, INT64_MAX, FLAGS },
    { NULL },
};

static const AVClass tams_muxer_class = {
    .class_name = "tams",
    .item_name  = av_default_item_name,
    .option     = tams_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_tams_muxer = {
    .p.name         = "tams",
    .p.long_name    = NULL_IF_CONFIG_SMALL("TAMS (Time-Addressable Media Store)"),
    .p.audio_codec  = AV_CODEC_ID_NONE,
    .p.video_codec  = AV_CODEC_ID_NONE,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags        = AVFMT_GLOBALHEADER,
    .p.priv_class   = &tams_muxer_class,
    .priv_data_size = sizeof(TAMSMuxContext),
    .init           = tams_init,
    .deinit         = tams_deinit,
    .write_header   = tams_write_header,
    .write_packet   = tams_write_packet,
    .write_trailer  = tams_write_trailer,
};
