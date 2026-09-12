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
 * Tests for TAMS demuxer internals: codec lookup, live detection, URL
 * building, and packet restamping/boundary classification. All pure or
 * near-pure functions exercised directly, no network I/O.
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#include "libavformat/tamsdec.c"

#include "libavcodec/codec_id.h"
#include "libavcodec/packet.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define FAIL(msg, ...) do { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); \
    return 1; \
} while (0)

typedef struct RestampFixture {
    AVFormatContext *s;
    AVFormatContext *sub_ctx;
    TAMSDemuxContext c;
    TAMSFlow flow;
    TAMSStreamContext sc;
    TAMSSegmentContext segc;
} RestampFixture;

/* ====================================================================
 * tams_codec_lookup
 * ==================================================================== */

static int test_codec_lookup(void)
{
    if (tams_codec_lookup("video/h264") != AV_CODEC_ID_H264)
        FAIL("codec_lookup: video/h264 wrong");
    if (tams_codec_lookup("audio/aac") != AV_CODEC_ID_AAC)
        FAIL("codec_lookup: audio/aac wrong");
    if (tams_codec_lookup("audio/mp2") != AV_CODEC_ID_MP2)
        FAIL("codec_lookup: audio/mp2 wrong");
    if (tams_codec_lookup("video/mp2t") != AV_CODEC_ID_NONE)
        FAIL("codec_lookup: unknown mime should return AV_CODEC_ID_NONE");

    printf("OK: codec_lookup\n");
    return 0;
}

/* ====================================================================
 * tams_same_host
 * ==================================================================== */

static int test_same_host(void)
{
    if (!tams_same_host("http://example.com/flows/1",
                        "http://example.com/flows/1/segments"))
        FAIL("same_host: identical hosts should match");
    if (!tams_same_host("http://EXAMPLE.com/a", "http://example.com/b"))
        FAIL("same_host: host comparison should be case-insensitive");
    if (tams_same_host("http://example.com/a", "http://other.com/b"))
        FAIL("same_host: different hosts should not match");

    printf("OK: same_host\n");
    return 0;
}

/* ====================================================================
 * tams_segment_duration_us
 * ==================================================================== */

static int test_segment_duration_us(void)
{
    TAMSFlow flow;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){6, 1};
    if (tams_segment_duration_us(&flow) != 6000000)
        FAIL("segment_duration_us: 6/1 should be 6000000us");

    flow.segment_duration = (AVRational){1, 2};
    if (tams_segment_duration_us(&flow) != 500000)
        FAIL("segment_duration_us: 1/2 should be 500000us");

    /* invalid/absent segment_duration defaults to 1 second */
    flow.segment_duration = (AVRational){0, 1};
    if (tams_segment_duration_us(&flow) != 1000000)
        FAIL("segment_duration_us: default should be 1000000us");

    printf("OK: segment_duration_us\n");
    return 0;
}

/* ====================================================================
 * tams_find_flow_by_id
 * ==================================================================== */

static int test_find_flow_by_id(void)
{
    TAMSDemuxContext c;
    TAMSFlow flows[2];

    memset(flows, 0, sizeof(flows));
    av_strlcpy(flows[0].id, "aaaaaaaa-0000-0000-0000-000000000000", sizeof(flows[0].id));
    av_strlcpy(flows[1].id, "bbbbbbbb-0000-0000-0000-000000000000", sizeof(flows[1].id));
    memset(&c, 0, sizeof(c));
    c.flows    = flows;
    c.nb_flows = 2;

    if (tams_find_flow_by_id(&c, "bbbbbbbb-0000-0000-0000-000000000000") != 1)
        FAIL("find_flow_by_id: should find index 1");
    if (tams_find_flow_by_id(&c, "cccccccc-0000-0000-0000-000000000000") != -1)
        FAIL("find_flow_by_id: unknown id should return -1");

    printf("OK: find_flow_by_id\n");
    return 0;
}

/* ====================================================================
 * tams_check_live / tams_check_live_expired
 * ==================================================================== */

