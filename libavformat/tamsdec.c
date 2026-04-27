/*
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

/*
 *
 * Copyright (c) Hoot Works Limited
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Demuxes one or more TAMS Flows
 *
 * References
 * https://bbc.github.io/tams/main/index.html
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#include "tams.h"
#include "avformat.h"
#include "avio.h"
#include "demux.h"
#include "avio_internal.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavcodec/codec_par.h"
#include "libavcodec/defs.h"
#include "libavcodec/codec.h"
#include "url.h"

#define AVRATIONAL_FORMAT "%d/%d"
#define AVRATIONAL_ARG(rational) rational.num, rational.den

/* MIME type to AVCodecID mapping for TAMS codec field */
static const struct {
    const char *mime;
    enum AVCodecID id;
} tams_codec_map[] = {
    { "video/h264",      AV_CODEC_ID_H264 },
    { "video/hevc",      AV_CODEC_ID_HEVC },
    { "video/h265",      AV_CODEC_ID_HEVC },
    { "video/vp8",       AV_CODEC_ID_VP8 },
    { "video/vp9",       AV_CODEC_ID_VP9 },
    { "video/av1",       AV_CODEC_ID_AV1 },
    { "video/mpeg2",     AV_CODEC_ID_MPEG2VIDEO },
    { "video/raw",       AV_CODEC_ID_RAWVIDEO },
    { "audio/aac",       AV_CODEC_ID_AAC },
    { "audio/mp4a-latm", AV_CODEC_ID_AAC },
    { "audio/opus",      AV_CODEC_ID_OPUS },
    { "audio/mp3",       AV_CODEC_ID_MP3 },
    { "audio/mpeg",      AV_CODEC_ID_MP3 },
    { "audio/flac",      AV_CODEC_ID_FLAC },
    { "audio/vorbis",    AV_CODEC_ID_VORBIS },
    { "audio/ac3",       AV_CODEC_ID_AC3 },
    { "audio/eac3",      AV_CODEC_ID_EAC3 },
    { "audio/pcm",       AV_CODEC_ID_PCM_S24LE },
    { "text/vtt",        AV_CODEC_ID_WEBVTT },
    { "text/srt",        AV_CODEC_ID_SRT },
};

typedef struct TAMSSegmentContext {
    int flow_index;
    TAMSFlowSegment *segments;
    int nb_segments;
    int cur_segment_index;
    AVFormatContext *sub_ctx;
    int is_live;
    int64_t poll_interval;
    int refcount;
} TAMSSegmentContext;

typedef struct TAMSStreamMapping {
    int flow_index;
    int parent_flow_index;
    int container_track_index;
} TAMSStreamMapping;

typedef struct TAMSStreamContext {
    int flow_index;
    int seg_ctx_index;
    int sub_stream_index;
    enum AVMediaType media_type;
    int container_track_index;
    int64_t current_ts;
    int eof;
    int extradata_copied;
} TAMSStreamContext;

typedef struct TAMSContext {
    const AVClass *class;

    TAMSFlow *flows;
    int nb_flows;

    TAMSStreamMapping *stream_mappings;
    int nb_streams;
    TAMSStreamContext *streams;

    TAMSSegmentContext *seg_ctxs;
    int nb_seg_ctxs;

    AVDictionary *avio_opts;

    int64_t live_threshold;
    int64_t live_timeout;
    int64_t seg_poll_init;
    int64_t seg_poll_max;
} TAMSContext;


static enum AVCodecID tams_codec_lookup(const char *mime)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(tams_codec_map); i++) {
        if (!strcmp(mime, tams_codec_map[i].mime))
            return tams_codec_map[i].id;
    }
    return AV_CODEC_ID_NONE;
}

static int tams_same_host(const char *url1, const char *url2)
{
    char host1[256] = "", host2[256] = "";
    av_url_split(NULL, 0, NULL, 0, host1, sizeof(host1), NULL, NULL, 0, url1);
    av_url_split(NULL, 0, NULL, 0, host2, sizeof(host2), NULL, NULL, 0, url2);
    return !av_strcasecmp(host1, host2);
}

static int64_t tams_parse_iso8601(const char *str)
{
    int64_t t;
    if (!str || !str[0])
        return 0;
    if (av_parse_time(&t, str, 0) < 0)
        return 0;
    return t;
}

/*
 * Convert a flow's segment duration from rational to microseconds.
 * Returns 1 second as default if no valid segment duration is specified.
 * Used for live stream polling intervals and timeout calculations.
 */
static int64_t tams_segment_duration_us(const TAMSFlow *flow)
{
    if (flow->segment_duration.num > 0 && flow->segment_duration.den > 0)
        return av_rescale(flow->segment_duration.num, 1000000,
                          flow->segment_duration.den);
    return 1000000;
}

static int tams_find_flow_by_id(const TAMSContext *c, const char *id)
{
    for (int i = 0; i < c->nb_flows; i++) {
        if (!strcmp(c->flows[i].id, id))
            return i;
    }
    return -1;
}

static int tams_check_live(TAMSContext *c, const TAMSFlow *flow)
{
    int64_t seg_updated, age, threshold;

    seg_updated = tams_parse_iso8601(flow->segments_updated);
    if (!seg_updated)
        return 0;

    age = av_gettime() - seg_updated;
    if (age < 0)
        age = 0;

    if (c->live_threshold >= 0)
        threshold = c->live_threshold * 1000000LL;
    else
        threshold = 2 * tams_segment_duration_us(flow);

    return age < threshold;
}

static int tams_check_live_expired(TAMSContext *c, const TAMSFlow *flow)
{
    int64_t seg_updated, age, timeout;

    seg_updated = tams_parse_iso8601(flow->segments_updated);
    if (!seg_updated)
        return 1;

    age = av_gettime() - seg_updated;
    if (age < 0)
        return 0;

    if (c->live_timeout >= 0)
        timeout = c->live_timeout * 1000000LL;
    else
        timeout = 4 * tams_segment_duration_us(flow);

    return age > timeout;
}

static int64_t tams_get_poll_init(TAMSContext *c, const TAMSFlow *flow)
{
    if (c->seg_poll_init >= 0)
        return c->seg_poll_init;
    return tams_segment_duration_us(flow);
}

