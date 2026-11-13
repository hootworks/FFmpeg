/*
 * Copyright (C) 2026 Hoot Works Ltd
 * Author    (C) 2026 Nick Ryan <hello.vectronic@gmail.com>
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
 * @file
 * Tests for TAMS demuxer internals.
 *
 * @author Nick Ryan
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

/* ====================================================================
 * Test helpers
 * ==================================================================== */

typedef struct PacketFixture {
    AVFormatContext *s;
    AVFormatContext *sub_ctx;
    TAMSDemuxContext c;
    TAMSFlow flow;
    TAMSStreamContext sc;
    TAMSSegmentContext segc;
} PacketFixture;

typedef struct URLFixture {
    AVFormatContext *s;
    TAMSDemuxContext c;
    TAMSFlow flow;
    TAMSSegmentContext segc;
} URLFixture;

static int url_fixture_init(URLFixture *f, const char *url, const char *flow_tr,
                            const char *status, const char *url_tr)
{
    memset(f, 0, sizeof(*f));

    f->s = avformat_alloc_context();
    if (!f->s || !(f->s->url = av_strdup(url)))
        return AVERROR(ENOMEM);

    av_strlcpy(f->flow.id, "abc", sizeof(f->flow.id));
    av_strlcpy(f->flow.status, status, sizeof(f->flow.status));
    if (ff_tams_timerange_from_str(flow_tr, &f->flow.timerange) < 0)
        return AVERROR(EINVAL);

    if (url_tr) {
        if (ff_tams_timerange_from_str(url_tr, &f->c.url_tr) < 0)
            return AVERROR(EINVAL);
        f->c.has_url_tr = 1;
    }

    f->c.flows      = &f->flow;
    f->c.nb_flows   = 1;
    f->s->priv_data = &f->c;
    return 0;
}

static void url_fixture_free(URLFixture *f)
{
    f->s->priv_data = NULL;
    avformat_free_context(f->s);
}

/* ====================================================================
 * Basic utility tests
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
 * Live status utility tests
 * ==================================================================== */

static int test_is_live_recent(void)
{
    TAMSDemuxContext c;
    TAMSSegmentContext segc = { 0 };
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    c.live_threshold_s = -1; /* auto: 2 x segment_duration */
    c.live_timeout_s   = -1; /* auto: 4 x segment_duration */

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){1, 1}; /* 1s -> threshold 2s, timeout 4s */
    /* an ISO 8601 date-time; "now" is also accepted by av_parse_time() */
    av_strlcpy(flow.segments_updated, "now", sizeof(flow.segments_updated));

    if (!tams_is_live(&c, &flow))
        FAIL("is_live_recent: a just-updated flow should be live");
    if (tams_is_live_expired(&c, &segc, &flow))
        FAIL("is_live_recent: a just-updated flow should not be expired");

    printf("OK: is_live_recent\n");
    return 0;
}

static int test_is_live_stale(void)
{
    TAMSDemuxContext c;
    TAMSSegmentContext segc = { 0 };
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    c.live_threshold_s = -1;
    c.live_timeout_s   = -1;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){1, 1};
    av_strlcpy(flow.segments_updated, "2000-01-01T00:00:00Z", sizeof(flow.segments_updated));

    if (tams_is_live(&c, &flow))
        FAIL("is_live_stale: a long-stale flow should not be live");
    if (!tams_is_live_expired(&c, &segc, &flow))
        FAIL("is_live_stale: a long-stale flow should be expired");

    printf("OK: is_live_stale\n");
    return 0;
}

static int test_is_live_no_segments_updated(void)
{
    TAMSDemuxContext c;
    TAMSSegmentContext segc = { 0 };
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    memset(&flow, 0, sizeof(flow)); /* segments_updated left empty */

    if (tams_is_live(&c, &flow))
        FAIL("is_live_no_segments_updated: empty segments_updated should not be live");
    if (!tams_is_live_expired(&c, &segc, &flow))
        FAIL("is_live_no_segments_updated: empty segments_updated should be treated as expired");

    printf("OK: is_live_no_segments_updated\n");
    return 0;
}

