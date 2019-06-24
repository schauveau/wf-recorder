#/bin/bash

VIDEO="$HOME/Videos/Firework-short.webm"
VIDEO="${1:-$VIDEO}"

mpv --fullscreen --loop "$VIDEO" &
sleep 1
./run.sh
kill -9 %mpv



