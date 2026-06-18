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
 * Tests for TAMS Flow processing
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#include "libavformat/tams.c"

#include "libavutil/mem.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define FAIL(msg, ...) do { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); \
    return 1; \
} while (0)

/* ====================================================================
 * Flow JSON test data
 * ==================================================================== */

const char *flow_json_video_raw =
    "{"
    "    \"id\": \"0fde9c11-da9d-434a-a113-d3b20a2cf251\","
    "    \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
    "    \"generation\": 0,"
    "    \"created\": \"2018-03-06T09:10:22Z\","
    "    \"metadata_updated\": \"2018-03-06T09:12:22Z\","
    "    \"segments_updated\": \"2018-03-06T11:14:32Z\","
    "    \"description\": \"video capture\","
    "    \"label\": \"capture_1\","
    "    \"format\": \"urn:x-nmos:format:video\","
    "    \"created_by\": \"tams-dev\","
    "    \"updated_by\": \"tams-dev\","
    "    \"tags\": {"
    "        \"input_quality\": \"intermediate\""
    "    },"
    "    \"codec\": \"video/raw\","
    "    \"container\": \"video/quicktime\","
    "    \"avg_bit_rate\": 1658880,"
    "    \"essence_parameters\": {"
    "        \"frame_rate\": {"
    "            \"numerator\": 50,"
    "            \"denominator\": 1"
    "        },"
    "        \"frame_width\": 1920,"
    "        \"frame_height\": 1080,"
    "        \"bit_depth\": 8,"
    "        \"interlace_mode\": \"progressive\","
    "        \"colorspace\": \"BT709\","
    "        \"transfer_characteristic\": \"SDR\","
    "        \"aspect_ratio\": {"
    "            \"numerator\": 16,"
    "            \"denominator\": 9"
    "        },"
    "        \"pixel_aspect_ratio\": {"
    "            \"numerator\": 1,"
    "            \"denominator\": 1"
    "        },"
    "        \"component_type\": \"YCbCr\","
    "        \"unc_parameters\": {"
    "            \"unc_type\": \"UYVY\""
    "        },"
    "        \"vert_chroma_subs\": 1,"
    "        \"horiz_chroma_subs\": 2"
    "    }"
    "}";

const char *flow_json_video_h264_vfr =
    "{"
    "    \"id\": \"4f79cfd1-c057-47f4-8e4d-1b126ca7bf34\","
    "    \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
    "    \"generation\": 0,"
    "    \"created\": \"2008-05-27T18:51:00Z\","
    "    \"metadata_updated\": \"2023-09-14T09:45:26Z\","
    "    \"segments_updated\": \"2023-09-14T09:45:26Z\","
    "    \"description\": \"Big Buck Bunny\","
    "    \"label\": \"bbb\","
    "    \"format\": \"urn:x-nmos:format:video\","
    "    \"created_by\": \"tams-dev\","
    "    \"updated_by\": \"tams-dev\","
    "    \"tags\": {"
    "        \"input_quality\": \"contribution\","
    "        \"_tams_segmentation_rate\": \"10\""
    "    },"
    "    \"codec\": \"video/h264\","
    "    \"container\": \"video/mp2t\","
    "    \"avg_bit_rate\": 2479,"
    "    \"essence_parameters\": {"
    "        \"vfr\": true,"
    "        \"frame_width\": 1280,"
    "        \"frame_height\": 720,"
    "        \"bit_depth\": 8,"
    "        \"interlace_mode\": \"progressive\","
    "        \"colorspace\": \"BT709\","
    "        \"transfer_characteristic\": \"SDR\","
    "        \"aspect_ratio\": {"
    "            \"numerator\": 16,"
    "            \"denominator\": 9"
    "        },"
    "        \"pixel_aspect_ratio\": {"
    "            \"numerator\": 1,"
    "            \"denominator\": 1"
    "        },"
    "        \"component_type\": \"YCbCr\","
    "        \"vert_chroma_subs\": 2,"
    "        \"horiz_chroma_subs\": 2,"
    "        \"avc_parameters\": {"
    "            \"profile\": 100,"
    "            \"level\": 31,"
    "            \"flags\": 0"
    "        }"
    "    },"
    "    \"collected_by\": ["
    "        \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\""
    "    ]"
    "}";

const char *flow_json_audio_aac_multi =
    "{"
    "    \"id\": \"94996f2e-0cb5-43d3-ab6c-db5a9cf667aa\","
    "    \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
    "    \"generation\": 1,"
    "    \"created\": \"2018-03-06T09:10:22Z\","
    "    \"metadata_updated\": \"2018-03-06T09:12:22Z\","
    "    \"segments_updated\": \"2018-03-06T11:14:32Z\","
    "    \"description\": \"audio capture web\","
    "    \"label\": \"capture_1\","
    "    \"format\": \"urn:x-nmos:format:audio\","
    "    \"created_by\": \"tams-dev\","
    "    \"updated_by\": \"tams-dev\","
    "    \"tags\": {"
    "        \"quality\": \"web\""
    "    },"
    "    \"codec\": \"audio/aac\","
    "    \"avg_bit_rate\": 128,"
    "    \"essence_parameters\": {"
    "        \"sample_rate\": 48000,"
    "        \"channels\": 2,"
    "        \"bit_depth\": 24,"
    "        \"codec_parameters\": {"
    "            \"coded_frame_size\": 1024,"
    "            \"mp4_oti\": 2"
    "        }"
    "    },"
    "    \"collected_by\": ["
    "        \"8159b781-6033-42e8-b3a1-8a879af5e1aa\""
    "    ]"
    "}";

const char *flow_json_data_ttml =
    "{"
    "    \"id\": \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\","
    "    \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
    "    \"format\": \"urn:x-nmos:format:data\","
    "    \"generation\": 0,"
    "    \"created\": \"2023-12-14T16:34:58.131620\","
    "    \"codec\": \"application/ttml+xml\","
    "    \"container\": \"application/ttml+xml\","
    "    \"essence_parameters\": {"
    "        \"data_type\": \"urn:x-tams:data:subtitle\""
    "    },"
    "    \"collected_by\": ["
    "        \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\""
    "    ]"
    "}";

