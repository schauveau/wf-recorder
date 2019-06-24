#!/bin/bash

APP=../build/wf-recorder-x

DURATION=4s

OPT="fps=25"
OPT_HW="$OPT,hwupload"

SOFT_ENC=(libx264 libx265 libvpx libvpx-vp9 mpeg4 libxvid)
VAAPI_ENC=(h264_vaapi hevc_vaapi vp8_vaapi vp9_vaapi)

ALL_ENC=( "${SOFT_ENC[@]}" "${VAAPI_ENC[*]}" )

# The default colorspace is crap. Better use bt470bg (PAL) or smpte170m (NTSC)
CS="-p colorspace=bt470bg"

CR1="-p color_range=1"
CR2="-p color_range=2"

# The FFMPEG colorspace (from man ffmpeg-codecs)
COLORSPACES=(
    "rgb"        
    "bt709" 
    "unknown" 
    "fcc" 
    "bt470bg"    # Good. (PAL)
    "smpte170m"  # Good. (NTSC)
    "smpte240m" 
    "ycgco" 
    "bt2020nc" 
    "bt2020c" 
    "smpte2085" 
    "unspecified" 
    "ycocg" 
    "bt2020_ncl" 
    "bt2020_cl"
)

# The file 'files' contain the list of all generated movie files
# Convenient to do an automated test 
FILES=files
rm -f $FILES

run () {
    id="$1"
    ext="$2"
    log="bench-$id.log"
    out="bench-$id.$2"
    shift
    shift
    echo
    if [ -f "$log" ] ; then
        echo "Skip: File $log already exists"
        return
    fi
    rm -f "$out" ; touch $out   # empty files are easier to detect than missing files 
    echo "####"
    echo "####  $id"
    echo "####"
    echo "$@" -f "bench-$id.$ext"
    echo timeout --signal=SIGINT $DURATION "$@" -f "bench-$id.$ext" > "$log"
    timeout --signal=SIGINT $DURATION time "$@" -f "bench-$id.$ext" >> "$log" 2>&1
    echo "bench-$id.$ext"  >> $FILES
    # Play a sound to indicate progress (nice when using a fullscreen animation)
    paplay ~/Pictures/bip.wav >/dev/null 2>&1
}


test1 () {

    if false ; then
        
        #run h264_mpeg2-auto     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e h264_vaapi $CR2
        #run h264_mpeg2-main     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e h264_vaapi $CR2 -p profile=main
        
        run h264_vaapi-auto     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e h264_vaapi $CS $CR2 
        run h264_vaapi-main     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e h264_vaapi $CS $CR2 -p profile=main
        run h264_vaapi-cb       mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e h264_vaapi $CS $CR2 -p profile=constrained_baseline
        run h264_vaapi-high     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e h264_vaapi $CS $CR2 -p profile=high
            
        # HEVC main10 profile is using 10bit yuv (instead of 8bit). This is pixel format p010
        run hevc_vaapi-auto     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e hevc_vaapi $CS $CR2 
        run hevc_vaapi-main     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e hevc_vaapi $CS $CR2 -p profile=main
        run hevc_vaapi-main10   mkv $APP -v "$OPT_HW,scale_vaapi=format=p010" -e hevc_vaapi $CS $CR2 -p profile=main10
        
        run vp8_vaapi-auto1     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e vp8_vaapi  $CS $CR1 
        run vp8_vaapi-auto2     mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e vp8_vaapi  $CS $CR2
        
        # VAProfileVP9Profile0 according to vainfo
        # The color range is obviously incorrect here but color_range has no effect on vp9_vaapi
        run vp9_vaapi-0         mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e vp9_vaapi  $CS $CR1 -p profile=0
        
        # An alternative is to do the color conversion on the CPU but this is slower
        run vp9_vaapi-0-cpu1    mkv $APP -v "format=nv12,$OPT_HW" -e vp9_vaapi $CS $CR1 -p profile=0
        run vp9_vaapi-0-cpu2    mkv $APP -v "format=nv12,$OPT_HW" -e vp9_vaapi $CS $CR2 -p profile=0

    fi
}


# Try all colorspaces
test2 () {
    for cs in "default" "${COLORSPACES[@]}"; do
        if [ "$cs" == "default" ] ; then
            opt=""
        else
            opt="-p colorspace=$cs"
        fi
        run colorspace-$cs mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e hevc_vaapi $CR2 -p profile=main $opt        
        #run colorspace-$cs mkv $APP -v "$OPT" -e libx264 $CR1 $opt   
    done
}


# Simply test: do all encoders with same options
test3 () {
    if true ; then
        CR[1]="$CR1"
        CR[2]="$CR2"
        for i in 1 2 ; do
            for enc in "${SOFT_ENC[@]}" ; do 
                run $enc-$i  mkv $APP -v "$OPT" -e $enc $CS ${CR[$i]}
            done
            for enc in "${VAAPI_ENC[@]}" ; do 
                run $enc-$i  mkv $APP -v "$OPT_HW,scale_vaapi=format=nv12" -e $enc $CS ${CR[$i]}
            done
        done
    fi
}


# Try all encoders with the default options
test4 () {
    if true ; then
        for enc in "${SOFT_ENC[@]}" "${VAAPI_ENC[@]}" ; do 
            run $enc  mkv $APP -e $enc 
        done
    fi
}

test4