static void tams_setup_common_stream_tags(AVStream *st, const TAMSFlow *flow)
{
    if (flow->label[0])
        av_dict_set(&st->metadata, "title", flow->label, 0);
    if (flow->description[0])
        av_dict_set(&st->metadata, "comment", flow->description, 0);
    if (flow->id[0])
        av_dict_set(&st->metadata, "tams_flow_id", flow->id, 0);
    if (flow->source_id[0])
        av_dict_set(&st->metadata, "tams_source_id", flow->source_id, 0);
    if (flow->codec[0])
        av_dict_set(&st->metadata, "tams_codec", flow->codec, 0);
    if (flow->max_bit_rate > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", flow->max_bit_rate);
        av_dict_set(&st->metadata, "tams_max_bit_rate", buf, 0);
    }
    if (flow->read_only)
        av_dict_set(&st->metadata, "tams_read_only", "true", 0);
    if (flow->generation > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", flow->generation);
        av_dict_set(&st->metadata, "tams_generation", buf, 0);
    }
    if (flow->segment_duration.num > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d/%d", flow->segment_duration.num, flow->segment_duration.den);
        av_dict_set(&st->metadata, "tams_segment_duration", buf, 0);
    }
    if (flow->created_by[0])
        av_dict_set(&st->metadata, "tams_created_by", flow->created_by, 0);
    if (flow->updated_by[0])
        av_dict_set(&st->metadata, "tams_updated_by", flow->updated_by, 0);
    if (flow->metadata_version[0])
        av_dict_set(&st->metadata, "tams_metadata_version", flow->metadata_version, 0);
    if (flow->created[0])
        av_dict_set(&st->metadata, "tams_created", flow->created, 0);
    if (flow->metadata_updated[0])
        av_dict_set(&st->metadata, "tams_metadata_updated", flow->metadata_updated, 0);
    if (flow->segments_updated[0])
        av_dict_set(&st->metadata, "tams_segments_updated", flow->segments_updated, 0);
    for (int i = 0; i < flow->nb_tags; i++) {
        char meta_key[160];
        snprintf(meta_key, sizeof(meta_key), "tams_tag_%s", flow->tags[i].key);
        av_dict_set(&st->metadata, meta_key, flow->tags[i].value, 0);
    }
}

static int tams_setup_video_stream(AVStream *st, const TAMSFlow *flow)
{
    if (flow->frame_width <= 0 || flow->frame_height <= 0) {
        av_log(NULL, AV_LOG_ERROR, "TAMS video flow missing required frame dimensions\n");
        return AVERROR_INVALIDDATA;
    }

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->width      = flow->frame_width;
    st->codecpar->height     = flow->frame_height;

    if (flow->frame_rate.num > 0) {
        st->avg_frame_rate = flow->frame_rate;
        st->r_frame_rate   = flow->frame_rate;
    }

    if (flow->pixel_aspect_ratio.num > 0)
        st->codecpar->sample_aspect_ratio = flow->pixel_aspect_ratio;

    if (flow->bit_depth > 0)
        st->codecpar->bits_per_raw_sample = flow->bit_depth;

    switch (flow->interlace_mode) {
    case TAMS_INTERLACE_PROGRESSIVE:
    case TAMS_INTERLACE_PSF:
        st->codecpar->field_order = AV_FIELD_PROGRESSIVE;
        break;
    case TAMS_INTERLACE_TFF:
        st->codecpar->field_order = AV_FIELD_TT;
        break;
    case TAMS_INTERLACE_BFF:
        st->codecpar->field_order = AV_FIELD_BB;
        break;
    default:
        break;
    }

    switch (flow->colorspace) {
    case TAMS_COLORSPACE_BT601:
        st->codecpar->color_space    = AVCOL_SPC_SMPTE170M;
        st->codecpar->color_primaries = AVCOL_PRI_SMPTE170M;
        break;
    case TAMS_COLORSPACE_BT709:
        st->codecpar->color_space    = AVCOL_SPC_BT709;
        st->codecpar->color_primaries = AVCOL_PRI_BT709;
        break;
    case TAMS_COLORSPACE_BT2020:
    case TAMS_COLORSPACE_BT2100:
        st->codecpar->color_space    = AVCOL_SPC_BT2020_NCL;
        st->codecpar->color_primaries = AVCOL_PRI_BT2020;
        break;
    default:
        break;
    }

    switch (flow->transfer_characteristic) {
    case TAMS_TRANSFER_SDR:
        st->codecpar->color_trc = AVCOL_TRC_BT709;
        break;
    case TAMS_TRANSFER_HLG:
        st->codecpar->color_trc = AVCOL_TRC_ARIB_STD_B67;
        break;
    case TAMS_TRANSFER_PQ:
        st->codecpar->color_trc = AVCOL_TRC_SMPTE2084;
        break;
    default:
        break;
    }

    if (flow->component_type == TAMS_COMPONENT_RGB) {
        if (flow->bit_depth > 8)
            st->codecpar->format = AV_PIX_FMT_GBRP10;
        else
            st->codecpar->format = AV_PIX_FMT_GBRP;
    } else if (flow->horiz_chroma_subs > 0 && flow->vert_chroma_subs > 0) {
        if (flow->horiz_chroma_subs == 2 && flow->vert_chroma_subs == 2) {
            st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
        } else if (flow->horiz_chroma_subs == 2 && flow->vert_chroma_subs == 1) {
            st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_YUV422P10 : AV_PIX_FMT_YUV422P;
        } else if (flow->horiz_chroma_subs == 1 && flow->vert_chroma_subs == 1) {
            st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P;
        }
    }

    tams_setup_common_stream_tags(st, flow);

    return 0;
}

static int tams_setup_audio_stream(AVStream *st, const TAMSFlow *flow)
{
    if (flow->sample_rate <= 0 || flow->channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "TAMS audio flow missing required sample_rate or channels\n");
        return AVERROR_INVALIDDATA;
    }

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->sample_rate = flow->sample_rate;

    av_channel_layout_default(&st->codecpar->ch_layout, flow->channels);

    if (flow->bit_depth > 0)
        st->codecpar->bits_per_raw_sample = flow->bit_depth;

    if (flow->coded_frame_size > 0)
        st->codecpar->frame_size = flow->coded_frame_size;

    tams_setup_common_stream_tags(st, flow);

    return 0;
}

static int tams_setup_subtitle_stream(AVStream *st, const TAMSFlow *flow)
{
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    tams_setup_common_stream_tags(st, flow);
    return 0;
}

static int tams_add_stream_mapping(TAMSContext *c, int flow_index,
                                   int parent_flow_index, int track_index)
{
    TAMSStreamMapping *tmp;

    tmp = av_realloc_array(c->stream_mappings, c->nb_streams + 1, sizeof(*tmp));
    if (!tmp)
        return AVERROR(ENOMEM);
    c->stream_mappings = tmp;
    
    c->stream_mappings[c->nb_streams].flow_index = flow_index;
    c->stream_mappings[c->nb_streams].parent_flow_index = parent_flow_index;
    c->stream_mappings[c->nb_streams].container_track_index = track_index;

    c->nb_streams++;
    return 0;
}

