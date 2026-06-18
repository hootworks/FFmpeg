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

/*
 * Return codes from tams_restamp_packet(), used by tams_read_packet() to
 * decide what to do with each packet after timestamp conversion.
 */
#define TAMS_PKT_OK      0  /* packet PTS is within the required segment timerange    */
#define TAMS_PKT_DISCARD 1  /* packet PTS is outside the required segment timerange,
                               DTS is before the segment end,
                               it might be required for decode, mark discard         */
#define TAMS_PKT_EOF     2  /* packet PTS is outside the required segment timerange,
                               and DTS is at or past the segment end, mark EOF       */

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
    { "audio/mp2",       AV_CODEC_ID_MP2 },
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
    TAMSFlowSegment *flow_segments;
    int nb_flow_segments;
    int cur_flow_segment_index;
    AVFormatContext *sub_ctx;
    int is_live;
    int64_t poll_interval;
    int refcount;
    /* Pagination cursor: exclusive lower bound (nanoseconds) for the next
     * segments fetch.  Stored separately from the segments array so it
     * survives tams_compact_segments() without keeping old entries alive. */
    int     has_fetch_cursor;
    int64_t fetch_cursor_ns;
} TAMSSegmentContext;

typedef struct TAMSStreamContext {
    int flow_index;
    /* -1 unless flow is part of a multi-flow container */
    int parent_flow_index;
    TAMSContainerMapping container_mapping;
    int has_container_mapping;

    int seg_ctx_index;
    int sub_stream_index;
    enum AVMediaType media_type;
    int64_t current_ts;
    int eof;
    int extradata_copied;
} TAMSStreamContext;