static int test_check_live_recent(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    c.live_threshold = -1; /* auto: 2 x segment_duration */
    c.live_timeout   = -1; /* auto: 4 x segment_duration */

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){1, 1}; /* 1s -> threshold 2s, timeout 4s */
    av_strlcpy(flow.segments_updated, "now", sizeof(flow.segments_updated));

    if (!tams_check_live(&c, &flow))
        FAIL("check_live_recent: a just-updated flow should be live");
    if (tams_check_live_expired(&c, &flow))
        FAIL("check_live_recent: a just-updated flow should not be expired");

    printf("OK: check_live_recent\n");
    return 0;
}

static int test_check_live_stale(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    c.live_threshold = -1;
    c.live_timeout   = -1;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){1, 1};
    av_strlcpy(flow.segments_updated, "2000-01-01T00:00:00Z", sizeof(flow.segments_updated));

    if (tams_check_live(&c, &flow))
        FAIL("check_live_stale: a long-stale flow should not be live");
    if (!tams_check_live_expired(&c, &flow))
        FAIL("check_live_stale: a long-stale flow should be expired");

    printf("OK: check_live_stale\n");
    return 0;
}

static int test_check_live_no_segments_updated(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    memset(&flow, 0, sizeof(flow)); /* segments_updated left empty */

    if (tams_check_live(&c, &flow))
        FAIL("check_live_no_segments_updated: empty segments_updated should not be live");
    if (!tams_check_live_expired(&c, &flow))
        FAIL("check_live_no_segments_updated: empty segments_updated should be treated as expired");

    printf("OK: check_live_no_segments_updated\n");
    return 0;
}

/* ====================================================================
 * tams_get_poll_init / tams_get_min_buffer_ns
 * ==================================================================== */

static int test_get_poll_init(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){2, 1}; /* 2s -> 2000000us */

    memset(&c, 0, sizeof(c));
    c.seg_poll_init = -1; /* auto */
    if (tams_get_poll_init(&c, &flow) != 2000000)
        FAIL("get_poll_init: auto should equal segment_duration_us");

    c.seg_poll_init = 12345;
    if (tams_get_poll_init(&c, &flow) != 12345)
        FAIL("get_poll_init: explicit value should be used as-is");

    printf("OK: get_poll_init\n");
    return 0;
}

static int test_get_min_buffer_ns(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){2, 1}; /* 2s -> 3 x 2s = 6s auto buffer */

    memset(&c, 0, sizeof(c));
    c.min_segment_buffer = -1; /* auto */
    if (tams_get_min_buffer_ns(&c, &flow) != INT64_C(6000000000))
        FAIL("get_min_buffer_ns: auto should be 3 x segment_duration");

    c.min_segment_buffer = 10; /* seconds */
    if (tams_get_min_buffer_ns(&c, &flow) != INT64_C(10000000000))
        FAIL("get_min_buffer_ns: explicit seconds should convert to ns");

    printf("OK: get_min_buffer_ns\n");
    return 0;
}

/* ====================================================================
 * tams_buffered_ns
 * ==================================================================== */

static int test_buffered_ns(void)
{
    TAMSDemuxContext c;
    TAMSSegmentContext segc;
    TAMSFlowSegment segs[3];

    memset(&c, 0, sizeof(c));
    memset(segs, 0, sizeof(segs));
    /* three 2-second segments: [0,2) [2,4) [4,6) */
    for (int i = 0; i < 3; i++) {
        segs[i].timerange.has_start = segs[i].timerange.has_end = 1;
        segs[i].timerange.start = INT64_C(2000000000) * i;
        segs[i].timerange.end   = INT64_C(2000000000) * (i + 1);
    }

    memset(&segc, 0, sizeof(segc));
    segc.flow_segments          = segs;
    segc.nb_flow_segments       = 3;
    segc.cur_flow_segment_index = 0;
    if (tams_buffered_ns(&c, &segc) != INT64_C(6000000000))
        FAIL("buffered_ns: expected 6s total from index 0");

    segc.cur_flow_segment_index = 2;
    if (tams_buffered_ns(&c, &segc) != INT64_C(2000000000))
        FAIL("buffered_ns: expected 2s remaining from index 2");

    segc.cur_flow_segment_index = 3;
    if (tams_buffered_ns(&c, &segc) != 0)
        FAIL("buffered_ns: expected 0 once fully consumed");

    printf("OK: buffered_ns\n");
    return 0;
}

