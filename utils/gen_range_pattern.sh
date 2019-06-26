#
# Generate a gradient checkboard patter suitable to detect
# color range issues (clipping in highlight and shadows).
#

convert -negate -size 512x512 pattern:checkerboard A.png
convert -size 512x512 gradient: B.png
convert \
    -negate -size 512x512 pattern:checkerboard \
    -size 512x512 gradient: \
    -compose Mathematics -define compose:args="0,1,0.2,-0.1" -composite \
    -depth 8 -define png:color-type=2 range-pattern.png

identify range-pattern.png
