# TAMS Implementation Architecture

## Overview

The TAMS (Time-Addressable Media Store) demuxer for FFmpeg implements support for reading media flows from BBC's TAMS API. It handles single flows (audio/video/data) and multi-flows that aggregate sub-flows with optional container mapping.

## Key Components

### Core Files

- **tams.h**: Public header with data structures and parsing function declarations
- **tams.c**: JSON parsing utilities and flow/segment parsing functions  
- **tamsdec.c**: Main demuxer implementation with input format definition

### Data Structures

#### TAMSFlow
Represents a single flow with complete metadata:
- Basic info: id, source_id, label, description, format, codec, container
- Timing: timerange, segment_duration
- Video params: frame_width, frame_height, frame_rate, color info
- Audio params: sample_rate, channels, bit_depth
- Multi-flow: flow_collection array for sub-flows

#### TAMSFlowSegment  
Represents one segment of media:
- object_id: unique identifier
- timerange: start/end timestamps in nanoseconds
- ts_offset: timestamp adjustment for flow timeline alignment
- get_url: HTTP URL to fetch media data

#### TAMSContext (demuxer private data)
- flows[]: Array of all flows (including fetched sub-flows)
- stream_mappings[]: Unified stream mapping structure (TAMSStreamMapping)
- seg_ctxs[]: Array of segment contexts (one per unique segment source)
- streams[]: Per-stream state (TAMSStreamContext)

#### TAMSStreamMapping
Unified structure for stream-to-flow mapping:
- flow_index: Which TAMSFlow this stream represents
- parent_flow_index: Parent flow index for container-mapped sub-flows (-1 if independent)
- container_track_index: Track mapping for multi-flows (-1 if not specified)

#### TAMSSegmentContext
Manages segments for one flow or multi-flow:
- flow_index: Which flow owns these segments
- segments[]: Array of TAMSFlowSegment 
- cur_segment_index: Current position in segment array
- sub_ctx: AVFormatContext for current segment's media
- is_live: Whether this is a live stream requiring polling
- poll_interval: Current backoff interval for live polling
- refcount: Number of streams sharing this context

#### TAMSStreamContext  
Per-stream state tracking:
- flow_index: Which TAMSFlow this stream represents
- seg_ctx_index: Index into TAMSContext.seg_ctxs[]
- sub_stream_index: Stream index within segment's sub-demuxer
- media_type: AVMEDIA_TYPE_* for stream matching
- container_track_index: Track mapping for multi-flows
- current_ts: Current timeline position
- eof: Stream exhausted flag

## Architecture Patterns

### Multi-Flow Support

Multi-flows aggregate multiple sub-flows. Two patterns are supported:

1. **Independent sub-flows**: Each sub-flow has its own `container` field and gets its own segment list via `/flows/{sub_flow_id}/segments`. Each gets its own TAMSSegmentContext.

2. **Container-mapped sub-flows**: Sub-flows have no `container` field, meaning they're muxed into the parent multi-flow's container. They share a single TAMSSegmentContext that fetches segments via `/flows/{multi_flow_id}/segments`. The `container_mapping` field identifies which track in each segment belongs to which sub-flow.

### Segment Context Sharing

Multiple streams can share the same TAMSSegmentContext when:
- They belong to sub-flows that are container-mapped into the same multi-flow
- The refcount tracks how many streams use each context
- When a segment EOF occurs, all streams sharing that context are notified

### Stream Selection Strategy

The read_packet function uses a simple strategy:
1. Find the stream with the lowest current_ts that isn't EOF
2. Ensure that stream has an open segment sub-demuxer
3. Read a packet from that stream's sub-demuxer
4. Apply timestamp adjustments and stream mapping
5. Update the stream's current_ts

This ensures reasonably interleaved output for multi-stream content.

### Timestamp Handling

TAMS uses a nanosecond timeline (TAMS_TIMEBASE = 1,000,000,000) for internal processing. The timestamp flow:

#### Stream Timebase Setup
- **Video streams with explicit frame rate**: `st->time_base = av_inv_q(flow->frame_rate)`
  - This ensures packets have correct timing based on TAMS flow metadata
- **Other streams**: Use default timebase handling

#### Packet Timestamp Processing
1. Read packet from segment sub-demuxer with its native timebase
2. Convert to TAMS stream timebase: `av_rescale_q(pkt->pts, sub_st->time_base, st->time_base)`
3. Convert to nanoseconds for internal processing: `av_rescale_q(pts, st->time_base, {1, TAMS_TIMEBASE})`
4. Apply segment timestamp offset: `pts_ns += segment.ts_offset`
5. Check against flow timerange boundaries
6. Set packet timestamp and stream index for output

This architecture ensures timestamp consistency between TAMS flow metadata and packet timing, fixing issues where segment containers report different frame rates than declared in the flow.

### Duration Handling

Duration information availability depends on the presence of timerange data and lazy segment loading:

#### Known Duration Scenarios
- **URL Timerange**: When `timerange=[start_end)` is specified in the initial request URL, duration is calculated as `end - start`
- **Flow Timerange**: When the flow response contains a `timerange` field, duration is derived from the flow metadata

#### Unknown Duration Scenarios
- **No Initial Timerange**: If neither URL nor flow response contains timerange information
- **Live Streams**: Ongoing streams where the end time is not predetermined
- **Lazy Loading Effect**: Since segments are loaded on-demand, total content duration cannot be determined without fetching all segments