/* Flow with partial essence_parameters (only required fields) */
const char *flow_json_video_minimal =
    "{"
    "    \"id\": \"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
    "    \"source_id\": \"11111111-2222-3333-4444-555555555555\","
    "    \"format\": \"urn:x-nmos:format:video\","
    "    \"codec\": \"video/h264\","
    "    \"essence_parameters\": {"
    "        \"frame_width\": 640,"
    "        \"frame_height\": 480"
    "    }"
    "}";

/* Flow with timerange and additional core fields */
const char *flow_json_with_timerange =
    "{"
    "    \"id\": \"bbbbbbbb-cccc-dddd-eeee-ffffffffffff\","
    "    \"source_id\": \"11111111-2222-3333-4444-555555555555\","
    "    \"format\": \"urn:x-nmos:format:video\","
    "    \"codec\": \"video/h264\","
    "    \"timerange\": \"[0:0_10:500000000)\","
    "    \"read_only\": true,"
    "    \"max_bit_rate\": 5000000,"
    "    \"segment_duration\": {\"numerator\": 6, \"denominator\": 1},"
    "    \"metadata_version\": \"v1.2.3\","
    "    \"essence_parameters\": {"
    "        \"frame_width\": 3840,"
    "        \"frame_height\": 2160,"
    "        \"frame_rate\": {\"numerator\": 60},"
    "        \"bit_depth\": 10,"
    "        \"interlace_mode\": \"interlaced_tff\","
    "        \"colorspace\": \"BT2020\","
    "        \"transfer_characteristic\": \"HLG\","
    "        \"component_type\": \"RGB\","
    "        \"horiz_chroma_subs\": 1,"
    "        \"vert_chroma_subs\": 1"
    "    }"
    "}";

/* Audio flow with unc_parameters */
const char *flow_json_audio_unc =
    "{"
    "    \"id\": \"cccccccc-dddd-eeee-ffff-aaaaaaaaaaaa\","
    "    \"source_id\": \"11111111-2222-3333-4444-555555555555\","
    "    \"format\": \"urn:x-nmos:format:audio\","
    "    \"codec\": \"audio/pcm\","
    "    \"essence_parameters\": {"
    "        \"sample_rate\": 96000,"
    "        \"channels\": 8,"
    "        \"bit_depth\": 32,"
    "        \"unc_parameters\": {"
    "            \"unc_type\": \"planar\""
    "        }"
    "    }"
    "}";

/* Multi-flow JSON test data */
const char *flow_json_multi_separate =
    "{"
    "    \"id\": \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\","
    "    \"source_id\": \"a77d0061-0878-4e8a-a114-772d03f952c1\","
    "    \"format\": \"urn:x-nmos:format:multi\","
    "    \"generation\": 0,"
    "    \"created\": \"2023-12-14T16:34:58.131620\","
    "    \"tags\": {"
    "        \"ingested_by\": \"ingest_service_api\""
    "    },"
    "    \"flow_collection\": ["
    "        {"
    "            \"id\": \"4f79cfd1-c057-47f4-8e4d-1b126ca7bf34\","
    "            \"role\": \"video\""
    "        },"
    "        {"
    "            \"id\": \"6101df05-06bb-41b8-8af4-cf7cd33df209\","
    "            \"role\": \"audio\""
    "        },"
    "        {"
    "            \"id\": \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\","
    "            \"role\": \"subtitles\""
    "        }"
    "    ]"
    "}";

const char *flow_json_multi_container_map =
    "{"
    "    \"id\": \"8159b781-6033-42e8-b3a1-8a879af5e1aa\","
    "    \"source_id\": \"3a24e39d-c6cf-4632-8c9e-36a8d305f007\","
    "    \"format\": \"urn:x-nmos:format:multi\","
    "    \"generation\": 0,"
    "    \"created\": \"2024-04-25T15:56:00.0Z\","
    "    \"tags\": {"
    "        \"ingested_by\": \"ingest_service_api\""
    "    },"
    "    \"container\": \"video/mp2t\","
    "    \"flow_collection\": ["
    "        {"
    "            \"id\": \"32b75860-83e5-48c9-8d6b-de46bdbc7f80\","
    "            \"role\": \"video\""
    "        },"
    "        {"
    "            \"id\": \"94996f2e-0cb5-43d3-ab6c-db5a9cf667aa\","
    "            \"role\": \"L\","
    "            \"container_mapping\": {"
    "                \"track_index\": 1,"
    "                \"format_track_index\": 0,"
    "                \"mp2ts_container\": {"
    "                    \"pid\": 257"
    "                }"
    "            }"
    "        },"
    "        {"
    "            \"id\": \"9ba0523e-22a0-4c69-a598-baf4be244a79\","
    "            \"role\": \"R\","
    "            \"container_mapping\": {"
    "                \"track_index\": 2,"
    "                \"format_track_index\": 1,"
    "                \"mp2ts_container\": {"
    "                    \"pid\": 258"
    "                }"
    "            }"
    "        }"
    "    ]"
    "}";

/* Segment JSON test data */
const char *segment_json =
    "{"
    "    \"object_id\": \"seg-001.ts\","
    "    \"timerange\": \"[0:0_5:0)\","
    "    \"ts_offset\": \"-0:700000000\","
    "    \"last_duration\": \"0:40000000\","
    "    \"get_urls\": ["
    "        {\"url\": \"https://example.com/seg-001.ts\", \"content_type\": \"video/mp2t\"}"
    "    ]"
    "}";

const char *segment_json_minimal =
    "{"
    "    \"object_id\": \"seg-002.mp4\","
    "    \"timerange\": \"[5:0_10:0)\""
    "}";

const char *segment_json_presigned =
    "{"
    "    \"object_id\": \"seg-004.ts\","
    "    \"timerange\": \"[20:0_25:0)\","
    "    \"get_urls\": ["
    "        {\"url\": \"https://api.example.com/seg-004.ts\", \"label\": \"direct\"},"
    "        {\"url\": \"https://s3.example.com/seg-004.ts?Signature=abc&Token=xyz\", \"presigned\": true}"
    "    ]"
    "}";

const char *segment_json_null_fields =
    "{"
    "    \"object_id\": \"seg-003.ts\","
    "    \"timerange\": \"[10:0_15:0)\","
    "    \"ts_offset\": null,"
    "    \"last_duration\": null"
    "}";

