#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <iostream>
#include <iomanip>
#include <sstream>

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <getopt.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "frame-writer.hpp"
#include "pulse.hpp"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "config.h"

// Known mapping between encoder names and hwaccel method
std::map<std::string,std::string> auto_hwaccel =
  {
   { "h264_vaapi",  "vaapi" },
   { "mpeg2_vaapi", "vaapi" },
   { "hevc_vaapi",  "vaapi" },
   { "mjpeg_vaapi", "vaapi" },
   { "vp8_vaapi",   "vaapi" },
   { "vp9_vaapi",   "vaapi" }
   };

// Default filters for some hwaccel methods.
std::map<std::string,std::string> auto_filter =
  {
   { "h264_vaapi" , "hwupload,scale_vaapi=format=nv12" }  ,
   { "hevc_vaapi" , "hwupload,scale_vaapi=format=nv12" }  ,
   { "vp8_vaapi"  , "hwupload,scale_vaapi=format=nv12" }  ,
   { "vp9_vaapi"  , "hwupload,scale_vaapi=format=nv12" }  ,
  }; 

std::mutex frame_writer_mutex, frame_writer_pending_mutex;
std::unique_ptr<FrameWriter> frame_writer;

static struct wl_shm *shm = NULL;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;

struct wf_recorder_output
{
    wl_output *output;
    zxdg_output_v1 *zxdg_output;
    std::string name, description;
    int32_t x, y, width, height;
};

std::vector<wf_recorder_output> available_outputs;

static void handle_xdg_output_logical_position(void*,
    zxdg_output_v1* zxdg_output, int32_t x, int32_t y)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output)
        {
            wo.x = x;
            wo.y = y;
        }
    }
}

static void handle_xdg_output_logical_size(void*,
    zxdg_output_v1* zxdg_output, int32_t w, int32_t h)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output)
        {
            wo.width = w;
            wo.height = h;
        }
    }
}

static void handle_xdg_output_done(void*, zxdg_output_v1*) { }

static void handle_xdg_output_name(void*, zxdg_output_v1 *zxdg_output_v1,
    const char *name)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.name = name;
    }
}

static void handle_xdg_output_description(void*, zxdg_output_v1 *zxdg_output_v1,
    const char *description)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.description = description;
    }
}


const zxdg_output_v1_listener xdg_output_implementation = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description
};

struct wf_buffer
{
    struct wl_buffer *wl_buffer;
    void *data;
    enum wl_shm_format format;
    int width, height, stride;
    bool y_invert;

  
    timespec presented;
    uint32_t base_usec;

    std::atomic<bool> released{true}; // if the buffer can be used to store new pending frames
    std::atomic<bool> available{false}; // if the buffer can be used to feed the encoder
};

std::atomic<bool> exit_main_loop{false};

#define MAX_BUFFERS 16
wf_buffer buffers[MAX_BUFFERS];
size_t active_buffer = 0;

bool buffer_copy_done = false;

static int backingfile(off_t size)
{
    char name[] = "/tmp/wf-recorder-shared-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
        return -1;
    }

    int ret;
    while ((ret = ftruncate(fd, size)) == EINTR) {
        // No-op
    }
    if (ret < 0) {
        close(fd);
        return -1;
    }

    unlink(name);
    return fd;
}