#### Implications
- **Format Context Duration**: May be `AV_NOPTS_VALUE` or 0 when timerange is unknown
- **Stream Duration**: Individual streams inherit duration from their associated flow timerange  
- **Seeking Limitations**: Unknown duration complicates seeking implementations
- **Progress Reporting**: Applications cannot show accurate progress without known duration
- **Pagination Impact**: Due to lazy segment loading, the full extent of content is not known until all segments are fetched

### Live Stream Support

Live detection based on `segments_updated` timestamp proximity to current time:
- If `segments_updated` age < `live_threshold`, mark as live
- Live streams poll for new segments with exponential backoff
- If `segments_updated` age > `live_timeout`, revert to non-live and EOF
- Configurable via demuxer options: live_threshold, live_timeout, seg_poll_init, seg_poll_max

### URL Construction

Base URL derived from input URL by:
1. Strip `/flows/{flow_id}` suffix to get base API URL
2. Preserve query parameters (especially auth headers)
3. Build specific endpoints:
   - Sub-flows: `{base}/flows/{sub_flow_id}{original_query}`
   - Segments: `{base}/flows/{flow_id}/segments{query_and_timerange}`

#### Timerange Handling
- If a timerange is specified on the original request URL, it is passed through to both sub-flow and segment requests
- If no timerange is present in the original request but the flow response contains a timerange, this flow timerange is used for segment requests
- For live streams, segment requests use timerange starting from the last known segment end to fetch new segments
- Timerange format: `[start_seconds:nanoseconds_end_seconds:nanoseconds)` (e.g., `[10:0_20:0)` for 10-20 seconds)

### HTTP Authentication

Bearer tokens and other auth headers preserved via `avio_opts` dictionary:
- Copied from initial request during header parsing
- Reused for sub-requests (sub-flows, segments) to the same hostname
- Token expiration handling is outside demuxer scope

#### Hostname-Based Authentication
- Authentication options are only copied when the target URL has the same hostname as the original request
- This prevents credentials from being sent to different domains for security
- Uses `tams_same_host()` function to compare hostnames (case-insensitive)
- Pre-signed URLs with different hostnames are accessed without authentication headers

#### Pre-signed URL Support
- Segment `get_url` fields often contain pre-signed URLs with embedded authentication
- Pre-signed URLs are preferred as they avoid credential transmission and provide fine-grained access control
- When segment URLs use different hostnames (CDN, object storage), no authentication headers are added
- This supports common TAMS deployment patterns where API and media endpoints use different domains

### Pagination Strategy

The TAMS API implements pagination for segment lists using HTTP response headers (e.g., `Link`, `X-Total-Count`), but these headers are not exposed through FFmpeg's HTTP implementation (`libavformat/http.c`). As a workaround, pagination is implemented using timerange-based requests:

#### Timerange-Based Pagination
- **Initial Request**: Uses the flow's complete timerange or user-specified timerange
- **Subsequent Requests**: Start from the end timestamp of the last fetched segment
- **Logic**: `[last_segment_end:0_)` for open-ended continuation requests
- **Live Streams**: Continuously poll using this pattern to fetch new segments

#### Limitations
- Cannot use standard HTTP pagination headers for optimization
- Relies on TAMS API's timerange filtering accuracy
- May result in duplicate segments at boundaries (handled by timestamp filtering)
- No way to determine total segment count ahead of time

## Implementation Flow

### Header Reading (tams_read_header)
1. Parse JSON response into flows array
2. Extract timerange from URL query if present
3. Process each flow to create AVStreams
4. For multi-flows, fetch any missing sub-flows
5. Set up stream mapping arrays
6. Create TAMSStreamContext for each stream
7. Create/find TAMSSegmentContext for each unique segment source

### Packet Reading (tams_read_packet) 
1. Select stream with minimum current_ts
2. Ensure segments are available (lazy fetch)
3. Ensure segment sub-demuxer is open
4. Read packet from sub-demuxer
5. Handle segment EOF (advance to next segment)
6. Apply timestamp adjustments and range filtering
7. Remap stream index and return packet

### Memory Management
- Flows array grows as sub-flows are fetched
- Segment arrays allocated per TAMSSegmentContext and grow during pagination/live scenarios
- Sub-demuxers opened/closed as segments are processed
- Clean shutdown in tams_close() frees all allocated memory

#### Dynamic Segment List Growth
- **Initial Allocation**: Segment arrays start empty and grow as segments are fetched
- **Pagination Growth**: Each timerange-based pagination request appends new segments to existing arrays
- **Live Stream Growth**: Continuous polling adds new segments indefinitely for live streams
- **Memory Pattern**: `av_realloc_array()` used for safe array expansion during segment parsing
- **No Automatic Cleanup**: Old segments remain in memory for potential seeking
- **Memory Considerations**: Long-running live streams may accumulate large segment lists over time

## Design Decisions

### Lazy Segment Fetching
Segments are fetched only when first needed, not during header parsing. This allows the demuxer to start quickly and handle very long or infinite (live) streams.

### Sub-Demuxer Per Segment  
Each media segment is opened as a separate AVFormatContext. This provides robust format detection and parsing without reimplementing container demuxing logic.

## Current Limitations

- No optimization for shared segment contexts (duplicated I/O)
- No segment caching or smart prefetching
- No support for nested multi-flows

## Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| live_threshold | -1 (auto) | Seconds: segments_updated age threshold for live detection |
| live_timeout | -1 (auto) | Seconds: max segments_updated age before reverting to non-live |
| seg_poll_init | -1 (auto) | Initial segment poll interval (microseconds) |
| seg_poll_max | 30000000 | Max segment poll interval (30 seconds) |

Auto values are computed as multiples of flow segment_duration.