static int tams_create_stream(AVFormatContext *s, const TAMSFlow *flow,
                              int flow_index, int parent_flow_index,
                              const TAMSFlowCollectionItem *collection_item)
{
    TAMSContext *c = s->priv_data;
    AVStream *st;
    int ret;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->id = s->nb_streams - 1;

    avpriv_set_pts_info(st, 64, 1, TAMS_TIMEBASE);

    st->codecpar->codec_id = tams_codec_lookup(flow->codec);

    if (flow->timerange.has_start && flow->timerange.has_end)
        st->duration = flow->timerange.end - flow->timerange.start;

    if (flow->avg_bit_rate > 0)
        st->codecpar->bit_rate = flow->avg_bit_rate;

    if (collection_item) {
        if (collection_item->role[0])
            av_dict_set(&st->metadata, "tams_role", collection_item->role, 0);
        if (collection_item->has_container_mapping) {
            const TAMSContainerMapping *m = &collection_item->container_mapping;
            char buf[32];
            if (m->has_track_index) {
                snprintf(buf, sizeof(buf), "%d", m->track_index);
                av_dict_set(&st->metadata, "tams_track_index", buf, 0);
            }
            if (m->has_format_track_index) {
                snprintf(buf, sizeof(buf), "%d", m->format_track_index);
                av_dict_set(&st->metadata, "tams_format_track_index", buf, 0);
            }
            if (m->has_mp2ts_pid) {
                snprintf(buf, sizeof(buf), "%d", m->mp2ts_pid);
                av_dict_set(&st->metadata, "tams_mp2ts_pid", buf, 0);
            }
            if (m->has_isobmff_track_id) {
                snprintf(buf, sizeof(buf), "%d", m->isobmff_track_id);
                av_dict_set(&st->metadata, "tams_isobmff_track_id", buf, 0);
            }
            if (m->has_mxf_track_id) {
                snprintf(buf, sizeof(buf), "%d", m->mxf_track_id);
                av_dict_set(&st->metadata, "tams_mxf_track_id", buf, 0);
            }
            if (m->mxf_package_uid[0])
                av_dict_set(&st->metadata, "tams_mxf_package_uid",
                            m->mxf_package_uid, 0);
            if (m->audio_channel_range[0])
                av_dict_set(&st->metadata, "tams_audio_channel_range",
                            m->audio_channel_range, 0);
        }
    }

    switch (flow->format) {
    case TAMS_FORMAT_VIDEO:
    case TAMS_FORMAT_IMAGE:
        ret = tams_setup_video_stream(st, flow);
        break;
    case TAMS_FORMAT_AUDIO:
        ret = tams_setup_audio_stream(st, flow);
        break;
    case TAMS_FORMAT_DATA:
        if (st->codecpar->codec_id != AV_CODEC_ID_NONE &&
            avcodec_get_type(st->codecpar->codec_id) == AVMEDIA_TYPE_SUBTITLE) {
            ret = tams_setup_subtitle_stream(st, flow);
        } else {
            st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
            tams_setup_common_stream_tags(st, flow);
            ret = 0;
        }
        break;
    default:
        st->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
        ret = 0;
        break;
    }

    if (ret < 0)
        return ret;

    {
        int track_idx = -1;
        if (collection_item && collection_item->has_container_mapping &&
            collection_item->container_mapping.has_track_index)
            track_idx = collection_item->container_mapping.track_index;
        return tams_add_stream_mapping(c, flow_index, parent_flow_index, track_idx);
    }
}

static int tams_build_base_url(const char *full_url, char *base_url, size_t base_size)
{
    const char *query = strchr(full_url, '?');
    int path_len = query ? (int)(query - full_url) : (int)strlen(full_url);
    const char *last_slash = NULL;
    
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

static int tams_fetch_sub_flow(AVFormatContext *s, const char *flow_id)
{
    TAMSContext *c = s->priv_data;
    AVIOContext *pb = NULL;
    AVDictionary *opts = NULL;
    AVBPrint buf;
    TAMSFlow *tmp;
    char url[4096];
    char base_url[2048];
    const char *query;
    int ret;

    ret = tams_build_base_url(s->url, base_url, sizeof(base_url));
    if (ret < 0)
        return ret;

    query = strchr(s->url, '?');
    if (base_url[0])
        snprintf(url, sizeof(url), "%s/%s%s",
                 base_url, flow_id, query ? query : "");
    else
        snprintf(url, sizeof(url), "%s%s",
                 flow_id, query ? query : "");

    av_log(s, AV_LOG_VERBOSE, "TAMS fetching sub-flow: %s\n", url);

    ret = av_dict_copy(&opts, c->avio_opts, 0);
    if (ret < 0)
        return ret;

    ret = s->io_open(s, &pb, url, AVIO_FLAG_READ, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR,
               "TAMS failed to fetch sub-flow %s: %s\n",
               flow_id, av_err2str(ret));
        return ret;
    }

    av_bprint_init(&buf, 0, INT_MAX);
    ret = avio_read_to_bprint(pb, &buf, SIZE_MAX);
    ff_format_io_close(s, &pb);

    if (ret < 0) {
        av_bprint_finalize(&buf, NULL);
        return ret;
    }

    av_log(s, AV_LOG_DEBUG, "TAMS sub-flow JSON:\n%s\n", buf.str);

    tmp = av_realloc_array(c->flows, c->nb_flows + 1, sizeof(*c->flows));
    if (!tmp) {
        av_bprint_finalize(&buf, NULL);
        return AVERROR(ENOMEM);
    }
    c->flows = tmp;
    memset(&c->flows[c->nb_flows], 0, sizeof(*c->flows));

    {
        const char *cursor = buf.str;
        ret = ff_tams_parse_flow(&cursor, &c->flows[c->nb_flows]);
    }
    av_bprint_finalize(&buf, NULL);
    if (ret < 0)
        return ret;

    c->nb_flows++;
    return c->nb_flows - 1;
}

/*
 * Process a multi-flow's collected sub-flows. Two cases exist and may be mixed
 * within a single multi-flow:
 *
 * 1. Independent sub-flow: the sub-flow has its own "container" field set,
 *    meaning it has its own segment list fetched via /flows/{sub_flow_id}/segments.
 *    Each such sub-flow gets its own TAMSSegmentContext (parent_flow_index = -1).
 *
 * 2. Container-mapped sub-flow: the sub-flow has NO "container" field, meaning
 *    its media is muxed into the parent multi-flow's container. Segments are
 *    fetched via /flows/{multi_flow_id}/segments and a single TAMSSegmentContext
 *    is shared by all container-mapped sub-flows. The container_mapping on each
 *    collection item identifies which track within the container belongs to which
 *    sub-flow.
 */
