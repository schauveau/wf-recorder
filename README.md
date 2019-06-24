# wf-recorder-x

This is a modified version of [wf-recorder](https://github.com/ammen99/wf-recorder) that uses FFMpeg filters to simplify the implementation of the video encoder. The intent was not and is still not to create an official fork of wl-recorder.

Be aware that the command line options of wf-recorder-x and wf-recorder are significantly different. Please use the `-h` or `--help` option (that is one of the new features) or read carefully the information below before complaining that it does not work exactly as with the original version.

# Installation

## from source

```
git clone https://github.com/schauveau/wf-recorder-x && cd wf-recorder-x
meson build --prefix=/usr --buildtype=release
ninja -C build
```
Optionally configure with `-Ddefault_codec='codec'`. The default is libx264. Now you can just run `./build/wf-recorder-x` or install it with `sudo ninja -C build install`.

# Usage
In its simplest form, run `wf-recorder-x` to start recording and use Ctrl+C to stop. This will create a file called recording.mp4 in the current working directory using the default codec.
 
```
Usage: ./build/wf-recorder-x[options] [-f output.mp4]
  -h, --help                       Show this help
  -s, --screen=SCREEN              Specify the output to use. SCREEN is the
                                   Wayland output number or identifier.
  -o, --output=FILENAME            Similar to --file but the FILENAME is formatted
                                   as described for the 'date' command. For example:
                                       --output screencast-%F-%Hh%Mm%Ss.mp4
  -f, --file=FILENAME              Set the name of the output file. The default
                                   is recording.mp4
  -g, --geometry=REGION            Specify a REGION on screen using the 'X,Y WxH' format
                                   compatible with 'slurp'
  -e, --encoder=ENCODER            Specify the FFMpeg encoder codec to use. 
                                   The default is 'libx264'.
                                   See also: ffmpeg -hide_banner -encoders
  -M, --hide-mouse                 Do not show the mouse cursor in the recording
  -m, --show-mouse                 Show the mouse cursor in the recording. This is the default.
  -p, --param=NAME=VALUE           Set an encoder codec parameter
                                   See also: ffmpeg -hide_banner -h encoder=...
  -d, --hw-device=DEVICE           Specify the hardware decoding device.
                                   This is only relevant if an hardware encoder
                                   is selected. For VAAPI encoders, that would 
                                   be something like /dev/dri/renderD128
      --hw-accel=NAME              Select an hardware accelerator.
                                   See also: ffmpeg -hide_banner -hwaccels
      --ffmeg-debug                Enable FFMpeg debug output
  -a, --audio[=DEVICE]             Enable audio recordig using the specified Pulseaudio
                                   device number or identifier
  -y, --yuv420p                    Use the encoding pixel format YUV210P if possible
                                   That option is mostly intended for software encoders.
                                   For hardware encorders, the pixel format is usually set
                                   using a filter
  -v, --video-filter=FILTERS       Specify the FFMpeg video filters.
  -T, --video-trace                Trace progress of video encoding
      --vaapi                      Alias for --hw-accel=vaapi

```

# New features (not in [wf-recorder](https://github.com/ammen99/wf-recorder) as of June 24th 2019)

## A proper --help or -h option

Obviously!

## Disable the capture of the mouse cursor.

See the `-M` option. 

That feature has nothing to do with the FFMpeg filters and it should be easy to implement in wf-recorder.

## Video filters from the FFMpeg library 'libavfilter'

This is the most important modification but the part that handles the filters in frame-writer.cc is surprisingly small, about 200 lines, but other parts of the code are greatly simplified (no more manual configuration for vaapi, HW frames management, ...). 

FFMpeg filters are documented [here](https://ffmpeg.org/ffmpeg-filters.html), [here](https://www.ffmpeg.org/doxygen/4.1/group__lavfi.html) and [here](https://trac.ffmpeg.org/wiki/FilteringGuide) but we are only interested by the [Video filters](http://ffmpeg.org/ffmpeg-filters.html#Video-Filters).

# Frequently Asked Question

## Did people really asked those question?

No. So far I just made them up. 

## Why the name wl-recorder-x? Does it work with X11? 

Absolutely not. The `x` stands for *eXperimental*. I am just bad at finding new names.

## Can I fill bug report or ask for new features?

Yes but be aware that this is just an experimental project. Support is likely to be very very limited. 

## Does it work with other accelerators than VAAPI?

In theory that should work if ffmpeg can also do it. 

The first step would be to find the proper ffmpeg command (see https://trac.ffmpeg.org/wiki/HWAccelIntro ) and then convert it to wl-encoder-x.

For example, if the following works for you,
```ffmpeg -i input.mp4 -c:v h264_nvenc -profile high444p -pixel_format yuv444p -preset default output.mp4```  
then I expect that the following should also work with wl-encoder-x:
```wl-encoder-x -e h264_nvenc -p profile=high444p -p preset=default -v 'format=yuv444p' ```

Remark: In the previous example, the conversion from RGB to YUV444p is done by the CPU. This is probably quite inefficient so hw-specific filters should probably be used (`hwupload`,`scale_npp`, ...).

Do not hesitate to fill a bug report if you have questions or additional information about any hw devices.   

# Interesting Software Video Filter 

Here is a non-exaustive list of video filters that could be interested to wl-recorder-x users. 

For more details, see https://ffmpeg.org/ffmpeg-filters.html#toc-Video-Filters or use `ffmpeg -h filter=xxxx` (where xxxx is the filter name).

## fps - Change the framerate by duplicating or dropping frames. 

Most monitors are now 60Hz or more which it probably too much in most cases. The fps filter provides a simple way to get a smaller framerate by dropping some frames.

**Example**: Record only 5 frames per second
```wl-recorder-x -v fps=5``` 

**Note**: The filter `framerate` can do the same using interpolation (instead of droping or duplicating frames) but it is probably not suitable for a real-time encoder.  

**Note**: When reducing the framerate, the `fps` filter should probably be executed first.

**Note**: The `fps` filter is cheap (no physical copies).

## framestep - Select one frame every N frames.

This is a good alternative to `fps` when you do not care about the exact frame rate.

**Example**: Keep only 1/3 of all frames which should give you approximately 20 fps on a typical 60 Hz monitor (59.9???? Hz in practice).

```wl-recorder-x -v framestep=3``` 

## vflip - Perform a vertical flip.

**Note**: This is a cheap vertical flip using pointer arithmetic. No physical copy.  

**Note**: In theory, vflip should never be necessary since the vertical orientation is already handled automatically according to the wayland output description. 


## drawtext - Draw text on top of the video

**Example**: Draw the current date and time in the top-left corner during the first 2 seconds. 

```
text=$(date +%c | sed 's/:/\\:/g')
wf-recorder-x -v "drawtext=enable='between(t,0,2)':text='$text':fontcolor=red:fontsize=20:x=40:y=40-ascent:box=1:boxborderw=4"```
```

**Note**: The `enable` options is provided by the [Timeline Editing](https://ffmpeg.org/ffmpeg-filters.html#Timeline-editing) functionnality shared my many filters.

## eq  - Adjust brightness, contrast, gamma, and saturation.

**Note**: VAAPI users may want to use procamp_vaapi instead.

## format - Change the pixel format

## scale: Scale the input video size and/or convert the image format

**Note**: VAAPI users may want to use scale_vaapi instead.

# Hardware acceleration

## General rules

I only have VAAPI but other HW frameworks should work fine (Yeah! Sure ...) assuming that the proper filters and encoder options are specified.  

The current status of Hardware Acceleration in FFMpeg is described [here](https://trac.ffmpeg.org/wiki/HWAccelIntro) 

## VAAPI

A lot of good tips are given [here](https://trac.ffmpeg.org/wiki/Hardware/VAAPI).

I am trying to configure wf-recorder-x  so that it works out of the box when a vaapi encoder is specified.  
```
wf-recorder-x -e h264_vaapi
wf-recorder-x -e hevc_vaapi
wf-recorder-x -e vp8_vaapi
wf-recorder-x -e vp9_vaapi
```
When a known vaapi encoder is used, the default behavior is to use the device `/dev/dri/renderD128`. That can be changed with the `-d` option:
```
wf-recorder-x -e h264_vaapi -d /dev/dri/renderD129
```
So far, the default profile of all VAAPI encoders is using the `nv12` pixel format and their default filter is defined as `hwupload,scale_vaapi=format=nv12`
  
- `hwupload` takes care of uploading the image to the device (in RGB format).
- `scale_vaapi=format=nv12`performs the conversion RGB -> NV12

Another possibility is to do first the conversion using `format=nv12,hwupload`but this is of course using more CPU (and less GPU).

Filters may be required if the selected profile is using a different pixel format than the default for that encoder. For example, on my system the VAAPI HEVC encoders supports the profile  `main10` is using the pixel format `p010` (so 10 bit depth vs 8 bits for `nv12`). The default filters are also not used when the `-v` option is used.
```
# Record at 25 frames per second 
wf-recorder-x -v "fps=25,hwupload,scale_vaapi=format=nv12" -e h264_vaapi
# Record using the HEVC main10 profile.
wf-recorder-x -v "hwupload,scale_vaapi=format=p010" -e hevc_vaapi -p profile=main10
```
A few filters can be used after hwupload, so on the GPU:
```
(shell) ffmpeg -hide_banner -filters | grep vaapi 
 ... deinterlace_vaapi V->V       Deinterlacing of VAAPI surfaces
 ... denoise_vaapi     V->V       VAAPI VPP for de-noise
 ... procamp_vaapi     V->V       ProcAmp (color balance) adjustments for hue, saturation, brightness, contrast
 ... scale_vaapi       V->V       Scale to/from VAAPI surfaces.
 ... sharpness_vaapi   V->V       VAAPI VPP for sharpness
```
`deinterlace_vaapi`, `denoise_vaapi`, and `sharpness_vaapi` are probably not relevant here.

`scale_vaapi` can scale the image and/or change the pixel format.

```
(shell) ffmpeg -hide_banner -h filter=scale_vaapi
Filter scale_vaapi
  Scale to/from VAAPI surfaces.
    Inputs:
       #0: default (video)
    Outputs:
       #0: default (video)
scale_vaapi AVOptions:
  w                 <string>     ..FV..... Output video width (default "iw")
  h                 <string>     ..FV..... Output video height (default "ih")
  format            <string>     ..FV..... Output video format (software format of hardware frames)
```

`procamp_vaapi` can be used to adjust brightness, contrast, saturation and hue on the GPU. 

```(shell) ffmpeg -hide_banner -h filter=procamp_vaapi
Filter procamp_vaapi
  ProcAmp (color balance) adjustments for hue, saturation, brightness, contrast
...
procamp_vaapi AVOptions:
  b                 <float>      ..FV..... Output video brightness (from -100 to 100) (default 0)
  brightness        <float>      ..FV..... Output video brightness (from -100 to 100) (default 0)
  s                 <float>      ..FV..... Output video saturation (from 0 to 10) (default 1)
  saturatio         <float>      ..FV..... Output video saturation (from 0 to 10) (default 1)
  c                 <float>      ..FV..... Output video contrast (from 0 to 10) (default 1)
  contrast          <float>      ..FV..... Output video contrast (from 0 to 10) (default 1)
  h                 <float>      ..FV..... Output video hue (from -180 to 180) (default 0)
  hue               <float>      ..FV..... Output video hue (from -180 to 180) (default 0)
```

**Remark**: Yes! The name of the argument is `saturatio`. This is likely a mistake that will be fixed in a later version of ffmpeg so better use its short alias `s`. 

For example, it is possible to generate a miniature 64x480 black & white recording at 10 fps as follow:
```
wf-recorder-x -v "fps=25,hwupload,scale_vaapi=format=nv12:w=640:h=480,procamp_vaapi=s=0'" -e h264_vaapi
```
# Color accuracy

Color accuracy is a very difficult topic, especially when converting images between different RGB and YUV formats. 

As of today, [wf-recorder](https://github.com/ammen99/wf-recorder) does not even attempt to handle the color space and the color range which can cause some significant differences in the appearance of the recording.

A simple procedure to check the accuracy of a screencast is
- Load a color chart such a as [this one](http://www.galerie-photo.com/images/mire-16cm-RVB.jpg) in your favorite image viewer.
- Do a short recording of the whole screen.
- Switch to the next or previous workspace.
- Play the video fullscreen indefinitely (e.g. `mpv --loop --fullscreen recording.mp4`)
- Use keyboard shortcuts to quickly swap between the two workspaces. 

The colors will never be perfectly accurate (especially on small text beccause of YUV compression) but two major issues can be noted on the color chart:
- A significant difference in the brightness indicates that the color range is incorrect. This is usually very noticeable at the two extremes of the gray scales.  
- A significant difference in the color saturation indicates that the color space is incorrect. 

## Color Range

There are 2 possible color ranges:
- The limited range ('mpeg', 'tv' or value 1) is only using values 16-235.
- The full range ('jpeg', 'pc', or value 2) is using all values 0-256.

For example an incorrect color range conversion can cause a drak gray (e.g. brightness < 20) to become totally black. The color range can normally be changed by passing a `color_range` option to the encoder (see man ffmpeg-codecs):
```
# Use the limited/MPEG/TV range
wf-recorder-x -e libx264 -p color_range=tv -f recording-full.mkv
# Use the full/JPEG/PC range
wf-recorder-x -e libx264 -p color_range=pc -f recording-limited.mkv
```

The default in wf-recording-x is to currently to use color_range=pc by default but, unfortunately, some encoders are ignoring that option. Here is the current state of the color range support on my system (Debian, VAAPI via i965_drv) using a mkv file format. 

- `h264_vaapi*, *hevc_vaapi`, `libx264`, `libx265`, `libxvid`

   > have correct with color_range=pc (the default)

- `vp8_vaapi`, `vp9_vaapi`, `libvpx`, and `libvpx-vp9`  
   > have incorrect color range. There is probably something specific that need to be doce for those VP format.

## Color space

The color space is controlled by setting the encoder option `colorspace` (see man ffmpeg-codecs). 

In found that `bt470bg` (PAL/SECAM) and `smpte170m`(NTSC)  are the only two color spaces that give good result. The default value in `wf-recording-x` is `bt470bg`. 

Remark: that has nothing to do with your actual location. 

# TODO list, Future improvements

## Adjust the default filter according to the selected profile. 

For example, `wf-recorder-x -e hevc_vaapi -p profile=main10` should work without having to give a filter.

The profile name could also be given in the `-e` options:
```wf-recorder-x -e hevc_vaapi:main10````

## Add command line options to stop

after a given time, or limit the size of the output file, ...

## Better shutdown on Ctrl-C

Can occasionally segfault on Ctrl-C and produce a corrupted file which is annoying.

## Allow multiple -v options

So allow
```wl-recorder-x -v fps=10 -v format=nv12 -v hwupload -e h264_vaapi```
instead of
```wl-recorder `-v fps=10,format=nv12,hwupload```

Also, it would be nice if the default filter could be inserted automatically between user-defined filters. Something like may be:

```wl-recorder-x -v fps=10 -v DEFAULT -v procamp_vaapi=contrast=1.5' -e h264_vaapi```


# FFMpeg tips

The ffmpeg and ffprobe commands can provide a lot of information about the FFMpeg capabilities.

## List all supported video encoders

Use `ffmpeg -encoders`. You can filter out the non-video encoders (audio, images, ...) as follow:

```
(shell) ffmpeg -hide_banner -encoders | grep -E '^ V' | grep -F '(codec' | cut -c 8- | sort
 flv                  FLV / Sorenson Spark / Sorenson H.263 (Flash Video) (codec flv1)
 h263_v4l2m2m         V4L2 mem2mem H.263 encoder wrapper (codec h263)
 h264_omx             OpenMAX IL H.264 video encoder (codec h264)
 h264_v4l2m2m         V4L2 mem2mem H.264 encoder wrapper (codec h264)
 h264_vaapi           H.264/AVC (VAAPI) (codec h264)
 hevc_v4l2m2m         V4L2 mem2mem HEVC encoder wrapper (codec hevc)
 ...
```

## List all supported HW accelerators.

Use `ffmpeg --hwaccels` 

```
(shell) ffmpeg -hide_banner -hwaccels
Hardware acceleration methods:
vdpau
vaapi
drm
```
  

## List all supported video filters.  
  
Use `ffmpeg --filters`  and keep only the `V->V` entries (i.e. video to video)
  
```  
(shell) ffmpeg -hide_banner -filters  | grep -F 'V->V' 
Hardware acceleration methods:  
vdpau  
vaapi  
drm  
```

## Get details about a specific encoder, filter, or muxer

```
(shell) ffmpeg  -hide_banner -h encoder=libx264
...
(shell) ffmpeg  -hide_banner -h filter=scale
...
(shell) ffmpeg  -hide_banner -h muxer=webm
...
```
## Useful bash aliases 

```
# Various aliases to get information using ffmpeg and ffprobe.
# See also man ffmpeg-codecs 

ff-list-encoders() { ffmpeg -hide_banner -encoders "$@" ; }
ff-list-filters() { ffmpeg -hide_banner -filters "$@" ; }
ff-list-decoders() { ffmpeg -hide_banner -decoders "$@" ; }
ff-list-hwaccels() { ffmpeg -hide_banner -hwaccels "$@" ; }

# Warning: most ffmpeg options also accept a few pixel formats
# aliases that are not listed here. For example, 'rgb32' means
# either 'argb' or 'bgra' depending of the system endianness.
ff-list-pixel-formats() { ffmpeg -hide_banner -pix_fmts "$@"; }
ff-list-pixel-formats-detailed() { ffprobe  -hide_banner -print_format json  -show_pixel_formats "$@" ; }

ff-video-encoders() { ff-list-encoders | grep -E '^ V' | grep -F '(codec' | cut -c 8- | sort ; }
ff-video-decoders() { ff-list-encoders | grep -E '^ V' | grep -F '(codec' | cut -c 8- | sort ; }
# Warning: that one does not show sinks and buffers 
ff-video-filters() { ff-list-filters  | grep --color=never -F 'V->V' ; }

ff-video-encoders-short() { ff-video-encoders | cut -c -18 ; }
ff-video-decoders-short() { ff-video-decoders | cut -c -18 ; }

ff-help-encoder() { ffmpeg -hide_banner -h encoder="$1" ; } 
ff-help-decoder() { ffmpeg -hide_banner -h decoder="$1" ; } 
ff-help-filter() { ffmpeg -hide_banner -h filter="$1" ; } 

# Dump the content of a video file

ff-show-format()  { ffprobe -hide_banner -print_format json  -show_format "$@" ; } 
ff-show-streams() { ffprobe -hide_banner -print_format json  -show_streams "$@" ; } 
ff-show-packets() { ffprobe -hide_banner -print_format json  -show_packets "$@" ; } 
ff-show-frames()  { ffprobe -hide_banner -print_format json  -show_frames "$@" ; } 

# Dump only the 1st video stream (so of index 0)
ff-show-video-streams-1() { ff-show-streams -select_streams v:0 "$@" ; } 
ff-show-video-packets-1() { ff-show-packets -select_streams v:0 "$@" ; } 
ff-show-video-frames-1() { ff-show-frames -select_streams v:0 "$@" ; } 
```

## The FFMpeg filter syntax for dummies 

The filters are separated by a comma so `fps=25,format=nv12,hwupload` is actually composed of 3 filters applied in sequence: `fps=25` then `format=nv12` then `hwupload`.

The `null` video filter does nothing. The input frame is passed to the output unchanged. For example, `fps=12,null,hwupload` is strictly equivalent to `fps=12,hwupload`

A `=`following the filter name indicates a list of arguments separated by colons `:`. Each argument shall be of the form `name=value` but, for some filters, the argument names can be omitted (i.e. positional parameters).  

Here are a few example using the Gaussian Blur filter (see `ffmpeg -hide_banner -h filter=gblur`):
- `gblur=sigma=0.7` is filter `gblur`with argument `sigma=0.7`
- `gblur=steps=3,sigma=0.7` is filter `gblur`with arguments `step=3` and `sigma=0.7`
- `gblur=0.7,3` is filter `gblur`with arguments `sigma=0.7`, `step=3` because sigma and step are the 1st and 2nd positional arguments of `gblur`. 


