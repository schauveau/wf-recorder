#!/bin/bash

M=10

if [[ $# != 3 ]] ; then
    echo "Usage: $0 image1 image2 output"
    echo
    echo "Uses ImageMmagick to compute the differences between of two images of "
    echo "sizes. The output is composed of two rows, ABC and DEF"
    echo
    echo "  A = The 1st image (image1)"
    echo "  B = The 2nd image (image2)"
    echo "  C = 0.5+(A-B)"
    echo "      Show the differences between each pixel relative to neutral gray"
    echo "      Ideally, that image should be prefectly gray"
    echo "  D = 0.0 + ${M}*max(A-B,0)" 
    echo "      Similar to C but shows only the positive differences amplified $M"
    echo "      times and relative to black".
    echo "  E = 0.5 + ${M}*(A-B)" 
    echo "      Similar to C but the differences are amplified $M times."
    echo "  F = 0.0 + ${M}*min(A-B,1.0)" 
    echo "      Similar to C but shows only the negative differences amplified $M"
    echo "      times and relative to white".
    echo 
    exit 1
fi
 
set -e


A="$1"
B="$2"
out="$3"

if [ -z "$prefix" ] ; then
    prefix="tmp_compare_image_$(date +%s)"
fi

diff="${prefix}_diff.png"
diff_x="${prefix}_diff_${M}.png"
more_x="${prefix}_more_${M}.png"
less_x="${prefix}_less_${M}.png"

convert "$A" "$B"   -compose Mathematics -define compose:args="0,1,-1,0.5" -composite "$diff" 
convert "$A" "$B"   -compose Mathematics -define compose:args="0,+$M,-$M,0.5" -composite "$diff_x" 
convert "$A" "$B"   -compose Mathematics -define compose:args="0,+$M,-$M,0" -composite "$more_x" 
#convert "$A" "$B"   -compose Mathematics -define compose:args="0,-$M,+$M,0" -composite "$less_x" 
convert "$A" "$B"   -compose Mathematics -define compose:args="0,+$M,-$M,1" -composite "$less_x" 

BORDER=5

montage -pointsize 30 \
        -label "A"                 "$A" \
        -label "B"                 "$B" \
        -label "0.5+(A-B)  [Gray]" "$diff"\
        -label "${M}xBrighter [Black]"     "$more_x" \
        -label "${M}xDiff [Gray]"         "$diff_x" \
        -label "${M}xDarker [White]"       "$less_x" \
        -tile 3x2 -border $BORDER -geometry +$BORDER+$BORDER  "$out"

rm -f "$diff" "$diff_x" "$more_x" "$less_x" 