static int tams_process_flow(AVFormatContext *s, int flow_index)
{
    TAMSContext *c = s->priv_data;
    const TAMSFlow *flow = &c->flows[flow_index];
    int seg_flow_index;

    if (flow->format != TAMS_FORMAT_MULTI)
        return tams_create_stream(s, flow, flow_index, -1, NULL);

    seg_flow_index = flow_index;

    av_log(s, AV_LOG_VERBOSE,
           "TAMS multi flow %s contains %d collected flows\n",
           flow->id, flow->nb_flow_collection);

    for (int i = 0; i < flow->nb_flow_collection; i++) {
        const TAMSFlowCollectionItem *item = &flow->flow_collection[i];
        int sub_index = tams_find_flow_by_id(c, item->id);
        int parent, ret;

        if (sub_index < 0) {
            av_log(s, AV_LOG_VERBOSE,
                   "TAMS multi flow %s: sub-flow %s (role=%s) "
                   "not in initial response, fetching\n",
                   flow->id, item->id, item->role);
            sub_index = tams_fetch_sub_flow(s, item->id);
            if (sub_index < 0) {
                av_log(s, AV_LOG_ERROR,
                       "TAMS multi flow %s: failed to fetch "
                       "collected flow %s (role=%s)\n",
                       flow->id, item->id, item->role);
                return AVERROR_INVALIDDATA;
            }
            flow = &c->flows[flow_index];
        }

        if (c->flows[sub_index].format == TAMS_FORMAT_MULTI) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS multi flow %s: nested multi flow %s not supported\n",
                   flow->id, item->id);
            return AVERROR_INVALIDDATA;
        }

        if (!c->flows[sub_index].timerange.has_start &&
            !c->flows[sub_index].timerange.has_end &&
            flow->timerange.has_start) {
            c->flows[sub_index].timerange = flow->timerange;
        }

        /* Sub-flow with its own container has independent segments;
         * sub-flow without a container shares the parent's segments. */
        parent = c->flows[sub_index].container[0] ? -1 : seg_flow_index;

        ret = tams_create_stream(s, &c->flows[sub_index], sub_index,
                                 parent, item);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int tams_parse_flows_json(TAMSContext *c, const char *json)
{
    const char *cursor = json;
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

            tmp = av_realloc_array(c->flows, c->nb_flows + 1, sizeof(*c->flows));
            if (!tmp)
                return AVERROR(ENOMEM);
            c->flows = tmp;

            ret = ff_tams_parse_flow(&cursor, &c->flows[c->nb_flows]);
            if (ret < 0)
                return ret;
            c->nb_flows++;

            ff_tams_json_skip_ws(&cursor);
            if (*cursor == ',')
                cursor++;
        }
    } else {
        c->flows = av_mallocz(sizeof(*c->flows));
        if (!c->flows)
            return AVERROR(ENOMEM);

        ret = ff_tams_parse_flow(&cursor, &c->flows[0]);
        if (ret < 0)
            return ret;
        c->nb_flows = 1;
    }

    return 0;
}

static void tams_build_clean_query(const char *query, char *out, size_t out_size)
{
    const char *p = query + 1;
    size_t len = 0;

    out[0] = '\0';
    if (out_size < 2) return;
    
    while (*p && len < out_size - 1) {
        const char *amp = strchr(p, '&');
        int plen = amp ? (int)(amp - p) : (int)strlen(p);

        if (strncmp(p, "timerange=", 10) != 0 && plen > 0) {
            if (len == 0) {
                if (len < out_size - 1) out[len++] = '?';
            } else {
                if (len < out_size - 1) out[len++] = '&';
            }
            int copy_len = FFMIN(plen, (int)(out_size - len - 1));
            if (copy_len > 0) {
                memcpy(out + len, p, copy_len);
                len += copy_len;
            }
        }
        p += plen;
        if (*p == '&') p++;
    }
    out[len] = '\0';
}

static int tams_build_segments_url(AVFormatContext *s, const TAMSFlow *flow,
                                   const TAMSSegmentContext *segc,
                                   char *url, size_t url_size)
{
    char base_url[2048];
    char tr_buf[128] = "";
    char clean_query[2048] = "";
    const char *query = strchr(s->url, '?');
    int ret;

    ret = tams_build_base_url(s->url, base_url, sizeof(base_url));
    if (ret < 0)
        return ret;

    if (query)
        tams_build_clean_query(query, clean_query, sizeof(clean_query));

    if (segc->nb_segments > 0) {
        TAMSFlowSegment *last = &segc->segments[segc->nb_segments - 1];
        if (last->timerange.has_end)
            snprintf(tr_buf, sizeof(tr_buf), "[%"PRId64":0_)",
                     last->timerange.end / TAMS_TIMEBASE);
    } else if (flow->timerange.has_start && flow->timerange.has_end) {
        snprintf(tr_buf, sizeof(tr_buf), "[%"PRId64":0_%"PRId64":0)",
                 flow->timerange.start / TAMS_TIMEBASE,
                 flow->timerange.end / TAMS_TIMEBASE);
    }

    if (base_url[0]) {
        if (clean_query[0] && tr_buf[0])
            snprintf(url, url_size, "%s/%s/segments%s&timerange=%s",
                     base_url, flow->id, clean_query, tr_buf);
        else if (clean_query[0])
            snprintf(url, url_size, "%s/%s/segments%s",
                     base_url, flow->id, clean_query);
        else if (tr_buf[0])
            snprintf(url, url_size, "%s/%s/segments?timerange=%s",
                     base_url, flow->id, tr_buf);
        else
            snprintf(url, url_size, "%s/%s/segments",
                     base_url, flow->id);
    } else {
        if (tr_buf[0])
            snprintf(url, url_size, "%s/segments?timerange=%s", flow->id, tr_buf);
        else
            snprintf(url, url_size, "%s/segments", flow->id);
    }

    return 0;
}

static int tams_fetch_segments(AVFormatContext *s, TAMSSegmentContext *segc)
{
    TAMSContext *c = s->priv_data;
    const TAMSFlow *flow = &c->flows[segc->flow_index];
    AVIOContext *pb = NULL;
    AVDictionary *opts = NULL;
    AVBPrint buf;
    char url[4096];
    int ret, old_nb = segc->nb_segments;

    tams_build_segments_url(s, flow, segc, url, sizeof(url));
    av_log(s, AV_LOG_VERBOSE, "TAMS fetching segments: %s\n", url);

    ret = av_dict_copy(&opts, c->avio_opts, 0);
    if (ret < 0)
        return ret;

    ret = s->io_open(s, &pb, url, AVIO_FLAG_READ, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS failed to fetch segments: %s\n",
               av_err2str(ret));
        return ret;
    }

    av_bprint_init(&buf, 0, INT_MAX);
    ret = avio_read_to_bprint(pb, &buf, SIZE_MAX);
    ff_format_io_close(s, &pb);

    if (ret < 0) {
        av_bprint_finalize(&buf, NULL);
        return ret;
    }

    av_log(s, AV_LOG_DEBUG, "TAMS segments JSON:\n%s\n", buf.str);

    {
        const char *cursor = buf.str;
        ff_tams_json_skip_ws(&cursor);
        if (*cursor == '[') {
            cursor++;
            while (1) {
                TAMSFlowSegment *tmp;
                ff_tams_json_skip_ws(&cursor);
                if (*cursor == ']')
                    break;
                tmp = av_realloc_array(segc->segments, segc->nb_segments + 1,
                                       sizeof(*segc->segments));
                if (!tmp) {
                    ret = AVERROR(ENOMEM);
                    break;
                }
                segc->segments = tmp;
                ret = ff_tams_parse_flow_segment(&cursor,
                                                &segc->segments[segc->nb_segments]);
                if (ret < 0)
                    break;
                segc->nb_segments++;
                ff_tams_json_skip_ws(&cursor);
                if (*cursor == ',')
                    cursor++;
            }
        }
    }
    av_bprint_finalize(&buf, NULL);

    av_log(s, AV_LOG_VERBOSE, "TAMS fetched %d new segments (total %d)\n",
           segc->nb_segments - old_nb, segc->nb_segments);

    return ret < 0 ? ret : 0;
}