/* ====================================================================
 * tams_compact_segments
 * ==================================================================== */

static int test_compact_segments(void)
{
    TAMSSegmentContext segc;
    TAMSFlowSegment *segs = av_calloc(4, sizeof(*segs));

    if (!segs)
        FAIL("compact_segments: alloc failed");
    for (int i = 0; i < 4; i++)
        av_strlcpy(segs[i].object_id, i == 0 ? "seg0" : i == 1 ? "seg1" :
                   i == 2 ? "seg2" : "seg3", sizeof(segs[i].object_id));

    memset(&segc, 0, sizeof(segc));
    segc.flow_segments          = segs;
    segc.nb_flow_segments       = 4;
    segc.cur_flow_segment_index = 2;

    tams_compact_segments(&segc);

    if (segc.nb_flow_segments != 2)
        FAIL("compact_segments: expected 2 remaining, got %d", segc.nb_flow_segments);
    if (segc.cur_flow_segment_index != 0)
        FAIL("compact_segments: cur_flow_segment_index should reset to 0");
    if (strcmp(segc.flow_segments[0].object_id, "seg2"))
        FAIL("compact_segments: wrong entry at index 0: %s", segc.flow_segments[0].object_id);
    if (strcmp(segc.flow_segments[1].object_id, "seg3"))
        FAIL("compact_segments: wrong entry at index 1: %s", segc.flow_segments[1].object_id);

    av_freep(&segc.flow_segments);
    printf("OK: compact_segments\n");
    return 0;
}

static int test_compact_segments_all_consumed(void)
{
    TAMSSegmentContext segc;
    TAMSFlowSegment *segs = av_calloc(2, sizeof(*segs));

    if (!segs)
        FAIL("compact_segments_all_consumed: alloc failed");

    memset(&segc, 0, sizeof(segc));
    segc.flow_segments          = segs;
    segc.nb_flow_segments       = 2;
    segc.cur_flow_segment_index = 2; /* fully consumed */

    tams_compact_segments(&segc);

    if (segc.nb_flow_segments != 0)
        FAIL("compact_segments_all_consumed: expected 0 remaining");
    if (segc.flow_segments)
        FAIL("compact_segments_all_consumed: array should be freed");

    printf("OK: compact_segments_all_consumed\n");
    return 0;
}

/* ====================================================================
 * tams_build_base_url / tams_build_clean_query / tams_resolve_relative_url
 * ==================================================================== */

static int test_build_base_url(void)
{
    char base[2048];

    /* /flows/<id>: strips only the last path segment, back to the /flows
     * collection endpoint so other flow ids can be appended later */
    if (tams_build_base_url("http://host/flows/abc-123", base, sizeof(base)) < 0)
        FAIL("build_base_url: call failed");
    if (strcmp(base, "http://host/flows"))
        FAIL("build_base_url: expected 'http://host/flows', got '%s'", base);

    /* /flows?source_id=...: already at the collection endpoint, special-
     * cased to keep the "/flows" suffix rather than stripping it */
    if (tams_build_base_url("http://host/flows?source_id=x", base, sizeof(base)) < 0)
        FAIL("build_base_url: call failed (flows list)");
    if (strcmp(base, "http://host/flows"))
        FAIL("build_base_url: expected 'http://host/flows', got '%s'", base);

    if (tams_build_base_url("relative-id", base, sizeof(base)) < 0)
        FAIL("build_base_url: call failed (no slash)");
    if (base[0])
        FAIL("build_base_url: expected empty base, got '%s'", base);

    printf("OK: build_base_url\n");
    return 0;
}