/* ====================================================================
 * Test helpers
 * ==================================================================== */

static int test_flow_parsing(const char *test_name, const char *flow_json,
                             TAMSFlow *out_flow)
{
    const char *cursor = flow_json;
    TAMSFlow flow;
    int ret;

    ret = ff_tams_parse_flow(&cursor, &flow);
    if (ret < 0)
        FAIL("%s: ff_tams_parse_flow returned %d", test_name, ret);

    if (!flow.id[0])
        FAIL("%s: id not parsed", test_name);
    if (!flow.source_id[0])
        FAIL("%s: source_id not parsed", test_name);
    if (flow.format == TAMS_FORMAT_UNKNOWN)
        FAIL("%s: format not parsed", test_name);

    if (out_flow)
        *out_flow = flow;

    printf("OK: %s (id=%s format=%d)\n", test_name, flow.id, flow.format);
    return 0;
}

static int test_flow_parsing_fails(const char *test_name, const char *flow_json)
{
    const char *cursor = flow_json;
    TAMSFlow flow;
    int ret;

    ret = ff_tams_parse_flow(&cursor, &flow);
    if (ret >= 0)
        FAIL("%s: expected failure but got success", test_name);

    printf("OK: %s (correctly failed with %d)\n", test_name, ret);
    return 0;
}

/* ====================================================================
 * Individual test functions
 * ==================================================================== */

static int test_video_raw(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("video_raw", flow_json_video_raw, &flow))
        return 1;

    if (strcmp(flow.id, "0fde9c11-da9d-434a-a113-d3b20a2cf251"))
        FAIL("video_raw: wrong id: %s", flow.id);
    if (strcmp(flow.source_id, "2aa143ac-0ab7-4d75-bc32-5c00c13d186f"))
        FAIL("video_raw: wrong source_id");
    if (flow.format != TAMS_FORMAT_VIDEO)
        FAIL("video_raw: wrong format %d", flow.format);
    if (strcmp(flow.codec, "video/raw"))
        FAIL("video_raw: wrong codec: %s", flow.codec);
    if (strcmp(flow.container, "video/quicktime"))
        FAIL("video_raw: wrong container: %s", flow.container);
    if (strcmp(flow.label, "capture_1"))
        FAIL("video_raw: wrong label: %s", flow.label);
    if (strcmp(flow.description, "video capture"))
        FAIL("video_raw: wrong description");
    if (flow.avg_bit_rate != 1658880)
        FAIL("video_raw: wrong avg_bit_rate: %d", flow.avg_bit_rate);
    if (flow.generation != 0)
        FAIL("video_raw: wrong generation");
    if (strcmp(flow.created_by, "tams-dev"))
        FAIL("video_raw: wrong created_by: %s", flow.created_by);
    if (strcmp(flow.updated_by, "tams-dev"))
        FAIL("video_raw: wrong updated_by");
    if (strcmp(flow.created, "2018-03-06T09:10:22Z"))
        FAIL("video_raw: wrong created: %s", flow.created);
    if (strcmp(flow.metadata_updated, "2018-03-06T09:12:22Z"))
        FAIL("video_raw: wrong metadata_updated");
    if (strcmp(flow.segments_updated, "2018-03-06T11:14:32Z"))
        FAIL("video_raw: wrong segments_updated");

    /* Tags */
    if (flow.nb_tags != 1)
        FAIL("video_raw: expected 1 tag, got %d", flow.nb_tags);
    if (strcmp(flow.tags[0].key, "input_quality") || strcmp(flow.tags[0].value, "intermediate"))
        FAIL("video_raw: wrong tag: %s=%s", flow.tags[0].key, flow.tags[0].value);

    /* Video essence */
    if (flow.frame_width != 1920)
        FAIL("video_raw: wrong frame_width: %d", flow.frame_width);
    if (flow.frame_height != 1080)
        FAIL("video_raw: wrong frame_height: %d", flow.frame_height);
    if (flow.frame_rate.num != 50 || flow.frame_rate.den != 1)
        FAIL("video_raw: wrong frame_rate: %d/%d", flow.frame_rate.num, flow.frame_rate.den);
    if (flow.bit_depth != 8)
        FAIL("video_raw: wrong bit_depth: %d", flow.bit_depth);
    if (flow.interlace_mode != TAMS_INTERLACE_PROGRESSIVE)
        FAIL("video_raw: wrong interlace_mode: %d", flow.interlace_mode);
    if (flow.colorspace != TAMS_COLORSPACE_BT709)
        FAIL("video_raw: wrong colorspace: %d", flow.colorspace);
    if (flow.transfer_characteristic != TAMS_TRANSFER_SDR)
        FAIL("video_raw: wrong transfer_characteristic");
    if (flow.aspect_ratio.num != 16 || flow.aspect_ratio.den != 9)
        FAIL("video_raw: wrong aspect_ratio");
    if (flow.pixel_aspect_ratio.num != 1 || flow.pixel_aspect_ratio.den != 1)
        FAIL("video_raw: wrong pixel_aspect_ratio");
    if (flow.component_type != TAMS_COMPONENT_YCBCR)
        FAIL("video_raw: wrong component_type");
    if (flow.horiz_chroma_subs != 2)
        FAIL("video_raw: wrong horiz_chroma_subs: %d", flow.horiz_chroma_subs);
    if (flow.vert_chroma_subs != 1)
        FAIL("video_raw: wrong vert_chroma_subs: %d", flow.vert_chroma_subs);

    /* Video unc_parameters */
    if (flow.video_unc_type != TAMS_VIDEO_UNC_UYVY)
        FAIL("video_raw: wrong video_unc_type: %d (expected TAMS_VIDEO_UNC_UYVY=%d)",
             flow.video_unc_type, TAMS_VIDEO_UNC_UYVY);

    return 0;
}

