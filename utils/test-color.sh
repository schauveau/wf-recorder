#!/bin/bash

#
# Usage: test-colors wf-recorder-x  [option ...]
#
# Execute the specified wf-recorder-x command with an additinnal --test-colors
# argument, extract the first frame of the output and display the color
# differences wth the reference image using compare_images.sh
#
#
# Example:
#
#   test-colors wf-recorder-x -p color_range=1 -e libx265 
#
#
#

HERE="$(dirname "$0")"

M=10  # Multiplication factor to make the differences more obvious

ts="$(date +%s)"
prefix="ctest_${ts}"
ref="${prefix}_ref.ppm"
log="${prefix}.log"
in="${prefix}_in.ppm"
out="${prefix}_out.png"

unset ANIMATE_TEST_COLOR

COLOR_TEST_PPM="ctest_ref.ppm"

# re-generate the reference file if it does not exist
if [ -f "$COLOR_TEST_PPM" ] ; then 
    export COLOR_TEST_PPM
fi

# Ask wf-recorder-x to generate an info file (so we can get
# the output file name)
export COLOR_TEST_INFO="${prefix}.info"

(
    set -e
    set -x

    "$@" --test-colors

    read movie < "$COLOR_TEST_INFO"

    echo "Movie file = $movie"
    
    if [ ! -f "$movie" ] ; then
        echo "=== No video file to process: '$movie'"
        exit 1
    fi
    
    ffmpeg -y -i "$movie" -vframes 1 "$in"
    
    if [ ! -f "$in" ] ; then
        echo "=== Failed to extract frame :'$in'" 
        exit 1
    fi
    
    "$HERE/compare_images.sh" "$COLOR_TEST_PPM" "$in" "$out"

    rm -f "$in"  

) 2>&1 | tee "$log"

echo "Output: $out"

imvr -u nearest_neighbour "$out" &
