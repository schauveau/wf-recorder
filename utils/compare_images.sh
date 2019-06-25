#!/bin/bash

if [[ $# != 3 ]] ; then
    exit 1
fi
 
set -e

M=10

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
convert "$A" "$B"   -compose Mathematics -define compose:args="0,-$M,+$M,0" -composite "$less_x" 

BORDER=5

montage -pointsize 30 \
        -label "A"                 "$A" \
        -label "B"                 "$B" \
        -label "0.5+(A-B)"         "$diff"\
        -label "0.5+${M}*(A-B)"    "$diff_x" \
        -label "${M}*max(A-B,0)"   "$more_x" \
        -label "${M}*max(B-A,0)"   "$less_x" \
        -tile 3x2 -border $BORDER -geometry +$BORDER+$BORDER  "$out"

rm -f "$diff" "$diff_x" "$more_x" "$less_x" 

