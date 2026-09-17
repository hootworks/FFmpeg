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
#include "url.h"
#include "tams.h"

#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/random_seed.h"
#include "libavutil/time.h"
#include "libavutil/uuid.h"

#include <string.h>

typedef struct TAMSFlowContext {
    int stream_index;    /* index into the owning container's sub_ctx->streams[] */
    char flow_id[TAMS_UUID_SIZE];
    int  has_flow_id;    /* came from -flow_map id= on this token */
    char source_id[TAMS_UUID_SIZE];
    int  has_source_id;  /* came from -flow_map source_id= on this token */
    TAMSFlow flow;
} TAMSFlowContext;

/* one physical container / nested muxer i.e. one 'flow=' token from -flow_map */
typedef struct TAMSContainerContext {
    int *stream_indices; /* indices into the parent AVFormatContext's streams[] */
    int  nb_streams;

    const AVOutputFormat *oformat;
    AVFormatContext *sub_ctx; /* nested muxer for this container's physical output */

    TAMSFlowContext *flow_ctxs; /* one entry per stream_indices[], sized nb_streams */

    int multi_flow_index; /* the 'multi_flow=N' correlation key, if any */
    int has_multi_flow;   /* true iff this container belongs to some multi-Flow */

    char container_mime[128]; /* this container's resolved Flow.container MIME */

    /*
     * The Flow that owns this container's storage/segments: its sole mono
     * Flow when nb_streams==1, else the parent multi-Flow when nb_streams>1
     * (a shared container is registered once, against the multi, per the
     * demuxer's container-mapped-sub-flow model. Member mono Flows for a
     * shared container get an id/essence/role but no container/segments of
     * their own). Resolved once both mono and multi Flows exist.
     */
    char owner_flow_id[TAMS_UUID_SIZE];

    int reference_stream_index; /* sub_ctx stream index used for boundary decisions */
    int64_t segment_duration_ns;
    int64_t segment_start_pts;  /* TAMS-ns, relative to c->start_tai_ns */
    int64_t next_boundary_ns;
    int has_segment_data;
    int64_t segment_index;

    /*
     * Eager pre-allocated storage for the *next* segment, requested at this
     * segment's start so it's already available when this one flushes.
     */
    char *next_put_url;
    char *next_object_id;
    char next_put_content_type[128]; /* from put_url's "content-type", may be empty */
} TAMSContainerContext;

/*
 * One independent multi-Flow i.e. one distinct 'multi_flow=N' correlation key
 * referenced by some flow= token(s)
 */
typedef struct TAMSMultiFlowContext {
    int index; /* the N value: an arbitrary correlation key, need not be dense */
    char flow_id[TAMS_UUID_SIZE];
    int  has_flow_id; /* came from a 'multi_flow=N,id=...' token */
    char source_id[TAMS_UUID_SIZE];
    int  has_source_id; /* came from a 'multi_flow=N,source_id=...' token */
    char label[256]; /* came from a 'multi_flow=N,label=...' token; ignored if has_flow_id */
    char description[1024]; /* likewise, from 'description=...' */
    TAMSTag tags[TAMS_MAX_TAGS]; /* likewise, from one or more 'tags.<name>=...' */
    int  nb_tags;
    TAMSFlow flow;

    int *container_indices; /* indices into TAMSMuxContext.container_ctxs[] belonging here */
    int  nb_container_indices;
} TAMSMultiFlowContext;

typedef struct TAMSMuxContext {
    const AVClass *class;

    /* AVOptions */
    char *flow_map_str;
    char *container_name;
    char *start_timestamp_str;
    char *headers_str;
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

    /* base "/flows" collection URL, derived once from s->url in write_header */
    char flows_base_url[2048];
    int64_t min_object_timeout_us;
    int64_t min_presigned_url_timeout_us;
} TAMSMuxContext;