static int test_video_h264_vfr(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("video_h264_vfr", flow_json_video_h264_vfr, &flow))
        return 1;

    if (strcmp(flow.id, "4f79cfd1-c057-47f4-8e4d-1b126ca7bf34"))
        FAIL("video_h264_vfr: wrong id");
    if (flow.format != TAMS_FORMAT_VIDEO)
        FAIL("video_h264_vfr: wrong format");
    if (strcmp(flow.codec, "video/h264"))
        FAIL("video_h264_vfr: wrong codec");
    if (flow.frame_width != 1280 || flow.frame_height != 720)
        FAIL("video_h264_vfr: wrong dimensions: %dx%d", flow.frame_width, flow.frame_height);
    if (!flow.vfr)
        FAIL("video_h264_vfr: vfr should be true");
    if (flow.horiz_chroma_subs != 2 || flow.vert_chroma_subs != 2)
        FAIL("video_h264_vfr: wrong chroma subs: %dx%d", flow.horiz_chroma_subs, flow.vert_chroma_subs);

    /* Two tags */
    if (flow.nb_tags != 2)
        FAIL("video_h264_vfr: expected 2 tags, got %d", flow.nb_tags);

    /* avc_parameters */
    if (!flow.has_avc_parameters)
        FAIL("video_h264_vfr: has_avc_parameters should be set");
    if (flow.avc_parameters.profile != 100)
        FAIL("video_h264_vfr: wrong avc profile: %d", flow.avc_parameters.profile);
    if (flow.avc_parameters.level != 31)
        FAIL("video_h264_vfr: wrong avc level: %d", flow.avc_parameters.level);
    if (flow.avc_parameters.flags != 0)
        FAIL("video_h264_vfr: wrong avc flags: %d", flow.avc_parameters.flags);

    return 0;
}

static int test_audio_aac(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("audio_aac", flow_json_audio_aac_multi, &flow))
        return 1;

    if (flow.format != TAMS_FORMAT_AUDIO)
        FAIL("audio_aac: wrong format");
    if (strcmp(flow.codec, "audio/aac"))
        FAIL("audio_aac: wrong codec");
    if (flow.generation != 1)
        FAIL("audio_aac: wrong generation: %d", flow.generation);
    if (flow.sample_rate != 48000)
        FAIL("audio_aac: wrong sample_rate: %d", flow.sample_rate);
    if (flow.channels != 2)
        FAIL("audio_aac: wrong channels: %d", flow.channels);
    if (flow.bit_depth != 24)
        FAIL("audio_aac: wrong bit_depth: %d", flow.bit_depth);
    if (flow.coded_frame_size != 1024)
        FAIL("audio_aac: wrong coded_frame_size: %d", flow.coded_frame_size);
    if (flow.mp4_oti != 2)
        FAIL("audio_aac: wrong mp4_oti: %d", flow.mp4_oti);

    return 0;
}

static int test_data_ttml(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("data_ttml", flow_json_data_ttml, &flow))
        return 1;

    if (flow.format != TAMS_FORMAT_DATA)
        FAIL("data_ttml: wrong format");
    if (strcmp(flow.codec, "application/ttml+xml"))
        FAIL("data_ttml: wrong codec: %s", flow.codec);
    if (strcmp(flow.data_type, "urn:x-tams:data:subtitle"))
        FAIL("data_ttml: wrong data_type: %s", flow.data_type);

    return 0;
}

static int test_video_minimal(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("video_minimal", flow_json_video_minimal, &flow))
        return 1;

    if (flow.frame_width != 640 || flow.frame_height != 480)
        FAIL("video_minimal: wrong dimensions: %dx%d", flow.frame_width, flow.frame_height);
    /* Default pixel_aspect_ratio should be 1:1 */
    if (flow.pixel_aspect_ratio.num != 1 || flow.pixel_aspect_ratio.den != 1)
        FAIL("video_minimal: wrong default pixel_aspect_ratio");
    /* Optional fields should be zero/unknown */
    if (flow.bit_depth != 0)
        FAIL("video_minimal: bit_depth should be 0");
    if (flow.interlace_mode != TAMS_INTERLACE_UNKNOWN)
        FAIL("video_minimal: interlace_mode should be unknown");
    if (flow.nb_tags != 0)
        FAIL("video_minimal: should have no tags");

    return 0;
}

static int test_flow_with_timerange(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("with_timerange", flow_json_with_timerange, &flow))
        return 1;

    if (!flow.timerange.has_start || !flow.timerange.has_end)
        FAIL("with_timerange: timerange not fully parsed");
    if (flow.timerange.start != 0)
        FAIL("with_timerange: wrong start: %"PRId64, flow.timerange.start);
    if (flow.timerange.end != INT64_C(10500000000))
        FAIL("with_timerange: wrong end: %"PRId64, flow.timerange.end);
    if (!flow.timerange.start_inclusive)
        FAIL("with_timerange: start should be inclusive");
    if (flow.timerange.end_inclusive)
        FAIL("with_timerange: end should be exclusive");
    if (!flow.read_only)
        FAIL("with_timerange: read_only should be true");
    if (flow.max_bit_rate != 5000000)
        FAIL("with_timerange: wrong max_bit_rate: %d", flow.max_bit_rate);
    if (flow.segment_duration.num != 6 || flow.segment_duration.den != 1)
        FAIL("with_timerange: wrong segment_duration: %d/%d",
             flow.segment_duration.num, flow.segment_duration.den);
    if (strcmp(flow.metadata_version, "v1.2.3"))
        FAIL("with_timerange: wrong metadata_version: %s", flow.metadata_version);

    /* Video essence with non-default values */
    if (flow.frame_width != 3840 || flow.frame_height != 2160)
        FAIL("with_timerange: wrong dimensions");
    if (flow.frame_rate.num != 60 || flow.frame_rate.den != 1)
        FAIL("with_timerange: wrong frame_rate");
    if (flow.bit_depth != 10)
        FAIL("with_timerange: wrong bit_depth");
    if (flow.interlace_mode != TAMS_INTERLACE_TFF)
        FAIL("with_timerange: wrong interlace_mode");
    if (flow.colorspace != TAMS_COLORSPACE_BT2020)
        FAIL("with_timerange: wrong colorspace");
    if (flow.transfer_characteristic != TAMS_TRANSFER_HLG)
        FAIL("with_timerange: wrong transfer_characteristic");
    if (flow.component_type != TAMS_COMPONENT_RGB)
        FAIL("with_timerange: wrong component_type");

    return 0;
}

