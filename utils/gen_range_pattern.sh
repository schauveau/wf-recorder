#!/bin/sh

#
# Generate a slight checkboard over a B&W gradient.
# The result is suitable image to detect
# clipping in highlight and shadows (due to bad color
# range interpretations).
#

convert \
    -size 512x512 -negate pattern:checkerboard \
    -size 512x512 gradient: -rotate 90 \
    -compose Mathematics -define compose:args="0,1,0.2,-0.1" -composite \
    -depth 8 -define png:color-type=2 range-pattern.png

identify range-pattern.png