static void tams_free_container_ctxs(TAMSContainerContext *container_ctxs, int nb_container_ctxs)
{
    if (!container_ctxs)
        return;
    for (int i = 0; i < nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &container_ctxs[i];

        av_freep(&cc->next_put_url);
        av_freep(&cc->next_object_id);
        av_freep(&cc->stream_indices);
        av_freep(&cc->flow_ctxs);
        if (cc->sub_ctx && cc->sub_ctx->pb) {
            uint8_t *buf = NULL;
            avio_close_dyn_buf(cc->sub_ctx->pb, &buf);
            cc->sub_ctx->pb = NULL;
            av_free(buf);
        }
        avformat_free_context(cc->sub_ctx);
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

/*
 * Per-codec default raw elementary-stream muxer, tried when -container isn't
 * given and the container has exactly one stream. AV_CODEC_ID_RAWVIDEO and
 * AV_CODEC_ID_PCM_S24LE are deliberately absent: their raw forms have no
 * header/sync bytes at all and can't be auto-probed, so they fall through
 * to the mp4 fallback below.
 */
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

/*
 * Resolve oformat using -container if given, else a per-codec raw-ES default
 * for a single-stream container, else fragmented mp4.
 */
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

/*
 * Allocate cc->sub_ctx and clone this container's mapped streams into it,
 * following segment_mux_init()'s pattern in segment.c
 */
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
    AVUUID uu;

    if (av_uuid_parse(s, uu) < 0)
        return 0;
    return s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
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

    /*
     * container_mapping (needed whenever >1 stream shares this container)
     * only has meaning inside a multi-Flow's flow_collection
     */
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
    char label[256];
    int has_label = 0;
    char description[1024];
    int has_description = 0;
    TAMSTag tags[TAMS_MAX_TAGS];
    int nb_tags = 0;
    TAMSMultiFlowContext *tmp_multi_flow_ctxs, *multi_flow_ctx;

    id[0] = source_id[0] = label[0] = description[0] = '\0';

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
        } else if (!strncmp(p, "label=", 6)) {
            const char *v = p + 6;
            const char *v_end = memchr(v, ',', tok_end - v);

            if (!v_end)
                v_end = tok_end;
            if (has_label || (size_t)(v_end - v) >= sizeof(label))
                return AVERROR(EINVAL);
            memcpy(label, v, v_end - v);
            label[v_end - v] = '\0';
            has_label = 1;
            p = v_end;
        } else if (!strncmp(p, "description=", 12)) {
            const char *v = p + 12;
            const char *v_end = memchr(v, ',', tok_end - v);

            if (!v_end)
                v_end = tok_end;
            if (has_description || (size_t)(v_end - v) >= sizeof(description))
                return AVERROR(EINVAL);
            memcpy(description, v, v_end - v);
            description[v_end - v] = '\0';
            has_description = 1;
            p = v_end;
        } else if (!strncmp(p, "tags.", 5)) {
            const char *name = p + 5;
            const char *eq = memchr(name, '=', tok_end - name);
            const char *v, *v_end;
            size_t name_len;

            if (!eq)
                return AVERROR(EINVAL);
            name_len = eq - name;
            v = eq + 1;
            v_end = memchr(v, ',', tok_end - v);
            if (!v_end)
                v_end = tok_end;
            if (nb_tags >= TAMS_MAX_TAGS ||
                name_len >= TAMS_TAG_KEY_SIZE || (size_t)(v_end - v) >= TAMS_TAG_VALUE_SIZE)
                return AVERROR(EINVAL);
            memcpy(tags[nb_tags].key, name, name_len);
            tags[nb_tags].key[name_len] = '\0';
            memcpy(tags[nb_tags].value, v, v_end - v);
            tags[nb_tags].value[v_end - v] = '\0';
            nb_tags++;
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
    if (has_label)
        av_strlcpy(multi_flow_ctx->label, label, sizeof(multi_flow_ctx->label));
    if (has_description)
        av_strlcpy(multi_flow_ctx->description, description, sizeof(multi_flow_ctx->description));
    if (nb_tags) {
        memcpy(multi_flow_ctx->tags, tags, nb_tags * sizeof(*tags));
        multi_flow_ctx->nb_tags = nb_tags;
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
 *   multi_attr       := 'id=' uuid | 'source_id=' uuid | 'label=' text |
 *                        'description=' text | 'tags.' name '=' text
 *
 * An AVStream index may appear in at most one flow_token (hard error on
 * duplicate assignment); it is never required to appear in any. Any stream
 * -flow_map doesn't mention defaults to its own standalone mono Flow, same
 * as when -flow_map is omitted entirely. A flow_token with more than one
 * stream requires multi_flow= (container mapping only has meaning inside a
 * multi-Flow's flow_collection). id= may repeat within one flow_token,
 * positionally paired with the streams= list; source_id=/multi_flow= may
 * each appear at most once per token. label=/description=/tags.<name>= are
 * only valid on a multi_flow_token (a mono Flow's label/description come
 * from that AVStream's "title"/"comment" metadata instead, and its tags
 * from any other AVStream metadata key -- see tams_flow_from_stream()) and
 * are ignored if that multi_flow_token also has id= (a pinned multi-Flow is
 * never modified); tags.<name>= may repeat, one 'tags.' attr per tag.
 * Because ',' and ' ' are the token/attr delimiters, text values may
 * contain neither.
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

    /*
     * Any stream not explicitly mentioned in a flow= token defaults to its
     * own standalone mono Flow, same as when -flow_map is omitted entirely
     */
    for (int i = 0; i < nb_streams; i++) {
        if (assigned[i])
            continue;
        ret = tams_append_mono_container(i, &container_ctxs, &nb_container_ctxs);
        if (ret < 0)
            goto fail;
        assigned[i] = 1;
    }

    /*
     * Auto-create a (property-less) TAMSMultiFlowContext for any multi_flow_index
     * referenced by a container that has no standalone 'multi_flow=N,...'
     * token of its own e.g. a brand-new multi with no id=/source_id= to set
     */
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

    /*
     * Cross-link each multi to its member containers; error on orphan
     * multi_flow= tokens (index never referenced by any flow= token)
     */
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

    /*
     * This muxer is AVFMT_NOFILE (it never opens s->pb; every request is
     * issued explicitly against derived URLs), so the generic -headers
     * AVOption never reaches a URLContext to be consumed there. Capture it
     * ourselves for ff_tams_request() to replay on every same-host request.
     */
    if (c->headers_str) {
        ret = av_dict_set(&c->avio_opts, "headers", c->headers_str, 0);
        if (ret < 0)
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

static int tams_build_stream_maps(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    c->stream_to_container = av_malloc_array(s->nb_streams, sizeof(*c->stream_to_container));
    c->stream_to_subindex  = av_malloc_array(s->nb_streams, sizeof(*c->stream_to_subindex));
    if (!c->stream_to_container || !c->stream_to_subindex)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->nb_streams; i++) {
        c->stream_to_container[i] = -1;
        c->stream_to_subindex[i]  = -1;
    }
    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];
        for (int j = 0; j < cc->nb_streams; j++) {
            c->stream_to_container[cc->stream_indices[j]] = i;
            c->stream_to_subindex[cc->stream_indices[j]]  = j;
        }
    }

    return 0;
}

static void tams_generate_uuid(char out[TAMS_UUID_SIZE])
{
    AVUUID uu;

    if (av_random_bytes(uu, sizeof(uu)) < 0) {
        for (size_t i = 0; i < sizeof(uu); i++)
            uu[i] = av_get_random_seed() & 0xFF;
    }
    uu[6] = (uu[6] & 0x0F) | 0x40; /* version 4 */
    uu[8] = (uu[8] & 0x3F) | 0x80; /* variant 10 */
    av_uuid_unparse(uu, out);
}

static int tams_validate_stream(AVFormatContext *s, const AVStream *st)
{
    const AVCodecParameters *par = st->codecpar;

    if (!ff_tams_mime_from_codec(par->codec_id)) {
        av_log(s, AV_LOG_ERROR, "TAMS: codec %s has no known TAMS codec MIME mapping\n",
               avcodec_get_name(par->codec_id));
        return AVERROR(EINVAL);
    }

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (par->width <= 0 || par->height <= 0) {
            av_log(s, AV_LOG_ERROR, "TAMS: video stream missing frame dimensions\n");
            return AVERROR(EINVAL);
        }
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0) {
            av_log(s, AV_LOG_ERROR, "TAMS: audio stream missing sample_rate or channels\n");
            return AVERROR(EINVAL);
        }
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        break;
    default:
        av_log(s, AV_LOG_ERROR, "TAMS: unsupported media type for stream %d\n", st->index);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int tams_validate_streams(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];
        int ret;

        for (int j = 0; j < cc->nb_streams; j++) {
            ret = tams_validate_stream(s, s->streams[cc->stream_indices[j]]);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int tams_resolve_container_mime(const AVFormatContext *s, const TAMSContainerContext *cc,
                               char *out, size_t out_size)
{
    if (!strcmp(cc->oformat->name, "mp4")) {
        enum AVMediaType dominant = AVMEDIA_TYPE_DATA;

        for (int j = 0; j < cc->nb_streams; j++) {
            enum AVMediaType t = s->streams[cc->stream_indices[j]]->codecpar->codec_type;
            if (t == AVMEDIA_TYPE_VIDEO) { dominant = t; break; }
            if (t == AVMEDIA_TYPE_AUDIO && dominant != AVMEDIA_TYPE_VIDEO) dominant = t;
        }
        av_strlcpy(out, dominant == AVMEDIA_TYPE_VIDEO ? "video/mp4" :
                        dominant == AVMEDIA_TYPE_AUDIO ? "audio/mp4" : "application/mp4",
                  out_size);
        return 0;
    }

    if (cc->nb_streams == 1) {
        const char *mime = ff_tams_mime_from_codec(s->streams[cc->stream_indices[0]]->codecpar->codec_id);
        if (mime) {
            av_strlcpy(out, mime, out_size);
            return 0;
        }
    }

    return AVERROR(EINVAL);
}

/*
 * Copy every AVStream metadata entry other than "title"/"comment" (already
 * consumed as label/description) into flow as a tag.
 */
static int tams_apply_stream_tags(AVFormatContext *s, const AVDictionary *metadata, TAMSFlow *flow)
{
    const AVDictionaryEntry *t = NULL;

    while ((t = av_dict_iterate(metadata, t))) {
        if (!strcmp(t->key, "title") || !strcmp(t->key, "comment"))
            continue;
        if (flow->nb_tags >= TAMS_MAX_TAGS ||
            strlen(t->key) >= TAMS_TAG_KEY_SIZE || strlen(t->value) >= TAMS_TAG_VALUE_SIZE) {
            av_log(s, AV_LOG_ERROR, "TAMS: too many stream metadata tags, or tag '%s' too long\n", t->key);
            return AVERROR(EINVAL);
        }
        av_strlcpy(flow->tags[flow->nb_tags].key, t->key, sizeof(flow->tags[flow->nb_tags].key));
        av_strlcpy(flow->tags[flow->nb_tags].value, t->value, sizeof(flow->tags[flow->nb_tags].value));
        flow->nb_tags++;
    }

    return 0;
}

/*
 * Resolve label/description for one mono Flow from that AVStream's own
 * "title"/"comment" metadata.
 */
static void tams_resolve_stream_metadata(const AVStream *st,
                                         char *label, size_t label_size,
                                         char *desc, size_t desc_size)
{
    AVDictionaryEntry *t;

    t = av_dict_get(st->metadata, "title", NULL, 0);
    av_strlcpy(label, t ? t->value : "", label_size);

    t = av_dict_get(st->metadata, "comment", NULL, 0);
    av_strlcpy(desc, t ? t->value : "", desc_size);
}

/*
 * Build a fresh TAMSFlow from a mapped AVStream's codec parameters.
 * When shared_container is true, this Flow is one member of a
 * container shared with other Flow and this Flow only contributes essence params
 * and an id for the multi's flow_collection.
 */
static int tams_flow_from_stream(AVFormatContext *s, const AVStream *ist,
                                 const char *container_mime, int shared_container,
                                 TAMSFlow *flow)
{
    const AVCodecParameters *par = ist->codecpar;
    const char *mime;
    int ret;

    memset(flow, 0, sizeof(*flow));

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        flow->format      = TAMS_FORMAT_VIDEO;
        flow->frame_width  = par->width;
        flow->frame_height = par->height;
        if (ist->avg_frame_rate.num > 0 && ist->avg_frame_rate.den > 0)
            flow->frame_rate = ist->avg_frame_rate;
        if (par->bits_per_raw_sample > 0)
            flow->bit_depth = par->bits_per_raw_sample;
        break;
    case AVMEDIA_TYPE_AUDIO:
        flow->format      = TAMS_FORMAT_AUDIO;
        flow->sample_rate  = par->sample_rate;
        flow->channels     = par->ch_layout.nb_channels;
        if (par->bits_per_raw_sample > 0)
            flow->bit_depth = par->bits_per_raw_sample;
        if (par->frame_size > 0)
            flow->coded_frame_size = par->frame_size;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        flow->format = TAMS_FORMAT_DATA;
        av_strlcpy(flow->data_type, "text", sizeof(flow->data_type));
        break;
    default:
        return AVERROR(EINVAL);
    }

    mime = ff_tams_mime_from_codec(par->codec_id);
    if (!mime)
        return AVERROR(EINVAL);
    av_strlcpy(flow->codec, mime, sizeof(flow->codec));
    if (!shared_container)
        av_strlcpy(flow->container, container_mime, sizeof(flow->container));
    flow->generation = 1;

    tams_resolve_stream_metadata(ist, flow->label, sizeof(flow->label),
                                 flow->description, sizeof(flow->description));

    ret = tams_apply_stream_tags(s, ist->metadata, flow);
    if (ret < 0)
        return ret;

    return 0;
}

/*
 * Thin wrapper binding this muxer's captured avio_opts/retry options to the
 * shared ff_tams_request() helper
 */
static int tams_request(AVFormatContext *s, const char *url, const char *method,
                        const uint8_t *body, int body_size, const char *content_type,
                        AVBPrint *out)
{
    TAMSMuxContext *c = s->priv_data;

    return ff_tams_request(s, &c->avio_opts, url, method, body, body_size, content_type,
                           c->retry_max, c->retry_backoff_us, out);
}

/*
 * c->flows_base_url must be the store's "/flows" collection URL.
 * Other endpoints (a Flow, its /storage, its /segments, and the store-wide
 * /service) are derived from it.
 */
static int tams_derive_urls(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;
    int is_exact = 0;
    int ret = ff_tams_get_base_url(s->url, c->flows_base_url, sizeof(c->flows_base_url),
                                           &is_exact);
    if (ret >= 0 && !is_exact)
        ret = AVERROR(EINVAL);
    if (ret < 0)
        av_log(s, AV_LOG_ERROR,
               "TAMS output URL must be the store's \"/flows\" collection endpoint "
               "(e.g. http://host/flows), got '%s'\n", s->url);
    return ret;
}

/*
 * GET /service once; warn (but continue) on an api_version mismatch, record
 * the storage-lifetime guarantees, and hard-error if any container's segment
 * duration risks outliving the presigned PUT URL allocated for it one
 * segment ago.
 */
static int tams_fetch_service_limits(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;
    char url[2100];
    AVBPrint buf;
    TAMSService service;
    int ret;

    ret = ff_tams_service_url(c->flows_base_url, url, sizeof(url));
    if (ret < 0)
        return ret;

    ret = tams_request(s, url, NULL, NULL, 0, NULL, &buf);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS: GET /service failed: %s\n", av_err2str(ret));
        return ret;
    }

    ret = ff_tams_service_from_json(buf.str, &service);
    av_bprint_finalize(&buf, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS: malformed /service response\n");
        return ret;
    }

    if (service.api_version[0] && strcmp(service.api_version, TAMS_API_VERSION))
        av_log(s, AV_LOG_WARNING,
               "TAMS service api_version '%s' does not match the version this "
               "muxer was written against ('%s'); continuing anyway\n",
               service.api_version, TAMS_API_VERSION);

    c->min_object_timeout_us = service.min_object_timeout > 0
                              ? service.min_object_timeout * INT64_C(1000000) : 300 * INT64_C(1000000);
    c->min_presigned_url_timeout_us = service.min_presigned_url_timeout > 0
                                     ? service.min_presigned_url_timeout * INT64_C(1000000) : 30 * INT64_C(1000000);

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        int64_t dur_us = c->container_ctxs[i].segment_duration_ns / 1000;
        if (dur_us > 0 && dur_us >= c->min_presigned_url_timeout_us) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS: segment duration (%"PRId64" us) may exceed the store's "
                   "min_presigned_url_timeout (%"PRId64" us); the eagerly-allocated "
                   "put_url for a segment could expire before it is uploaded\n",
                   dur_us, c->min_presigned_url_timeout_us);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

/*
 * Resolve each container's target segment duration in nanoseconds.
 * Use -segment_duration if given, else a pinned flow's own segment_duration,
 * else the 2-second default for newly-created flows.
 */
static void tams_resolve_segment_duration(AVFormatContext *s, TAMSContainerContext *cc)
{
    TAMSMuxContext *c = s->priv_data;

    if (c->segment_duration >= 0) {
        cc->segment_duration_ns = c->segment_duration * TAMS_TIMEBASE;
        return;
    }
    for (int i = 0; i < cc->nb_streams; i++) {
        const TAMSFlow *flow = &cc->flow_ctxs[i].flow;
        if (flow->segment_duration.num > 0 && flow->segment_duration.den > 0) {
            cc->segment_duration_ns = av_rescale(flow->segment_duration.num, TAMS_TIMEBASE,
                                                 flow->segment_duration.den);
            return;
        }
    }
    cc->segment_duration_ns = 2 * TAMS_TIMEBASE;
}

/*
 * GET an existing Flow by id and validate that exactly one Flow object was
 * returned (guards against a malformed or unexpected response, since
 * ff_tams_flows_from_json() also accepts a /flows collection array).
 */
static int tams_get_and_validate_flow(AVFormatContext *s, const char *flow_id, TAMSFlow *out)
{
    TAMSMuxContext *c = s->priv_data;
    char url[2100];
    AVBPrint buf;
    TAMSFlow *flows = NULL;
    int nb_flows = 0;
    int ret;

    ret = ff_tams_flow_url(c->flows_base_url, flow_id, url, sizeof(url));
    if (ret < 0)
        return ret;

    ret = tams_request(s, url, NULL, NULL, 0, NULL, &buf);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS: failed to GET pinned Flow %s: %s\n",
               flow_id, av_err2str(ret));
        return ret;
    }

    ret = ff_tams_flows_from_json(buf.str, &flows, &nb_flows);
    av_bprint_finalize(&buf, NULL);
    if (ret < 0 || nb_flows != 1) {
        av_freep(&flows);
        av_log(s, AV_LOG_ERROR, "TAMS: malformed response for pinned Flow %s\n", flow_id);
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    }

    *out = flows[0];
    av_free(flows);
    return 0;
}