static int test_build_clean_query(void)
{
    char out[256];

    tams_build_clean_query("?timerange=[0:0_1:0)&foo=bar&source_id=xyz", out, sizeof(out));
    if (strcmp(out, "?foo=bar"))
        FAIL("build_clean_query: expected '?foo=bar', got '%s'", out);

    tams_build_clean_query("?timerange=[0:0_1:0)&source_id=xyz", out, sizeof(out));
    if (out[0])
        FAIL("build_clean_query: expected empty result, got '%s'", out);

    printf("OK: build_clean_query\n");
    return 0;
}

static int test_resolve_relative_url(void)
{
    char out[2048];

    if (tams_resolve_relative_url("http://host/flows/abc/segments", "obj-1.ts",
                                  out, sizeof(out)) < 0)
        FAIL("resolve_relative_url: call failed (simple)");
    if (strcmp(out, "http://host/flows/abc/obj-1.ts"))
        FAIL("resolve_relative_url: expected 'http://host/flows/abc/obj-1.ts', got '%s'", out);

    if (tams_resolve_relative_url("http://host/flows/abc/segments", "../other/obj-1.ts",
                                  out, sizeof(out)) < 0)
        FAIL("resolve_relative_url: call failed (..)");
    if (strcmp(out, "http://host/flows/other/obj-1.ts"))
        FAIL("resolve_relative_url: expected '.. ' resolution, got '%s'", out);

    printf("OK: resolve_relative_url\n");
    return 0;
}

/* ====================================================================
 * tams_probe
 * ==================================================================== */

static int test_probe_format_urn(void)
{
    AVProbeData p;
    const char *json = "{\"format\":\"urn:x-nmos:format:video\"}";

    memset(&p, 0, sizeof(p));
    p.buf      = (unsigned char *)json;
    p.buf_size = (int)strlen(json);

    if (tams_probe(&p) != AVPROBE_SCORE_MAX)
        FAIL("probe_format_urn: expected AVPROBE_SCORE_MAX");

    printf("OK: probe_format_urn\n");
    return 0;
}

static int test_probe_fallback_keys(void)
{
    AVProbeData p;
    const char *json =
        "{\"source_id\": \"x\", \"essence_parameters\": {}, \"timerange\": \"_\"}";

    memset(&p, 0, sizeof(p));
    p.buf      = (unsigned char *)json;
    p.buf_size = (int)strlen(json);

    if (tams_probe(&p) != AVPROBE_SCORE_EXTENSION)
        FAIL("probe_fallback_keys: expected AVPROBE_SCORE_EXTENSION");

    printf("OK: probe_fallback_keys\n");
    return 0;
}

static int test_probe_rejects_non_json(void)
{
    AVProbeData p;
    const char *text = "not json at all";

    memset(&p, 0, sizeof(p));
    p.buf      = (unsigned char *)text;
    p.buf_size = (int)strlen(text);

    if (tams_probe(&p) != 0)
        FAIL("probe_rejects_non_json: expected score 0");

    printf("OK: probe_rejects_non_json\n");
    return 0;
}