static int test_is_live_status(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    c.live_threshold_s = -1;
    c.live_timeout_s   = -1;

    /* status wins over a missing/stale segments_updated */
    memset(&flow, 0, sizeof(flow)); /* segments_updated left empty */
    av_strlcpy(flow.status, "ingesting", sizeof(flow.status));
    if (!tams_is_live(&c, &flow))
        FAIL("is_live_status: an ingesting flow should be live");

    /* an ISO 8601 date-time; "now" is also accepted by av_parse_time() */
    av_strlcpy(flow.segments_updated, "now", sizeof(flow.segments_updated));
    av_strlcpy(flow.status, "closed_complete", sizeof(flow.status));
    if (tams_is_live(&c, &flow))
        FAIL("is_live_status: a closed_complete flow should not be live");

    printf("OK: is_live_status\n");
    return 0;
}

/*
 * The Flow (and so segments_updated) is only re-fetched when a live reader
 * reaches the live edge, never while segments keep arriving, so by then the
 * copy held can be long out of date. Expiry must therefore also count this
 * reader's own last sight of new segments (last_new_seg_us), otherwise the
 * first empty poll would end a Flow that is still being written.
 */
static int test_is_live_expiry_uses_reader_activity(void)
{
    TAMSDemuxContext c;
    TAMSSegmentContext segc = { 0 };
    TAMSFlow flow;

    memset(&c, 0, sizeof(c));
    c.live_threshold_s = -1;
    c.live_timeout_s   = -1; /* auto: 4 x segment_duration */

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){1, 1}; /* 1s -> timeout 4s */
    /* segments_updated alone is decades past the timeout */
    av_strlcpy(flow.segments_updated, "2000-01-01T00:00:00Z", sizeof(flow.segments_updated));

    /* ... but this reader saw new segments just now: not expired */
    segc.last_new_seg_us = av_gettime();
    if (tams_is_live_expired(&c, &segc, &flow))
        FAIL("is_live_expiry_uses_reader_activity: recently seen segments should not be expired");

    /* ... and once the reader has also seen nothing for longer than the
     * timeout (5s > 4s), the Flow is expired */
    segc.last_new_seg_us = av_gettime() - 5000000;
    if (!tams_is_live_expired(&c, &segc, &flow))
        FAIL("is_live_expiry_uses_reader_activity: no new segments for 5s should be expired");

    printf("OK: is_live_expiry_uses_reader_activity\n");
    return 0;
}

static int test_poll_init_us(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){2, 1}; /* 2s -> 2000000us */

    memset(&c, 0, sizeof(c));
    c.seg_poll_init_us = -1; /* auto */
    if (tams_poll_init_us(&c, &flow) != 2000000)
        FAIL("poll_init_us: auto should equal segment_duration_us");

    c.seg_poll_init_us = 12345;
    if (tams_poll_init_us(&c, &flow) != 12345)
        FAIL("poll_init_us: explicit value should be used as-is");

    printf("OK: poll_init_us\n");
    return 0;
}

/* ====================================================================
 * Buffer utility tests
 * ==================================================================== */

static int test_min_buffer_ns(void)
{
    TAMSDemuxContext c;
    TAMSFlow flow;

    memset(&flow, 0, sizeof(flow));
    flow.segment_duration = (AVRational){2, 1}; /* 2s -> 3 x 2s = 6s auto buffer */

    memset(&c, 0, sizeof(c));
    c.min_segment_buffer_s = -1; /* auto */
    if (tams_min_buffer_ns(&c, &flow) != INT64_C(6000000000))
        FAIL("min_buffer_ns: auto should be 3 x segment_duration");

    c.min_segment_buffer_s = 10; /* seconds */
    if (tams_min_buffer_ns(&c, &flow) != INT64_C(10000000000))
        FAIL("min_buffer_ns: explicit seconds should convert to ns");

    printf("OK: min_buffer_ns\n");
    return 0;
}

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
 * Segment tests
 * ==================================================================== */