typedef struct TAMSContext {
    const AVClass *class;

    TAMSFlow *flows;
    int nb_flows;

    TAMSSegmentContext *seg_ctxs;
    int nb_seg_ctxs;

    AVDictionary *avio_opts;

    int64_t live_threshold;
    int64_t live_timeout;
    int64_t seg_poll_init;
    int64_t seg_poll_max;
    int64_t min_segment_buffer;
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

    seg_updated = ff_tams_parse_iso8601(flow->segments_updated);
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

    seg_updated = ff_tams_parse_iso8601(flow->segments_updated);
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

/* Target nanoseconds of segments to pre-buffer ahead of the current position.
 * Defaults to 3 x segment_duration when min_segment_buffer is not set. */
static int64_t tams_get_min_buffer_ns(TAMSContext *c, const TAMSFlow *flow)
{
    if (c->min_segment_buffer >= 0)
        return c->min_segment_buffer * 1000000000LL;
    return 3LL * tams_segment_duration_us(flow) * 1000LL;
}

/*
 * Compute total nanoseconds of segment time buffered ahead of
 * cur_flow_segment_index.
 */
static int64_t tams_buffered_ns(TAMSContext *c, const TAMSSegmentContext *segc)
{
    int64_t total = 0;

    for (int i = segc->cur_flow_segment_index; i < segc->nb_flow_segments; i++) {
        const TAMSFlowSegment *seg = &segc->flow_segments[i];
        total += seg->timerange.end - seg->timerange.start;
    }
    return total;
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

        av_log(s, AV_LOG_VERBOSE, "    Flow[%d]: id=%s, format=%s(%d)",
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
            av_log(s, AV_LOG_VERBOSE, ", sub_flows=%d", flow->nb_flow_collection_items);
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
    av_log(s, AV_LOG_VERBOSE, "  Stream contexts: %d\n", s->nb_streams);
    for (int i = 0; i < s->nb_streams; i++) {
        const TAMSStreamContext *sc = s->streams[i]->priv_data;
        AVStream *st = s->streams[i];
        const TAMSFlow *flow = &c->flows[sc->flow_index];
        const char *media_type_name = av_get_media_type_string(sc->media_type);

        av_log(s, AV_LOG_VERBOSE, "    Stream[%d]: flow_index=%d", i, sc->flow_index);

        if (sc->parent_flow_index >= 0) {
            av_log(s, AV_LOG_VERBOSE, " (sub-flow, parent_flow_index=%d)",
                   sc->parent_flow_index);
        }

        av_log(s, AV_LOG_VERBOSE, ", type=%s, seg_ctx_index=%d",
               media_type_name ? media_type_name : "unknown", sc->seg_ctx_index);

        if (sc->has_container_mapping) {
            const TAMSContainerMapping *m = &sc->container_mapping;
            if (m->has_track_index)
                av_log(s, AV_LOG_VERBOSE, ", track_index=%d", m->track_index);
            if (m->has_format_track_index)
                av_log(s, AV_LOG_VERBOSE, ", format_track_index=%d", m->format_track_index);
            if (m->has_mp2ts_pid)
                av_log(s, AV_LOG_VERBOSE, ", mp2ts_pid=%d", m->mp2ts_pid);
            if (m->has_isobmff_track_id)
                av_log(s, AV_LOG_VERBOSE, ", isobmff_track_id=%d", m->isobmff_track_id);
            if (m->has_mxf_track_id)
                av_log(s, AV_LOG_VERBOSE, ", mxf_track_id=%d", m->mxf_track_id);
            if (m->mxf_package_uid[0])
                av_log(s, AV_LOG_VERBOSE, ", mxf_package_uid=%s", m->mxf_package_uid);
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

        av_log(s, AV_LOG_VERBOSE, "    TAMSSegmentContext[%d]: flow_index=%d (%s), refcount=%d",
               i, segc->flow_index, flow->id, segc->refcount);

        if (segc->is_live) {
            av_log(s, AV_LOG_VERBOSE, ", live=yes, poll=%"PRId64"us", segc->poll_interval);
        }

        av_log(s, AV_LOG_VERBOSE, "\n");
    }

    /* Show stream to TAMSSegmentContext relationships */
    av_log(s, AV_LOG_VERBOSE, "  Stream->TAMSSegmentContext relationships:\n");
    for (int i = 0; i < c->nb_seg_ctxs; i++) {
        av_log(s, AV_LOG_VERBOSE, "    TAMSSegmentContext[%d] serves streams: ", i);
        int first = 1;
        for (int j = 0; j < s->nb_streams; j++) {
            const TAMSStreamContext *scj = s->streams[j]->priv_data;
            if (scj->seg_ctx_index == i) {
                if (!first)
                    av_log(s, AV_LOG_VERBOSE, ", ");
                av_log(s, AV_LOG_VERBOSE, "%d", j);
                first = 0;
            }
        }
        av_log(s, AV_LOG_VERBOSE, "\n");
    }
}

/*
 * Validate the codec parameters discovered by avformat_find_stream_info()
 * against the authoritative values declared in the flow JSON.
 *
 * The TAMS spec treats flow metadata as ground truth: if the segment disagrees
 * on dimensions, sample rate, or channel count the data is considered corrupt.
 * Frame rate is only checked when both sides report a non-zero value, because
 * some container formats do not carry it reliably.
 *
 * Must be called after tams_resolve_sub_stream() so that sub_stream_index is set.
 */
static int tams_validate_segment_stream(AVFormatContext *s,
                                        const TAMSStreamContext *sc,
                                        int tams_index)
{
    TAMSContext *c = s->priv_data;
    TAMSSegmentContext *segc = &c->seg_ctxs[sc->seg_ctx_index];
    const TAMSFlow *flow = &c->flows[sc->flow_index];
    AVCodecParameters *par;

    if (sc->sub_stream_index < 0)
        return 0;

    par = segc->sub_ctx->streams[sc->sub_stream_index]->codecpar;

    if (flow->codec[0]) {
        enum AVCodecID expected = tams_codec_lookup(flow->codec);
        if (expected != AV_CODEC_ID_NONE && par->codec_id != AV_CODEC_ID_NONE &&
            par->codec_id != expected) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS flow %.8s: segment codec %s does not match flow codec %s\n",
                   flow->id, avcodec_get_name(par->codec_id), flow->codec);
            return AVERROR_INVALIDDATA;
        }
    }

    if (flow->format == TAMS_FORMAT_VIDEO || flow->format == TAMS_FORMAT_IMAGE) {
        if (flow->frame_width > 0 && par->width != flow->frame_width) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS flow %.8s: segment width %d != flow width %d\n",
                   flow->id, par->width, flow->frame_width);
            return AVERROR_INVALIDDATA;
        }
        if (flow->frame_height > 0 && par->height != flow->frame_height) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS flow %.8s: segment height %d != flow height %d\n",
                   flow->id, par->height, flow->frame_height);
            return AVERROR_INVALIDDATA;
        }
        /* Only validate frame rate when the flow declares one and the segment
         * container reports a usable value; some formats omit it entirely. */
        if (flow->frame_rate.num > 0 && flow->frame_rate.den > 0) {
            AVStream *sub_st = segc->sub_ctx->streams[sc->sub_stream_index];
            AVRational seg_rate = sub_st->avg_frame_rate.num > 0
                                  ? sub_st->avg_frame_rate
                                  : sub_st->r_frame_rate;
            if (seg_rate.num > 0 && seg_rate.den > 0) {
                /* Cross-multiply to compare rationals without floating point. */
                if ((int64_t)flow->frame_rate.num * seg_rate.den !=
                    (int64_t)seg_rate.num * flow->frame_rate.den) {
                    av_log(s, AV_LOG_ERROR,
                           "TAMS flow %.8s: segment frame rate %d/%d != "
                           "flow frame rate %d/%d\n",
                           flow->id,
                           seg_rate.num, seg_rate.den,
                           flow->frame_rate.num, flow->frame_rate.den);
                    return AVERROR_INVALIDDATA;
                }
            }
        }

        return 0;
    }

    if (flow->format == TAMS_FORMAT_AUDIO) {
        if (flow->sample_rate > 0 && par->sample_rate != flow->sample_rate) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS flow %.8s: segment sample rate %d != flow sample rate %d\n",
                   flow->id, par->sample_rate, flow->sample_rate);
            return AVERROR_INVALIDDATA;
        }
        if (flow->channels > 0 &&
            par->ch_layout.nb_channels != flow->channels) {
            av_log(s, AV_LOG_ERROR,
                   "TAMS flow %.8s: segment channel count %d != flow channel count %d\n",
                   flow->id, par->ch_layout.nb_channels, flow->channels);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
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

    switch (flow->video_unc_type) {
    case TAMS_VIDEO_UNC_YUYV:  st->codecpar->format = AV_PIX_FMT_YUYV422;      break;
    case TAMS_VIDEO_UNC_UYVY:  st->codecpar->format = AV_PIX_FMT_UYVY422;      break;
    case TAMS_VIDEO_UNC_AYUV:  st->codecpar->format = AV_PIX_FMT_YUVA444P;     break;
    case TAMS_VIDEO_UNC_V210:  st->codecpar->format = AV_PIX_FMT_YUV422P10;    break;
    case TAMS_VIDEO_UNC_V216:  st->codecpar->format = AV_PIX_FMT_Y216;         break;
    case TAMS_VIDEO_UNC_RGB:   st->codecpar->format = AV_PIX_FMT_RGB24;        break;
    case TAMS_VIDEO_UNC_RGBX:  st->codecpar->format = AV_PIX_FMT_RGB0;         break;
    case TAMS_VIDEO_UNC_XRGB:  st->codecpar->format = AV_PIX_FMT_0RGB;         break;
    case TAMS_VIDEO_UNC_BGRX:  st->codecpar->format = AV_PIX_FMT_BGR0;         break;
    case TAMS_VIDEO_UNC_XBGR:  st->codecpar->format = AV_PIX_FMT_0BGR;         break;
    case TAMS_VIDEO_UNC_RGBA:  st->codecpar->format = AV_PIX_FMT_RGBA;         break;
    case TAMS_VIDEO_UNC_ARGB:  st->codecpar->format = AV_PIX_FMT_ARGB;         break;
    case TAMS_VIDEO_UNC_BGRA:  st->codecpar->format = AV_PIX_FMT_BGRA;         break;
    case TAMS_VIDEO_UNC_ABGR:  st->codecpar->format = AV_PIX_FMT_ABGR;         break;
    case TAMS_VIDEO_UNC_ALPHA: st->codecpar->format = AV_PIX_FMT_GRAY8;        break;
    default:
        /* TAMS_VIDEO_UNC_UNKNOWN or TAMS_VIDEO_UNC_PLANAR: use chroma-subs logic */
        if (flow->component_type == TAMS_COMPONENT_RGB) {
            st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_GBRP10 : AV_PIX_FMT_GBRP;
        } else if (flow->horiz_chroma_subs > 0 && flow->vert_chroma_subs > 0) {
            if (flow->horiz_chroma_subs == 2 && flow->vert_chroma_subs == 2)
                st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
            else if (flow->horiz_chroma_subs == 2 && flow->vert_chroma_subs == 1)
                st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_YUV422P10 : AV_PIX_FMT_YUV422P;
            else if (flow->horiz_chroma_subs == 1 && flow->vert_chroma_subs == 1)
                st->codecpar->format = flow->bit_depth > 8 ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P;
        }
        break;
    }

    if (flow->has_avc_parameters &&
        st->codecpar->codec_id == AV_CODEC_ID_H264) {
        st->codecpar->profile = flow->avc_parameters.profile;
        st->codecpar->level   = flow->avc_parameters.level;
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

static int tams_create_stream(AVFormatContext *s, const TAMSFlow *flow,
                              int flow_index, int parent_flow_index,
                              const TAMSFlowCollectionItem *collection_item)
{
    TAMSStreamContext *sc;
    AVStream *st;
    int has_cm;
    int ret;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    sc = av_mallocz(sizeof(*sc));
    if (!sc)
        return AVERROR(ENOMEM);
    st->priv_data = sc;

    has_cm = collection_item && collection_item->has_container_mapping;
    sc->flow_index          = flow_index;
    sc->parent_flow_index   = parent_flow_index;
    sc->has_container_mapping = has_cm;
    if (has_cm)
        sc->container_mapping = collection_item->container_mapping;

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

    return 0;
}

/*
 * Extract the base URL up to (and including) the "/flows" path component from
 * a TAMS input URL.  Both the /flows?source_id=... and /flows/<id> forms are
 * supported.  The result is written to base_url and is used to construct
 * sub-flow and segment fetch URLs.
 */
static int tams_build_base_url(const char *full_url, char *base_url, size_t base_size)
{
    const char *query = strchr(full_url, '?');
    int path_len = query ? (int)(query - full_url) : (int)strlen(full_url);
    const char *last_slash = NULL;

    /* If the path ends with "/flows", keep it so segments URLs are correct
     * for both /flows/<id> and /flows?source_id=... input forms. */
    if (path_len >= 6 && strncmp(full_url + path_len - 6, "/flows", 6) == 0) {
        if (path_len >= (int)base_size)
            return AVERROR(ENAMETOOLONG);
        memcpy(base_url, full_url, path_len);
        base_url[path_len] = '\0';
        return 0;
    }

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

/*
 * Strip timerange= and source_id= from the original URL query string; these
 * are not forwarded to the segments endpoint (timerange is rebuilt below).
 */
static void tams_build_clean_query(const char *query, char *out, size_t out_size)
{
    const char *p = query + 1;
    size_t len = 0;

    out[0] = '\0';
    if (out_size < 2) return;

    while (*p && len < out_size - 1) {
        const char *amp = strchr(p, '&');
        int plen = amp ? (int)(amp - p) : (int)strlen(p);

        if (strncmp(p, "timerange=", 10) != 0 && strncmp(p, "source_id=", 10) != 0 && plen > 0) {
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

/*
 * Build the URL for a segments request, embedding a timerange query parameter
 * to implement pagination.
 *
 * The TAMS segments endpoint (GET /flows/{flowId}/segments) supports server-
 * driven pagination via the Link, X-Paging-Limit, and X-Paging-NextKey
 * response headers.  However, FFmpeg's AVIOContext API does not expose HTTP
 * response headers to the demuxer layer, so those headers are unavailable here.
 *
 * Instead, pagination is driven by an explicit fetch cursor stored in
 * segc->fetch_cursor_ns (the exclusive lower bound for the next page).  The
 * cursor is set by tams_fetch_segments() after each successful fetch and
 * survives tams_compact_segments() without keeping old segment entries alive.
 * Full nanosecond precision is preserved to avoid placing the lower bound
 * inside the final segment of the previous page.
 *
 * For the very first request (no cursor yet), if the flow declares a bounded
 * timerange that window is forwarded so the server can skip segments outside
 * it entirely.
 */
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

    /* Use the explicit fetch cursor (set after each successful fetch) as the
     * pagination start point, so the cursor survives segment array compaction.
     * Fall back to the flow's declared timerange for the very first request. */
    if (segc->has_fetch_cursor) {
        int64_t cur_sec = segc->fetch_cursor_ns / TAMS_TIMEBASE;
        int64_t cur_ns  = segc->fetch_cursor_ns % TAMS_TIMEBASE;
        snprintf(tr_buf, sizeof(tr_buf), "[%"PRId64":%"PRId64"_)", cur_sec, cur_ns);
    } else if (flow->timerange.has_start && flow->timerange.has_end) {
        int64_t start_sec = flow->timerange.start / TAMS_TIMEBASE;
        int64_t start_ns  = flow->timerange.start % TAMS_TIMEBASE;
        int64_t end_sec   = flow->timerange.end   / TAMS_TIMEBASE;
        int64_t end_ns    = flow->timerange.end   % TAMS_TIMEBASE;
        snprintf(tr_buf, sizeof(tr_buf), "[%"PRId64":%"PRId64"_%"PRId64":%"PRId64")",
                 start_sec, start_ns, end_sec, end_ns);
    }

    /* Combine base URL, flow id, any passthrough query params, and the
     * timerange filter into the final segments URL. */
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

/*
 * Fetch and parse a single TAMS Flow object by its UUID from the server.
 * Used when a multi-flow's flow_collection references a sub-flow that was
 * not included in the initial /flows response.  Appends the parsed flow to
 * c->flows and returns its index, or a negative AVERROR on failure.
 */
static int tams_fetch_sub_flow(AVFormatContext *s, const char *flow_id)
{
    TAMSContext *c = s->priv_data;
    AVIOContext *pb = NULL;
    AVDictionary *opts = NULL;
    AVBPrint buf;
    TAMSFlow *tmp;
    char url[4096];
    char base_url[2048];
    int ret;

    ret = tams_build_base_url(s->url, base_url, sizeof(base_url));
    if (ret < 0)
        return ret;

    if (base_url[0])
        snprintf(url, sizeof(url), "%s/%s", base_url, flow_id);
    else
        snprintf(url, sizeof(url), "%s", flow_id);

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
 * Process a specified flow.
 * 
 * For multi-flow each collected sub-flow is processed. Two cases exist and may be
 * mixed within a single multi-flow:
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
           flow->id, flow->nb_flow_collection_items);

    for (int i = 0; i < flow->nb_flow_collection_items; i++) {
        const TAMSFlowCollectionItem *item = &flow->flow_collection_items[i];
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

/*
 * Fetch one page of segments for a segment context.  The timerange parameter
 * embedded in the URL by tams_build_segments_url() acts as the pagination
 * cursor (see that function's comment for why server-side pagination headers
 * cannot be used).  Each call appends the newly received segments to
 * segc->segments and updates segc->fetch_cursor_ns; the caller compares
 * nb_segments before and after to detect whether another page exists.
 */
static int tams_fetch_segments(AVFormatContext *s, TAMSSegmentContext *segc)
{
    TAMSContext *c = s->priv_data;
    const TAMSFlow *flow = &c->flows[segc->flow_index];
    AVIOContext *pb = NULL;
    AVDictionary *opts = NULL;
    AVBPrint buf;
    char url[4096];
    int ret, old_nb = segc->nb_flow_segments;

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

    ret = ff_tams_parse_flow_segments_json(buf.str,
                                           &segc->flow_segments, &segc->nb_flow_segments);
    av_bprint_finalize(&buf, NULL);
    if (ret < 0)
        return ret;

    /* Update the pagination cursor from the last received segment so it
     * survives tams_compact_segments() without keeping old entries alive. */
    if (segc->nb_flow_segments > 0) {
        const TAMSFlowSegment *last = &segc->flow_segments[segc->nb_flow_segments - 1];
        segc->fetch_cursor_ns = last->timerange.end;
        segc->has_fetch_cursor = 1;
    }

    av_log(s, AV_LOG_VERBOSE, "TAMS fetched %d new segments (total %d)\n",
           segc->nb_flow_segments - old_nb, segc->nb_flow_segments);

    return 0;
}

/*
 * Resolve a relative URL (starting with ./ or ../) against a base URL.
 * Resolves . and .. path components against the directory portion of base_url.
 * Does not ascend above the scheme://host/ root.
 */
static int tams_resolve_relative_url(const char *base_url, const char *rel,
                                      char *out, size_t out_size)
{
    const char *query = strchr(base_url, '?');
    int url_path_len = query ? (int)(query - base_url) : (int)strlen(base_url);
    char work[4096];
    int wlen = 0, min_wlen = 0, dir_len = 0;
    const char *p;

    for (int i = url_path_len - 1; i >= 0; i--) {
        if (base_url[i] == '/') {
            dir_len = i + 1;
            break;
        }
    }

    if (dir_len >= (int)sizeof(work))
        return AVERROR(ENAMETOOLONG);
    memcpy(work, base_url, dir_len);
    wlen = dir_len;

    {
        const char *scheme_end = strstr(work, "://");
        if (scheme_end) {
            const char *host_slash = strchr(scheme_end + 3, '/');
            if (host_slash)
                min_wlen = (int)(host_slash - work) + 1;
        }
    }

    p = rel;
    while (*p) {
        if (strncmp(p, "../", 3) == 0 || strcmp(p, "..") == 0) {
            if (wlen > min_wlen && work[wlen - 1] == '/')
                wlen--;
            while (wlen > min_wlen && work[wlen - 1] != '/')
                wlen--;
            p += (p[2] == '/') ? 3 : 2;
        } else if (strncmp(p, "./", 2) == 0) {
            p += 2;
        } else {
            while (*p) {
                if (wlen >= (int)sizeof(work) - 1)
                    return AVERROR(ENAMETOOLONG);
                work[wlen++] = *p;
                if (*p++ == '/')
                    break;
            }
        }
    }
    work[wlen] = '\0';

    if (wlen >= (int)out_size)
        return AVERROR(ENAMETOOLONG);
    memcpy(out, work, wlen + 1);
    return 0;
}

/*
 * Resolve which stream within a segment's container corresponds to a TAMS stream.
 * Called once when a segment is first opened. Priority order per TAMS spec:
 *   1. Container-specific (mp2ts_pid, isobmff_track_id, mxf_track_id)
 *   2. format_track_index (nth stream of the same media type)
 *   3. track_index (absolute position)
 *   4. First stream of matching media type (fallback)
 */
static void tams_resolve_sub_stream(AVFormatContext *s,
                                    TAMSStreamContext *sc,
                                    const TAMSSegmentContext *segc)
{
    AVFormatContext *sub = segc->sub_ctx;
    const char *fmt = sub->iformat->name;

    if (sc->has_container_mapping) {
        const TAMSContainerMapping *m = &sc->container_mapping;

        /* 1a. MPEG-TS: match by PID stored in AVStream::id */
        if (m->has_mp2ts_pid && strstr(fmt, "mpegts")) {
            for (unsigned i = 0; i < sub->nb_streams; i++) {
                if (sub->streams[i]->id == m->mp2ts_pid) {
                    sc->sub_stream_index = i;
                    av_log(s, AV_LOG_DEBUG,
                           "TAMS resolved via mp2ts_pid %d\n", m->mp2ts_pid);
                    return;
                }
            }
            av_log(s, AV_LOG_WARNING,
                   "TAMS mp2ts_pid %d not found in segment\n", m->mp2ts_pid);
        }

        /* 1b. ISOBMFF (MP4/MOV): match by track_id stored in AVStream::id */
        if (m->has_isobmff_track_id && strstr(fmt, "mov")) {
            for (unsigned i = 0; i < sub->nb_streams; i++) {
                if (sub->streams[i]->id == m->isobmff_track_id) {
                    sc->sub_stream_index = i;
                    av_log(s, AV_LOG_DEBUG,
                           "TAMS resolved via isobmff_track_id %d\n", m->isobmff_track_id);
                    return;
                }
            }
            av_log(s, AV_LOG_WARNING,
                   "TAMS isobmff_track_id %d not found in segment\n", m->isobmff_track_id);
        }

        /* 1c. MXF: match by track_id stored in AVStream::id; optionally verify package_uid */
        if (m->has_mxf_track_id && strstr(fmt, "mxf")) {
            for (unsigned i = 0; i < sub->nb_streams; i++) {
                if (sub->streams[i]->id == m->mxf_track_id) {
                    if (m->mxf_package_uid[0]) {
                        const AVDictionaryEntry *e =
                            av_dict_get(sub->streams[i]->metadata, "package_uid", NULL, 0);
                        if (!e || strcmp(e->value, m->mxf_package_uid) != 0)
                            continue;
                    }
                    sc->sub_stream_index = i;
                    av_log(s, AV_LOG_DEBUG,
                           "TAMS resolved via mxf_track_id %d\n", m->mxf_track_id);
                    return;
                }
            }
            av_log(s, AV_LOG_WARNING,
                   "TAMS mxf_track_id %d not found in segment\n", m->mxf_track_id);
        }

        /* 2. format_track_index: nth stream of the same media type */
        if (m->has_format_track_index) {
            int count = 0;
            for (unsigned i = 0; i < sub->nb_streams; i++) {
                if (sub->streams[i]->codecpar->codec_type == sc->media_type) {
                    if (count == m->format_track_index) {
                        sc->sub_stream_index = i;
                        av_log(s, AV_LOG_DEBUG,
                               "TAMS resolved via format_track_index %d\n",
                               m->format_track_index);
                        return;
                    }
                    count++;
                }
            }
            av_log(s, AV_LOG_WARNING,
                   "TAMS format_track_index %d not found\n", m->format_track_index);
        }

        /* 3. track_index: absolute stream position */
        if (m->has_track_index) {
            if (m->track_index < (int)sub->nb_streams) {
                sc->sub_stream_index = m->track_index;
                av_log(s, AV_LOG_DEBUG,
                       "TAMS resolved via track_index %d\n", m->track_index);
                return;
            }
            av_log(s, AV_LOG_WARNING,
                   "TAMS track_index %d out of range (sub has %u streams)\n",
                   m->track_index, sub->nb_streams);
        }
    }

    /* 4. Fallback: first stream of matching media type */
    for (unsigned i = 0; i < sub->nb_streams; i++) {
        if (sub->streams[i]->codecpar->codec_type == sc->media_type) {
            sc->sub_stream_index = i;
            av_log(s, AV_LOG_DEBUG,
                   "TAMS resolved via media type fallback to sub-stream %u\n", i);
            return;
        }
    }

    sc->sub_stream_index = -1;
    av_log(s, AV_LOG_WARNING,
           "TAMS could not resolve sub-stream for media type %d\n", sc->media_type);
}

/*
 * Open the segment container at segc->cur_flow_segment_index.
 *
 * Resolves any relative get_url, allocates a child AVFormatContext that
 * inherits the parent's I/O callbacks and interrupt handler, opens the
 * container via avformat_open_input(), and probes stream information.
 * On success the child context is available in segc->sub_ctx and each
 * stream's sub_stream_index has been resolved via tams_resolve_sub_stream().
 */
static int tams_open_segment(AVFormatContext *s, TAMSSegmentContext *segc)
{
    TAMSContext *c = s->priv_data;
    TAMSFlowSegment *seg = &segc->flow_segments[segc->cur_flow_segment_index];
    AVDictionary *opts = NULL;
    char resolved_url[4096];
    const char *seg_url = seg->get_url;
    int ret;

    if (strncmp(seg_url, "./", 2) == 0 || strncmp(seg_url, "../", 3) == 0) {
        ret = tams_resolve_relative_url(s->url, seg_url, resolved_url, sizeof(resolved_url));
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "TAMS failed to resolve relative segment URL %s: %s\n",
                   seg_url, av_err2str(ret));
            return ret;
        }
        av_log(s, AV_LOG_VERBOSE, "TAMS resolved relative segment URL %s -> %s\n",
               seg_url, resolved_url);
        seg_url = resolved_url;
    }

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

    if (tams_same_host(s->url, seg_url)) {
        if ((ret = av_dict_copy(&opts, c->avio_opts, 0)) < 0)
            goto fail;
    }

    av_log(s, AV_LOG_VERBOSE, "TAMS opening segment with object ID: %s\n, for flow %s", seg->object_id, c->flows[segc->flow_index].id);

    ret = avformat_open_input(&segc->sub_ctx, seg_url, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS failed to open segment %s: %s\n",
               seg_url, av_err2str(ret));
        goto fail;
    }

    ret = avformat_find_stream_info(segc->sub_ctx, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "TAMS failed to find stream info in segment\n");
        avformat_close_input(&segc->sub_ctx);
        goto fail;
    }

    if (segc->cur_flow_segment_index == 0) {
        const TAMSFlow *flow = &c->flows[segc->flow_index];
        av_log(s, AV_LOG_VERBOSE, "TAMS first segment stream info for flow %s (%s), object ID: %s\n",
            flow->id, flow->label[0] ? flow->label : "unlabeled", seg->object_id);
        if (av_log_get_level() >= AV_LOG_VERBOSE)
            av_dump_format(segc->sub_ctx, 0, seg_url, 0);
    }

    {
        int segc_index = (int)(segc - c->seg_ctxs);
        for (int i = 0; i < s->nb_streams; i++) {
            TAMSStreamContext *sc = s->streams[i]->priv_data;
            if (sc->seg_ctx_index == segc_index)
                tams_resolve_sub_stream(s, sc, segc);
        }
        for (int j = 0; j < segc->sub_ctx->nb_streams; j++) {
            int used = 0;
            for (int i = 0; i < s->nb_streams; i++) {
                const TAMSStreamContext *sc = s->streams[i]->priv_data;
                if (sc->seg_ctx_index == segc_index && sc->sub_stream_index == j) {
                    used = 1;
                    break;
                }
            }
            if (!used)
                segc->sub_ctx->streams[j]->discard = AVDISCARD_ALL;
        }
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

/*
 * Discard all segments before cur_flow_segment_index to reclaim memory.
 */
static void tams_compact_segments(TAMSSegmentContext *segc)
{
    int discard_from_index = segc->cur_flow_segment_index;
    if (discard_from_index == 0)
        return;
    int remaining_count = segc->nb_flow_segments - discard_from_index;
    if (remaining_count > 0)
        memmove(segc->flow_segments, segc->flow_segments + discard_from_index,
                remaining_count * sizeof(*segc->flow_segments));
    segc->nb_flow_segments       = remaining_count;
    segc->cur_flow_segment_index = 0;

    if (remaining_count > 0) {
        TAMSFlowSegment *tmp = av_realloc_array(segc->flow_segments, remaining_count,
                                                sizeof(*segc->flow_segments));
        if (tmp)
            segc->flow_segments = tmp;
            /* if shrink via av_realloc_array failed we still have valid memory */
    } else {
        av_freep(&segc->flow_segments);
    }
}

/*
 * Ensure the segment context has at least min_segment_buffer seconds of
 * unconsumed segments ahead of cur_flow_segment_index.
 *
 * Returns immediately if the pre-buffer target is already met.  Otherwise
 * compacts already-processed entries and fetches new pages in a loop:
 *
 * - Non-live: keeps fetching until the buffer is full, no new segments arrive,
 *   or the last segment's end covers the flow's declared end.  Returns
 *   AVERROR_EOF only when all fetched segments have been consumed AND no new
 *   ones arrived.  Issues a warning when the buffer falls short of the target.
 *
 * - Live: same fetch loop, but when no new segments arrive it waits with
 *   exponential backoff (capped at seg_poll_max) and retries.  Returns
 *   AVERROR_EOF when the live flow is detected as expired, or AVERROR_EXIT on
 *   interrupt.
 */
static int tams_ensure_segments(AVFormatContext *s, TAMSSegmentContext *segc)
{
    TAMSContext *c = s->priv_data;
    const TAMSFlow *flow = &c->flows[segc->flow_index];
    int64_t needed_ns = tams_get_min_buffer_ns(c, flow);
    int64_t buffered;
    int ret;

    buffered = tams_buffered_ns(c, segc);
    if (buffered >= needed_ns)
        return 0;

    /* Compact already-processed entries before fetching to cap memory use. */
    tams_compact_segments(segc);

    while (!ff_check_interrupt(&s->interrupt_callback)) {
        int old_nb = segc->nb_flow_segments;

        ret = tams_fetch_segments(s, segc);
        if (ret < 0)
            return ret;

        buffered = tams_buffered_ns(c, segc);

        if (buffered >= needed_ns) {
            if (segc->is_live)
                segc->poll_interval = tams_get_poll_init(c, flow);
            return 0;
        }

        if (!segc->is_live) {
            if (segc->nb_flow_segments == old_nb)
                break;  /* no new pages available */
            if (flow->timerange.has_end && segc->nb_flow_segments > 0) {
                const TAMSFlowSegment *last = &segc->flow_segments[segc->nb_flow_segments - 1];
                if (last->timerange.end >= flow->timerange.end)
                    break;  /* reached declared end of flow */
            }
            /* Got some segments but buffer not yet full; fetch the next page. */
        } else {
            if (segc->nb_flow_segments > old_nb) {
                segc->poll_interval = tams_get_poll_init(c, flow);
                /* Got new segments but buffer not yet full; fetch immediately. */
                continue;
            }

            if (tams_check_live_expired(c, flow)) {
                av_log(s, AV_LOG_VERBOSE, "TAMS live flow expired, ending\n");
                segc->is_live = 0;
                break;
            }

            /* No new segments yet; wait with exponential backoff. */
            {
                int64_t wait_end = av_gettime_relative() + segc->poll_interval;
                while (av_gettime_relative() < wait_end) {
                    if (ff_check_interrupt(&s->interrupt_callback))
                        return AVERROR_EXIT;
                    av_usleep(FFMIN(100000, wait_end - av_gettime_relative()));
                }
            }
            segc->poll_interval = FFMIN(segc->poll_interval * 2, c->seg_poll_max);
        }
    }

    if (ff_check_interrupt(&s->interrupt_callback))
        return AVERROR_EXIT;

    if (segc->cur_flow_segment_index >= segc->nb_flow_segments)
        return AVERROR_EOF;

    buffered = tams_buffered_ns(c, segc);
    if (buffered < needed_ns)
        av_log(s, AV_LOG_WARNING,
               "TAMS: only %.1f s buffered for flow %s, target %.1f s\n",
               buffered / 1e9, flow->id, needed_ns / 1e9);

    return 0;
}

/*
 * Copy codec extradata from the first segment into the parent TAMS stream.
 *
 * The flow JSON is the authority for all declared codec parameters (dimensions,
 * sample rate, etc.) and those are already set from it.  Extradata (e.g. H.264
 * SPS/PPS, AAC AudioSpecificConfig) is not expressed in the flow JSON and must
 * be obtained from the segment container.  Only the raw extradata bytes are
 * copied; no other codecpar fields are touched.
 */
static int tams_copy_extradata(AVFormatContext *s, TAMSStreamContext *sc, int tams_index)
{
    TAMSContext *c = s->priv_data;
    TAMSSegmentContext *segc = &c->seg_ctxs[sc->seg_ctx_index];
    AVStream *parent_st = s->streams[tams_index];
    AVCodecParameters *sub_par, *par;

    if (sc->extradata_copied || sc->sub_stream_index < 0)
        return 0;

    sc->extradata_copied = 1;

    sub_par = segc->sub_ctx->streams[sc->sub_stream_index]->codecpar;
    par     = parent_st->codecpar;

    if (!sub_par->extradata || sub_par->extradata_size <= 0 || par->extradata)
        return 0;

    par->extradata = av_malloc(sub_par->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata)
        return AVERROR(ENOMEM);
    memcpy(par->extradata, sub_par->extradata, sub_par->extradata_size);
    memset(par->extradata + sub_par->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = sub_par->extradata_size;

    ffstream(parent_st)->need_context_update = 1;

    return 0;
}

/*
 * Find the TAMS stream index that matches a packet from a segment's sub-demuxer.
 * This resolves packets from segment containers back to the original TAMS stream
 * by matching both the segment context and sub-stream index.
 */
static int tams_find_stream_for_sub_packet(AVFormatContext *s, int seg_ctx_index,
                                            int sub_stream_index)
{
    for (int i = 0; i < s->nb_streams; i++) {
        TAMSStreamContext *sc = s->streams[i]->priv_data;
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
    ret = ff_tams_parse_flows_json(buf.str, &c->flows, &c->nb_flows);
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

    /* Process each flow to create AVStreams (each with an allocated TAMSStreamContext
     * in st->priv_data). Segment contexts are built in the loop below. */
    {
        int nb_initial_flows = c->nb_flows;
        for (int i = 0; i < nb_initial_flows; i++) {

            ret = tams_process_flow(s, i);
            if (ret < 0)
                return ret;
        }
    }

    if (s->nb_streams == 0) {
        av_log(s, AV_LOG_ERROR, "No stream contexts created from TAMS flows\n");
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

    for (int i = 0; i < s->nb_streams; i++) {
        TAMSStreamContext *sc = s->streams[i]->priv_data;
        TAMSSegmentContext *segc = NULL;
        const TAMSFlow *seg_flow;

        /* Determine which flow owns this stream's segments.
         * Container-mapped sub-flows have parent_flow_index set to the
         * multi-flow parent (shared segments). Independent sub-flows and
         * non-multi flows have parent_flow_index == -1 (own segments). */
        int seg_flow_index = sc->parent_flow_index >= 0
                         ? sc->parent_flow_index
                         : sc->flow_index;

        sc->current_ts = INT64_MIN;
        sc->sub_stream_index = -1;

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
            if (c->seg_ctxs[j].flow_index == seg_flow_index) {
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
            segc->flow_index = seg_flow_index;
            seg_flow = &c->flows[seg_flow_index];
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

/*
 * Convert and validate the timestamps of a single packet read from a segment
 * sub-demuxer, writing the results back into the packet.
 *
 * TAMS stores media in discrete segment objects whose timestamps are
 * container-relative (e.g. a fresh MP4 whose first frame is at PTS 0).
 * To turn those into the continuous, 0-based presentation times that
 * callers expect this function performs three operations in order:
 *
 *   Step 1 - timebase conversion
 *     Raw container PTS/DTS/duration are in the segment container's timebase
 *     (e.g. 1/90000 for H.264 in MP4).  These are rescaled to the TAMS
 *     nanosecond timebase (1/1000000000) that is set on every output stream.
 *
 *   Step 2 - per-segment timestamp offset
 *     The TAMS spec defines ts_offset as the value added to each container
 *     timestamp to obtain a TAI timestamp (defaults to 0:0 when absent).
 *     Subtracting the flow's timerange start produces a 0-based presentation
 *     time:  presentation_pts = container_pts + ts_offset - flow_start.
 *
 *   Step 3 - flow boundary filtering
 *     Packets whose PTS falls outside the flow's declared timerange are
 *     handled based on their DTS relative to the segment end:
 *     - DTS before segment end  -> TAMS_PKT_DISCARD (may be a decode reference)
 *     - DTS at/past segment end -> TAMS_PKT_EOF
 */
static int tams_restamp_packet(AVFormatContext *s,
                               TAMSSegmentContext *segc,
                               const TAMSFlowSegment *seg,
                               TAMSStreamContext *sc,
                               int tams_index,
                               AVPacket *pkt)
{
    TAMSContext *c       = s->priv_data;
    const TAMSFlow *flow = &c->flows[sc->flow_index];
    AVStream *st         = s->streams[tams_index];
    AVStream *sub_st     = segc->sub_ctx->streams[pkt->stream_index];
    int64_t pts_ns, dts_ns, dur_ns;

    /* Step 1: timebase conversion */

    pts_ns = (pkt->pts != AV_NOPTS_VALUE)
           ? av_rescale_q(pkt->pts, sub_st->time_base, st->time_base)
           : AV_NOPTS_VALUE;
    dts_ns = (pkt->dts != AV_NOPTS_VALUE)
           ? av_rescale_q(pkt->dts, sub_st->time_base, st->time_base)
           : AV_NOPTS_VALUE;
    dur_ns = av_rescale_q(pkt->duration, sub_st->time_base, st->time_base);

    /* Step 2: per-segment timestamp offset
     * presentation_pts = container_pts + ts_offset - flow_start */

    {
        int64_t flow_start = flow->timerange.has_start ? flow->timerange.start : 0;
        int64_t ts_off     = seg->ts_offset - flow_start;

        if (pts_ns != AV_NOPTS_VALUE)
            pts_ns += ts_off;
        if (dts_ns != AV_NOPTS_VALUE)
            dts_ns += ts_off;
    }

    /* Write results back into the packet unconditionally so that
     * TAMS_PKT_DISCARD callers receive a fully populated packet. */

    pkt->pts          = pts_ns;
    pkt->dts          = dts_ns;
    pkt->duration     = dur_ns;
    pkt->stream_index = tams_index;

    /* current_ts drives the scheduler in tams_read_packet() that picks which
     * stream to service next; keep it up to date. */
    if (pts_ns != AV_NOPTS_VALUE)
        sc->current_ts = pts_ns;
    else if (dts_ns != AV_NOPTS_VALUE)
        sc->current_ts = dts_ns;

    /* Step 3: flow boundary filtering */

    if (pts_ns != AV_NOPTS_VALUE) {
        int outside = (pts_ns < 0);
        if (!outside && flow->timerange.has_end) {
            int64_t flow_start = flow->timerange.has_start ? flow->timerange.start : 0;
            int64_t flow_dur   = flow->timerange.end - flow_start;
            outside = (pts_ns >= flow_dur);
        }
        if (outside) {
            if (seg->timerange.has_end) {
                int64_t flow_start = flow->timerange.has_start ? flow->timerange.start : 0;
                int64_t seg_end_ns = seg->timerange.end - flow_start;
                if (dts_ns != AV_NOPTS_VALUE && dts_ns < seg_end_ns)
                    return TAMS_PKT_DISCARD;
                return TAMS_PKT_EOF;
            }
            /* Segment has no declared end; packet may still be a decode reference. */
            return TAMS_PKT_DISCARD;
        }
    }

    return TAMS_PKT_OK;
}

static int tams_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    TAMSContext *c = s->priv_data;

retry:
    {
        TAMSStreamContext *best = NULL;

        for (int i = 0; i < s->nb_streams; i++) {
            TAMSStreamContext *sc = s->streams[i]->priv_data;
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
            int ret, tams_index;

            if (!segc->sub_ctx) {
                ret = tams_ensure_segments(s, segc);
                if (ret < 0) {
                    if (ret == AVERROR_EOF) {
                        for (int i = 0; i < s->nb_streams; i++) {
                            TAMSStreamContext *sc_i = s->streams[i]->priv_data;
                            if (sc_i->seg_ctx_index == best->seg_ctx_index) {
                                sc_i->eof = 1;
                                av_log(s, AV_LOG_DEBUG, "End of file reached for segment context %d, stream: %d\n", best->seg_ctx_index, i);
                            }
                        }
                        goto retry;
                    }
                    return ret;
                }
                ret = tams_open_segment(s, segc);
                if (ret < 0)
                    return ret;
                for (int i = 0; i < s->nb_streams; i++) {
                    TAMSStreamContext *sc_i = s->streams[i]->priv_data;
                    if (sc_i->seg_ctx_index == best->seg_ctx_index) {
                        ret = tams_validate_segment_stream(s, sc_i, i);
                        if (ret < 0)
                            return ret;
                        ret = tams_copy_extradata(s, sc_i, i);
                        if (ret < 0)
                            return ret;
                    }
                }
            }

            seg = &segc->flow_segments[segc->cur_flow_segment_index];

            ret = av_read_frame(segc->sub_ctx, pkt);
            if (ret == AVERROR_EOF) {
                tams_close_segment(segc);
                segc->cur_flow_segment_index++;
                continue;
            }
            if (ret < 0)
                return ret;

            av_log(s, AV_LOG_TRACE, "Sub-stream segment packet: ");
            av_pkt_dump_log2(s, AV_LOG_TRACE, pkt, 0, s->streams[pkt->stream_index]);

            tams_index = tams_find_stream_for_sub_packet(s, best->seg_ctx_index,
                                                         pkt->stream_index);
            if (tams_index < 0) {
                av_packet_unref(pkt);
                continue;
            }

            {
                int restamp;

                restamp = tams_restamp_packet(s, segc, seg,
                                              s->streams[tams_index]->priv_data,
                                              tams_index, pkt);
                if (restamp == TAMS_PKT_DISCARD) {
                    pkt->flags |= AV_PKT_FLAG_DISCARD;
                    return 0;
                }
                if (restamp == TAMS_PKT_EOF) {
                    av_packet_unref(pkt);
                    ((TAMSStreamContext *)s->streams[tams_index]->priv_data)->eof = 1;
                    goto retry;
                }

                av_log(s, AV_LOG_TRACE, "Mapped TAMS packet: ");
                av_pkt_dump_log2(s, AV_LOG_TRACE, pkt, 0, s->streams[pkt->stream_index]);

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
            av_freep(&segc->flow_segments);
        }
    }
    av_freep(&c->seg_ctxs);
    for (int i = 0; i < s->nb_streams; i++)
        av_freep(&s->streams[i]->priv_data);
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

static int tams_seek(AVFormatContext *s, int stream_index,
                     int64_t timestamp, int flags)
{
    TAMSContext *c = s->priv_data;
    int64_t seek_ns;

    if (stream_index < 0 || stream_index >= (int)s->nb_streams)
        return AVERROR(EINVAL);

    seek_ns = av_rescale_q(timestamp,
                           s->streams[stream_index]->time_base,
                           (AVRational){1, TAMS_TIMEBASE});

    for (int i = 0; i < c->nb_seg_ctxs; i++) {
        TAMSSegmentContext *segc = &c->seg_ctxs[i];
        const TAMSFlow *flow = &c->flows[segc->flow_index];
        int64_t tai_ns;
        int found_index = 0;
        int ret;

        if (segc->is_live)
            return AVERROR(ENOSYS);

        /* Convert output-timebase seek timestamp to TAI. */
        tai_ns = seek_ns +
                 (flow->timerange.has_start ? flow->timerange.start : 0);

        /* Reset the segment window to fetch a fresh page starting at the seek
         * target.  The fetch cursor is set to the seek position so the next
         * tams_fetch_segments() call requests segments at or after tai_ns,
         * avoiding a full re-scan of the entire flow's segment history. */
        tams_close_segment(segc);
        segc->nb_flow_segments       = 0;
        segc->cur_flow_segment_index = 0;
        segc->fetch_cursor_ns   = tai_ns;
        segc->has_fetch_cursor  = 1;

        ret = tams_ensure_segments(s, segc);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
        if (segc->nb_flow_segments == 0)
            continue;

        /* Find the last fetched segment whose start is at or before the target.
         * The server may return segments slightly before tai_ns when the page
         * boundary falls mid-segment, so scan all entries in the window. */
        for (int j = 0; j < segc->nb_flow_segments; j++) {
            const TAMSFlowSegment *seg = &segc->flow_segments[j];
            if (seg->timerange.start <= tai_ns)
                found_index = j;
        }

        segc->cur_flow_segment_index = found_index;
    }

    for (int i = 0; i < s->nb_streams; i++) {
        TAMSStreamContext *sc = s->streams[i]->priv_data;
        sc->eof        = 0;
        sc->current_ts = seek_ns;
    }

    ff_read_frame_flush(s);
    return 0;
}

#define OFFSET(x) offsetof(TAMSContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM

static const AVOption tams_options[] = {
    { "live_threshold", "if segments_updated - now() < live_threshold (in seconds) then flow is treated as live (-1=auto: 2 x segment_duration)",
        OFFSET(live_threshold), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
    { "live_timeout", "if segments_updated - now() > live_timeout (in seconds) then live flow is treated as complete (-1=auto: 4 x segment_duration)",
        OFFSET(live_timeout), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
    { "seg_poll_init", "Initial segment poll interval (in microseconds) for live streams (-1=auto: segment_duration)",
        OFFSET(seg_poll_init), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
    { "seg_poll_max", "Max segment poll interval (in microseconds) when backoff is applied to seg_poll_init for live streams",
        OFFSET(seg_poll_max), AV_OPT_TYPE_INT64, {.i64 = 30000000}, 0, INT_MAX, FLAGS },
    { "min_segment_buffer", "target seconds of segments to buffer ahead of current position (-1=auto: 3 x segment_duration)",
        OFFSET(min_segment_buffer), AV_OPT_TYPE_INT64, {.i64 = -1}, -1, INT_MAX, FLAGS },
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