static int test_audio_unc(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("audio_unc", flow_json_audio_unc, &flow))
        return 1;

    if (flow.sample_rate != 96000)
        FAIL("audio_unc: wrong sample_rate: %d", flow.sample_rate);
    if (flow.channels != 8)
        FAIL("audio_unc: wrong channels: %d", flow.channels);
    if (flow.bit_depth != 32)
        FAIL("audio_unc: wrong bit_depth");
    if (flow.audio_unc_type != TAMS_AUDIO_UNC_PLANAR)
        FAIL("audio_unc: wrong audio_unc_type: %d", flow.audio_unc_type);

    return 0;
}

static int test_multi_separate(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("multi_separate", flow_json_multi_separate, &flow))
        return 1;

    if (flow.format != TAMS_FORMAT_MULTI)
        FAIL("multi_separate: wrong format %d", flow.format);
    if (strcmp(flow.id, "e85efab4-993b-4ad6-9af3-4cd8d0d38860"))
        FAIL("multi_separate: wrong id");
    if (flow.nb_flow_collection_items != 3)
        FAIL("multi_separate: expected 3 collection items, got %d",
             flow.nb_flow_collection_items);

    if (strcmp(flow.flow_collection_items[0].id,
              "4f79cfd1-c057-47f4-8e4d-1b126ca7bf34"))
        FAIL("multi_separate: wrong collection[0].id");
    if (strcmp(flow.flow_collection_items[0].role, "video"))
        FAIL("multi_separate: wrong collection[0].role");
    if (flow.flow_collection_items[0].has_container_mapping)
        FAIL("multi_separate: collection[0] should not have container_mapping");

    if (strcmp(flow.flow_collection_items[1].id,
              "6101df05-06bb-41b8-8af4-cf7cd33df209"))
        FAIL("multi_separate: wrong collection[1].id");
    if (strcmp(flow.flow_collection_items[1].role, "audio"))
        FAIL("multi_separate: wrong collection[1].role");

    if (strcmp(flow.flow_collection_items[2].role, "subtitles"))
        FAIL("multi_separate: wrong collection[2].role");

    return 0;
}

static int test_multi_container_map(void)
{
    TAMSFlow flow;
    if (test_flow_parsing("multi_container_map", flow_json_multi_container_map, &flow))
        return 1;

    if (flow.format != TAMS_FORMAT_MULTI)
        FAIL("multi_container_map: wrong format %d", flow.format);
    if (strcmp(flow.id, "8159b781-6033-42e8-b3a1-8a879af5e1aa"))
        FAIL("multi_container_map: wrong id");
    if (strcmp(flow.container, "video/mp2t"))
        FAIL("multi_container_map: wrong container: %s", flow.container);
    if (flow.nb_flow_collection_items != 3)
        FAIL("multi_container_map: expected 3 collection items, got %d",
             flow.nb_flow_collection_items);

    if (strcmp(flow.flow_collection_items[0].role, "video"))
        FAIL("multi_container_map: wrong collection[0].role");
    if (flow.flow_collection_items[0].has_container_mapping)
        FAIL("multi_container_map: collection[0] should not have container_mapping");

    if (strcmp(flow.flow_collection_items[1].id,
              "94996f2e-0cb5-43d3-ab6c-db5a9cf667aa"))
        FAIL("multi_container_map: wrong collection[1].id");
    if (strcmp(flow.flow_collection_items[1].role, "L"))
        FAIL("multi_container_map: wrong collection[1].role");
    if (!flow.flow_collection_items[1].has_container_mapping)
        FAIL("multi_container_map: collection[1] should have container_mapping");
    if (!flow.flow_collection_items[1].container_mapping.has_track_index ||
        flow.flow_collection_items[1].container_mapping.track_index != 1)
        FAIL("multi_container_map: wrong collection[1].track_index");
    if (!flow.flow_collection_items[1].container_mapping.has_format_track_index ||
        flow.flow_collection_items[1].container_mapping.format_track_index != 0)
        FAIL("multi_container_map: wrong collection[1].format_track_index");
    if (!flow.flow_collection_items[1].container_mapping.has_mp2ts_pid ||
        flow.flow_collection_items[1].container_mapping.mp2ts_pid != 257)
        FAIL("multi_container_map: wrong collection[1].mp2ts_pid");

    if (strcmp(flow.flow_collection_items[2].role, "R"))
        FAIL("multi_container_map: wrong collection[2].role");
    if (!flow.flow_collection_items[2].has_container_mapping)
        FAIL("multi_container_map: collection[2] should have container_mapping");
    if (flow.flow_collection_items[2].container_mapping.track_index != 2)
        FAIL("multi_container_map: wrong collection[2].track_index");
    if (flow.flow_collection_items[2].container_mapping.format_track_index != 1)
        FAIL("multi_container_map: wrong collection[2].format_track_index");
    if (flow.flow_collection_items[2].container_mapping.mp2ts_pid != 258)
        FAIL("multi_container_map: wrong collection[2].mp2ts_pid");

    return 0;
}

static int test_flow_array(void)
{
    const char *flow_json_array =
        "["
        "    {"
        "        \"id\": \"4f79cfd1-c057-47f4-8e4d-1b126ca7bf34\","
        "        \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
        "        \"format\": \"urn:x-nmos:format:video\","
        "        \"codec\": \"video/h264\","
        "        \"essence_parameters\": {"
        "            \"frame_width\": 1280,"
        "            \"frame_height\": 720"
        "        }"
        "    },"
        "    {"
        "        \"id\": \"94996f2e-0cb5-43d3-ab6c-db5a9cf667aa\","
        "        \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
        "        \"format\": \"urn:x-nmos:format:audio\","
        "        \"codec\": \"audio/aac\","
        "        \"essence_parameters\": {"
        "            \"sample_rate\": 48000,"
        "            \"channels\": 2"
        "        }"
        "    },"
        "    {"
        "        \"id\": \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\","
        "        \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
        "        \"format\": \"urn:x-nmos:format:data\","
        "        \"codec\": \"application/ttml+xml\","
        "        \"essence_parameters\": {"
        "            \"data_type\": \"urn:x-tams:data:subtitle\""
        "        }"
        "    }"
        "]";

    const char *cursor = flow_json_array;
    TAMSFlow flows[3];
    int ret, count = 0;

    ff_tams_json_skip_ws(&cursor);
    if (*cursor != '[')
        FAIL("flow_array: expected '['");
    cursor++;

    while (1) {
        ff_tams_json_skip_ws(&cursor);
        if (*cursor == ']')
            break;
        if (count >= 3)
            FAIL("flow_array: too many flows");
        ret = ff_tams_parse_flow(&cursor, &flows[count]);
        if (ret < 0)
            FAIL("flow_array: parse flow %d failed: %d", count, ret);
        count++;
        ff_tams_json_skip_ws(&cursor);
        if (*cursor == ',')
            cursor++;
    }

    if (count != 3)
        FAIL("flow_array: expected 3 flows, got %d", count);
    if (flows[0].format != TAMS_FORMAT_VIDEO)
        FAIL("flow_array: flow 0 should be video");
    if (flows[1].format != TAMS_FORMAT_AUDIO)
        FAIL("flow_array: flow 1 should be audio");
    if (flows[2].format != TAMS_FORMAT_DATA)
        FAIL("flow_array: flow 2 should be data");

    printf("OK: flow_array (parsed %d flows)\n", count);
    return 0;
}