static int tams_check_pinned_essence(AVFormatContext *s, const AVStream *ist, const TAMSFlow *flow)
{
    const AVCodecParameters *par = ist->codecpar;

    if (flow->codec[0]) {
        const char *mime = ff_tams_mime_from_codec(par->codec_id);
        if (mime && strcmp(mime, flow->codec)) {
            av_log(s, AV_LOG_ERROR, "TAMS: pinned Flow %s codec %s != stream codec %s\n",
                   flow->id, flow->codec, mime);
            return AVERROR(EINVAL);
        }
    }
    if (flow->format == TAMS_FORMAT_VIDEO) {
        if (flow->frame_width != par->width || flow->frame_height != par->height) {
            av_log(s, AV_LOG_ERROR, "TAMS: pinned Flow %s dimensions %dx%d != stream %dx%d\n",
                   flow->id, flow->frame_width, flow->frame_height, par->width, par->height);
            return AVERROR(EINVAL);
        }
    } else if (flow->format == TAMS_FORMAT_AUDIO) {
        if (flow->sample_rate != par->sample_rate || flow->channels != par->ch_layout.nb_channels) {
            av_log(s, AV_LOG_ERROR, "TAMS: pinned Flow %s sample_rate/channels %d/%d != stream %d/%d\n",
                   flow->id, flow->sample_rate, flow->channels,
                   par->sample_rate, par->ch_layout.nb_channels);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

/* PUT /flows/{id} to create a new Flow. */
static int tams_create_flow(AVFormatContext *s, TAMSFlow *flow)
{
    TAMSMuxContext *c = s->priv_data;
    char url[2100];
    AVBPrint json;
    int ret;

    ret = ff_tams_flow_url(c->flows_base_url, flow->id, url, sizeof(url));
    if (ret < 0)
        return ret;

    av_bprint_init(&json, 0, INT_MAX);
    ret = ff_tams_flow_to_json(&json, flow);
    if (ret < 0) {
        av_bprint_finalize(&json, NULL);
        return ret;
    }

    ret = tams_request(s, url, "PUT", (const uint8_t *)json.str, json.len, "application/json", NULL);
    av_bprint_finalize(&json, NULL);
    if (ret < 0)
        av_log(s, AV_LOG_ERROR, "TAMS: failed to create Flow %s: %s\n", flow->id, av_err2str(ret));

    return ret;
}

/*
 * For every multi-Flow pinned to an existing id, GET+validate it,
 * then match every one of its member streams that wasn't
 * itself explicitly pinned against a remaining, essence-type-compatible
 * entry in the fetched flow_collection. Hard
 * errors on ambiguity (more than one equally-valid candidate) or leftovers
 * (an unclaimed flow_collection entry, or a member stream matching nothing).
 */
static int tams_prematch_pinned_multis(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    for (int i = 0; i < c->nb_multi_flow_ctxs; i++) {
        TAMSMultiFlowContext *mc = &c->multi_flow_ctxs[i];
        uint8_t claimed[TAMS_MAX_COLLECTION_ITEMS] = { 0 };
        int ret;

        if (!mc->has_flow_id)
            continue;

        ret = tams_get_and_validate_flow(s, mc->flow_id, &mc->flow);
        if (ret < 0)
            return ret;
        if (mc->flow.format != TAMS_FORMAT_MULTI) {
            av_log(s, AV_LOG_ERROR, "TAMS: pinned multi_flow=%d Flow %s is not a multi-Flow\n",
                   mc->index, mc->flow_id);
            return AVERROR(EINVAL);
        }

        /* mark entries already claimed by an explicit id= pin on a member stream */
        for (int j = 0; j < mc->nb_container_indices; j++) {
            TAMSContainerContext *cc = &c->container_ctxs[mc->container_indices[j]];
            for (int k = 0; k < cc->nb_streams; k++) {
                if (!cc->flow_ctxs[k].has_flow_id)
                    continue;
                for (int m = 0; m < mc->flow.nb_flow_collection_items; m++) {
                    if (!strcmp(mc->flow.flow_collection_items[m].id, cc->flow_ctxs[k].flow_id)) {
                        claimed[m] = 1;
                        break;
                    }
                }
            }
        }

        /*
         * Match every unpinned member stream against a remaining,
         * essence-type compatible flow_collection entry
         */
        for (int j = 0; j < mc->nb_container_indices; j++) {
            TAMSContainerContext *cc = &c->container_ctxs[mc->container_indices[j]];

            for (int k = 0; k < cc->nb_streams; k++) {
                TAMSFlowContext *fc = &cc->flow_ctxs[k];
                enum AVMediaType want;
                const char *want_role;
                int match = -1;

                if (fc->has_flow_id)
                    continue;

                want = s->streams[cc->stream_indices[k]]->codecpar->codec_type;
                want_role = want == AVMEDIA_TYPE_VIDEO ? "video" :
                           want == AVMEDIA_TYPE_AUDIO ? "audio" : "data";

                for (int m = 0; m < mc->flow.nb_flow_collection_items; m++) {
                    if (claimed[m] || strcmp(mc->flow.flow_collection_items[m].role, want_role))
                        continue;
                    if (match >= 0) {
                        av_log(s, AV_LOG_ERROR,
                               "TAMS: ambiguous match for stream %d against pinned multi_flow=%d's "
                               "flow_collection (more than one unclaimed '%s' entry)\n",
                               cc->stream_indices[k], mc->index, want_role);
                        return AVERROR(EINVAL);
                    }
                    match = m;
                }
                if (match < 0) {
                    av_log(s, AV_LOG_ERROR,
                           "TAMS: no matching flow_collection entry for stream %d "
                           "against pinned multi_flow=%d\n",
                           cc->stream_indices[k], mc->index);
                    return AVERROR(EINVAL);
                }

                claimed[match] = 1;
                av_strlcpy(fc->flow_id, mc->flow.flow_collection_items[match].id, sizeof(fc->flow_id));
                fc->has_flow_id = 1;
            }
        }

        for (int m = 0; m < mc->flow.nb_flow_collection_items; m++) {
            if (!claimed[m]) {
                av_log(s, AV_LOG_ERROR,
                       "TAMS: pinned multi_flow=%d's flow_collection entry %s (role=%s) "
                       "was not claimed by any mapped stream\n",
                       mc->index, mc->flow.flow_collection_items[m].id,
                       mc->flow.flow_collection_items[m].role);
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

/*
 * Resolve every mono Flow via GET and validation if -flow_map gave an id=,
 * else build and PUT a new one.
 */
static int tams_resolve_mono_flows(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];
        int shared_container = cc->nb_streams > 1;
        int ret;

        ret = tams_resolve_container_mime(s, cc, cc->container_mime, sizeof(cc->container_mime));
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "TAMS: could not determine a container MIME type\n");
            return ret;
        }

        for (int j = 0; j < cc->nb_streams; j++) {
            TAMSFlowContext *fc = &cc->flow_ctxs[j];
            AVStream *ist = s->streams[cc->stream_indices[j]];

            if (fc->has_flow_id) {
                ret = tams_get_and_validate_flow(s, fc->flow_id, &fc->flow);
                if (ret < 0)
                    return ret;
                ret = tams_check_pinned_essence(s, ist, &fc->flow);
                if (ret < 0)
                    return ret;
                if (fc->has_source_id && strcmp(fc->flow.source_id, fc->source_id)) {
                    av_log(s, AV_LOG_ERROR,
                           "TAMS: pinned Flow %s source_id %s != -flow_map source_id %s\n",
                           fc->flow_id, fc->flow.source_id, fc->source_id);
                    return AVERROR(EINVAL);
                }
                continue;
            }

            ret = tams_flow_from_stream(s, ist, cc->container_mime, shared_container, &fc->flow);
            if (ret < 0)
                return ret;

            tams_generate_uuid(fc->flow_id);
            fc->has_flow_id = 1;
            av_strlcpy(fc->flow.id, fc->flow_id, sizeof(fc->flow.id));

            if (fc->has_source_id)
                av_strlcpy(fc->flow.source_id, fc->source_id, sizeof(fc->flow.source_id));
            else
                tams_generate_uuid(fc->flow.source_id);

            ret = tams_create_flow(s, &fc->flow);
            if (ret < 0)
                return ret;
        }

        tams_resolve_segment_duration(s, cc);
    }

    return 0;
}

/*
 * Resolve every multi-Flow. If -flow_map specified an id= then GET it,
 * otherwise create and PUT a fresh multi-Flow whose
 * flow_collection lists every member container's now-resolved mono Flows.
 */
static int tams_resolve_multi_flows(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    for (int i = 0; i < c->nb_multi_flow_ctxs; i++) {
        TAMSMultiFlowContext *mc = &c->multi_flow_ctxs[i];
        int ret;

        /*
         * Pinned multis were already GET-validated and member-matched in
         * tams_prematch_pinned_multis(); never rebuilt/re-created here
         */
        if (mc->has_flow_id)
            continue;

        memset(&mc->flow, 0, sizeof(mc->flow));
        mc->flow.format = TAMS_FORMAT_MULTI;
        mc->flow.generation = 1;

        tams_generate_uuid(mc->flow_id);
        mc->has_flow_id = 1;
        av_strlcpy(mc->flow.id, mc->flow_id, sizeof(mc->flow.id));

        if (mc->has_source_id)
            av_strlcpy(mc->flow.source_id, mc->source_id, sizeof(mc->flow.source_id));
        else
            tams_generate_uuid(mc->flow.source_id);

        av_strlcpy(mc->flow.label, mc->label, sizeof(mc->flow.label));
        av_strlcpy(mc->flow.description, mc->description, sizeof(mc->flow.description));
        memcpy(mc->flow.tags, mc->tags, mc->nb_tags * sizeof(*mc->tags));
        mc->flow.nb_tags = mc->nb_tags;

        for (int j = 0; j < mc->nb_container_indices; j++) {
            TAMSContainerContext *cc = &c->container_ctxs[mc->container_indices[j]];
            int shared_container = cc->nb_streams > 1;

            /*
             * A shared container's bytes belong to the multi-Flow itself,
             * not to any one member so carry its MIME on the multi Flow.
             */
            if (shared_container && !mc->flow.container[0])
                av_strlcpy(mc->flow.container, cc->container_mime, sizeof(mc->flow.container));

            for (int k = 0; k < cc->nb_streams; k++) {
                TAMSFlowCollectionItem *item;
                enum AVMediaType t;

                if (mc->flow.nb_flow_collection_items >= TAMS_MAX_COLLECTION_ITEMS) {
                    av_log(s, AV_LOG_ERROR, "TAMS: multi_flow=%d has too many member streams\n",
                           mc->index);
                    return AVERROR(EINVAL);
                }
                item = &mc->flow.flow_collection_items[mc->flow.nb_flow_collection_items++];
                av_strlcpy(item->id, cc->flow_ctxs[k].flow_id, sizeof(item->id));

                t = s->streams[cc->stream_indices[k]]->codecpar->codec_type;
                av_strlcpy(item->role, t == AVMEDIA_TYPE_VIDEO ? "video" :
                                      t == AVMEDIA_TYPE_AUDIO ? "audio" : "data",
                          sizeof(item->role));

                /*
                 * container_mapping only has meaning for a shared container:
                 * it identifies which track within it this Flow occupies
                 */
                if (shared_container) {
                    item->has_container_mapping = 1;
                    item->container_mapping.format_track_index = k;
                    item->container_mapping.has_format_track_index = 1;
                    if (!strcmp(cc->oformat->name, "mp4")) {
                        item->container_mapping.isobmff_track_id = k + 1;
                        item->container_mapping.has_isobmff_track_id = 1;
                    }
                }
            }
        }

        ret = tams_create_flow(s, &mc->flow);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/*
 * POST /flows/{owner_flow_id}/storage {"limit":1}, stashing the put_url/
 * object_id for the *next* segment. Safe to do a full segment ahead of
 * when it's needed due to the per the store's min_object_timeout/
 * min_presigned_url_timeout guarantees.
 */
static int tams_alloc_next_storage(AVFormatContext *s, TAMSContainerContext *cc)
{
    TAMSMuxContext *c = s->priv_data;
    char url[2100];
    AVBPrint resp;
    TAMSMediaObject *objects = NULL;
    int nb_objects = 0;
    int ret;

    ret = ff_tams_flow_subresource_url(c->flows_base_url, cc->owner_flow_id, "storage",
                                       url, sizeof(url));
    if (ret < 0)
        return ret;

    ret = tams_request(s, url, "POST", (const uint8_t *)"{\"limit\":1}", 11,
                       "application/json", &resp);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS: failed to allocate storage for Flow %s: %s\n",
               cc->owner_flow_id, av_err2str(ret));
        return ret;
    }

    ret = ff_tams_storage_allocation_from_json(resp.str, &objects, &nb_objects);
    av_bprint_finalize(&resp, NULL);
    if (ret < 0 || nb_objects < 1) {
        av_log(s, AV_LOG_ERROR, "TAMS: malformed storage-allocation response for Flow %s\n",
               cc->owner_flow_id);
        av_free(objects);
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    }

    av_freep(&cc->next_put_url);
    av_freep(&cc->next_object_id);
    cc->next_put_url   = av_strdup(objects[0].put_url.url);
    cc->next_object_id = av_strdup(objects[0].object_id);
    av_strlcpy(cc->next_put_content_type, objects[0].put_url.content_type,
              sizeof(cc->next_put_content_type));
    av_free(objects);
    if (!cc->next_put_url || !cc->next_object_id)
        return AVERROR(ENOMEM);

    return 0;
}

/*
 * Close the current segment's dyn-buf, PUT its bytes to the container's
 * owner Flow and POST one /segments registration, then (unless final)
 * reopen a fresh dyn-buf and eagerly allocate storage for the segment
 * after that.
 */
static int tams_flush_segment(AVFormatContext *s, TAMSContainerContext *cc, int is_final)
{
    TAMSMuxContext *c = s->priv_data;
    uint8_t *data = NULL;
    int size, ret = 0;
    TAMSTimeRange tr = { 0 };
    TAMSFlowSegment seg = { 0 };
    char segs_url[2100];
    AVBPrint json;

    size = avio_close_dyn_buf(cc->sub_ctx->pb, &data);
    cc->sub_ctx->pb = NULL;

    tr.has_start = 1;
    tr.start_inclusive = 1;
    tr.start = c->start_tai_ns + cc->segment_start_pts;
    tr.has_end = 1;
    tr.end_inclusive = 0;
    tr.end = c->start_tai_ns + cc->next_boundary_ns;

    if (!cc->next_put_url) {
        av_log(s, AV_LOG_ERROR, "TAMS: no pre-allocated storage for Flow %s\n", cc->owner_flow_id);
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = tams_request(s, cc->next_put_url, "PUT", data, size,
                       cc->next_put_content_type[0] ? cc->next_put_content_type : NULL, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS: failed to PUT segment bytes for Flow %s: %s\n",
               cc->owner_flow_id, av_err2str(ret));
        goto end;
    }

    av_strlcpy(seg.object_id, cc->next_object_id, sizeof(seg.object_id));
    seg.timerange = tr;
    seg.ts_offset = tr.start;

    av_bprint_init(&json, 0, INT_MAX);
    ret = ff_tams_flow_segment_to_json(&json, &seg);
    if (ret >= 0)
        ret = ff_tams_flow_subresource_url(c->flows_base_url, cc->owner_flow_id, "segments",
                                           segs_url, sizeof(segs_url));
    if (ret >= 0)
        ret = tams_request(s, segs_url, "POST", (const uint8_t *)json.str, json.len,
                           "application/json", NULL);
    av_bprint_finalize(&json, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS: failed to register segment for Flow %s: %s\n",
               cc->owner_flow_id, av_err2str(ret));
        goto end;
    }

    av_freep(&cc->next_put_url);
    av_freep(&cc->next_object_id);

    cc->segment_index++;
    cc->has_segment_data  = 0;
    cc->segment_start_pts = cc->next_boundary_ns;

    if (!is_final) {
        ret = avio_open_dyn_buf(&cc->sub_ctx->pb);
        if (ret < 0)
            goto end;
        ret = tams_alloc_next_storage(s, cc);
    }

end:
    av_free(data);
    return ret;
}

/*
 * Log a summary of the resolved AVStream -> container -> Flow/multi-Flow
 * mapping.
 */
static void tams_log_mapping_summary(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;

    av_log(s, AV_LOG_VERBOSE, "TAMS mapping summary:\n");

    av_log(s, AV_LOG_VERBOSE, "  Containers: %d\n", c->nb_container_ctxs);
    for (int i = 0; i < c->nb_container_ctxs; i++) {
        const TAMSContainerContext *cc = &c->container_ctxs[i];

        av_log(s, AV_LOG_VERBOSE, "    Container[%d]: oformat=%s, owner=%s\n",
               i, cc->oformat->name, cc->owner_flow_id);
        for (int j = 0; j < cc->nb_streams; j++) {
            av_log(s, AV_LOG_VERBOSE, "      stream %d -> ", cc->stream_indices[j]);
            ff_tams_log_flow_summary(s, AV_LOG_VERBOSE, &cc->flow_ctxs[j].flow);
        }
    }

    av_log(s, AV_LOG_VERBOSE, "  Multi-Flows: %d\n", c->nb_multi_flow_ctxs);
    for (int i = 0; i < c->nb_multi_flow_ctxs; i++) {
        av_log(s, AV_LOG_VERBOSE, "    Multi[%d]: ", i);
        ff_tams_log_flow_summary(s, AV_LOG_VERBOSE, &c->multi_flow_ctxs[i].flow);
    }
}

static int tams_write_header(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;
    int ret;

    ret = tams_validate_streams(s);
    if (ret < 0)
        return ret;

    if (c->start_timestamp_str) {
        ret = ff_tams_timestamp_from_str(c->start_timestamp_str, &c->start_tai_ns);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Invalid -start_timestamp '%s'\n", c->start_timestamp_str);
            return ret;
        }
    } else {
        c->start_tai_ns = av_gettime() * INT64_C(1000);
    }

    ret = tams_derive_urls(s);
    if (ret < 0)
        return ret;

    ret = tams_build_containers(s);
    if (ret < 0)
        return ret;

    ret = tams_build_stream_maps(s);
    if (ret < 0)
        return ret;

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];

        cc->reference_stream_index = 0;
        for (int j = 0; j < cc->nb_streams; j++) {
            if (cc->sub_ctx->streams[j]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                cc->reference_stream_index = j;
                break;
            }
        }
    }

    /*
     * Match existing multi-Flows' members first, so an implicitly-matched
     * member takes a GET+validate path below rather than creating a brand new Flow.
     */
    ret = tams_prematch_pinned_multis(s);
    if (ret < 0)
        return ret;

    /*
     * Resolves each container's flow.container MIME internally, before any
     * freshly-built Flow is PUT-created.
     */
    ret = tams_resolve_mono_flows(s);
    if (ret < 0)
        return ret;

    ret = tams_resolve_multi_flows(s);
    if (ret < 0)
        return ret;

    /*
     * A shared container (nb_streams > 1) is registered once, against its
     * owning multi-Flow, never against any one member.
     */
    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];

        if (cc->nb_streams == 1) {
            av_strlcpy(cc->owner_flow_id, cc->flow_ctxs[0].flow_id, sizeof(cc->owner_flow_id));
            continue;
        }
        for (int j = 0; j < c->nb_multi_flow_ctxs; j++) {
            if (c->multi_flow_ctxs[j].index == cc->multi_flow_index) {
                av_strlcpy(cc->owner_flow_id, c->multi_flow_ctxs[j].flow_id, sizeof(cc->owner_flow_id));
                break;
            }
        }
    }

    ret = tams_fetch_service_limits(s);
    if (ret < 0)
        return ret;

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        TAMSContainerContext *cc = &c->container_ctxs[i];

        ret = avio_open_dyn_buf(&cc->sub_ctx->pb);
        if (ret < 0)
            return ret;

        ret = avformat_write_header(cc->sub_ctx, NULL);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "TAMS: nested muxer write_header failed: %s\n", av_err2str(ret));
            return ret;
        }

        cc->has_segment_data  = 0;
        cc->segment_start_pts = 0;
        cc->next_boundary_ns  = cc->segment_duration_ns;

        ret = tams_alloc_next_storage(s, cc);
        if (ret < 0)
            return ret;
    }

    tams_log_mapping_summary(s);

    return 0;
}