static struct wl_buffer *create_shm_buffer(uint32_t fmt,
    int width, int height, int stride, void **data_out)
{
    int size = stride * height;

    int fd = backingfile(size);
    if (fd < 0) {
        fprintf(stderr, "creating a buffer file for %d B failed: %m\n", size);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %m\n");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        stride, fmt);
    wl_shm_pool_destroy(pool);

    *data_out = data;
    return buffer;
}

static void frame_handle_buffer(void *, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
    uint32_t width, uint32_t height, uint32_t stride)
{
    auto& buffer = buffers[active_buffer];

    buffer.format = (wl_shm_format)format;
    buffer.width = width;
    buffer.height = height;
    buffer.stride = stride;

    if (!buffer.wl_buffer) {
        buffer.wl_buffer =
            create_shm_buffer(format, width, height, stride, &buffer.data);
    }

    if (buffer.wl_buffer == NULL) {
        fprintf(stderr, "failed to create buffer\n");
        exit(EXIT_FAILURE);
    }

    zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
}

static void frame_handle_flags(void*, struct zwlr_screencopy_frame_v1 *, uint32_t flags) {
    buffers[active_buffer].y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t tv_sec_hi, uint32_t tv_sec_low, uint32_t tv_nsec) {

    auto& buffer = buffers[active_buffer];
    buffer_copy_done = true;
    buffer.presented.tv_sec = ((1ll * tv_sec_hi) << 32ll) | tv_sec_low;
    buffer.presented.tv_nsec = tv_nsec;
}

static void frame_handle_failed(void *, struct zwlr_screencopy_frame_v1 *) {
    fprintf(stderr, "failed to copy frame\n");
    exit_main_loop = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

static void handle_global(void*, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t) {

    if (strcmp(interface, wl_output_interface.name) == 0)
    {
        auto output = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 1);
        wf_recorder_output wro;
        wro.output = output;
        available_outputs.push_back(wro);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        shm = (wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
    {
        screencopy_manager = (zwlr_screencopy_manager_v1*) wl_registry_bind(registry, name,
            &zwlr_screencopy_manager_v1_interface, 1);
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        xdg_output_manager = (zxdg_output_manager_v1*) wl_registry_bind(registry, name,
            &zxdg_output_manager_v1_interface, 2); // version 2 for name & description, if available
    }
}

static void handle_global_remove(void*, struct wl_registry *, uint32_t) {
    // Who cares?
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static uint64_t timespec_to_usec (const timespec& ts)
{
    return ts.tv_sec * 1000000ll + 1ll * ts.tv_nsec / 1000ll;
}

static int next_frame(int frame)
{
    return (frame + 1) % MAX_BUFFERS;
}

struct PixelFormatInfo {
  wl_shm_format wl_fmt ;
  InputFormat   fmt ;
  bool          alpha_is_zero ; 
  const char *  name ;
} ;

PixelFormatInfo supported_pixel_formats [] =
  {
   { WL_SHM_FORMAT_ARGB8888, INPUT_FORMAT_BGR0, true,  "argb8888" },
   { WL_SHM_FORMAT_XRGB8888, INPUT_FORMAT_BGR0, false, "xrgb8888" },
   { WL_SHM_FORMAT_ABGR8888, INPUT_FORMAT_RGB0, true,  "abgr8888" },
   { WL_SHM_FORMAT_XBGR8888, INPUT_FORMAT_RGB0, false, "xbgr8888" },
  };

#if 0
static const char *
get_wl_shf_format_name(wl_shm_format format)
{
  for ( auto & info : supported_pixel_formats ) { 
    if ( info.wl_fmt == format) {
      return info.name;
    }
  }
  return "?"; 
}
#endif

static InputFormat get_input_format(wl_shm_format wl_fmt) 
{
  for ( auto & info : supported_pixel_formats ) { 
    if ( info.wl_fmt == wl_fmt) {
      return info.fmt;
    }
  }

  fprintf(stderr, "Unsupported buffer format %d, exiting.", wl_fmt);
  std::exit(0);

#if 0
    if (buffer.format == WL_SHM_FORMAT_ARGB8888)
        return INPUT_FORMAT_BGR0;
    if (buffer.format == WL_SHM_FORMAT_XRGB8888)
        return INPUT_FORMAT_BGR0;

    if (buffer.format == WL_SHM_FORMAT_XBGR8888)
        return INPUT_FORMAT_RGB0;
    if (buffer.format == WL_SHM_FORMAT_ABGR8888)
        return INPUT_FORMAT_RGB0;

    fprintf(stderr, "Unsupported buffer format %d, exiting.", buffer.format);
    std::exit(0);
#endif
}

static void write_loop(FrameWriterParams params, PulseReaderParams pulseParams)
{
    /* Ignore SIGINT, main loop is responsible for the exit_main_loop signal */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    int last_encoded_frame = 0;
    std::unique_ptr<PulseReader> pr;

    while(!exit_main_loop)
    {
        // wait for frame to become available
        while(buffers[last_encoded_frame].available != true) {
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        auto& buffer = buffers[last_encoded_frame];

        frame_writer_pending_mutex.lock();
        frame_writer_mutex.lock();
        frame_writer_pending_mutex.unlock();

        if (!frame_writer)
        {
            /* This is the first time buffer attributes are available */
            params.format = get_input_format(buffer.format);
            params.width = buffer.width;
            params.height = buffer.height;
            frame_writer = std::unique_ptr<FrameWriter> (new FrameWriter(params));

            if (params.enable_audio)
            {
                pulseParams.audio_frame_size = frame_writer->get_audio_buffer_size();
                pr = std::unique_ptr<PulseReader> (new PulseReader(pulseParams));
                pr->start();
            }
        }

        frame_writer->add_frame((unsigned char*)buffer.data, buffer.base_usec,
            buffer.y_invert);

        frame_writer_mutex.unlock();

        buffer.available = false;
        buffer.released = true;

        last_encoded_frame = next_frame(last_encoded_frame);
    }

    std::lock_guard<std::mutex> lock(frame_writer_mutex);
    /* Free the PulseReader connection first. This way it'd flush any remaining
     * frames to the FrameWriter */
    pr = nullptr;
    frame_writer = nullptr;
}

void handle_sigint(int)
{
    exit_main_loop = true;
}

static void check_has_protos()
{
    if (shm == NULL) {
        fprintf(stderr, "compositor is missing wl_shm\n");
        exit(EXIT_FAILURE);
    }
    if (screencopy_manager == NULL) {
        fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
        exit(EXIT_FAILURE);
    }

    if (xdg_output_manager == NULL)
    {
        fprintf(stderr, "compositor doesn't support xdg-output-unstable-v1\n");
        exit(EXIT_FAILURE);
    }

    if (available_outputs.empty())
    {
        fprintf(stderr, "no outputs available\n");
        exit(EXIT_FAILURE);
    }
}

wl_display *display = NULL;

static void sync_wayland()
{
    wl_display_dispatch(display);
    wl_display_roundtrip(display);
}


static void load_output_info()
{
    for (auto& wo : available_outputs)
    {
        wo.zxdg_output = zxdg_output_manager_v1_get_xdg_output(
            xdg_output_manager, wo.output);
        zxdg_output_v1_add_listener(wo.zxdg_output,
            &xdg_output_implementation, NULL);
    }

    sync_wayland();
}

static wf_recorder_output* choose_interactive()
{
    fprintf(stdout, "Please select an output from the list to capture (enter output no.):\n");

    int i = 1;
    for (auto& wo : available_outputs)
    {
        printf("%d. Name: %s Description: %s\n", i++, wo.name.c_str(),
            wo.description.c_str());
    }

    printf("Enter output no.:");
    fflush(stdout);

    int choice;
    if (scanf("%d", &choice) != 1 || choice > (int)available_outputs.size() || choice <= 0)
        return nullptr;

    return &available_outputs[choice - 1];
}

struct capture_region
{
    int32_t x, y;
    int32_t width, height;

    capture_region()
        : capture_region(0, 0, 0, 0) {}

    capture_region(int32_t _x, int32_t _y, int32_t _width, int32_t _height)
        : x(_x), y(_y), width(_width), height(_height) { }

    /* Make sure that dimension is even, while trying to keep the segment
     * [coordinate, coordinate+dimension) as good as possible (i.e not going
     * out of the monitor) */
    void make_even(int32_t& coordinate, int32_t& dimension)
    {
        if (dimension % 2 == 0)
            return;

        /* We need to increase dimension to make it an even number */
        ++dimension;

        /* Try to decrease coordinate. If coordinate > 0, we can always lower it
         * by 1 pixel and stay inside the screen. */
        coordinate = std::max(coordinate - 1, 0);
    }

    void set_from_string(std::string geometry_string)
    {
        if (sscanf(geometry_string.c_str(), "%d,%d %dx%d", &x, &y, &width, &height) != 4)
        {
            fprintf(stderr, "Bad geometry: %s, capturing whole output instead.\n",
                geometry_string.c_str());
            x = y = width = height = 0;
            return;
        }

        /* ffmpeg requires even width and height */
        make_even(x, width);
        make_even(y, height);
        printf("Adjusted geometry: %d,%d %dx%d\n", x, y, width, height);
    }

    bool is_selected()
    {
        return width > 0 && height > 0;
    }

    bool contained_in(const capture_region& output) const
    {
        return
            output.x <= x &&
            output.x + output.width >= x + width &&
            output.y <= y &&
            output.y + output.height >= y + height;
    }
};

static wf_recorder_output* detect_output_from_region(const capture_region& region)
{
    for (auto& wo : available_outputs)
    {
        const capture_region output_region{wo.x, wo.y, wo.width, wo.height};
        if (region.contained_in(output_region))
        {
            std::cout << "Detected output based on geometry: " << wo.name << std::endl;
            return &wo;
        }
    }

    std::cerr << "Failed to detect output based on geometry (is your geometry overlapping outputs?)" << std::endl;
    return nullptr;
}

// One enums for each command line argument.
// Alphanumerical values indicate a short option.
// Other values are long options only

// LONGARG is a unique identifier for arguments without a short name. 
#define LONGARG  (-__LINE__)

static const int ARG_HELP           = 'h';
static const int ARG_SCREEN         = 's';
static const int ARG_FILE           = 'f';  
static const int ARG_FILE_FORMAT    = 'F';  
static const int ARG_GEOMETRY       = 'g';
static const int ARG_OUTPUT         = 'o';
static const int ARG_ENCODER        = 'e';
static const int ARG_ENCODER_PARAM  = 'p';  // encoder parameter
static const int ARG_HW_DEVICE      = 'd';
static const int ARG_HW_ACCEL       = LONGARG ;
static const int ARG_AUDIO          = 'a';
static const int ARG_HIDE_MOUSE     = 'M';
static const int ARG_SHOW_MOUSE     = 'm';
static const int ARG_YUV420P        = 'y';
static const int ARG_VIDEO_FILTER   = 'v'; 
static const int ARG_VIDEO_TRACE    = 'T'; 
static const int ARG_SET_TEST_FORMAT = LONGARG; 
static const int ARG_TEST_COLORS    = LONGARG; 
static const int ARG_FFMPEG_DEBUG   = LONGARG ;
static const int ARG_VAAPI          = LONGARG ;
      

static struct option options[] =
  {
   { "help",            no_argument      , NULL, ARG_HELP },
   { "screen",          required_argument, NULL, ARG_SCREEN },
   { "output",          required_argument, NULL, ARG_OUTPUT },
   { "file",            required_argument, NULL, ARG_FILE },
   { "file-format",     required_argument, NULL, ARG_FILE_FORMAT },
   { "geometry",        required_argument, NULL, ARG_GEOMETRY },
   { "encoder",         required_argument, NULL, ARG_ENCODER },
   { "hide-mouse",      no_argument,       NULL, ARG_HIDE_MOUSE },
   { "show-mouse",      no_argument,       NULL, ARG_SHOW_MOUSE },
   { "param",           required_argument, NULL, ARG_ENCODER_PARAM },
   { "hw-device",       required_argument, NULL, ARG_HW_DEVICE },
   { "hw-accel",        required_argument, NULL, ARG_HW_ACCEL },
   { "ffmpeg-debug",    no_argument,       NULL, ARG_FFMPEG_DEBUG },
   { "audio",           optional_argument, NULL, ARG_AUDIO },
   { "yuv420p",         no_argument,       NULL, ARG_YUV420P},   
   { "video-filter",    required_argument, NULL, ARG_VIDEO_FILTER},
   { "video-trace",     no_argument,       NULL, ARG_VIDEO_TRACE },   
   { "vaapi",           no_argument,       NULL, ARG_VAAPI },   
   { "set-test-format", required_argument, NULL, ARG_SET_TEST_FORMAT },   
   { "test-colors",     no_argument,       NULL, ARG_TEST_COLORS },   
   { 0,                 0,                 NULL,  0  }
  };

static const char * default_filename = "recording.mp4" ;

static bool is_alphanum(int v) {
  return
       ( 'a' <= v && v <= 'z' )
    || ( 'A' <= v && v <= 'Z' )
    || ( '0' <= v && v <= '9' )
    ;
}

const char *long_name(int arg) {
  int k=0;
  while (true) {
    struct option & entry = options[k++] ;
    if ( entry.name==0 && entry.has_arg==0 && entry.flag==0 && entry.val==0) {
      break;
    }
    if ( entry.val==arg ) {
      return entry.name;
    }
 }
 return "????";
}

// Transforms the options array into a suitable 'optstring' for getopt_long.
static char * gen_optstring() {
  std::stringstream s ;
  int k=0 ;
  while (true) {
    struct option & entry = options[k++] ;
    if ( entry.name==0 && entry.has_arg==0 && entry.flag==0 && entry.val==0) {
      return strdup(s.str().c_str());
    }
    if ( is_alphanum(entry.val) ) {
      s << char(entry.val);
      if (entry.has_arg==no_argument) {      
      } else if (entry.has_arg==required_argument) {
        s << ':' ;
      } else if (entry.has_arg==optional_argument) {
        s << ':' << ':' ;
      } else {
        abort(); 
      }
    }
  }
  
}

static void show_usage(std::ostream &out, const char *app)
{
  out << "Usage: " << app << "[options] [-f output.mp4]" << std::endl;
  int k=0 ; 
  std::string indent(35,' ');
  while (true) {
    struct option & entry = options[k++] ;
    if ( entry.name==0 && entry.has_arg==0 && entry.flag==0 && entry.val==0)
      break ;
    bool hide=false;
    std::string argname("ARG");
    std::stringstream text;
    switch (entry.val) {
    case ARG_HELP:
      text  << "Show this help" ;
      break;
    case ARG_SCREEN:
      argname = "SCREEN";
      text << "Specify the output to use. SCREEN is the" << std::endl << indent ;
      text << "Wayland output number or identifier.";
      break;
    case ARG_OUTPUT:
      argname = "FILENAME";
      text << "Similar to --" << long_name(ARG_FILE) << " but the FILENAME is formatted" << std::endl << indent;
      text << "as described for the 'date' command. For example:" << std::endl << indent;
      text << "    --" << long_name(ARG_OUTPUT) << " screencast-%F-%Hh%Mm%Ss.mp4";
      break;
    case ARG_FILE:
      argname = "FILENAME";
      text << "Set the name of the output file. The default" << std::endl << indent;
      text << "is " << default_filename  ;
      break;
    case ARG_FILE_FORMAT:
      argname = "FILENAME";
      text << "Set the name format of the output file. The default is to" << std::endl << indent;
      text << "autodetect the format from the file name";
      break;
    case ARG_GEOMETRY:
      argname = "REGION";
      text << "Specify a REGION on screen using the 'X,Y WxH' format" << std::endl << indent;
      text << "compatible with 'slurp'"; 
      break;
    case ARG_ENCODER:
      argname = "ENCODER";
      text << "Specify the FFMpeg encoder codec to use. " << std::endl << indent; 
      text << "The default is '" << DEFAULT_CODEC << "'." << std::endl << indent;
      text << "See also: ffmpeg -hide_banner -encoders" ;
      break;
    case ARG_HW_DEVICE:
      argname = "DEVICE";
      text << "Specify the hardware decoding device." << std::endl << indent; 
      text << "This is only relevant if an hardware encoder" << std::endl << indent; 
      text << "is selected. For VAAPI encoders, that would " << std::endl << indent; 
      text << "be something like /dev/dri/renderD128";
      break;
    case ARG_HW_ACCEL:
      argname = "NAME";
      text << "Select an hardware accelerator." << std::endl << indent; 
      text << "See also: ffmpeg -hide_banner -hwaccels" ;
      break;
    case ARG_FFMPEG_DEBUG:
      text << "Enable FFMpeg debug output";
      break;
    case ARG_ENCODER_PARAM:
      argname = "NAME=VALUE";      
      text << "Set a parameter of the encoder." << std::endl << indent; 
      text << "See also: ffmpeg -hide_banner -h encoder=...";
      break;
    case ARG_HIDE_MOUSE:
      text << "Do not show the mouse cursor in the recording";
      break;
    case ARG_SHOW_MOUSE:
      text << "Show the mouse cursor in the recording. This is the default.";
      break;
    case ARG_AUDIO:
      argname = "DEVICE";
      text << "Enable audio recordig using the specified Pulseaudio" << std::endl << indent ;      
      text << "device number or identifier";
      break;
    case ARG_YUV420P:
      text << "Use the encoding pixel format YUV210P if possible" << std::endl << indent; 
      text << "That option is mostly intended for software encoders."<< std::endl << indent ;
      text << "For hardware encorders, the pixel format is usually set"<< std::endl << indent;
      text << "using a filter";      
      break;
    case ARG_VIDEO_FILTER:
      argname = "FILTERS";
      text << "Specify the FFMpeg video filters.";
      break;
    case ARG_VIDEO_TRACE: 
      // hide = true ;
      text << "Trace progress of video encoding";
      break;
    case ARG_VAAPI:
      text << "Alias for --" << long_name(ARG_HW_ACCEL) << "=vaapi" ;
      break;
    case ARG_SET_TEST_FORMAT:
      argname = "FORMAT";
      text << "Set the input pixel format for the builtin tests.";
      break;
    case ARG_TEST_COLORS:
      text << "Generate a color test pattern.";
      break;
    default:
      text << "TO BE DOCUMENTED" ;
      break;
    }
    if (hide)
      continue ;
    std::stringstream header; 
    const char *sep="" ;
    if ( is_alphanum(entry.val) ) {
      // Assume that values in the character range are
      // also used for a short argument
      header << "  -" << char(entry.val) ;
      sep=", ";
    } else {
      header << "    ";
      sep="  ";
    }
    if (entry.name) {
      header << sep << "--" << entry.name ;
    }
    if (entry.has_arg==no_argument) {
      // done
    } else if (entry.has_arg==required_argument) {
      header << (entry.name?'=':' ')  << argname ;
    } else if (entry.has_arg==optional_argument) {
      if (entry.name)
        header << "[=" << argname << "]";
      else
        header << "[" << argname << "]";
    } else {
      abort(); 
    }
    
    out << std::setw(indent.size()) << std::left << header.str() <<  text.str() << std::endl ; 
  }
}

std::string time_format(const char *fmt)
{
  char buffer[PATH_MAX];
  struct timeval tv;
  gettimeofday (&tv, NULL);
  struct tm *ptm = localtime(&tv.tv_sec);
  int n = strftime(buffer, sizeof(buffer), fmt, ptm);
  if (n==0) {
    fprintf(stderr,"Failed to perform time format\n");
    exit(1);
  }
  return buffer;
}

constexpr const char* default_cmdline_output = "interactive";

//
// A few variables controled by command line options
// and used in do_wayland_capture(). 
// FIXME: Should not be global
//
std::string cmdline_output = default_cmdline_output;
capture_region selected_region{};
bool show_cursor = true ;
PulseReaderParams pulseParams;

class Image {
public:
  typedef uint32_t color_t ;
  
  int width ;
  int height ;
  int linesize ;
  wl_shm_format fmt ;

  color_t * data=0 ;

  color_t outside_color = 0 ;

  uint32_t default_alpha = 0xff ;
  
  Image(int w,int h,wl_shm_format fmt) : width(w), height(h), linesize(w), fmt(fmt)
  {
    data = new color_t[linesize*height] ;    
  }

  // pack(0x11,0x22,0x33,044) is 0x11223344 
  static color_t pack(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return  (a<<24) | (b<<16) | (c<<8) | (d<<0) ;
  }

  // unpack(0x11223344,a,b,c,d) gives a=0x11, b=0x22, c=0x33, d=0x44 
  static void unpack(color_t col, uint8_t &a, uint8_t &b, uint8_t &c, uint8_t &d) {
    a = (col>>24) ;
    b = (col>>16) ;
    c = (col>>8) ;
    d = (col>>0) ;
  }
  
  color_t gray(uint8_t v) {
    return rgb(v,v,v,default_alpha) ;
  }
    
  color_t gray(uint8_t v, uint8_t alpha) {
    return rgb(v,v,v,alpha) ;
  }
    
  color_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return rgb(r,g,b,default_alpha) ;
  }
    
  color_t rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    switch(fmt) {
    case WL_SHM_FORMAT_ARGB8888: return pack(a,r,g,b) ; 
    case WL_SHM_FORMAT_XRGB8888: return pack(a,r,g,b) ; 
    case WL_SHM_FORMAT_ABGR8888: return pack(a,b,g,r) ; 
    case WL_SHM_FORMAT_XBGR8888: return pack(a,b,g,r) ; 
    default:
        return 0x66666666u ; // Hoops  
    }
  }

  void color2rgba( color_t col, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
  {
    switch(fmt) {
    case WL_SHM_FORMAT_ARGB8888: unpack(col, a,r,g,b) ; break;
    case WL_SHM_FORMAT_XRGB8888: unpack(col, a,r,g,b) ; break;
    case WL_SHM_FORMAT_ABGR8888: unpack(col, a,b,g,r) ; break;
    case WL_SHM_FORMAT_XBGR8888: unpack(col, a,b,g,r) ; break; 
    default:
      r = g = b = a = 0x66 ;
    }
  }
  
  inline color_t & at(int x, int y) { return data[x+y*linesize] ; }

  
  inline bool valid(int x, int y) {
    return x>=0 && x<width && y>0 && y < height ;
  }
  
  void set(int x,int y, color_t color) {
    if (valid(x,y))
      at(x,y) = color ; 
  }

  color_t get(int x,int y) {
    if (valid(x,y))
      return at(x,y) ;
    else
      return outside_color ; 
  }
  
  inline void fill(color_t color)
  {
    fillbox(0,0,width,height,color);
  }
  
  inline void fillbox(int x, int y, int w, int h, color_t color)
  {
    if (x<0) { x=0; w+=x; } 
    if (y<0) { y=0; h+=y; }
    w = std::min(w,width-x) ;
    h = std::min(h,height-y) ;
    for (int j=0; j<h; j++) {   
      for (int i=0; i<w; i++) {
        at(x+i,y+j) = color ;
      }
    }
  }

  inline void write_ppm(std::string filename) {

    FILE * f = fopen(filename.c_str(),"wb") ; 
    if (!f) {
      fprintf(stderr,"Failed to open ppm file '%s' for writing\n",filename.c_str());
      exit(1);
    }
    fprintf(f,"P6\n# Generated by wl-recorder-x\n%d %d\n%d\n", width, height, 255);
    for (int y=0; y<height ; y++) {
      for (int x=0; x<width ; x++) {
        uint8_t data[4] ;
        this->color2rgba( at(x,y), data[0],data[1],data[2],data[3]) ;
        fwrite(data,3,1,f);
      }
    }
    fclose(f);
  }
  
} ;


uint32_t color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
  return (r<<0) | (g<<8) | (b<<16) | (a<<24) ;
}

struct rgb {
  int r,g,b ;
} ;

void
draw_color_scale(std::string name,
                 FILE *out,
                 Image &img, int x0, int y0, int w, int h,
                 int step,
                 rgb c0,
                 rgb c1)

{
  // Boxes width and height
  int bw = w/step ;
  int bh = h;
  
  // 8bit to 16bit colors to avoid rounding errors
  int R0 = (c0.r << 8) | c0.r ;
  int G0 = (c0.g << 8) | c0.g ;
  int B0 = (c0.b << 8) | c0.b ;
  int R1 = (c1.r << 8) | c1.r ;
  int G1 = (c1.g << 8) | c1.g ;
  int B1 = (c1.b << 8) | c1.b ;

  if (out) fprintf(out,"#%s\n" , name.c_str() ) ;
  if (out) fprintf(out,"%d\n" , step);
  for (int i=0;i<step;i++) {
    int x = x0+i*bw ;
    int y = y0 ;
    int r = ( ( (step-1-i)*R0 + i*R1 ) / (step-1) ) >> 8 ;
    int g = ( ( (step-1-i)*G0 + i*G1 ) / (step-1) ) >> 8 ;
    int b = ( ( (step-1-i)*B0 + i*B1 ) / (step-1) ) >> 8 ;
    if (out) fprintf(out,"%d %d = %d %d %d\n",x+bw/2,y+bh/2,r,g,b) ;
    img.fillbox(x,y, bw, bh, img.rgb(r,g,b) ) ;
  }
}

//
// Fake input: generate a color test pattern
//
int do_test_colors(FrameWriterParams params, wl_shm_format wl_fmt)
{
  int length = 100 ; // number of frames to generate

  int w = 512;
  int h = 512;    
  params.format = get_input_format(wl_fmt);
  params.width  = w;
  params.height = h;
  params.enable_audio = false ;

  {
    FrameWriter frame_writer(params);

    Image img( w, h, wl_fmt) ;
    int64_t usec = 0 ;
    
    img.fill( img.gray(128) ) ;

    int dy = h/32 ;
    int y  = dy ;

    int step = 32;

    int v0,v1 ;

    const char *info = getenv("COLOR_TEST_INFO");
    const char *ref = getenv("COLOR_TEST_PPM");

    FILE *f = 0 ;

    if (info) {
      f = fopen(info,"w");
      if (f) {
        fprintf(stderr, "generating %s\n", info) ;
      } else {
        fprintf(stderr, "failed to open '%s' for writing\n", info) ;
      }
    }
      
    if (f) fprintf(f,"%s\n",params.file.c_str());
    if (f) fprintf(f,"%d %d\n",w,h);
          
    v0=0 ; v1=255; 
    draw_color_scale("full-g" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v1,v1} ); y += dy;
    draw_color_scale("full-r" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v0,v0} ); y += dy; 
    draw_color_scale("full-g" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v1,v0} ); y += dy;
    draw_color_scale("full-b" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v0,v1} ); y += dy;
    draw_color_scale("full-y" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v1,v0} ); y += dy;
    draw_color_scale("full-m" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v0,v1} ); y += dy;
    draw_color_scale("full-c" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v1,v1} ); y += dy;
    y += dy ;    

    v0 = 0 ; v1 = 31 ; 
    draw_color_scale("low-g" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v1,v1} ); y += dy;
    draw_color_scale("low-r" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v0,v0} ); y += dy;
    draw_color_scale("low-g" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v1,v0} ); y += dy;
    draw_color_scale("low-b" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v0,v1} ); y += dy;
    draw_color_scale("low-y" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v1,v0} ); y += dy;
    draw_color_scale("low-m" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v0,v1} ); y += dy;
    draw_color_scale("low-c" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v1,v1} ); y += dy;
    y += dy ;

    v0=255; v1=255-31 ;
    draw_color_scale("high-g" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v1,v1} ); y += dy;
    draw_color_scale("high-r" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v0,v0} ); y += dy;
    draw_color_scale("high-g" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v1,v0} ); y += dy;
    draw_color_scale("high-b" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v0,v1} ); y += dy;
    draw_color_scale("high-y" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v1,v0} ); y += dy;
    draw_color_scale("high-m" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v1,v0,v1} ); y += dy;
    draw_color_scale("high-c" ,f, img, 0, y, w, dy, step, rgb{v0,v0,v0} , rgb{v0,v1,v1} ); y += dy;

    y += dy ;
    while (y<h) {
      for (int x=0;x<w;x++) {
        int v0 = (x*255)/(w-1) ;
        int v1 = std::min(std::max(0,v0 + ((y-x)&8) - 4),255); 
        Image::color_t c0 = img.rgb(v1,v1,v1) ;
        img.set(x,y,c0) ;
      }      
      y++ ;
    }

    if (f) fclose(f);

    if (ref) {
      fprintf(stderr,"Writing reference image '%s'\n",ref);
      img.write_ppm(ref) ; 
    }
    
    for (int i=0;i<length;i++) {
      
      if (getenv("ANIMATE_TEST_COLOR")) {
        img.fillbox( 0, h-20, w, 15 , img.rgb(0,0,30) ) ;
        img.fillbox( w/2 + (w*0.25)*sin(+0.0+1*2*(M_PI*i)/length), h-20 , 10, 15 , img.rgb(255,0,0) ) ;
        img.fillbox( w/2 + (w*0.13)*sin(-0.3-2*2*(M_PI*i)/length), h-20 , 10, 15 , img.rgb(0,255,0) ) ;
        img.fillbox( w/2 + (w*0.13)*sin(-1.5+2*2*(M_PI*i)/length), h-20 , 10, 15 , img.rgb(0,0,255) ) ;
      }
      
      frame_writer.add_frame( (unsigned char*) img.data, usec, false);
      usec += 1000000/20 ;
    }
  }
  return EXIT_SUCCESS;
}