static int tams_open_segment(AVFormatContext *s, TAMSSegmentContext *segc)
{
    TAMSContext *c = s->priv_data;
    TAMSFlowSegment *seg = &segc->segments[segc->cur_segment_index];
    AVDictionary *opts = NULL;
    int ret;

    segc->sub_ctx = avformat_alloc_context();
    if (!segc->sub_ctx)
        return AVERROR(ENOMEM);

    segc->sub_ctx->io_open    = s->io_open;
    segc->sub_ctx->io_close2  = s->io_close2;
    segc->sub_ctx->opaque     = s->opaque;
    segc->sub_ctx->flags     |= s->flags & ~AVFMT_FLAG_CUSTOM_IO;
    segc->sub_ctx->interrupt_callback = s->interrupt_callback;

    if ((ret = ff_copy_whiteblacklists(segc->sub_ctx, s)) < 0)
        goto fail;

    if (tams_same_host(s->url, seg->get_url)) {
        if ((ret = av_dict_copy(&opts, c->avio_opts, 0)) < 0)
            goto fail;
    }

    av_log(s, AV_LOG_VERBOSE, "TAMS opening segment: %s\n", seg->get_url);

    ret = avformat_open_input(&segc->sub_ctx, seg->get_url, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS failed to open segment %s: %s\n",
               seg->get_url, av_err2str(ret));
        goto fail;
    }

    ret = avformat_find_stream_info(segc->sub_ctx, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS failed to find stream info in segment\n");
        avformat_close_input(&segc->sub_ctx);
        goto fail;
    }

    /* Log stream info for the first segment of each flow */
    if (segc->cur_segment_index == 0) {
        const TAMSFlow *flow = &c->flows[segc->flow_index];
        av_log(s, AV_LOG_INFO, "TAMS first segment stream info for flow %.8s... (%s):\n",
            flow->id, flow->label[0] ? flow->label : "unlabeled");
        av_dump_format(segc->sub_ctx, 0, seg->get_url, 0);
    }

    return 0;

fail:
    if (segc->sub_ctx) {
        avformat_free_context(segc->sub_ctx);
        segc->sub_ctx = NULL;
    }
    av_dict_free(&opts);
    return ret;
}

static void tams_close_segment(TAMSSegmentContext *segc)
{
    if (segc->sub_ctx)
        avformat_close_input(&segc->sub_ctx);
}

static int tams_ensure_segments(AVFormatContext *s, TAMSSegmentContext *segc)
{
    TAMSContext *c = s->priv_data;
    int ret;

    if (segc->cur_segment_index < segc->nb_segments)
        return 0;

    if (!segc->is_live) {
        const TAMSFlow *flow = &c->flows[segc->flow_index];
        if (segc->nb_segments > 0)
            return AVERROR_EOF;
        while (1) {
            int old_nb = segc->nb_segments;
            ret = tams_fetch_segments(s, segc);
            if (ret < 0)
                return ret;
            if (segc->nb_segments == old_nb)
                break;
            if (flow->timerange.has_end && segc->nb_segments > 0) {
                TAMSFlowSegment *last = &segc->segments[segc->nb_segments - 1];
                if (last->timerange.has_end &&
                    last->timerange.end >= flow->timerange.end)
                    break;
            }
        }
        if (segc->nb_segments == 0)
            return AVERROR_EOF;
        return 0;
    }

    while (!ff_check_interrupt(&s->interrupt_callback)) {
        const TAMSFlow *flow = &c->flows[segc->flow_index];
        int old_nb = segc->nb_segments;

        ret = tams_fetch_segments(s, segc);
        if (ret < 0)
            return ret;

        if (segc->nb_segments > old_nb) {
            segc->poll_interval = tams_get_poll_init(c, flow);
            return 0;
        }

        if (tams_check_live_expired(c, flow)) {
            av_log(s, AV_LOG_VERBOSE, "TAMS live flow expired, ending\n");
            segc->is_live = 0;
            return AVERROR_EOF;
        }

        {
            int64_t wait_end = av_gettime_relative() + segc->poll_interval;
            while (av_gettime_relative() < wait_end) {
                if (ff_check_interrupt(&s->interrupt_callback))
                    return AVERROR_EXIT;
                av_usleep(FFMIN(100000, wait_end - av_gettime_relative()));
            }
        }
        segc->poll_interval = FFMIN(segc->poll_interval * 2,
                                    c->seg_poll_max);
    }

    return AVERROR_EXIT;
}

/*
 * Debug logging function to display the complete mapping of TAMS flows, streams,
 * and segment contexts. Shows how flows are mapped to AVStreams and segment contexts.
 */