static int test_bad_json(void)
{
    /* Truncate flow_json_video_h264_vfr halfway */
    size_t len = strlen(flow_json_video_h264_vfr);
    char *bad = av_malloc(len / 2 + 1);
    if (!bad)
        FAIL("bad_json: malloc failed");
    memcpy(bad, flow_json_video_h264_vfr, len / 2);
    bad[len / 2] = '\0';

    int result = test_flow_parsing_fails("bad_json_truncated", bad);
    av_free(bad);
    return result;
}

/* ====================================================================
 * Timestamp and timerange tests
 * ==================================================================== */

static int test_timestamps(void)
{
    int64_t ts;
    int ret;

    ret = ff_tams_parse_timestamp("0:0", &ts);
    if (ret < 0 || ts != 0)
        FAIL("timestamp 0:0 -> %"PRId64, ts);

    ret = ff_tams_parse_timestamp("1:40000000", &ts);
    if (ret < 0 || ts != INT64_C(1040000000))
        FAIL("timestamp 1:40000000 -> %"PRId64, ts);

    ret = ff_tams_parse_timestamp("100:0", &ts);
    if (ret < 0 || ts != INT64_C(100000000000))
        FAIL("timestamp 100:0 -> %"PRId64, ts);

    ret = ff_tams_parse_timestamp("-1:500000000", &ts);
    if (ret < 0 || ts != INT64_C(-1500000000))
        FAIL("timestamp -1:500000000 -> %"PRId64, ts);

    /* Invalid: no colon */
    ret = ff_tams_parse_timestamp("12345", &ts);
    if (ret >= 0)
        FAIL("timestamp '12345' should fail");

    /* Invalid: empty */
    ret = ff_tams_parse_timestamp("", &ts);
    if (ret >= 0)
        FAIL("timestamp '' should fail");

    printf("OK: timestamps\n");
    return 0;
}

static int test_timeranges(void)
{
    TAMSTimeRange tr;
    int ret;

    /* Standard inclusive-exclusive range */
    ret = ff_tams_parse_timerange("[0:0_10:0)", &tr);
    if (ret < 0)
        FAIL("timerange [0:0_10:0)");
    if (!tr.has_start || !tr.has_end)
        FAIL("timerange [0:0_10:0): missing start/end");
    if (tr.start != 0 || tr.end != INT64_C(10000000000))
        FAIL("timerange [0:0_10:0): wrong values");
    if (!tr.start_inclusive || tr.end_inclusive)
        FAIL("timerange [0:0_10:0): wrong inclusivity");

    /* Eternity: _ */
    ret = ff_tams_parse_timerange("_", &tr);
    if (ret < 0)
        FAIL("timerange _");
    if (tr.has_start || tr.has_end)
        FAIL("timerange _: should have no start/end");

    /* Never: () */
    ret = ff_tams_parse_timerange("()", &tr);
    if (ret < 0)
        FAIL("timerange ()");
    if (tr.has_start || tr.has_end)
        FAIL("timerange (): should have no start/end");
    if (tr.start_inclusive || tr.end_inclusive)
        FAIL("timerange (): should not be inclusive");

    /* Instantaneous: [1:0] */
    ret = ff_tams_parse_timerange("[1:0]", &tr);
    if (ret < 0)
        FAIL("timerange [1:0]");
    if (!tr.has_start || !tr.has_end)
        FAIL("timerange [1:0]: missing start/end");
    if (tr.start != INT64_C(1000000000) || tr.end != INT64_C(1000000000))
        FAIL("timerange [1:0]: wrong values");
    if (!tr.start_inclusive || !tr.end_inclusive)
        FAIL("timerange [1:0]: should be inclusive");

    /* Fully inclusive range */
    ret = ff_tams_parse_timerange("[5:500000000_20:0]", &tr);
    if (ret < 0)
        FAIL("timerange [5:500000000_20:0]");
    if (tr.start != INT64_C(5500000000) || tr.end != INT64_C(20000000000))
        FAIL("timerange [5:500000000_20:0]: wrong values");
    if (!tr.start_inclusive || !tr.end_inclusive)
        FAIL("timerange [5:500000000_20:0]: wrong inclusivity");

    /* Negative timestamp */
    ret = ff_tams_parse_timerange("[-1:0_0:0)", &tr);
    if (ret < 0)
        FAIL("timerange [-1:0_0:0)");
    if (tr.start != INT64_C(-1000000000) || tr.end != 0)
        FAIL("timerange [-1:0_0:0): wrong values: %"PRId64"_%"PRId64, tr.start, tr.end);

    printf("OK: timeranges\n");
    return 0;
}

/* ====================================================================
 * Segment parsing tests
 * ==================================================================== */

