FATE_SAMPLES_TAMS-$(call DEMDEC, TAMS,, AAC_DEMUXER) += fate-tams-audio
fate-tams-audio: CMD = framecrc -i $(TARGET_SAMPLES)/tams/api/flows/audio-flow.json -c copy

FATE_SAMPLES_TAMS-$(call DEMDEC, TAMS,, MPEGTS_DEMUXER) += fate-tams-video
fate-tams-video: CMD = framecrc -i $(TARGET_SAMPLES)/tams/api/flows/video-flow.json -c copy

FATE_SAMPLES_TAMS-$(call DEMDEC, TAMS,, MPEGTS_DEMUXER AAC_DEMUXER) += fate-tams-multi
fate-tams-multi: CMD = framecrc -i $(TARGET_SAMPLES)/tams/api/flows/multi-flow.json -c copy

FATE_SAMPLES_TAMS-$(call DEMDEC, TAMS,, MOV_DEMUXER) += fate-tams-multi-mapped
fate-tams-multi-mapped: CMD = framecrc -i $(TARGET_SAMPLES)/tams/api/flows/multi-mapped-flow.json -c copy

FATE_SAMPLES_TAMS += $(FATE_SAMPLES_TAMS-yes)
FATE_SAMPLES_FFMPEG += $(FATE_SAMPLES_TAMS)
fate-tams-demux: $(FATE_SAMPLES_TAMS)