static int test_compact_segments(void)
{
    TAMSSegmentContext segc;
    TAMSFlowSegment *segs = av_calloc(4, sizeof(*segs));

    if (!segs)
        FAIL("compact_segments: alloc failed");
    for (int i = 0; i < 4; i++)
        snprintf(segs[i].object_id, sizeof(segs[i].object_id), "seg%d", i);

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
 * URL utility tests
 * ==================================================================== */

static int test_build_base_url(void)
{
    char base[2048];

    /* /flows/<id>: strips only the last path segment, back to the /flows
     * collection endpoint so other flow ids can be appended later */
    if (ff_tams_derive_base_url("http://host/flows/abc-123", base, sizeof(base), NULL) < 0)
        FAIL("build_base_url: call failed");
    if (strcmp(base, "http://host/flows"))
        FAIL("build_base_url: expected 'http://host/flows', got '%s'", base);

    /* /flows?source_id=...: already at the collection endpoint,
    * does not strip the last path segment */
    if (ff_tams_derive_base_url("http://host/flows?source_id=x", base, sizeof(base), NULL) < 0)
        FAIL("build_base_url: call failed (flows list)");
    if (strcmp(base, "http://host/flows"))
        FAIL("build_base_url: expected 'http://host/flows', got '%s'", base);

    if (ff_tams_derive_base_url("relative-id", base, sizeof(base), NULL) < 0)
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

/*
 * The first segments request is limited to the Flow's timerange. Later
 * pages start at the cursor and keep the end where reading stops. A local
 * file takes no query string.
 */
static int test_build_segments_url(void)
{
    static const struct {
        const char *url, *flow_tr, *url_tr;
        int was_live, is_local;
        int64_t cursor_ns; /* < 0: first request */
        const char *expected;
    } cases[] = {
        { "http://host/flows/abc", "[5:0_)",     NULL, 0, 0, -1,
          "http://host/flows/abc/segments?timerange=[5:0_)" },
        { "http://host/flows/abc", "[5:0_10:0)", NULL, 0, 0, -1,
          "http://host/flows/abc/segments?timerange=[5:0_10:0)" },
        { "http://host/flows/abc", "_",          NULL, 0, 0, -1,
          "http://host/flows/abc/segments" },
        { "http://host/flows/abc", "[5:0_10:0)", NULL, 0, 0, INT64_C(7000000000),
          "http://host/flows/abc/segments?timerange=[7:0_10:0)" },
        { "http://host/flows/abc", "[5:0_10:0)", NULL, 1, 0, INT64_C(7000000000),
          "http://host/flows/abc/segments?timerange=[7:0_)" },
        { "http://host/flows/abc", "[5:0_10:0)", "[0:0_9:0)", 1, 0, INT64_C(7000000000),
          "http://host/flows/abc/segments?timerange=[7:0_9:0)" },
        { "samples/flows/abc.json", "[5:0_10:0)", NULL, 0, 1, -1,
          "samples/flows/abc/segments" },
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(cases); i++) {
        URLFixture f;
        char out[4096];

        if (url_fixture_init(&f, cases[i].url, cases[i].flow_tr, "", cases[i].url_tr) < 0)
            FAIL("build_segments_url: fixture init failed (case %d)", i);

        f.c.is_local     = cases[i].is_local;
        f.segc.was_live  = cases[i].was_live;
        if (cases[i].cursor_ns >= 0) {
            f.segc.has_fetch_cursor = 1;
            f.segc.fetch_cursor_ns  = cases[i].cursor_ns;
        }
        tams_build_segments_url(f.s, &f.flow, &f.segc, out, sizeof(out));
        url_fixture_free(&f);

        if (strcmp(out, cases[i].expected))
            FAIL("build_segments_url: case %d: expected '%s', got '%s'", i, cases[i].expected, out);
    }

    printf("OK: build_segments_url\n");
    return 0;
}

/* ====================================================================
 * Timerange tests
 * ==================================================================== */

/*
 * The Flow's timerange is intersected with the URL timerange. With no
 * overlap it is never, unless the Flow is live and may still reach the URL
 * timerange: after its end, or with no segments yet ("()").
 */
static int test_apply_url_timerange(void)
{
    static const struct {
        const char *flow_tr, *status, *url_tr, *expected;
    } cases[] = {
        { "[0:0_10:0)",  "closed_complete", "[5:0_20:0)",  "[5:0_10:0)"  }, /* overlap */
        { "[0:0_10:0)",  "closed_complete", "[20:0_30:0)", "()"          }, /* after the end */
        { "[0:0_10:0)",  "ingesting",       "[20:0_30:0)", "[20:0_30:0)" }, /* live: wait */
        { "[0:0_10:0)",  "ingesting",       "[10:0_20:0)", "[10:0_20:0)" }, /* live: from the end */
        { "[10:0_20:0)", "ingesting",       "[0:0_5:0)",   "()"          }, /* live, before the start */
        { "()",          "ingesting",       "[20:0_30:0)", "[20:0_30:0)" }, /* live, no segments yet */
        { "()",          "closed_complete", "[20:0_30:0)", "()"          }, /* nothing to read */
        { "[0:0_10:0)",  "closed_complete", NULL,          "[0:0_10:0)"  }, /* no URL timerange */
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(cases); i++) {
        URLFixture f;
        char out[128];

        if (url_fixture_init(&f, "http://host/flows/abc", cases[i].flow_tr,
                             cases[i].status, cases[i].url_tr) < 0)
            FAIL("apply_url_timerange: fixture init failed (case %d)", i);

        tams_apply_url_timerange(f.s, &f.flow);
        ff_tams_timerange_to_str(&f.flow.timerange, out, sizeof(out));
        url_fixture_free(&f);

        if (strcmp(out, cases[i].expected))
            FAIL("apply_url_timerange: %s %s with %s: expected %s, got %s",
                 cases[i].flow_tr, cases[i].status,
                 cases[i].url_tr ? cases[i].url_tr : "no URL timerange",
                 cases[i].expected, out);
    }

    printf("OK: apply_url_timerange\n");
    return 0;
}

/*
 * A non-live Flow stops at its own (already intersected) end. A live Flow's
 * end is only how far it has been written, so only a URL timerange end counts.
 */
static int test_final_end(void)
{
    static const struct {
        const char *flow_tr, *url_tr;
        int was_live, found;
        int64_t end_ns;
        int end_inclusive;
    } cases[] = {
        { "[0:0_10:0)", NULL,        0, 1, INT64_C(10000000000), 0 },
        { "[0:0_)",     NULL,        0, 0, 0,                    0 },
        { "[0:0_10:0)", NULL,        1, 0, 0,                    0 },
        { "[0:0_10:0)", "[0:0_5:0]", 1, 1, INT64_C(5000000000),  1 },
        { "[0:0_10:0)", "[5:0_)",    1, 0, 0,                    0 },
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(cases); i++) {
        URLFixture f;
        int64_t end_ns = 0;
        int end_inclusive = 0, found;

        if (url_fixture_init(&f, "http://host/flows/abc", cases[i].flow_tr,
                             "", cases[i].url_tr) < 0)
            FAIL("final_end: fixture init failed (case %d)", i);

        f.segc.was_live = cases[i].was_live;
        found = tams_final_end(&f.c, &f.segc, &f.flow, &end_ns, &end_inclusive);
        url_fixture_free(&f);

        if (found != cases[i].found)
            FAIL("final_end: case %d: expected found=%d, got %d", i, cases[i].found, found);
        if (found && (end_ns != cases[i].end_ns || end_inclusive != cases[i].end_inclusive))
            FAIL("final_end: case %d: expected %"PRId64" inclusive=%d, got %"PRId64" inclusive=%d",
                 i, cases[i].end_ns, cases[i].end_inclusive, end_ns, end_inclusive);
    }

    printf("OK: final_end\n");
    return 0;
}


/* A local file holds all the segments, so there is no next page to request. */
static int test_fetch_segments_local_single_page(void)
{
    URLFixture f;
    int ret;

    if (url_fixture_init(&f, "samples/flows/abc.json", "[0:0_10:0)", "", NULL) < 0)
        FAIL("fetch_segments_local_single_page: fixture init failed");

    f.c.is_local            = 1;
    f.segc.has_fetch_cursor = 1;
    f.segc.fetch_cursor_ns  = INT64_C(5000000000);
    ret = tams_fetch_segments(f.s, &f.segc);
    url_fixture_free(&f);

    if (ret != 0 || f.segc.nb_flow_segments != 0)
        FAIL("fetch_segments_local_single_page: expected 0 and no segments, got %d and %d",
             ret, f.segc.nb_flow_segments);

    printf("OK: fetch_segments_local_single_page\n");
    return 0;
}

/* ====================================================================
 * Probe tests
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

/* ====================================================================
 * Packet mapping/classification test helpers
 * ==================================================================== */

static int packet_fixture_init(PacketFixture *f, int64_t flow_end_ns,
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
    f->sc.next_pts_ns   = AV_NOPTS_VALUE;
    f->sc.prev_seg_end_ns = AV_NOPTS_VALUE;

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
    seg_out->has_ts_offset           = 1;

    return 0;
}

static void packet_fixture_free(PacketFixture *f)
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

/* ====================================================================
 * Packet timestamp mapping tests
 * ==================================================================== */

/*
 * Setup: Flow [0,10s). Segment [0,5s) with ts_offset 0.
 * The segment timebase is 1/1000. The packet is at 1s with a 40ms duration.
 * Expected: pts and dts are 1s in ns. The duration is 40ms in ns.
 * The stream index is the output stream. current_ts_ns is the packet pts.
 */
static int test_map_packet_timestamps_ok(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;

    /* flow [0,10s), segment [0,5s), ts_offset 0; segment timebase is 1/1000 */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("map_packet_timestamps_ok: fixture init failed");

    pkt = make_test_packet(1000, 1000, 40); /* 1s pts/dts, 40ms duration */
    if (!pkt) { packet_fixture_free(&f); FAIL("map_packet_timestamps_ok: alloc packet failed"); }

    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);

    if (pkt->pts != INT64_C(1000000000) || pkt->dts != INT64_C(1000000000))
        FAIL("map_packet_timestamps_ok: wrong pts/dts: %"PRId64"/%"PRId64, pkt->pts, pkt->dts);
    if (pkt->duration != INT64_C(40000000))
        FAIL("map_packet_timestamps_ok: wrong duration: %"PRId64, pkt->duration);
    if (pkt->stream_index != 0)
        FAIL("map_packet_timestamps_ok: wrong stream_index: %d", pkt->stream_index);
    if (f.sc.current_ts_ns != pkt->pts)
        FAIL("map_packet_timestamps_ok: current_ts_ns not updated");

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: map_packet_timestamps_ok\n");
    return 0;
}

/*
 * Setup: Flow starting at 1000s. Segment [1005s,1010s) with ts_offset 1005s.
 * Packets are at container pts 0 and 1s.
 * Expected: they map to 5s and 6s on the Flow timeline.
 * The mapping is pts + ts_offset - flow_start.
 */
static int test_map_packet_timestamps_ts_offset_and_flow_start(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;

    if (packet_fixture_init(&f, INT64_C(1010000000000), INT64_C(1005000000000), 1,
                            INT64_C(1010000000000), 0, INT64_C(1005000000000), &seg) < 0)
        FAIL("map_packet_timestamps_ts_offset_and_flow_start: fixture init failed");
    f.flow.timerange.start = INT64_C(1000000000000);

    pkt = make_test_packet(0, 0, 40);
    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (pkt->pts != INT64_C(5000000000) || pkt->dts != INT64_C(5000000000))
        FAIL("map_packet_timestamps_ts_offset_and_flow_start: pts/dts=%"PRId64"/%"PRId64,
             pkt->pts, pkt->dts);
    av_packet_free(&pkt);

    pkt = make_test_packet(1000, 1000, 40);
    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (pkt->pts != INT64_C(6000000000))
        FAIL("map_packet_timestamps_ts_offset_and_flow_start: second pts=%"PRId64, pkt->pts);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: map_packet_timestamps_ts_offset_and_flow_start\n");
    return 0;
}

/*
 * Setup: Flow [0,10s). Segment [0,5s) with ts_offset 0.
 * A first packet is mapped at 0. The segment's ts_offset is then changed.
 * A second packet is at 500ms.
 * Expected: the offset is cached from the first packet.
 * The second packet maps to 500ms. The changed ts_offset is ignored.
 */
static int test_map_packet_timestamps_offset_cached_once(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;

    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("map_packet_timestamps_offset_cached_once: fixture init failed");

    pkt = make_test_packet(0, 0, 0);
    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (pkt->pts != 0)
        FAIL("map_packet_timestamps_offset_cached_once: first packet pts=%"PRId64, pkt->pts);
    av_packet_free(&pkt);

    if (!f.segc.cur_pts_offset_set || f.segc.cur_pts_offset_ns != 0)
        FAIL("map_packet_timestamps_offset_cached_once: offset should be cached as 0");

    /* mutate the segment's ts_offset as if a later packet carried a
     * different value; the cached offset must NOT change mid-segment */
    seg.ts_offset = INT64_C(999000000000);

    pkt = make_test_packet(500, 500, 0); /* 500ms in */
    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (pkt->pts != INT64_C(500000000))
        FAIL("map_packet_timestamps_offset_cached_once: cached offset was not honoured, pts=%"PRId64,
             pkt->pts);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: map_packet_timestamps_offset_cached_once\n");
    return 0;
}

/*
 * Setup: Segment [5s,10s) with no ts_offset.
 * Its media timestamps start at 3600s. MPEG-TS often does this. Packets are at 3600s and 3601s.
 * Expected: the offset is inferred from the first packet. The packets map to 5s and 6s.
 */
static int test_map_packet_timestamps_infer_missing_ts_offset(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;

    /* segment [5s,10s) without ts_offset whose media starts at 3600s (as
     * MPEG-TS often does): the offset is inferred from the first packet */
    if (packet_fixture_init(&f, INT64_C(10000000000), INT64_C(5000000000), 1,
                            INT64_C(10000000000), 0, 0, &seg) < 0)
        FAIL("map_packet_timestamps_infer_ts_offset: fixture init failed");
    seg.has_ts_offset = 0;

    pkt = make_test_packet(3600000, 3600000, 40);
    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (pkt->pts != INT64_C(5000000000))
        FAIL("map_packet_timestamps_infer_ts_offset: pts=%"PRId64, pkt->pts);
    av_packet_free(&pkt);

    pkt = make_test_packet(3601000, 3601000, 40);
    tams_map_packet_timestamps(f.s, &f.segc, &seg, &f.sc, 0, pkt);
    if (pkt->pts != INT64_C(6000000000))
        FAIL("map_packet_timestamps_infer_ts_offset: second pts=%"PRId64, pkt->pts);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: map_packet_timestamps_infer_missing_ts_offset\n");
    return 0;
}

/* ====================================================================
 * Packet classification tests
 * ==================================================================== */

/*
 * Setup: Flow [0,10s). Segment [0,5s). The packet is at 1s with a 40ms duration.
 * Expected: the packet is kept (TAMS_PKT_OK). next_pts_ns moves to the packet end.
 */
static int test_classify_packet_ok(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* flow [0,10s), segment [0,5s) */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_ok: fixture init failed");

    pkt = make_test_packet(INT64_C(1000000000), INT64_C(1000000000), INT64_C(40000000));
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);

    if (ret != TAMS_PKT_OK)
        FAIL("classify_packet_ok: expected TAMS_PKT_OK, got %d", ret);
    if (f.sc.next_pts_ns != pkt->pts + pkt->duration)
        FAIL("classify_packet_ok: next_pts_ns not updated");

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_ok\n");
    return 0;
}