static void tams_log_mapping_summary(AVFormatContext *s)
{
    TAMSContext *c = s->priv_data;
    const char *flow_type_names[] = {
        "UNKNOWN", "VIDEO", "AUDIO", "DATA", "MULTI", "IMAGE"
    };
    
    av_log(s, AV_LOG_VERBOSE, "TAMS mapping summary:\n");
    
    /* Log all flows with their details */
    av_log(s, AV_LOG_VERBOSE, "  Flows: %d\n", c->nb_flows);
    for (int i = 0; i < c->nb_flows; i++) {
        const TAMSFlow *flow = &c->flows[i];
        const char *type_name = (flow->format < FF_ARRAY_ELEMS(flow_type_names)) 
                               ? flow_type_names[flow->format] : "INVALID";
        
        av_log(s, AV_LOG_VERBOSE, "    Flow[%d]: id=%.8s..., format=%s(%d)", 
               i, flow->id, type_name, flow->format);
        
        if (flow->format == TAMS_FORMAT_VIDEO || flow->format == TAMS_FORMAT_IMAGE) {
            if (flow->frame_rate.num > 0) {
                av_log(s, AV_LOG_VERBOSE, ", frame_rate=" AVRATIONAL_FORMAT, 
                       AVRATIONAL_ARG(flow->frame_rate));
            }
            if (flow->frame_width > 0 && flow->frame_height > 0) {
                av_log(s, AV_LOG_VERBOSE, ", resolution=%dx%d", 
                       flow->frame_width, flow->frame_height);
            }
        } else if (flow->format == TAMS_FORMAT_AUDIO) {
            if (flow->sample_rate > 0) {
                av_log(s, AV_LOG_VERBOSE, ", sample_rate=%d", flow->sample_rate);
            }
            if (flow->channels > 0) {
                av_log(s, AV_LOG_VERBOSE, ", channels=%d", flow->channels);
            }
        } else if (flow->format == TAMS_FORMAT_MULTI) {
            av_log(s, AV_LOG_VERBOSE, ", sub_flows=%d", flow->nb_flow_collection);
        }
        
        if (flow->timerange.has_start || flow->timerange.has_end) {
            av_log(s, AV_LOG_VERBOSE, ", timerange=[%s%"PRId64"_%"PRId64"%s)",
                   flow->timerange.start_inclusive ? "[" : "(",
                   flow->timerange.has_start ? flow->timerange.start : 0,
                   flow->timerange.has_end ? flow->timerange.end : 0,
                   flow->timerange.end_inclusive ? "]" : ")");
        }
        
        av_log(s, AV_LOG_VERBOSE, "\n");
    }
    
    /* Log all streams with their mappings */
    av_log(s, AV_LOG_VERBOSE, "  Streams: %d\n", c->nb_streams);
    for (int i = 0; i < c->nb_streams; i++) {
        const TAMSStreamContext *sc = &c->streams[i];
        const TAMSStreamMapping *mapping = &c->stream_mappings[i];
        AVStream *st = s->streams[i];
        const TAMSFlow *flow = &c->flows[sc->flow_index];
        const char *media_type_name = av_get_media_type_string(sc->media_type);
        
        av_log(s, AV_LOG_VERBOSE, "    Stream[%d]: flow_idx=%d", i, sc->flow_index);
        
        if (mapping->parent_flow_index >= 0) {
            av_log(s, AV_LOG_VERBOSE, " (sub-flow, parent=%d)", mapping->parent_flow_index);
        }
        
        av_log(s, AV_LOG_VERBOSE, ", type=%s, seg_ctx_idx=%d", 
               media_type_name ? media_type_name : "unknown", sc->seg_ctx_index);
        
        if (sc->container_track_index >= 0) {
            av_log(s, AV_LOG_VERBOSE, ", track_idx=%d", sc->container_track_index);
        }
        
        av_log(s, AV_LOG_VERBOSE, ", time_base=" AVRATIONAL_FORMAT, 
               AVRATIONAL_ARG(st->time_base));
        
        if (flow->format == TAMS_FORMAT_VIDEO || flow->format == TAMS_FORMAT_IMAGE) {
            av_log(s, AV_LOG_VERBOSE, ", avg_frame_rate=" AVRATIONAL_FORMAT, 
                   AVRATIONAL_ARG(st->avg_frame_rate));
        }
        
        av_log(s, AV_LOG_VERBOSE, "\n");
    }
    
    /* Log segment contexts */
    av_log(s, AV_LOG_VERBOSE, "  Segment contexts: %d\n", c->nb_seg_ctxs);
    for (int i = 0; i < c->nb_seg_ctxs; i++) {
        const TAMSSegmentContext *segc = &c->seg_ctxs[i];
        const TAMSFlow *flow = &c->flows[segc->flow_index];
        
        av_log(s, AV_LOG_VERBOSE, "    SegCtx[%d]: flow_idx=%d (%.8s...), refcount=%d",
               i, segc->flow_index, flow->id, segc->refcount);
        
        if (segc->is_live) {
            av_log(s, AV_LOG_VERBOSE, ", live=yes, poll=%"PRId64"us", segc->poll_interval);
        }
        
        av_log(s, AV_LOG_VERBOSE, ", segments=%d", segc->nb_segments);
        
        av_log(s, AV_LOG_VERBOSE, "\n");
    }
    
    /* Show stream to segment context relationships */
    av_log(s, AV_LOG_VERBOSE, "  Stream->SegCtx relationships:\n");
    for (int i = 0; i < c->nb_seg_ctxs; i++) {
        av_log(s, AV_LOG_VERBOSE, "    SegCtx[%d] serves streams: ", i);
        int first = 1;
        for (int j = 0; j < c->nb_streams; j++) {
            if (c->streams[j].seg_ctx_index == i) {
                if (!first) av_log(s, AV_LOG_VERBOSE, ", ");
                av_log(s, AV_LOG_VERBOSE, "%d", j);
                first = 0;
            }
        }
        av_log(s, AV_LOG_VERBOSE, "\n");
    }
}

/*
 * Resolve which stream within a segment's container corresponds to a TAMS stream.
 * Uses container track mapping if available, otherwise falls back to media type
 * matching to find the appropriate sub-stream index within the segment.
 */
static int tams_resolve_sub_stream(AVFormatContext *s, TAMSStreamContext *sc)
{
    TAMSContext *c = s->priv_data;
    TAMSSegmentContext *segc = &c->seg_ctxs[sc->seg_ctx_index];
    AVFormatContext *sub = segc->sub_ctx;

    /* First try explicit container track mapping if specified */
    if (sc->container_track_index >= 0) {
        if (sc->container_track_index < (int)sub->nb_streams) {
            sc->sub_stream_index = sc->container_track_index;
            av_log(s, AV_LOG_DEBUG, "TAMS resolved stream via track_index %d\n", 
                   sc->container_track_index);
            return 0;
        } else {
            av_log(s, AV_LOG_WARNING, 
                   "TAMS container track_index %d out of range (sub has %u streams)\n",
                   sc->container_track_index, sub->nb_streams);
        }
    }

    /* Fall back to matching by media type */
    for (unsigned i = 0; i < sub->nb_streams; i++) {
        if (sub->streams[i]->codecpar->codec_type == sc->media_type) {
            sc->sub_stream_index = i;
            av_log(s, AV_LOG_DEBUG, "TAMS resolved stream via media type match to sub-stream %u\n", i);
            return 0;
        }
    }

    /* No match found */
    sc->sub_stream_index = -1;
    av_log(s, AV_LOG_WARNING, "TAMS could not resolve sub-stream for media type %d\n", sc->media_type);
    return 0;
}

static void tams_copy_extradata(AVFormatContext *s, TAMSStreamContext *sc, int tams_idx)
{
    TAMSContext *c = s->priv_data;
    TAMSSegmentContext *segc = &c->seg_ctxs[sc->seg_ctx_index];
    AVStream *parent_st = s->streams[tams_idx];
    AVStream *sub_st;

    if (sc->extradata_copied || sc->sub_stream_index < 0)
        return;

    sub_st = segc->sub_ctx->streams[sc->sub_stream_index];
    if (sub_st->codecpar->extradata && sub_st->codecpar->extradata_size > 0 &&
        !parent_st->codecpar->extradata) {
        avcodec_parameters_copy(parent_st->codecpar, sub_st->codecpar);
        avpriv_set_pts_info(parent_st, 64, 1, TAMS_TIMEBASE);
    }

    sc->extradata_copied = 1;
}

/*
 * Find the TAMS stream index that matches a packet from a segment's sub-demuxer.
 * This resolves packets from segment containers back to the original TAMS stream
 * by matching both the segment context and sub-stream index.
 */
static int tams_find_stream_for_sub_packet(TAMSContext *c, int seg_ctx_index,
                                            int sub_stream_index)
{
    for (int i = 0; i < c->nb_streams; i++) {
        TAMSStreamContext *sc = &c->streams[i];
        if (sc->seg_ctx_index == seg_ctx_index &&
            sc->sub_stream_index == sub_stream_index && !sc->eof)
            return i;
    }
    return -1;
}