static int restamp_fixture_init(RestampFixture *f, int64_t flow_end_ns,
                                 int64_t seg_start_ns, int seg_start_inclusive,
                                 int64_t seg_end_ns, int seg_end_inclusive,
                                 int64_t ts_offset,
                                 TAMSFlowSegment *seg_out)
{
    AVStream *st, *sub_st;

    memset(f, 0, sizeof(*f));

    f->s = avformat_alloc_context();
    f->sub_ctx = avformat_alloc_context();
    if (!f->s || !f->sub_ctx)
        return AVERROR(ENOMEM);

    st = avformat_new_stream(f->s, NULL);
    sub_st = avformat_new_stream(f->sub_ctx, NULL);
    if (!st || !sub_st)
        return AVERROR(ENOMEM);

    st->time_base     = (AVRational){1, TAMS_TIMEBASE};
    sub_st->time_base = (AVRational){1, 1000};

    memset(&f->flow, 0, sizeof(f->flow));
    f->flow.timerange.has_start = 1;
    f->flow.timerange.start     = 0;
    f->flow.timerange.has_end   = 1;
    f->flow.timerange.end       = flow_end_ns;
    f->flow.timerange.end_inclusive = 0;

    f->c.flows    = &f->flow;
    f->c.nb_flows = 1;
    f->s->priv_data = &f->c;

    memset(&f->sc, 0, sizeof(f->sc));
    f->sc.flow_index = 0;

    memset(&f->segc, 0, sizeof(f->segc));
    f->segc.sub_ctx = f->sub_ctx;

    memset(seg_out, 0, sizeof(*seg_out));
    seg_out->timerange.has_start     = 1;
    seg_out->timerange.start         = seg_start_ns;
    seg_out->timerange.start_inclusive = seg_start_inclusive;
    seg_out->timerange.has_end       = 1;
    seg_out->timerange.end           = seg_end_ns;
    seg_out->timerange.end_inclusive = seg_end_inclusive;
    seg_out->ts_offset               = ts_offset;

    return 0;
}

static void restamp_fixture_free(RestampFixture *f)
{
    f->s->priv_data = NULL;
    avformat_free_context(f->s);
    avformat_free_context(f->sub_ctx);
}

static AVPacket *make_test_packet(int64_t pts, int64_t dts, int64_t duration)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return NULL;
    pkt->pts          = pts;
    pkt->dts          = dts;
    pkt->duration     = duration;
    pkt->stream_index = 0;
    return pkt;
}

static int test_restamp_ok(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* flow [0,10s), segment [0,5s), no ts_offset */
    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 1,
                             INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("restamp_ok: fixture init failed");

    pkt = make_test_packet(1000, 1000, 40); /* 1s pts/dts, 40ms duration */
    if (!pkt) { restamp_fixture_free(&f); FAIL("restamp_ok: alloc packet failed"); }

    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (ret != TAMS_PKT_OK)
        FAIL("restamp_ok: expected TAMS_PKT_OK, got %d", ret);
    if (pkt->pts != INT64_C(1000000000))
        FAIL("restamp_ok: wrong pts: %"PRId64, pkt->pts);
    if (pkt->duration != INT64_C(40000000))
        FAIL("restamp_ok: wrong duration: %"PRId64, pkt->duration);
    if (pkt->stream_index != 0)
        FAIL("restamp_ok: wrong stream_index: %d", pkt->stream_index);
    if (f.sc.current_ts != pkt->pts)
        FAIL("restamp_ok: current_ts not updated");
    if (f.sc.next_pts != pkt->pts + pkt->duration)
        FAIL("restamp_ok: next_pts not updated");

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_ok\n");
    return 0;
}

static int test_restamp_offset_cached_once(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 1,
                             INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("restamp_offset_cached_once: fixture init failed");

    pkt = make_test_packet(0, 0, 0);
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (ret != TAMS_PKT_OK || pkt->pts != 0)
        FAIL("restamp_offset_cached_once: first packet unexpected result ret=%d pts=%"PRId64,
             ret, pkt->pts);
    av_packet_free(&pkt);

    if (!f.segc.cur_pts_offset_set || f.segc.cur_pts_offset != 0)
        FAIL("restamp_offset_cached_once: offset should be cached as 0");

    /* mutate the segment's ts_offset as if a later packet carried a
     * different value; the cached offset must NOT change mid-segment */
    seg.ts_offset = INT64_C(999000000000);

    pkt = make_test_packet(500, 500, 0); /* 500ms in */
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (ret != TAMS_PKT_OK)
        FAIL("restamp_offset_cached_once: second packet unexpected ret %d", ret);
    if (pkt->pts != INT64_C(500000000))
        FAIL("restamp_offset_cached_once: cached offset was not honoured, pts=%"PRId64,
             pkt->pts);

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_offset_cached_once\n");
    return 0;
}