static int tams_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    TAMSMuxContext *c = s->priv_data;
    int container_idx = c->stream_to_container[pkt->stream_index];
    int sub_idx = c->stream_to_subindex[pkt->stream_index];
    TAMSContainerContext *cc = &c->container_ctxs[container_idx];
    int64_t ts_ns = AV_NOPTS_VALUE;
    int ret;

    if (pkt->pts != AV_NOPTS_VALUE)
        ts_ns = av_rescale_q(pkt->pts, s->streams[pkt->stream_index]->time_base,
                             (AVRational){ 1, TAMS_TIMEBASE });

    if (sub_idx == cc->reference_stream_index && (pkt->flags & AV_PKT_FLAG_KEY) &&
        cc->has_segment_data && ts_ns != AV_NOPTS_VALUE && ts_ns >= cc->next_boundary_ns) {
        ret = tams_flush_segment(s, cc, 0);
        if (ret < 0)
            return ret;
        cc->next_boundary_ns = ts_ns + cc->segment_duration_ns;
    }

    if (!cc->has_segment_data) {
        cc->has_segment_data = 1;
        if (sub_idx == cc->reference_stream_index && ts_ns != AV_NOPTS_VALUE) {
            cc->segment_start_pts = ts_ns;
            cc->next_boundary_ns  = ts_ns + cc->segment_duration_ns;
        }
    }

    return ff_write_chained(cc->sub_ctx, sub_idx, pkt, s, 0);
}