static int test_segment_full(void)
{
    const char *cursor = segment_json;
    TAMSFlowSegment seg;
    int ret;

    ret = ff_tams_parse_flow_segment(&cursor, &seg);
    if (ret < 0)
        FAIL("segment_full: parse failed: %d", ret);

    if (strcmp(seg.object_id, "seg-001.ts"))
        FAIL("segment_full: wrong object_id: %s", seg.object_id);
    if (!seg.timerange.has_start || !seg.timerange.has_end)
        FAIL("segment_full: timerange not parsed");
    if (seg.timerange.start != 0 || seg.timerange.end != INT64_C(5000000000))
        FAIL("segment_full: wrong timerange");
    if (seg.ts_offset != INT64_C(-700000000))
        FAIL("segment_full: wrong ts_offset: %"PRId64, seg.ts_offset);
    if (!seg.has_last_duration || seg.last_duration != INT64_C(40000000))
        FAIL("segment_full: wrong last_duration: %"PRId64, seg.last_duration);
    if (strcmp(seg.get_url, "https://example.com/seg-001.ts"))
        FAIL("segment_full: wrong get_url: %s", seg.get_url);

    printf("OK: segment_full\n");
    return 0;
}

static int test_segment_minimal(void)
{
    const char *cursor = segment_json_minimal;
    TAMSFlowSegment seg;
    int ret;

    ret = ff_tams_parse_flow_segment(&cursor, &seg);
    if (ret < 0)
        FAIL("segment_minimal: parse failed: %d", ret);

    if (strcmp(seg.object_id, "seg-002.mp4"))
        FAIL("segment_minimal: wrong object_id");
    if (seg.ts_offset != 0)
        FAIL("segment_minimal: ts_offset should default to 0");
    if (seg.has_last_duration)
        FAIL("segment_minimal: should not have last_duration");
    if (seg.get_url[0])
        FAIL("segment_minimal: should not have get_url");

    printf("OK: segment_minimal\n");
    return 0;
}

static int test_segment_null_fields(void)
{
    const char *cursor = segment_json_null_fields;
    TAMSFlowSegment seg;
    int ret;

    ret = ff_tams_parse_flow_segment(&cursor, &seg);
    if (ret < 0)
        FAIL("segment_null: parse failed: %d", ret);

    if (seg.ts_offset != 0)
        FAIL("segment_null: null ts_offset should default to 0");
    if (seg.has_last_duration)
        FAIL("segment_null: null last_duration should not set has_last_duration");

    printf("OK: segment_null_fields\n");
    return 0;
}

static int test_segment_presigned(void)
{
    const char *cursor = segment_json_presigned;
    TAMSFlowSegment seg;
    int ret;

    ret = ff_tams_parse_flow_segment(&cursor, &seg);
    if (ret < 0)
        FAIL("segment_presigned: parse failed: %d", ret);

    if (strcmp(seg.get_url,
              "https://s3.example.com/seg-004.ts?Signature=abc&Token=xyz"))
        FAIL("segment_presigned: wrong get_url (expected presigned): %s",
             seg.get_url);

    printf("OK: segment_presigned\n");
    return 0;
}

/* ====================================================================
 * ff_tams_parse_iso8601 tests
 * ==================================================================== */

static int test_parse_iso8601(void)
{
    int64_t t;

    /* NULL and empty inputs must return 0 */
    t = ff_tams_parse_iso8601(NULL);
    if (t != 0)
        FAIL("iso8601 NULL: expected 0, got %"PRId64, t);

    t = ff_tams_parse_iso8601("");
    if (t != 0)
        FAIL("iso8601 empty: expected 0, got %"PRId64, t);

    /* Invalid string must return 0 */
    t = ff_tams_parse_iso8601("not-a-date");
    if (t != 0)
        FAIL("iso8601 invalid: expected 0, got %"PRId64, t);

    /* Valid UTC datetime must return a non-zero epoch microsecond value */
    t = ff_tams_parse_iso8601("2018-03-06T09:10:22Z");
    if (t == 0)
        FAIL("iso8601 2018-03-06T09:10:22Z: expected non-zero");
    /* Sanity-check: must be after 2010-01-01 (1262304000s) and before 2030-01-01 */
    if (t < INT64_C(1262304000) * 1000000 || t > INT64_C(1893456000) * 1000000)
        FAIL("iso8601 2018-03-06T09:10:22Z: value out of plausible range: %"PRId64, t);

    /* Second valid sample */
    t = ff_tams_parse_iso8601("2023-09-14T09:45:26Z");
    if (t == 0)
        FAIL("iso8601 2023-09-14T09:45:26Z: expected non-zero");

    printf("OK: parse_iso8601\n");
    return 0;
}

/* ====================================================================
 * ff_tams_parse_flows_json tests
 * ==================================================================== */

static int test_parse_flows_json_single(void)
{
    TAMSFlow *flows = NULL;
    int nb_flows = 0;
    int ret;

    ret = ff_tams_parse_flows_json(flow_json_video_raw, &flows, &nb_flows);
    if (ret < 0)
        FAIL("parse_flows_json_single: returned %d", ret);
    if (nb_flows != 1)
        FAIL("parse_flows_json_single: expected 1 flow, got %d", nb_flows);
    if (!flows)
        FAIL("parse_flows_json_single: flows is NULL");
    if (flows[0].format != TAMS_FORMAT_VIDEO)
        FAIL("parse_flows_json_single: wrong format %d", flows[0].format);
    if (strcmp(flows[0].id, "0fde9c11-da9d-434a-a113-d3b20a2cf251"))
        FAIL("parse_flows_json_single: wrong id: %s", flows[0].id);
    av_free(flows);

    printf("OK: parse_flows_json_single\n");
    return 0;
}

