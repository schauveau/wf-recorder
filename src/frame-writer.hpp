// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#ifndef FRAME_WRITER
#define FRAME_WRITER

#include <stdint.h>
#include <string>
#include <vector>
#include <map>

#define AUDIO_RATE 44100

extern "C"
{
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/mathematics.h>
    #include <libavformat/avformat.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/pixdesc.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/opt.h>
  
}

enum InputFormat
{
     INPUT_FORMAT_BGR0,
     INPUT_FORMAT_RGB0
};

struct FrameWriterParams
{
    std::string file;
    int width;
    int height;

    InputFormat format;

    std::string video_filter;
  
    std::string codec;
    std::map<std::string, std::string> codec_options;

    std::string hw_method; // If not empty then the HW method to used (e.g. "vaapi")
    std::string hw_device; // Device description needed by some HW methods.

    int64_t audio_sync_offset;

    bool enable_audio;
    bool enable_ffmpeg_debug_output;

    bool trace_video_progress; 
    bool to_yuv;
};

class FrameWriter
{
  FrameWriterParams params;
  void load_codec_options(AVDictionary **dict);

  //    SwsContext* swsCtx;
  AVOutputFormat* outputFmt=NULL;
  AVStream* videoStream=NULL;
  AVCodecContext* videoCodecCtx=NULL;
  AVFormatContext* fmtCtx=NULL;

  AVFilterContext * videoFilterSourceCtx = NULL;
  AVFilterContext * videoFilterSinkCtx = NULL;
  AVFilterGraph   * videoFilterGraph = NULL;

  // Properties of the video filter output.
  struct {
    int width;
    int height;
    AVRational sar;  // sample aspect ratio
    AVPixelFormat pix_fmt; 
    AVRational time_base;
    AVRational frame_rate; // can be 1/0 if unknown
  } vfilter ;
  
  AVBufferRef *hw_device_context = NULL;
  AVBufferRef *hw_frame_context = NULL;
  
  AVPixelFormat get_input_format();
  void init_hw_accel();
  void init_codecs();
  void init_video_filters(AVCodec *codec);
  void init_video_stream();
  
  AVFrame *encoder_frame = NULL;
  AVFrame *hw_frame = NULL;
  
  SwrContext *swrCtx=NULL;
  AVStream *audioStream=NULL;
  AVCodecContext *audioCodecCtx=NULL;
  void init_swr();
  void init_audio_stream();
  void send_audio_pkt(AVFrame *frame);
  
  void finish_frame(AVPacket& pkt, bool isVideo);
  
public :
  FrameWriter(const FrameWriterParams& params);
  void add_frame(const uint8_t* pixels, int64_t usec, bool y_invert);
  
  /* Buffer must have size get_audio_buffer_size() */
  void add_audio(const void* buffer);
  size_t get_audio_buffer_size();
  
  ~FrameWriter();
};

#include <memory>
#include <mutex>
#include <atomic>

extern std::mutex frame_writer_mutex, frame_writer_pending_mutex;
extern std::unique_ptr<FrameWriter> frame_writer;
extern std::atomic<bool> exit_main_loop;

#endif // FRAME_WRITER