static int test_restamp_discard_at_exclusive_segment_start(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* exclusive segment start: pts exactly at the boundary must discard */
    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 0,
                             INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("restamp_discard_start: fixture init failed");

    pkt = make_test_packet(0, 0, 0);
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (ret != TAMS_PKT_DISCARD)
        FAIL("restamp_discard_start: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_discard_at_exclusive_segment_start\n");
    return 0;
}

static int test_restamp_eoseg(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* segment [0,5s) exclusive end; both pts and dts past the end -> EOSEG */
    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 1,
                             INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("restamp_eoseg: fixture init failed");

    pkt = make_test_packet(6000, 6000, 0); /* 6s, past the 5s segment end */
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (ret != TAMS_PKT_EOSEG)
        FAIL("restamp_eoseg: expected TAMS_PKT_EOSEG, got %d", ret);

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_eoseg\n");
    return 0;
}

static int test_restamp_discard_still_needed_at_segment_end(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* pts past segment end, but dts still within it (B-frame reordering):
     * must DISCARD, not EOSEG, so the decoder still gets the reference. */
    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 1,
                             INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("restamp_discard_seg_end: fixture init failed");

    pkt = make_test_packet(6000, 4000, 0); /* pts=6s, dts=4s (< 5s seg end) */
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (ret != TAMS_PKT_DISCARD)
        FAIL("restamp_discard_seg_end: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_discard_still_needed_at_segment_end\n");
    return 0;
}

static int test_restamp_eof(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* flow ends at 10s; segment end is unbounded within the flow window so
     * only the flow-end boundary is exercised here */
    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 1,
                             INT64_C(20000000000), 0, 0, &seg) < 0)
        FAIL("restamp_eof: fixture init failed");

    pkt = make_test_packet(11000, 11000, 0); /* 11s: past flow end, dts too */
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (ret != TAMS_PKT_EOF)
        FAIL("restamp_eof: expected TAMS_PKT_EOF, got %d", ret);

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_eof\n");
    return 0;
}

static int test_restamp_discard_still_needed_at_flow_end(void)
{
    RestampFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* pts past flow end, but dts still within it: must DISCARD, not EOF */
    if (restamp_fixture_init(&f, INT64_C(10000000000), 0, 1,
                             INT64_C(20000000000), 0, 0, &seg) < 0)
        FAIL("restamp_discard_flow_end: fixture init failed");

    pkt = make_test_packet(11000, 9000, 0); /* pts=11s, dts=9s (< 10s flow end) */
    ret = tams_restamp_packet(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (ret != TAMS_PKT_DISCARD)
        FAIL("restamp_discard_flow_end: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    restamp_fixture_free(&f);
    printf("OK: restamp_discard_still_needed_at_flow_end\n");
    return 0;
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(int argc, char *argv[])
{
    int ret = 0;

    ret |= test_codec_lookup();
    ret |= test_same_host();
    ret |= test_segment_duration_us();
    ret |= test_find_flow_by_id();

    ret |= test_check_live_recent();
    ret |= test_check_live_stale();
    ret |= test_check_live_no_segments_updated();

    ret |= test_get_poll_init();
    ret |= test_get_min_buffer_ns();
    ret |= test_buffered_ns();

    ret |= test_compact_segments();
    ret |= test_compact_segments_all_consumed();

    ret |= test_build_base_url();
    ret |= test_build_clean_query();
    ret |= test_resolve_relative_url();

    ret |= test_probe_format_urn();
    ret |= test_probe_fallback_keys();
    ret |= test_probe_rejects_non_json();

    ret |= test_restamp_ok();
    ret |= test_restamp_offset_cached_once();
    ret |= test_restamp_discard_at_exclusive_segment_start();
    ret |= test_restamp_eoseg();
    ret |= test_restamp_discard_still_needed_at_segment_end();
    ret |= test_restamp_eof();
    ret |= test_restamp_discard_still_needed_at_flow_end();

    if (ret)
        printf("\nSOME TESTS FAILED\n");
    else
        printf("\nALL TESTS PASSED\n");

    return ret;
}
