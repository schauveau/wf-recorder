#!/bin/bash

#
# This script uses ffmpeg to encode a short video from a reference
# image using the ffmpeg options passed as arguments. 
#
# Then it extracts the first frame and uses 'convert' (from ImageMagick)
# to highlight the color differences.
#
# The following environments variables can be used to control the behavior:
#   CODEC  = The codec to use
#   REF    = The input reference image
#   VIEWER = The name of the image viewer 
#   MULT   = A sequence of multiplication factors used to highly the differences (e.g "1 10")
#
#
# If no command line options are specified then some reasonnable defaults
# are provided according to the codec
#
# Examples of good reference images:
#    http://www.galerie-photo.com/images/mire-16cm-RVB.jpg
#    http://www.allinbox.com/mires/yotho.jpg
#    http://fred.just.free.fr/Photo/mire10x15.png
#
# Usage example:
#
#   export VIEWER=display
#   export REF=$HOME/Pictures/mire.png
#   ./show-ffmpeg-ranges.sh 
#   CODEC=h264_vaapi ./show-ffmpeg-ranges.sh -vaapi_device /dev/dri/renderD128  -vf 'showinfo,hwupload,scale_vaapi=format=nv12' -color_range pc 
#   Currently alway bad color range for vp9  
#   CODEC=vp9_vaapi ./show-ffmpeg-ranges.sh -vaapi_device /dev/dri/renderD128  -vf 'showinfo,hwupload,scale_vaapi=format=nv12' -c:v vp9_vaapi
#   but works well when yuv conversion is done on CPU 
#   CODEC=vp9_vaapi /show-ffmpeg-ranges.sh -vaapi_device /dev/dri/renderD128  -vf 'format=nv12,showinfo,hwupload' -c:v vp9_vaapi
#
#

set -e # Stop on any error

if [ -z "$CODEC" ] ; then
    echo "Please specify an environement variable CODEC"
    exit 1
fi

ARGS_VAAPI=( -vaapi_device /dev/dri/renderD128  -vf 'showinfo,hwupload,scale_vaapi=format=nv12'  ) 

if [ "$#" == 0 ] ; then
    # provide default args according to the given codec
    case "$CODEC" in
        h264_vaapi) ARGS=( "${ARGS_VAAPI[@]}" -color_range pc ) ;;
        hevc_vaapi) ARGS=( "${ARGS_VAAPI[@]}" -color_range pc ) ;;
        vp9_vaapi)  ARGS=( "${ARGS_VAAPI[@]}" ) ;;
        *_vaapi)    ARGS=( "${ARGS_VAAPI[@]}" ) ;;
        *)          ARGS=( -vf'showinfo' ) 
    esac
else
    ARGS=( "$@" )
fi

# A few environment variables tp control the behavior

if [ -z "$REF" ] ; then
    REF="reference.png"
fi   

if [ -z "$VIEWER" ] ; then
    VIEWER="true"  # do not use any image viewer by default
fi

if [ -z "$MULT" ] ; then
    MULT="1 10"
fi

# Convert reference image to PPM format. 
# that is imported by ffmpeg as rgb24 with
# default colorspace
convert "$REF" "in.ppm"   

PS4='##### ' 
set -x

ffmpeg -y -i "in.ppm" -t 1  "${ARGS[@]}" -c:v "$CODEC" "$CODEC-out.mkv"

ffmpeg -y -i "$CODEC-out.mkv" -vframes 1 "$CODEC-out.png"
$VIEWER "$CODEC-out.png"

  # Show differences amplified N times
for N in $MULT ; do
    convert "in.ppm" "$CODEC-out.png"  -compose Mathematics -define compose:args="0,+$N,-$N,0.5" -composite "$CODEC-diff$N.png"
    $VIEWER "$CODEC-diff$N.png"
done

set +x

echo -n "# Using ffmpeg arguments: "
printf " %b" "${ARGS[@]}"
echo