/*
 * Setup: Segment (0s,5s) with an exclusive start. The packet is at 0s.
 * Expected: the packet is discarded (TAMS_PKT_DISCARD).
 */
static int test_classify_packet_discard_at_exclusive_segment_start(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* exclusive segment start: pts exactly at the boundary must discard */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 0,
                            INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_discard_start: fixture init failed");

    pkt = make_test_packet(0, 0, 0);
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);

    if (ret != TAMS_PKT_DISCARD)
        FAIL("classify_packet_discard_start: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_discard_at_exclusive_segment_start\n");
    return 0;
}

/*
 * Setup: Segment [0,5s). The packet pts and dts are both 6s.
 * Expected: the segment ends (TAMS_PKT_EOSEG).
 */
static int test_classify_packet_eoseg(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* segment [0,5s) exclusive end; both pts and dts past the end -> EOSEG */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_eoseg: fixture init failed");

    pkt = make_test_packet(INT64_C(6000000000), INT64_C(6000000000), 0); /* 6s, past the 5s segment end */
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);

    if (ret != TAMS_PKT_EOSEG)
        FAIL("classify_packet_eoseg: expected TAMS_PKT_EOSEG, got %d", ret);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_eoseg\n");
    return 0;
}

/*
 * Setup: Segment [0,5s). The packet pts is 6s and its dts is 4s. This is B-frame reordering.
 * Expected: the packet is discarded (TAMS_PKT_DISCARD).
 */