static int tams_read_header(AVFormatContext *s)
{
    TAMSContext *c = s->priv_data;
    AVBPrint buf;
    int ret;

    /* Read full JSON response from the IO context */
    av_bprint_init(&buf, 0, INT_MAX);
    ret = avio_read_to_bprint(s->pb, &buf, SIZE_MAX);
    if (ret < 0 || !avio_feof(s->pb)) {
        av_log(s, AV_LOG_ERROR, "Failed to read TAMS response\n");
        if (ret == 0)
            ret = AVERROR_INVALIDDATA;
        av_bprint_finalize(&buf, NULL);
        return ret;
    }

    av_log(s, AV_LOG_DEBUG, "TAMS JSON response:\n%s\n", buf.str);

    /* Parse flow(s) from JSON */
    ret = tams_parse_flows_json(c, buf.str);
    av_bprint_finalize(&buf, NULL);
    if (ret < 0)
        return ret;

    if ((ret = ffio_copy_url_options(s->pb, &c->avio_opts)) < 0)
        return ret;

    if (c->nb_flows == 0) {
        av_log(s, AV_LOG_ERROR, "No flows found in TAMS response\n");
        return AVERROR_INVALIDDATA;
    }

    {
        const char *query = strchr(s->url, '?');
        const char *tr_str = query ? strstr(query, "timerange=") : NULL;
        TAMSTimeRange url_tr = {0};
        int has_url_tr = 0;

        if (tr_str) {
            tr_str += 10;
            char tr_val[128];
            const char *end = strchr(tr_str, '&');
            int tlen = end ? (int)(end - tr_str) : (int)strlen(tr_str);
            if (tlen > 0 && tlen < (int)sizeof(tr_val)) {
                memcpy(tr_val, tr_str, tlen);
                tr_val[tlen] = '\0';
                if (ff_tams_parse_timerange(tr_val, &url_tr) >= 0)
                    has_url_tr = 1;
            }
        }

        if (has_url_tr) {
            for (int i = 0; i < c->nb_flows; i++) {
                if (!c->flows[i].timerange.has_start &&
                    !c->flows[i].timerange.has_end)
                    c->flows[i].timerange = url_tr;
            }
        }
    }

    // Process each flow to create streams and segment contexts
    {
        int nb_initial_flows = c->nb_flows;
        for (int i = 0; i < nb_initial_flows; i++) {

            ret = tams_process_flow(s, i);
            if (ret < 0)
                return ret;
        }
    }

    if (c->nb_streams == 0) {
        av_log(s, AV_LOG_ERROR, "No streams created from TAMS flows\n");
        return AVERROR_INVALIDDATA;
    }

    {
        int64_t max_dur = 0;
        for (int i = 0; i < s->nb_streams; i++) {
            if (s->streams[i]->duration > max_dur)
                max_dur = s->streams[i]->duration;
        }
        if (max_dur > 0)
            s->duration = av_rescale(max_dur, AV_TIME_BASE, TAMS_TIMEBASE);
    }

    c->streams = av_calloc(c->nb_streams, sizeof(*c->streams));
    if (!c->streams)
        return AVERROR(ENOMEM);

    for (int i = 0; i < c->nb_streams; i++) {
        TAMSStreamContext *sc = &c->streams[i];
        TAMSSegmentContext *segc = NULL;
        const TAMSFlow *seg_flow;

        /* Determine which flow owns this stream's segments.
         * Container-mapped sub-flows have parent_flow_index set to the
         * multi-flow parent (shared segments). Independent sub-flows and
         * non-multi flows have parent_flow_index == -1 (own segments). */
        int seg_flow_idx = c->stream_mappings[i].parent_flow_index >= 0
                         ? c->stream_mappings[i].parent_flow_index
                         : c->stream_mappings[i].flow_index;

        sc->flow_index = c->stream_mappings[i].flow_index;
        sc->current_ts = INT64_MIN;
        sc->sub_stream_index = -1;
        sc->container_track_index = c->stream_mappings[i].container_track_index;

        switch (c->flows[sc->flow_index].format) {
        case TAMS_FORMAT_VIDEO:
        case TAMS_FORMAT_IMAGE:
            sc->media_type = AVMEDIA_TYPE_VIDEO;
            break;
        case TAMS_FORMAT_AUDIO:
            sc->media_type = AVMEDIA_TYPE_AUDIO;
            break;
        case TAMS_FORMAT_DATA:
            sc->media_type = s->streams[i]->codecpar->codec_type;
            break;
        default:
            sc->media_type = AVMEDIA_TYPE_UNKNOWN;
            break;
        }

        sc->seg_ctx_index = -1;
        for (int j = 0; j < c->nb_seg_ctxs; j++) {
            if (c->seg_ctxs[j].flow_index == seg_flow_idx) {
                sc->seg_ctx_index = j;
                break;
            }
        }

        if (sc->seg_ctx_index < 0) {
            TAMSSegmentContext *tmp = av_realloc_array(c->seg_ctxs,
                c->nb_seg_ctxs + 1, sizeof(*c->seg_ctxs));
            if (!tmp)
                return AVERROR(ENOMEM);
            c->seg_ctxs = tmp;
            segc = &c->seg_ctxs[c->nb_seg_ctxs];
            memset(segc, 0, sizeof(*segc));
            segc->flow_index = seg_flow_idx;
            seg_flow = &c->flows[seg_flow_idx];
            segc->is_live = tams_check_live(c, seg_flow);
            if (segc->is_live) {
                segc->poll_interval = tams_get_poll_init(c, seg_flow);
                av_log(s, AV_LOG_VERBOSE,
                       "TAMS segment context %d (flow %s) detected as live "
                       "(poll=%"PRId64"us)\n",
                       c->nb_seg_ctxs, seg_flow->id, segc->poll_interval);
            }
            sc->seg_ctx_index = c->nb_seg_ctxs;
            c->nb_seg_ctxs++;
        }

        c->seg_ctxs[sc->seg_ctx_index].refcount++;
    }

    tams_log_mapping_summary(s);

    return 0;
}

static int tams_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    TAMSContext *c = s->priv_data;

