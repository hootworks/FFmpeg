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
 * Tests for the TAMS muxer's -flow_map option parser (pure functions,
 * no I/O).
 *
 * @author Nick Ryan
 * @file
 * @ingroup lavu_tams
 */

#include "libavformat/tamsenc.c"

#include "libavcodec/codec_id.h"
#include "libavcodec/packet.h"

#include <stdio.h>

#define FAIL(msg, ...) do { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); \
    return 1; \
} while (0)

static void free_result(TAMSContainerContext *containers, int nb_containers,
                         TAMSMultiFlowContext *multis, int nb_multis)
{
    tams_free_container_ctxs(containers, nb_containers);
    tams_free_multi_flow_ctxs(multis, nb_multis);
}

/* ====================================================================
 * Default / no -flow_map
 * ==================================================================== */

static int test_flow_map_default(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;

    ret = tams_parse_flow_map(NULL, 3, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_default: returned %d", ret);
    if (nb_containers != 3 || nb_multis != 0)
        FAIL("flow_map_default: expected 3 containers/0 multis, got %d/%d",
             nb_containers, nb_multis);
    for (int i = 0; i < 3; i++) {
        if (containers[i].nb_streams != 1 || containers[i].stream_indices[0] != i ||
            containers[i].has_multi_flow)
            FAIL("flow_map_default: container %d wrong", i);
    }

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_default\n");
    return 0;
}

static int test_flow_map_empty_string(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;

    ret = tams_parse_flow_map("", 2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0 || nb_containers != 2 || nb_multis != 0)
        FAIL("flow_map_empty_string: expected default, got ret=%d nb_containers=%d nb_multis=%d",
             ret, nb_containers, nb_multis);

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_empty_string\n");
    return 0;
}

/* ====================================================================
 * Single flow= tokens: streams=, id=, source_id=
 * ==================================================================== */

static int test_flow_map_explicit_split(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;

    ret = tams_parse_flow_map("flow=streams=0 flow=streams=1", 2,
                              &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_explicit_split: returned %d", ret);
    if (nb_containers != 2 || nb_multis != 0)
        FAIL("flow_map_explicit_split: expected 2 containers, got %d", nb_containers);
    if (containers[0].nb_streams != 1 || containers[0].stream_indices[0] != 0)
        FAIL("flow_map_explicit_split: container 0 wrong");
    if (containers[1].nb_streams != 1 || containers[1].stream_indices[0] != 1)
        FAIL("flow_map_explicit_split: container 1 wrong");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_explicit_split\n");
    return 0;
}

static int test_flow_map_unmentioned_stream_gets_default(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;

    /* stream 1 is never mentioned -- should default to its own standalone
     * mono Flow rather than erroring, same as full -flow_map omission */
    ret = tams_parse_flow_map("flow=streams=0,id=4f79cfd1-c057-47f4-8e4d-1b126ca7bf34",
                              2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_unmentioned_stream_gets_default: returned %d", ret);
    if (nb_containers != 2 || nb_multis != 0)
        FAIL("flow_map_unmentioned_stream_gets_default: expected 2 containers, got %d",
             nb_containers);
    if (!containers[0].flow_ctxs[0].has_flow_id)
        FAIL("flow_map_unmentioned_stream_gets_default: container 0 should be pinned");
    if (containers[1].nb_streams != 1 || containers[1].stream_indices[0] != 1 ||
        containers[1].has_multi_flow || containers[1].flow_ctxs[0].has_flow_id)
        FAIL("flow_map_unmentioned_stream_gets_default: container 1 should be a "
             "plain default for stream 1");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_unmentioned_stream_gets_default\n");
    return 0;
}

static int test_flow_map_id_pin(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    const char *str = "flow=streams=0,id=4f79cfd1-c057-47f4-8e4d-1b126ca7bf34";

    ret = tams_parse_flow_map(str, 1, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_id_pin: returned %d", ret);
    if (nb_containers != 1 || containers[0].nb_streams != 1)
        FAIL("flow_map_id_pin: expected 1 container/1 flow");
    if (!containers[0].flow_ctxs[0].has_flow_id ||
        strcmp(containers[0].flow_ctxs[0].flow_id, "4f79cfd1-c057-47f4-8e4d-1b126ca7bf34"))
        FAIL("flow_map_id_pin: wrong flow_id: %s", containers[0].flow_ctxs[0].flow_id);
    if (containers[0].flow_ctxs[0].has_source_id)
        FAIL("flow_map_id_pin: source_id should not be set");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_id_pin\n");
    return 0;
}

static int test_flow_map_id_and_source_id(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    const char *str =
        "flow=streams=0,id=4f79cfd1-c057-47f4-8e4d-1b126ca7bf34,"
        "source_id=11111111-1111-1111-1111-111111111111";

    ret = tams_parse_flow_map(str, 1, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_id_and_source_id: returned %d", ret);
    if (!containers[0].flow_ctxs[0].has_flow_id ||
        strcmp(containers[0].flow_ctxs[0].flow_id, "4f79cfd1-c057-47f4-8e4d-1b126ca7bf34"))
        FAIL("flow_map_id_and_source_id: wrong flow_id");
    if (!containers[0].flow_ctxs[0].has_source_id ||
        strcmp(containers[0].flow_ctxs[0].source_id, "11111111-1111-1111-1111-111111111111"))
        FAIL("flow_map_id_and_source_id: wrong source_id: %s",
             containers[0].flow_ctxs[0].source_id);

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_id_and_source_id\n");
    return 0;
}

/* ====================================================================
 * Shared/muxed containers: multi-stream flow= tokens
 * ==================================================================== */

static int test_flow_map_shared_container_requires_multi(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;

    ret = tams_parse_flow_map("flow=streams=0,1,multi_flow=0", 2,
                              &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_shared_container_requires_multi: returned %d", ret);
    if (nb_containers != 1 || containers[0].nb_streams != 2)
        FAIL("flow_map_shared_container_requires_multi: expected 1 container of 2 streams");
    if (!containers[0].has_multi_flow || containers[0].multi_flow_index != 0)
        FAIL("flow_map_shared_container_requires_multi: multi_flow not linked");
    if (nb_multis != 1 || multis[0].index != 0 ||
        multis[0].nb_container_indices != 1 || multis[0].container_indices[0] != 0)
        FAIL("flow_map_shared_container_requires_multi: multi cross-link wrong");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_shared_container_requires_multi\n");
    return 0;
}

static int test_flow_map_shared_container_positional_ids(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    const char *str =
        "flow=streams=0,1,id=e85efab4-993b-4ad6-9af3-4cd8d0d38860,"
        "id=4f79cfd1-c057-47f4-8e4d-1b126ca7bf34,multi_flow=0";

    ret = tams_parse_flow_map(str, 2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_shared_container_positional_ids: returned %d", ret);
    if (containers[0].nb_streams != 2)
        FAIL("flow_map_shared_container_positional_ids: expected 2 flows in container");
    if (!containers[0].flow_ctxs[0].has_flow_id ||
        strcmp(containers[0].flow_ctxs[0].flow_id, "e85efab4-993b-4ad6-9af3-4cd8d0d38860"))
        FAIL("flow_map_shared_container_positional_ids: flows[0] wrong: %s",
             containers[0].flow_ctxs[0].flow_id);
    if (!containers[0].flow_ctxs[1].has_flow_id ||
        strcmp(containers[0].flow_ctxs[1].flow_id, "4f79cfd1-c057-47f4-8e4d-1b126ca7bf34"))
        FAIL("flow_map_shared_container_positional_ids: flows[1] wrong: %s",
             containers[0].flow_ctxs[1].flow_id);

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_shared_container_positional_ids\n");
    return 0;
}

static int test_flow_map_shared_container_partial_id(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    /* only stream 0 gets an explicit id=; stream 1 is left for implicit
     * resolution later (in write_header), not a parse-time error */
    const char *str =
        "flow=streams=0,1,id=e85efab4-993b-4ad6-9af3-4cd8d0d38860,multi_flow=0";

    ret = tams_parse_flow_map(str, 2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_shared_container_partial_id: returned %d", ret);
    if (!containers[0].flow_ctxs[0].has_flow_id)
        FAIL("flow_map_shared_container_partial_id: flows[0] should be pinned");
    if (containers[0].flow_ctxs[1].has_flow_id)
        FAIL("flow_map_shared_container_partial_id: flows[1] should be unpinned");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_shared_container_partial_id\n");
    return 0;
}

/* ====================================================================
 * multi_flow= linkage: new multi, standalone property tokens, multiple
 * independent multis in one invocation
 * ==================================================================== */

static int test_flow_map_new_multi(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    const char *str =
        "flow=streams=0,multi_flow=0 "
        "flow=streams=1,id=4f79cfd1-c057-47f4-8e4d-1b126ca7bf34,multi_flow=0";

    ret = tams_parse_flow_map(str, 2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_new_multi: returned %d", ret);
    if (nb_containers != 2 || nb_multis != 1)
        FAIL("flow_map_new_multi: expected 2 containers/1 multi, got %d/%d",
             nb_containers, nb_multis);
    if (multis[0].has_flow_id || multis[0].has_source_id)
        FAIL("flow_map_new_multi: multi should have no properties (fully new)");
    if (multis[0].nb_container_indices != 2)
        FAIL("flow_map_new_multi: expected both containers linked to the multi");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_new_multi\n");
    return 0;
}

static int test_flow_map_existing_multi_property_token(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    const char *str =
        "flow=streams=0,multi_flow=0 flow=streams=1,multi_flow=0 "
        "multi_flow=0,id=e85efab4-993b-4ad6-9af3-4cd8d0d38860";

    ret = tams_parse_flow_map(str, 2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_existing_multi_property_token: returned %d", ret);
    if (nb_multis != 1 || !multis[0].has_flow_id ||
        strcmp(multis[0].flow_id, "e85efab4-993b-4ad6-9af3-4cd8d0d38860"))
        FAIL("flow_map_existing_multi_property_token: wrong multi flow_id");
    if (multis[0].nb_container_indices != 2)
        FAIL("flow_map_existing_multi_property_token: expected 2 linked containers");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_existing_multi_property_token\n");
    return 0;
}

static int test_flow_map_multi_source_id(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    const char *str =
        "flow=streams=0,multi_flow=0 flow=streams=1,multi_flow=0 "
        "multi_flow=0,source_id=11111111-1111-1111-1111-111111111111";

    ret = tams_parse_flow_map(str, 2, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_multi_source_id: returned %d", ret);
    if (!multis[0].has_source_id ||
        strcmp(multis[0].source_id, "11111111-1111-1111-1111-111111111111"))
        FAIL("flow_map_multi_source_id: wrong source_id: %s", multis[0].source_id);
    if (multis[0].has_flow_id)
        FAIL("flow_map_multi_source_id: flow_id should not be given");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_multi_source_id\n");
    return 0;
}

static int test_flow_map_two_independent_multis(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    /* two separate multi-Flow groupings in one invocation: streams 0,1
     * under multi 0 (existing), streams 2,3 under multi 1 (existing) */
    const char *str =
        "flow=streams=0,multi_flow=0 flow=streams=1,multi_flow=0 "
        "flow=streams=2,multi_flow=1 flow=streams=3,multi_flow=1 "
        "multi_flow=0,id=aaaaaaaa-0000-0000-0000-000000000000 "
        "multi_flow=1,id=bbbbbbbb-0000-0000-0000-000000000000";

    ret = tams_parse_flow_map(str, 4, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_two_independent_multis: returned %d", ret);
    if (nb_containers != 4 || nb_multis != 2)
        FAIL("flow_map_two_independent_multis: expected 4 containers/2 multis, got %d/%d",
             nb_containers, nb_multis);
    if (!multis[0].has_flow_id ||
        strcmp(multis[0].flow_id, "aaaaaaaa-0000-0000-0000-000000000000"))
        FAIL("flow_map_two_independent_multis: multi[0] wrong");
    if (!multis[1].has_flow_id ||
        strcmp(multis[1].flow_id, "bbbbbbbb-0000-0000-0000-000000000000"))
        FAIL("flow_map_two_independent_multis: multi[1] wrong");
    if (multis[0].nb_container_indices != 2 || multis[1].nb_container_indices != 2)
        FAIL("flow_map_two_independent_multis: wrong container linkage counts");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_two_independent_multis\n");
    return 0;
}

static int test_flow_map_standalone_and_grouped_mixed(void)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;
    /* stream 0 fully standalone, streams 1+2 grouped under a new multi */
    const char *str =
        "flow=streams=0 flow=streams=1,multi_flow=0 flow=streams=2,multi_flow=0";

    ret = tams_parse_flow_map(str, 3, &containers, &nb_containers, &multis, &nb_multis);
    if (ret < 0)
        FAIL("flow_map_standalone_and_grouped_mixed: returned %d", ret);
    if (nb_containers != 3 || nb_multis != 1)
        FAIL("flow_map_standalone_and_grouped_mixed: expected 3 containers/1 multi");
    if (containers[0].has_multi_flow)
        FAIL("flow_map_standalone_and_grouped_mixed: container 0 should be standalone");
    if (!containers[1].has_multi_flow || !containers[2].has_multi_flow)
        FAIL("flow_map_standalone_and_grouped_mixed: containers 1/2 should be grouped");

    free_result(containers, nb_containers, multis, nb_multis);
    printf("OK: flow_map_standalone_and_grouped_mixed\n");
    return 0;
}

/* ====================================================================
 * Failure cases
 * ==================================================================== */

static int test_flow_map_fails(const char *test_name, const char *str, int nb_streams)
{
    TAMSContainerContext *containers;
    TAMSMultiFlowContext *multis;
    int nb_containers, nb_multis, ret;

    ret = tams_parse_flow_map(str, nb_streams, &containers, &nb_containers, &multis, &nb_multis);
    if (ret >= 0) {
        free_result(containers, nb_containers, multis, nb_multis);
        FAIL("%s: expected failure but got success", test_name);
    }

    printf("OK: %s (correctly failed with %d)\n", test_name, ret);
    return 0;
}

/* ====================================================================
 * tams_is_valid_uuid
 * ==================================================================== */

static int test_is_valid_uuid(void)
{
    if (!tams_is_valid_uuid("11111111-2222-3333-4444-555555555555"))
        FAIL("is_valid_uuid: canonical lowercase UUID should be valid");
    if (!tams_is_valid_uuid("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"))
        FAIL("is_valid_uuid: uppercase UUID should be valid");
    if (tams_is_valid_uuid("not-a-uuid"))
        FAIL("is_valid_uuid: garbage should be invalid");
    if (tams_is_valid_uuid("11111111-2222-3333-4444-55555555555"))
        FAIL("is_valid_uuid: too short should be invalid");
    if (tams_is_valid_uuid("11111111x2222-3333-4444-555555555555"))
        FAIL("is_valid_uuid: wrong separator should be invalid");

    printf("OK: is_valid_uuid\n");
    return 0;
}

/* ====================================================================
 * tams_init integration test
 * ==================================================================== */

static int test_init_missing_multi_flow_fails(void)
{
    AVFormatContext *s = avformat_alloc_context();
    TAMSMuxContext *c;
    int ret;

    if (!s)
        FAIL("init_missing_multi_flow: alloc_context failed");

    s->oformat = &ff_tams_muxer.p;
    if (avformat_new_stream(s, NULL) == NULL ||
        avformat_new_stream(s, NULL) == NULL) {
        avformat_free_context(s);
        FAIL("init_missing_multi_flow: new_stream failed");
    }

    s->priv_data = av_mallocz(sizeof(TAMSMuxContext));
    if (!s->priv_data) {
        avformat_free_context(s);
        FAIL("init_missing_multi_flow: priv_data alloc failed");
    }
    c = s->priv_data;
    c->class = &tams_muxer_class;
    av_opt_set_defaults(c);

    /* two streams sharing one container, but no multi_flow= given: invalid */
    c->flow_map_str = av_strdup("flow=streams=0,1");

    ret = tams_init(s);
    if (ret >= 0) {
        tams_deinit(s);
        av_freep(&c->flow_map_str);
        av_freep(&s->priv_data);
        avformat_free_context(s);
        FAIL("init_missing_multi_flow: expected failure but got success");
    }

    tams_deinit(s);
    av_freep(&c->flow_map_str);
    av_freep(&s->priv_data);
    avformat_free_context(s);

    printf("OK: init_missing_multi_flow_fails (correctly failed with %d)\n", ret);
    return 0;
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(int argc, char *argv[])
{
    int ret = 0;

    ret |= test_flow_map_default();
    ret |= test_flow_map_empty_string();
    ret |= test_flow_map_explicit_split();
    ret |= test_flow_map_unmentioned_stream_gets_default();
    ret |= test_flow_map_id_pin();
    ret |= test_flow_map_id_and_source_id();

    ret |= test_flow_map_shared_container_requires_multi();
    ret |= test_flow_map_shared_container_positional_ids();
    ret |= test_flow_map_shared_container_partial_id();

    ret |= test_flow_map_new_multi();
    ret |= test_flow_map_existing_multi_property_token();
    ret |= test_flow_map_multi_source_id();
    ret |= test_flow_map_two_independent_multis();
    ret |= test_flow_map_standalone_and_grouped_mixed();

    ret |= test_is_valid_uuid();
    ret |= test_init_missing_multi_flow_fails();

    printf("#### The following should fail ####\n");
    ret |= test_flow_map_fails("flow_map_bad_token", "foo=streams=0", 1);
    ret |= test_flow_map_fails("flow_map_out_of_range", "flow=streams=5", 2);
    ret |= test_flow_map_fails("flow_map_duplicate_assignment",
                                "flow=streams=0 flow=streams=0", 2);
    ret |= test_flow_map_fails("flow_map_empty_list", "flow=streams=", 2);
    ret |= test_flow_map_fails("flow_map_malformed_index", "flow=streams=abc", 2);
    ret |= test_flow_map_fails("flow_map_shared_container_missing_multi",
                                "flow=streams=0,1", 2);
    ret |= test_flow_map_fails("flow_map_too_many_ids",
                                "flow=streams=0,id=e85efab4-993b-4ad6-9af3-4cd8d0d38860,"
                                "id=4f79cfd1-c057-47f4-8e4d-1b126ca7bf34", 1);
    ret |= test_flow_map_fails("flow_map_bad_uuid",
                                "flow=streams=0,id=not-a-uuid", 1);
    ret |= test_flow_map_fails("flow_map_duplicate_source_id",
                                "flow=streams=0,source_id=11111111-1111-1111-1111-111111111111,"
                                "source_id=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1);
    ret |= test_flow_map_fails("flow_map_duplicate_multi_flow_attr",
                                "flow=streams=0,multi_flow=0,multi_flow=1", 1);
    ret |= test_flow_map_fails("flow_map_unknown_attr",
                                "flow=streams=0,bogus=1", 1);
    ret |= test_flow_map_fails("flow_map_multi_no_attrs", "multi_flow=0", 1);
    ret |= test_flow_map_fails("flow_map_multi_duplicate_index",
                                "flow=streams=0,multi_flow=0 flow=streams=1,multi_flow=0 "
                                "multi_flow=0,id=aaaaaaaa-0000-0000-0000-000000000000 "
                                "multi_flow=0,id=bbbbbbbb-0000-0000-0000-000000000000", 2);
    ret |= test_flow_map_fails("flow_map_orphan_multi_flow_token",
                                "flow=streams=0 "
                                "multi_flow=0,id=aaaaaaaa-0000-0000-0000-000000000000", 1);
    printf("#### End failing tests ####\n");

    if (ret)
        printf("\nSOME TESTS FAILED\n");
    else
        printf("\nALL TESTS PASSED\n");

    return ret;
}