static int test_classify_packet_discard_still_needed_at_segment_end(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* pts past segment end, but dts still within it (B-frame reordering):
     * must DISCARD, not EOSEG, so the decoder still gets the reference. */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(5000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_discard_seg_end: fixture init failed");

    pkt = make_test_packet(INT64_C(6000000000), INT64_C(4000000000), 0); /* pts=6s, dts=4s (< 5s seg end) */
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);

    if (ret != TAMS_PKT_DISCARD)
        FAIL("classify_packet_discard_seg_end: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_discard_still_needed_at_segment_end\n");
    return 0;
}

/*
 * Setup: Flow [0,10s). Segment [0,20s) extends past the Flow end. The packet pts and dts are both 11s.
 * Expected: the stream ends (TAMS_PKT_EOF).
 */
static int test_classify_packet_eof(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* flow ends at 10s; segment end is unbounded within the flow window so
     * only the flow-end boundary is exercised here */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(20000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_eof: fixture init failed");

    pkt = make_test_packet(INT64_C(11000000000), INT64_C(11000000000), 0); /* 11s: past flow end, dts too */
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);

    if (ret != TAMS_PKT_EOF)
        FAIL("classify_packet_eof: expected TAMS_PKT_EOF, got %d", ret);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_eof\n");
    return 0;
}