static int test_parse_flows_json_array(void)
{
    static const char *flow_json_array =
        "["
        "    {"
        "        \"id\": \"4f79cfd1-c057-47f4-8e4d-1b126ca7bf34\","
        "        \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
        "        \"format\": \"urn:x-nmos:format:video\","
        "        \"codec\": \"video/h264\","
        "        \"essence_parameters\": {"
        "            \"frame_width\": 1280,"
        "            \"frame_height\": 720"
        "        }"
        "    },"
        "    {"
        "        \"id\": \"94996f2e-0cb5-43d3-ab6c-db5a9cf667aa\","
        "        \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
        "        \"format\": \"urn:x-nmos:format:audio\","
        "        \"codec\": \"audio/aac\","
        "        \"essence_parameters\": {"
        "            \"sample_rate\": 48000,"
        "            \"channels\": 2"
        "        }"
        "    },"
        "    {"
        "        \"id\": \"e85efab4-993b-4ad6-9af3-4cd8d0d38860\","
        "        \"source_id\": \"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\","
        "        \"format\": \"urn:x-nmos:format:data\","
        "        \"codec\": \"application/ttml+xml\","
        "        \"essence_parameters\": {"
        "            \"data_type\": \"urn:x-tams:data:subtitle\""
        "        }"
        "    }"
        "]";

    TAMSFlow *flows = NULL;
    int nb_flows = 0;
    int ret;

    ret = ff_tams_parse_flows_json(flow_json_array, &flows, &nb_flows);
    if (ret < 0)
        FAIL("parse_flows_json_array: returned %d", ret);
    if (nb_flows != 3)
        FAIL("parse_flows_json_array: expected 3 flows, got %d", nb_flows);
    if (!flows)
        FAIL("parse_flows_json_array: flows is NULL");
    if (flows[0].format != TAMS_FORMAT_VIDEO)
        FAIL("parse_flows_json_array: flow 0 should be video");
    if (flows[1].format != TAMS_FORMAT_AUDIO)
        FAIL("parse_flows_json_array: flow 1 should be audio");
    if (flows[2].format != TAMS_FORMAT_DATA)
        FAIL("parse_flows_json_array: flow 2 should be data");
    if (flows[0].frame_width != 1280 || flows[0].frame_height != 720)
        FAIL("parse_flows_json_array: flow 0 wrong dimensions");
    if (flows[1].sample_rate != 48000 || flows[1].channels != 2)
        FAIL("parse_flows_json_array: flow 1 wrong audio params");
    av_free(flows);

    printf("OK: parse_flows_json_array\n");
    return 0;
}

static int test_parse_flows_json_empty_array(void)
{
    TAMSFlow *flows = NULL;
    int nb_flows = 0;
    int ret;

    ret = ff_tams_parse_flows_json("[]", &flows, &nb_flows);
    if (ret < 0)
        FAIL("parse_flows_json_empty: returned %d", ret);
    if (nb_flows != 0)
        FAIL("parse_flows_json_empty: expected 0 flows, got %d", nb_flows);
    av_free(flows);

    printf("OK: parse_flows_json_empty_array\n");
    return 0;
}

static int test_tag_array_values(void)
{
    const char *json =
        "{"
        "    \"id\": \"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "    \"source_id\": \"11111111-2222-3333-4444-555555555555\","
        "    \"format\": \"urn:x-nmos:format:video\","
        "    \"codec\": \"video/h264\","
        "    \"tags\": {"
        "        \"lang\": [\"en\", \"fr\"],"
        "        \"auth_classes\": [\"public\"],"
        "        \"label\": \"simple\""
        "    },"
        "    \"essence_parameters\": {"
        "        \"frame_width\": 1920,"
        "        \"frame_height\": 1080"
        "    }"
        "}";
    const char *cursor = json;
    TAMSFlow flow;
    int ret;

    ret = ff_tams_parse_flow(&cursor, &flow);
    if (ret < 0)
        FAIL("tag_array: parse failed: %d", ret);
    if (flow.nb_tags != 3)
        FAIL("tag_array: expected 3 tags, got %d", flow.nb_tags);
    if (strcmp(flow.tags[0].key, "lang"))
        FAIL("tag_array: wrong tag[0].key: %s", flow.tags[0].key);
    if (strcmp(flow.tags[0].value, "en,fr"))
        FAIL("tag_array: wrong tag[0].value: '%s' (expected 'en,fr')", flow.tags[0].value);
    if (strcmp(flow.tags[1].key, "auth_classes"))
        FAIL("tag_array: wrong tag[1].key: %s", flow.tags[1].key);
    if (strcmp(flow.tags[1].value, "public"))
        FAIL("tag_array: wrong tag[1].value: '%s' (expected 'public')", flow.tags[1].value);
    if (strcmp(flow.tags[2].key, "label"))
        FAIL("tag_array: wrong tag[2].key: %s", flow.tags[2].key);
    if (strcmp(flow.tags[2].value, "simple"))
        FAIL("tag_array: wrong tag[2].value: '%s' (expected 'simple')", flow.tags[2].value);

    printf("OK: tag_array_values\n");
    return 0;
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(int argc, char *argv[])
{
    int ret = 0;

    /* Timestamp and timerange parsing */
    ret |= test_timestamps();
    ret |= test_timeranges();

    /* ISO 8601 datetime parsing */
    ret |= test_parse_iso8601();

    /* Single flow parsing */
    ret |= test_video_raw();
    ret |= test_video_h264_vfr();
    ret |= test_audio_aac();
    ret |= test_data_ttml();

    /* Partial/minimal essence_parameters */
    ret |= test_video_minimal();

    /* Additional core fields: timerange, read_only, max_bit_rate, segment_duration, etc. */
    ret |= test_flow_with_timerange();

    /* Uncompressed audio parameters */
    ret |= test_audio_unc();

    /* Tag array values (TAMS 8.0+ schema) */
    ret |= test_tag_array_values();

    /* Multi-flow: separate flows */
    ret |= test_multi_separate();

    /* Multi-flow: container mapping */
    ret |= test_multi_container_map();

    /* Multi-flow JSON array (low-level API) */
    ret |= test_flow_array();

    /* ff_tams_parse_flows_json (high-level API) */
    ret |= test_parse_flows_json_single();
    ret |= test_parse_flows_json_array();
    ret |= test_parse_flows_json_empty_array();

    /* Segment parsing */
    ret |= test_segment_full();
    ret |= test_segment_minimal();
    ret |= test_segment_null_fields();
    ret |= test_segment_presigned();

    /* Invalid/bad JSON */
    printf("#### The following should fail ####\n");
    ret |= test_bad_json();
    ret |= test_flow_parsing_fails("not_an_object", "[1, 2, 3]");
    ret |= test_flow_parsing_fails("invalid_json", "{\"id\": }");
    ret |= test_flow_parsing_fails("unterminated_string", "{\"id\": \"abc}");
    ret |= test_flow_parsing_fails("missing_brace", "{\"id\": \"abc\"");
    printf("#### End failing tests ####\n");

    if (ret)
        printf("\nSOME TESTS FAILED\n");
    else
        printf("\nALL TESTS PASSED\n");

    return ret;
}
