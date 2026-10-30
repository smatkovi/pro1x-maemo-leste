#!/bin/sh
# A stand-in for pro1x-callaudio in run.sh: notes its start and its end
# (SIGTERM from pro1x-modemd after the last call) in $FAKE_AUDIO_LOG.
echo "start $$" >> "$FAKE_AUDIO_LOG"
trap 'echo "stop $$" >> "$FAKE_AUDIO_LOG"; exit 0' TERM
echo "callaudio: ready (stand-in)"
while :; do sleep 1; done
