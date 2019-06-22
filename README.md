# wf-recorder-x

This is a modified version of [wf-recorder](https://github.com/ammen99/wf-recorder) that uses FFMpeg filters to simplify the implementation of the video encoder. The intent was not and is still not to create an official fork from wl-recorder. 

Be aware that the command line options of wf-recorder-x are significantly differnent from wf-recorder. Please use the -h or --help option (that is one of the new features) or read carefully the information below before complaining that it does not work exactly as the original.

# Installation

## from source

```
git clone https://github.com/schauveau/wf-recorder-x && cd wf-recorder-x
meson build --prefix=/usr --buildtype=release
ninja -C build
```
Optionally configure with `-Ddefault_codec='codec'`. The default is libx264. Now you can just run `./build/wf-recorder-x` or install it with `sudo ninja -C build install`.

# Usage
In it's simplest form, run `wf-recorder-x` to start recording and use Ctrl+C to stop. This will create a file called recording.mp4 in the current working directory using the default codec.
 
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