int do_wayland_capture(FrameWriterParams ffmpegParams) 
{
  
    display = wl_display_connect(NULL);
    if (display == NULL) {
        fprintf(stderr, "failed to create display: %m\n");
        return EXIT_FAILURE;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    sync_wayland();

    check_has_protos();
    load_output_info();

    wf_recorder_output *chosen_output = nullptr;
    if (available_outputs.size() == 1)
    {
        chosen_output = &available_outputs[0];
        if (chosen_output->name != cmdline_output &&
            cmdline_output != default_cmdline_output)
        {
            std::cerr << "Couldn't find requested output "
                << cmdline_output << std::endl;
            return EXIT_FAILURE;
        }
    } else
    {
        for (auto& wo : available_outputs)
        {
            if (wo.name == cmdline_output)
                chosen_output = &wo;
        }

        if (chosen_output == NULL)
        {
            if (cmdline_output != default_cmdline_output)
            {
                std::cerr << "Couldn't find requested output "
                    << cmdline_output.c_str() << std::endl;
                return EXIT_FAILURE;
            }

            if (selected_region.is_selected())
            {
                chosen_output = detect_output_from_region(selected_region);
            }
            else
            {
                chosen_output = choose_interactive();
            }
        }
    }


    if (chosen_output == nullptr)
    {
        fprintf(stderr, "Failed to select output, exiting\n");
        return EXIT_FAILURE;
    }

    if (selected_region.is_selected())
    {
        if (!selected_region.contained_in({chosen_output->x, chosen_output->y,
            chosen_output->width, chosen_output->height}))
        {
            fprintf(stderr, "Invalid region to capture: must be completely "
                "inside the output\n");
            selected_region = capture_region{};
        }
    }

    printf("selected region %d %d %d %d\n", selected_region.x, selected_region.y, selected_region.width, selected_region.height);

    timespec first_frame;
    first_frame.tv_sec = -1;
    first_frame.tv_nsec = 0;

    active_buffer = 0;
    for (auto& buffer : buffers)
    {
        buffer.wl_buffer = NULL;
        buffer.available = false;
        buffer.released = true;
    }

    bool spawned_thread = false;
    std::thread writer_thread;

    signal(SIGINT, handle_sigint);

    while(!exit_main_loop)
    {

        // wait for a free buffer
        while(buffers[active_buffer].released != true) {
          std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        buffer_copy_done = false;
        struct zwlr_screencopy_frame_v1 *frame = NULL;

        /* Capture the whole output if the user hasn't provided a good geometry */
        if (!selected_region.is_selected())
        {
            frame = zwlr_screencopy_manager_v1_capture_output(
                screencopy_manager,
                show_cursor ? 1 : 0,
                chosen_output->output);
        } else
        {
            frame = zwlr_screencopy_manager_v1_capture_output_region(
                screencopy_manager,
                show_cursor ? 1 : 0,
                chosen_output->output,
                selected_region.x - chosen_output->x,
                selected_region.y - chosen_output->y,
                selected_region.width, selected_region.height);
        }

        zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);

        while (!buffer_copy_done && wl_display_dispatch(display) != -1) {
            // This space is intentionally left blank
          // std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        auto& buffer = buffers[active_buffer];
        //std::cout << "first buffer at " << timespec_to_usec(get_ct()) / 1.0e6<< std::endl;

        if (!spawned_thread)
        {
            writer_thread = std::thread([=] () {
                write_loop(ffmpegParams, pulseParams);
            });

            spawned_thread = true;
        }

        if (first_frame.tv_sec == -1)
            first_frame = buffer.presented;

        buffer.base_usec = timespec_to_usec(buffer.presented)
            - timespec_to_usec(first_frame);

        buffer.released = false;
        buffer.available = true;

        active_buffer = next_frame(active_buffer);
        zwlr_screencopy_frame_v1_destroy(frame);
    }

    writer_thread.join();

    for (auto& buffer : buffers)
        wl_buffer_destroy(buffer.wl_buffer);

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    FrameWriterParams params;
    params.file = default_filename;
    params.codec = DEFAULT_CODEC;
    params.enable_ffmpeg_debug_output = false;
    params.enable_audio = false;
    params.to_yuv = false;
    params.trace_video_progress=false;   

    //    FrameWriter::dump_available_encoders(std::cout);
    
    std::string cmdline_output = default_cmdline_output;

    // Some generic default encoder options
    params.codec_options["colorspace"]="bt470bg" ; 
    params.codec_options["color_range"]="jpeg" ;

    enum Mode
      {
       MODE_WAYLAND_CAPTURE, 
       MODE_TEST_COLORS
      } ;

    Mode mode = MODE_WAYLAND_CAPTURE ;
    wl_shm_format test_format = WL_SHM_FORMAT_XRGB8888 ; 
      
    int c, i;
    std::string param;
    size_t pos;
    const char *optstring = gen_optstring();
    //  std::cerr << "optstring = " << optstring << "\n";    
    while((c = getopt_long(argc, argv, optstring, options, &i)) != -1)
    {
        switch(c)
        {
            case ARG_HELP:
              show_usage(std::cerr, argv[0]);
                return EXIT_FAILURE;
                
            case ARG_OUTPUT:
              {
                params.file = time_format(optarg);
              }
              break ;
              
            case ARG_FILE:
                params.file = optarg;
                break;

            case ARG_FILE_FORMAT:
                params.file_format = optarg;
                break;

            case ARG_SCREEN:
                cmdline_output = optarg;
                break;

            case ARG_GEOMETRY:
                selected_region.set_from_string(optarg);
                break;

            case ARG_ENCODER:
                params.codec = optarg;
                break;

            case ARG_HW_ACCEL:
                params.hw_method = optarg;
                break;

            case ARG_HW_DEVICE:
                params.hw_device = optarg;
                break;

            case ARG_FFMPEG_DEBUG:
                params.enable_ffmpeg_debug_output = true;
                break;

            case ARG_AUDIO:
                params.enable_audio = true;
                pulseParams.audio_source = optarg ? strdup(optarg) : NULL;
                break;

            case ARG_YUV420P:
                params.to_yuv = true;
                break;

            case ARG_VIDEO_FILTER:
                params.video_filter = optarg;
                break;

            case ARG_SET_TEST_FORMAT:
              {
                bool found=false;
                for ( const PixelFormatInfo & it : supported_pixel_formats ) {
                  found = !strcmp(it.name,optarg) ;
                  if (found) {
                    test_format = it.wl_fmt ;
                    break;
                  }
                }
                if (!found) {
                  std::cerr << "Unknown pixel format '" << optarg << "'\n" ;
                  std::cerr << "Expected:" ;                  
                  char sep = ' ';
                  for ( const PixelFormatInfo & it : supported_pixel_formats ) {
                    std::cerr << sep << it.name  ;
                    sep=' ';
                  }
                  std::cerr << std::endl ;
                  exit(1);
                }
              }
              break;
                
            case ARG_ENCODER_PARAM:
                param = optarg;
                pos = param.find("=");
                if (pos != std::string::npos )
                {
                  auto optname = param.substr(0, pos);
                  auto optvalue = param.substr(pos + 1, param.length() - pos - 1);                  
                  params.codec_options[optname] = optvalue;
                  // If given 'name=' then clear the option
                  if ( optvalue.empty() ) {
                    auto it = params.codec_options.find(optname) ;
                    if ( it != params.codec_options.end() )
                        params.codec_options.erase(it) ;
                  }
                } else
                {
                  fprintf(stderr,"Malformed encoder option '%s' (expect 'NAME=VALUE')\n", optarg);
                  exit(1);
                }
                break;

           case ARG_VIDEO_TRACE:
                params.trace_video_progress=true;
                break; 

           case ARG_VAAPI:
                params.hw_method = "vaapi";
                break;

           case ARG_SHOW_MOUSE:
                show_cursor = true;
                break;

           case ARG_HIDE_MOUSE:
                show_cursor = false;
                break;

           case ARG_TEST_COLORS:
                mode = MODE_TEST_COLORS;
                break;
                
            default:
                printf("Non implemented command line option (%s)\n", optarg);
               return EXIT_FAILURE ;
        }
    }

    // No unprocessed arguments
    if (optind != argc) {
      fprintf(stderr,"Unexpected argument '%s'\n",argv[optind]);
      return EXIT_FAILURE;
    }

    // Guess the hw_method for some known codecs.
    if ( params.hw_method.empty() && !params.codec.empty() ) {
      auto it = auto_hwaccel.find(params.codec) ;
      if ( it != auto_hwaccel.end() ) {
        fprintf(stderr, "Using %s for encoder %s\n", it->second.c_str(), params.codec.c_str());
        params.hw_method = it->second ;
      }
    }   

    // Guess a video filter for some hw methods
    if ( params.video_filter.empty() && !params.codec.empty() ) {
      std::cerr << "GUESSING filter\n";
      auto it = auto_filter.find(params.codec) ;
      if ( it != auto_filter.end() ) {
        fprintf(stderr, "Using default filter '%s' for hardware %s\n", it->second.c_str(), params.hw_method.c_str());
        params.video_filter = it->second ;
      }
    }
    
    // Sensible default when using vaapi. 
    if ( params.hw_method == "vaapi" ) {
      // TODO: find the first render device.
      if ( params.hw_device.empty() || params.hw_device=="auto" ) {
        params.hw_device = "/dev/dri/renderD128" ;
      }
    }

    switch(mode) {
    case MODE_WAYLAND_CAPTURE:
      return do_wayland_capture(params) ;
    case MODE_TEST_COLORS:
      return do_test_colors(params, test_format);
    default:
      return EXIT_SUCCESS;
    }
}