/*
 * Setup: Flow [0,10s). Segment [0,20s). The packet pts is 11s and its dts is 9s.
 * Expected: the packet is discarded (TAMS_PKT_DISCARD).
 */
static int test_classify_packet_discard_still_needed_at_flow_end(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* pts past flow end, but dts still within it: must DISCARD, not EOF */
    if (packet_fixture_init(&f, INT64_C(10000000000), 0, 1,
                            INT64_C(20000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_discard_flow_end: fixture init failed");

    pkt = make_test_packet(INT64_C(11000000000), INT64_C(9000000000), 0); /* pts=11s, dts=9s (< 10s flow end) */
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);

    if (ret != TAMS_PKT_DISCARD)
        FAIL("classify_packet_discard_flow_end: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_discard_still_needed_at_flow_end\n");
    return 0;
}

/*
 * Setup: Segment [5s,10s). The stream delivered up to 4.96s before it.
 * One packet is at 4.96s. It is new: it starts where the stream ended.
 * Another is at 4.9s. The stream already delivered it.
 * New packets before a segment start are normal. Audio after a keyframe
 * cut is one cause. Open-GOP leading pictures are another.
 * Expected: the packet at 4.96s is kept (TAMS_PKT_OK).
 * The packet at 4.9s is discarded (TAMS_PKT_DISCARD).
 */
static int test_classify_packet_keep_continuation_before_segment_start(void)
{
    PacketFixture f;
    TAMSFlowSegment seg;
    AVPacket *pkt;
    int ret;

    /* segment [5s,10s); before it this stream delivered up to 4.96s, so a
     * packet at 4.96s that landed in this segment (audio after a keyframe
     * cut, or an open-GOP leading picture) is kept */
    if (packet_fixture_init(&f, INT64_C(10000000000), INT64_C(5000000000), 1,
                            INT64_C(10000000000), 0, 0, &seg) < 0)
        FAIL("classify_packet_continuation: fixture init failed");
    f.sc.prev_seg_end_ns = INT64_C(4960000000);
    f.sc.next_pts_ns     = INT64_C(6000000000); /* a later keyframe came first */

    pkt = make_test_packet(INT64_C(4960000000), INT64_C(4960000000), INT64_C(20000000));
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);
    if (ret != TAMS_PKT_OK)
        FAIL("classify_packet_continuation: expected TAMS_PKT_OK, got %d", ret);
    av_packet_free(&pkt);

    /* but a packet already covered by what was delivered is discarded */
    pkt = make_test_packet(INT64_C(4900000000), INT64_C(4900000000), INT64_C(20000000));
    ret = tams_classify_packet(f.s, &f.segc, &seg, &f.sc, pkt);
    if (ret != TAMS_PKT_DISCARD)
        FAIL("classify_packet_continuation: expected TAMS_PKT_DISCARD, got %d", ret);

    av_packet_free(&pkt);
    packet_fixture_free(&f);
    printf("OK: classify_packet_keep_continuation_before_segment_start\n");
    return 0;
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(int argc, char *argv[])
{
    int ret = 0;

    ret |= test_segment_duration_us();
    ret |= test_find_flow_by_id();

    ret |= test_is_live_recent();
    ret |= test_is_live_stale();
    ret |= test_is_live_no_segments_updated();
    ret |= test_is_live_status();
    ret |= test_is_live_expiry_uses_reader_activity();

    ret |= test_poll_init_us();
    ret |= test_min_buffer_ns();
    ret |= test_buffered_ns();

    ret |= test_compact_segments();
    ret |= test_compact_segments_all_consumed();

    ret |= test_build_base_url();
    ret |= test_build_clean_query();

    ret |= test_apply_url_timerange();
    ret |= test_final_end();
    ret |= test_build_segments_url();
    ret |= test_fetch_segments_local_single_page();

    ret |= test_probe_format_urn();
    ret |= test_probe_fallback_keys();
    ret |= test_probe_rejects_non_json();

    ret |= test_map_packet_timestamps_ok();
    ret |= test_map_packet_timestamps_ts_offset_and_flow_start();
    ret |= test_map_packet_timestamps_offset_cached_once();
    ret |= test_map_packet_timestamps_infer_missing_ts_offset();

    ret |= test_classify_packet_ok();
    ret |= test_classify_packet_discard_at_exclusive_segment_start();
    ret |= test_classify_packet_eoseg();
    ret |= test_classify_packet_discard_still_needed_at_segment_end();
    ret |= test_classify_packet_eof();
    ret |= test_classify_packet_discard_still_needed_at_flow_end();
    ret |= test_classify_packet_keep_continuation_before_segment_start();

    if (ret)
        printf("\nSOME TESTS FAILED\n");
    else
        printf("\nALL TESTS PASSED\n");

    return ret;
}