static int tams_finalize_container(AVFormatContext *s, TAMSContainerContext *cc)
{
    int ret, final_ret = 0;

    if (!cc->sub_ctx || !cc->sub_ctx->pb)
        return 0;

    av_write_frame(cc->sub_ctx, NULL);
    ret = av_write_trailer(cc->sub_ctx);
    if (ret < 0)
        final_ret = ret;

    if (avio_tell(cc->sub_ctx->pb) > 0 || cc->has_segment_data) {
        ret = tams_flush_segment(s, cc, 1);
        if (ret < 0 && final_ret >= 0)
            final_ret = ret;
    } else {
        uint8_t *buf = NULL;
        avio_close_dyn_buf(cc->sub_ctx->pb, &buf);
        cc->sub_ctx->pb = NULL;
        av_free(buf);
    }

    return final_ret;
}

static int tams_write_trailer(AVFormatContext *s)
{
    TAMSMuxContext *c = s->priv_data;
    int ret = 0;

    for (int i = 0; i < c->nb_container_ctxs; i++) {
        int r = tams_finalize_container(s, &c->container_ctxs[i]);
        if (r < 0 && ret >= 0)
            ret = r;
    }

    return ret;
}

#define OFFSET(x) offsetof(TAMSMuxContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM

static const AVOption tams_options[] = {
    { "flow_map", "map AVStreams to Flows/multi-Flows: space-separated "
        "\"flow=streams=0,id=<uuid>,source_id=<uuid>,multi_flow=N\" and "
        "\"multi_flow=N,id=<uuid>,source_id=<uuid>,label=<text>,description=<text>,"
        "tags.<name>=<text>\" tokens (a mono Flow's label/description/tags come from its "
        "AVStream's metadata instead; default, and for any stream not mentioned: its own "
        "standalone mono Flow)",
        OFFSET(flow_map_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "container", "container format (default: codec-specific raw ES for a single-stream "
        "container, fragmented mp4 for a shared multi-stream container)",
        OFFSET(container_name), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "start_timestamp", "TAMS timestamp string overriding the wall-clock timeline origin",
        OFFSET(start_timestamp_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "headers", "extra HTTP headers (e.g. \"Authorization: Bearer <token>\\r\\n\") to send "
        "with every TAMS request",
        OFFSET(headers_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
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
    .p.audio_codec  = AV_CODEC_ID_AAC,
    .p.video_codec  = AV_CODEC_ID_H264,
    .p.subtitle_codec = AV_CODEC_ID_SUBRIP,
    .p.flags        = AVFMT_GLOBALHEADER | AVFMT_NOFILE,
    .p.priv_class   = &tams_muxer_class,
    .priv_data_size = sizeof(TAMSMuxContext),
    .init           = tams_init,
    .deinit         = tams_deinit,
    .write_header   = tams_write_header,
    .write_packet   = tams_write_packet,
    .write_trailer  = tams_write_trailer,
};