retry:
    {
        TAMSStreamContext *best = NULL;

        for (int i = 0; i < c->nb_streams; i++) {
            TAMSStreamContext *sc = &c->streams[i];
            if (sc->eof)
                continue;
            if (!best || sc->current_ts < best->current_ts)
                best = sc;
        }

        if (!best)
            return AVERROR_EOF;

        while (1) {
            TAMSSegmentContext *segc = &c->seg_ctxs[best->seg_ctx_index];
            TAMSFlowSegment *seg;
            AVStream *sub_st;
            const TAMSFlow *flow;
            int64_t pts_ns, dts_ns, dur_ns;
            int ret, tams_idx;

            if (!segc->sub_ctx) {
                ret = tams_ensure_segments(s, segc);
                if (ret < 0) {
                    if (ret == AVERROR_EOF) {
                        for (int i = 0; i < c->nb_streams; i++) {
                            if (c->streams[i].seg_ctx_index == best->seg_ctx_index)
                                c->streams[i].eof = 1;
                        }
                        goto retry;
                    }
                    return ret;
                }
                ret = tams_open_segment(s, segc);
                if (ret < 0)
                    return ret;
                for (int i = 0; i < c->nb_streams; i++) {
                    if (c->streams[i].seg_ctx_index == best->seg_ctx_index) {
                        tams_resolve_sub_stream(s, &c->streams[i]);
                        tams_copy_extradata(s, &c->streams[i], i);
                    }
                }
            }

            seg = &segc->segments[segc->cur_segment_index];

            ret = av_read_frame(segc->sub_ctx, pkt);
            if (ret == AVERROR_EOF) {
                tams_close_segment(segc);
                segc->cur_segment_index++;
                continue;
            }
            if (ret < 0)
                return ret;

            av_log(s, AV_LOG_TRACE, "Got segment packet: pts=%" PRId64 ", dts=%" PRId64
                    ", duration=%" PRId64 ", stream_index=%d, time_base=" AVRATIONAL_FORMAT "\n", 
                    pkt->pts, pkt->dts, pkt->duration, pkt->stream_index, AVRATIONAL_ARG(pkt->time_base));

            tams_idx = tams_find_stream_for_sub_packet(c, best->seg_ctx_index,
                                                         pkt->stream_index);
            if (tams_idx < 0) {
                av_packet_unref(pkt);
                continue;
            }

            {
                TAMSStreamContext *target = &c->streams[tams_idx];
                sub_st = segc->sub_ctx->streams[pkt->stream_index];

                AVStream *st = s->streams[tams_idx];
                
                /* Convert timestamps from segment timebase to TAMS stream timebase */
                pts_ns = (pkt->pts != AV_NOPTS_VALUE)
                    ? av_rescale_q(pkt->pts, sub_st->time_base, st->time_base)
                    : AV_NOPTS_VALUE;
                dts_ns = (pkt->dts != AV_NOPTS_VALUE)
                    ? av_rescale_q(pkt->dts, sub_st->time_base, st->time_base)
                    : AV_NOPTS_VALUE;
                dur_ns = av_rescale_q(pkt->duration, sub_st->time_base, st->time_base);
                
                /* Convert to TAMS nanosecond timebase for internal processing */
                if (pts_ns != AV_NOPTS_VALUE)
                    pts_ns = av_rescale_q(pts_ns, st->time_base, (AVRational){1, TAMS_TIMEBASE});
                if (dts_ns != AV_NOPTS_VALUE) 
                    dts_ns = av_rescale_q(dts_ns, st->time_base, (AVRational){1, TAMS_TIMEBASE});
                dur_ns = av_rescale_q(dur_ns, st->time_base, (AVRational){1, TAMS_TIMEBASE});
                
                flow = &c->flows[target->flow_index];

                if (seg->has_ts_offset) {
                    if (pts_ns != AV_NOPTS_VALUE)
                        pts_ns += seg->ts_offset;
                    if (dts_ns != AV_NOPTS_VALUE)
                        dts_ns += seg->ts_offset;
                }
                if (flow->timerange.has_start &&
                    pts_ns != AV_NOPTS_VALUE &&
                    pts_ns < flow->timerange.start) {
                    av_packet_unref(pkt);
                    continue;
                }
                if (flow->timerange.has_end &&
                    pts_ns != AV_NOPTS_VALUE &&
                    pts_ns >= flow->timerange.end) {
                    av_packet_unref(pkt);
                    target->eof = 1;
                    goto retry;
                }

                pkt->pts      = pts_ns;
                pkt->dts      = dts_ns;
                pkt->duration = dur_ns;
                pkt->stream_index = tams_idx;

                if (pts_ns != AV_NOPTS_VALUE)
                    target->current_ts = pts_ns;
                else if (dts_ns != AV_NOPTS_VALUE)
                    target->current_ts = dts_ns;

                av_log(s, AV_LOG_TRACE, "Converted to flow packet: pts=%" PRId64 ", dts=%" PRId64
                    ", duration=%" PRId64 ", stream_index=%d, time_base=" AVRATIONAL_FORMAT "\n", 
                    pkt->pts, pkt->dts, pkt->duration, pkt->stream_index, AVRATIONAL_ARG(pkt->time_base));

                return 0;
            }
        }
    }
}

static int tams_close(AVFormatContext *s)
{
    TAMSContext *c = s->priv_data;

    if (c->seg_ctxs) {
        for (int i = 0; i < c->nb_seg_ctxs; i++) {
            TAMSSegmentContext *segc = &c->seg_ctxs[i];
            tams_close_segment(segc);
            av_freep(&segc->segments);
        }
    }
    av_freep(&c->seg_ctxs);
    av_freep(&c->streams);
    av_freep(&c->stream_mappings);
    av_dict_free(&c->avio_opts);
    av_freep(&c->flows);

    return 0;
}

static int tams_probe(const AVProbeData *p)
{
    static const char *const tams_tags[] = {
        "\"source_id\"",
        "\"essence_parameters\"",
        "\"timerange\"",
    };
    const char *buf = (const char *)p->buf;
    unsigned i, count = 0;

    /* Skip leading whitespace */
    ff_tams_json_skip_ws(&buf);

    /* TAMS /flows returns a JSON array, /flows/<id> returns a JSON object */
    if (*buf != '[' && *buf != '{')
        return 0;

    /* Look for TAMS-specific NMOS format URNs - definitive match */
    for (i = 0; i < FF_ARRAY_ELEMS(tams_format_urns); i++) {
        if (strstr((const char *)p->buf, tams_format_urns[i]))
            return AVPROBE_SCORE_MAX;
    }

    /* Fall back to checking for TAMS-specific JSON keys */
    for (i = 0; i < FF_ARRAY_ELEMS(tams_tags); i++) {
        const char *t = strstr((const char *)p->buf, tams_tags[i]);
        if (!t)
            continue;
        t += strlen(tams_tags[i]);
        t += strspn(t, " \t\r\n");
        if (*t == ':')
            count++;
    }

    if (count == FF_ARRAY_ELEMS(tams_tags))
        return AVPROBE_SCORE_EXTENSION;

    return 0;
}

static int tams_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    return AVERROR_EOF;
}

#define OFFSET(x) offsetof(TAMSContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM

static const AVOption tams_options[] = {
    { "live_threshold", "segments_updated threshold for live status in seconds (-1=auto: 2 x segment_duration)",
        OFFSET(live_threshold), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
    { "live_timeout", "segments_updated threshold for reverting to non-live status in seconds (-1=auto: 4 x segment_duration)",
        OFFSET(live_timeout), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
    { "seg_poll_init", "Initial segment poll interval in microseconds (-1=auto: segment_duration)",
        OFFSET(seg_poll_init), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
    { "seg_poll_max", "Max segment poll interval in microseconds",
        OFFSET(seg_poll_max), AV_OPT_TYPE_INT64, {.i64 = 30000000}, 0, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass tams_class = {
    .class_name = "tams",
    .item_name  = av_default_item_name,
    .option     = tams_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_tams_demuxer = {
    .p.name         = "tams",
    .p.long_name    = NULL_IF_CONFIG_SMALL("TAMS (Time-Addressable Media Store)"),
    .p.flags        = AVFMT_NO_BYTE_SEEK,
    .p.priv_class   = &tams_class,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .priv_data_size = sizeof(TAMSContext),
    .read_probe     = tams_probe,
    .read_header    = tams_read_header,
    .read_packet    = tams_read_packet,
    .read_close     = tams_close,
    .read_seek      = tams_seek
};
