// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp
//
// Audio encoding - thanks to wlstream, a lot of the code/ideas are taken from there

#include <iostream>
#include "frame-writer.hpp"
#include <vector>
#include <queue>
#include <cstring>
#include "averr.h"

// REMARK: Removed FPS=60 because using 1/FPS as time_base
//         only makes sense if the FPS is the actual framerate
//         (and it is probably not).
//         The 'usec' argument of add_frame is in μs so it
//         makes more sense to use time_base=1/1000000 for the
//         video stream.

// US_RATIONAL = 1us as a AVRational

static const AVRational US_RATIONAL{1,1000000} ;

#define AUDIO_RATE 44100

class FFmpegInitialize
{
public :

    FFmpegInitialize()
    {
        // Loads the whole database of available codecs and formats.
        av_register_all();
    }
};


static FFmpegInitialize ffmpegInitialize;

void FrameWriter::init_hw_accel()
{
#if 1
  if ( params.hw_method == "" )
    return ; 

  AVHWDeviceType hwtype = av_hwdevice_find_type_by_name( params.hw_method.c_str() );
  if ( hwtype == AV_HWDEVICE_TYPE_NONE) {
    std::cerr << "Unknowns or unavailable HW method '" << params.hw_method << "'\n";
    std::exit(-1);
  }

  int ret = av_hwdevice_ctx_create(&this->hw_device_context,
                                   hwtype,
                                   params.hw_device.c_str(),
                                   NULL,
                                   0);
  if (ret != 0)
    {
      std::cerr << "Failed to create HW device '" << params.hw_device << "': " << averr(ret) << std::endl;
      std::exit(-1);
    }

  // TODO: store hw_device_context in the filter
  
#else
  int ret = av_hwdevice_ctx_create(&this->hw_device_context,
                                   av_hwdevice_find_type_by_name("vaapi"), params.hw_device.c_str(), NULL, 0);

    if (ret != 0)
    {
        std::cerr << "Failed to create hw encoding device " << params.hw_device << ": " << averr(ret) << std::endl;
        std::exit(-1);
    }

    this->hw_frame_context = av_hwframe_ctx_alloc(hw_device_context);
    if (!this->hw_frame_context)
    {
        std::cerr << "Failed to initialize hw frame context" << std::endl;
        av_buffer_unref(&hw_device_context);
        std::exit(-1);
    }

    AVHWFramesConstraints *cst;
    cst = av_hwdevice_get_hwframe_constraints(hw_device_context, NULL);
    if (!cst)
    {
        std::cerr << "Failed to get hwframe constraints" << std::endl;
        av_buffer_unref(&hw_device_context);
        std::exit(-1);
    }

    AVHWFramesContext *ctx = (AVHWFramesContext*)this->hw_frame_context->data;
    ctx->width = params.width;
    ctx->height = params.height;
    ctx->format = cst->valid_hw_formats[0];
    ctx->sw_format = AV_PIX_FMT_NV12;

    if ((ret = av_hwframe_ctx_init(hw_frame_context)))
    {
        std::cerr << "Failed to initialize hwframe context: " << averr(ret) << std::endl;
        av_buffer_unref(&hw_device_context);
        av_buffer_unref(&hw_frame_context);
        std::exit(-1);
    }
#endif
}

void FrameWriter::load_codec_options(AVDictionary **dict)
{
    static const std::map<std::string, std::string> default_x264_options = {
        {"tune", "zerolatency"},
        {"preset", "ultrafast"},
        {"crf", "20"},
    };

    if (params.codec.find("libx264") != std::string::npos ||
        params.codec.find("libx265") != std::string::npos)
    {
        for (const auto& param : default_x264_options)
        {
            if (!params.codec_options.count(param.first))
                params.codec_options[param.first] = param.second;
        }
    }

    for (auto& opt : params.codec_options)
    {
        std::cout << "Setting codec option: " << opt.first << "=" << opt.second << std::endl;
        av_dict_set(dict, opt.first.c_str(), opt.second.c_str(), 0);
    }
}

bool is_fmt_supported(AVPixelFormat fmt, const AVPixelFormat *supported)
{
    for (int i = 0; supported[i] != AV_PIX_FMT_NONE; i++)
    {
        if (supported[i] == fmt)
            return true;
    }

    return false;
}

AVPixelFormat FrameWriter::get_input_format()
{
    return params.format == INPUT_FORMAT_BGR0 ?
        AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0;
}

AVPixelFormat FrameWriter::choose_sw_format(AVCodec *codec)
{
    /* First case: if the codec supports getting the appropriate RGB format
     * directly, we want to use it since we don't have to convert data */
    auto in_fmt = get_input_format();
    if (is_fmt_supported(in_fmt, codec->pix_fmts))
        return in_fmt;

    /* Otherwise, try to use the already tested YUV420p */
    if (is_fmt_supported(AV_PIX_FMT_YUV420P, codec->pix_fmts))
        return AV_PIX_FMT_YUV420P;

    /* Lastly, use the first supported format */
    return codec->pix_fmts[0];
}

void FrameWriter::init_video_filters(AVCodec *codec)
{
  int err;

  AVFilterGraph *filter_graph = avfilter_graph_alloc();

 
  const AVFilter * source = avfilter_get_by_name("buffer");
  const AVFilter * sink   = avfilter_get_by_name("buffersink");
  
  if (!source || !sink) {
    std::cerr << "filtering source or sink element not found\n";
    exit(-1);
  }

  // Build the configuration of the 'buffer' filter.
  // See: ffmpeg -h filter=buffer
  // See: https://ffmpeg.org/ffmpeg-filters.html#buffer

  const int sz=300 ; // TODO: use a std::stringstream?
  char source_args[sz];
  err = snprintf(source_args, sz, 
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                 // video size
                 params.width, params.height,
                 // pix_fmt
                 int(this->get_input_format()),
                 // time_base. We use micro-seconds
                 US_RATIONAL.num, US_RATIONAL.den, 
                 // pixel_aspect
                 1,1
                 // sws_param
                 /* unset */
                 );
  if (err >= sz) {
    std::cerr << "I am paranoid" << std::endl;;
    exit(-1);
  }
 

  AVFilterContext *source_ctx = NULL;
  err = avfilter_graph_create_filter(&source_ctx, source, "Source",
                                     source_args, NULL, filter_graph);
  if (err < 0) {
    std::cerr << "Cannot create video filter in: " << averr(err) << std::endl;;
    exit(-1);
  }
  

  AVFilterContext * sink_ctx = NULL ;
  err = avfilter_graph_create_filter(&sink_ctx, sink, "Sink",
                                     NULL, NULL, filter_graph);
  if (err < 0) {
    std::cerr << "Cannot create video filter out: " << averr(err) << std::endl;;
    exit(-1);
  }

  // Indicate which pixel formats are accepted by our codec. 
  
#if 1 
  err = av_opt_set_int_list(sink_ctx, "pix_fmts", codec->pix_fmts,
                            AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

#else
  // Was written like that in ffmpeg example. 
  // Probably correct but the av_opt_set_int_list macro is more appropriate
 
  err = av_opt_set_bin(sink_ctx, "pix_fmts",
                       (uint8_t*)&enc_ctx->pix_fmt,
                       sizeof(enc_ctx->pix_fmt),
                       AV_OPT_SEARCH_CHILDREN);
#endif
  if (err < 0) {
    std::cerr << "Failed to set pix_fmts: " << averr(err) << std::endl;;
    exit(-1);
  }

  // Create the connections to the filter graph
  //
  // The in/out swap is not a mistake:
  //
  //   ----------       -----------------------------      --------
  //   | Source | ----> | in -> filter_graph -> out | ---> | Sink | 
  //   ----------       -----------------------------      --------
  //
  // The 'in' of filter_graph is the output of the Source buffer
  // The 'out' of filter_graph is the input of the Sink buffer
  //
  
  AVFilterInOut *outputs = avfilter_inout_alloc();
  outputs->name       = av_strdup("in");
  outputs->filter_ctx = source_ctx; 
  outputs->pad_idx    = 0;
  outputs->next       = NULL;
  
  AVFilterInOut *inputs  = avfilter_inout_alloc();
  inputs->name       = av_strdup("out");
  inputs->filter_ctx = sink_ctx;
  inputs->pad_idx    = 0;
  inputs->next       = NULL;
  
  if (!outputs->name || !inputs->name) {
    std::cerr << "Failed to parse allocate inout filter links" << std::endl ;
    exit(-1);
  }

  std::string filter_text = params.video_filter ;
  if ( filter_text.empty() ) {
    filter_text = "null" ;     // "null" is the dummy video filter
  }
  
  err = avfilter_graph_parse_ptr(filter_graph,
                                 filter_text.c_str(),
                                 &inputs,
                                 &outputs,
                                 NULL);
  if (err < 0) {
    std::cerr << "Failed to parse graph filter: " << averr(err) << std::endl;;    
    exit(-1) ;
  }

  err = avfilter_graph_config(filter_graph, NULL);
  if (err<0) {
    std::cerr << "Failed to configure graph filter: " << averr(err) << std::endl;;    
    exit(-1) ;
  }

  if (1) {
    printf("#######################################\n");
    printf("%s\n",avfilter_graph_dump(filter_graph,0));
    printf("#######################################\n");
    printf("nb_inputs = %d\n",sink_ctx->nb_inputs);
    printf("nb_outputs = %d\n",sink_ctx->nb_outputs);
    AVFilterLink  * out = sink_ctx->inputs[0] ;
    printf("    out.w = %d\n",out->w);
    printf("    out.h = %d\n",out->h);
    printf("    out.time_base = %d/%d\n", out->time_base.num,out->time_base.den);
    printf("    out.frame_rate = %d/%d\n", out->frame_rate.num,out->frame_rate.den);    
  }


  this->videoFilterGraph = filter_graph;
  this->videoFilterSourceCtx = source_ctx;
  this->videoFilterSinkCtx = sink_ctx;
  
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

}

void FrameWriter::init_video_stream()
{
    AVDictionary *options = NULL;
    load_codec_options(&options);

    // ===================
    AVCodec* codec = avcodec_find_encoder_by_name(params.codec.c_str());
    if (!codec)
    {
        std::cerr << "Failed to find the given codec: " << params.codec << std::endl;
        std::exit(-1);
    }

    init_video_filters(codec);

    // The (input of the) video filter sink is the output of the whole filter.  
    AVFilterLink * filter_output = this->videoFilterSinkCtx->inputs[0] ;
    // The 'filtered' values below represent what we are going to feed to
    // the video stream.

    int        filtered_w = filter_output->w ;
    int        filtered_h = filter_output->h ;
    AVRational filtered_sample_aspect_ratio = filter_output->sample_aspect_ratio ;

    // Can a filter change the time_base? Probably not but if 
    // that happens then we probably need to do something about that. 
    // AVRational filtered_time_base = filter_output->time_base;   

    // Can we make use of the frame rate if it is set by
    // some a filter such as 'fps'?    
    // AVRational filtered_frame_rate = filter_output->frame_rate; // can be 1/0 if unknown
    
    videoStream = avformat_new_stream(fmtCtx, codec);
    if (!videoStream)
    {
        std::cerr << "Failed to open stream" << std::endl;
        std::exit(-1);
    }

    videoCodecCtx = videoStream->codec;
    
#if 1
    videoCodecCtx->width      = filtered_w ;
    videoCodecCtx->height     = filtered_h ;
    videoCodecCtx->time_base  = US_RATIONAL ;
    videoCodecCtx->sample_aspect_ratio = filtered_sample_aspect_ratio;
#else
    videoCodecCtx->width      = params.width;
    videoCodecCtx->height     = params.height;
    videoCodecCtx->time_base  = US_RATIONAL ;
#endif
    
    if (params.codec.find("vaapi") != std::string::npos)
    {
        videoCodecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
        init_hw_accel();
        videoCodecCtx->hw_frames_ctx = av_buffer_ref(hw_frame_context);
        if (params.to_yuv)
            init_sws(AV_PIX_FMT_NV12);
    } else
    {
        videoCodecCtx->pix_fmt = choose_sw_format(codec);
        std::cout << "Choosing pixel format " <<
            av_get_pix_fmt_name(videoCodecCtx->pix_fmt) << std::endl;
        init_sws(videoCodecCtx->pix_fmt);
    }

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int err;
    if ((err = avcodec_open2(videoCodecCtx, codec, &options)) < 0)
    {
        std::cerr << "avcodec_open2 failed: " << averr(err) << std::endl;
        std::exit(-1);
    }
    av_dict_free(&options);

    // Use the same time_base for the stream than for the codec.
    // This is not strictly necessary but that makes sense.
    // Reminder:
    //   The value written now is just a hint for avformat_write_header().
    //   It can be modified so we cannot avoid calling av_packet_rescale_ts()
    //   on all packets.
    videoStream->time_base = US_RATIONAL;

}

static uint64_t get_codec_channel_layout(AVCodec *codec)
{
      int i = 0;
      if (!codec->channel_layouts)
          return AV_CH_LAYOUT_STEREO;
      while (1) {
          if (!codec->channel_layouts[i])
              break;
          if (codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
              return codec->channel_layouts[i];
          i++;
      }
      return codec->channel_layouts[0];
}

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return AV_SAMPLE_FMT_S16;
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

void FrameWriter::init_audio_stream()
{
    AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec)
    {
        std::cerr << "Failed to find the aac codec" << std::endl;
        std::exit(-1);
    }

    audioStream = avformat_new_stream(fmtCtx, codec);
    if (!audioStream)
    {
        std::cerr << "Failed to open audio stream" << std::endl;
        std::exit(-1);
    }

    audioCodecCtx = audioStream->codec;
    audioCodecCtx->bit_rate = lrintf(128000.0f);
    audioCodecCtx->sample_fmt = get_codec_sample_fmt(codec);
    audioCodecCtx->channel_layout = get_codec_channel_layout(codec);
    audioCodecCtx->sample_rate = AUDIO_RATE;
    audioCodecCtx->time_base = (AVRational) { 1, 1000 };
    audioCodecCtx->channels = av_get_channel_layout_nb_channels(audioCodecCtx->channel_layout);

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int err;
    if ((err = avcodec_open2(audioCodecCtx, codec, NULL)) < 0)
    {
        std::cerr << "(audio) avcodec_open2 failed " << err << std::endl;
        std::exit(-1);
    }

    swrCtx = swr_alloc();
    if (!swrCtx)
    {
        std::cerr << "Failed to allocate swr context" << std::endl;
        std::exit(-1);
    }

    av_opt_set_int(swrCtx, "in_sample_rate", AUDIO_RATE, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", audioCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", audioCodecCtx->sample_fmt, 0);
    av_opt_set_channel_layout(swrCtx, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_channel_layout(swrCtx, "out_channel_layout", audioCodecCtx->channel_layout, 0);

    if (swr_init(swrCtx))
    {
        std::cerr << "Failed to initialize swr" << std::endl;
        std::exit(-1);
    }
}

void FrameWriter::init_codecs()
{
    init_video_stream();
    if (params.enable_audio)
        init_audio_stream();
    av_dump_format(fmtCtx, 0, params.file.c_str(), 1);
    if (avio_open(&fmtCtx->pb, params.file.c_str(), AVIO_FLAG_WRITE))
    {
        std::cerr << "avio_open failed" << std::endl;
        std::exit(-1);
    }
    AVDictionary *dummy = NULL;
    if (avformat_write_header(fmtCtx, &dummy) != 0)
    {
        std::cerr << "Failed to write file header" << std::endl;
        std::exit(-1);
    }
    av_dict_free(&dummy);
}

void FrameWriter::init_sws(AVPixelFormat format)
{
    swsCtx = sws_getContext(params.width, params.height, get_input_format(),
        params.width, params.height, format,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);

    if (!swsCtx)
    {
        std::cerr << "Failed to create sws context" << std::endl;
        std::exit(-1);
    }
}

FrameWriter::FrameWriter(const FrameWriterParams& _params) :
    params(_params)
{
    if (params.enable_ffmpeg_debug_output)
        av_log_set_level(AV_LOG_DEBUG);

    
    // Preparing the data concerning the format and codec,
    // in order to write properly the header, frame data and end of file.
    this->outputFmt = av_guess_format(NULL, params.file.c_str(), NULL);
    if (!outputFmt)
    {
        std::cerr << "Failed to guess output format for file " << params.file << std::endl;
        std::exit(-1);
    }

    if (avformat_alloc_output_context2(&this->fmtCtx, NULL, NULL, params.file.c_str()) < 0)
    {
        std::cerr << "Failed to allocate output context" << std::endl;
        std::exit(-1);
    }

    init_codecs();

#if 0
    encoder_frame = av_frame_alloc();
    if (hw_device_context && params.to_yuv) {
        encoder_frame->format = AV_PIX_FMT_NV12;
    } else if (hw_device_context) {
        encoder_frame->format = get_input_format();
    } else {
        encoder_frame->format = videoCodecCtx->pix_fmt;
    }
    encoder_frame->width = params.width;
    encoder_frame->height = params.height;
    if (av_frame_get_buffer(encoder_frame, 1))
    {
        std::cerr << "Failed to allocate frame buffer" << std::endl;
        std::exit(-1);
    }

    if (hw_device_context)
    {
        hw_frame = av_frame_alloc();
        AVHWFramesContext *frctx = (AVHWFramesContext*)hw_frame_context->data;
        hw_frame->format = frctx->format;
        hw_frame->hw_frames_ctx = av_buffer_ref(hw_frame_context);
        hw_frame->width = params.width;
        hw_frame->height = params.height;

        if (av_hwframe_get_buffer(hw_frame_context, hw_frame, 0))
        {
            std::cerr << "failed to hw frame buffer" << std::endl;
            std::exit(-1);
        }
    }
#endif
}

void FrameWriter::add_frame(const uint8_t* pixels, int64_t usec, bool y_invert)
{
#if 1
  // Ignore y_invert! Can easily be done with a filter

  int err;

  // Create a frame for the pixels
  AVFrame * frame = av_frame_alloc();

  frame->width       = params.width;
  frame->height      = params.height;
  frame->format      = get_input_format(); // a 32 bit RGBx pixel format
  frame->data[0]     = (uint8_t*) pixels;
  frame->linesize[0] = 4*params.width;
  frame->pts         = usec;  // because our time_base is US_RATIONAL

  // Is that needed? That makes sense for a RGB 'screencast'
  // but the documentation says that this is about the 'YUV range'
  // so this is probably ignored.
  if (false) av_frame_set_color_range(frame,AVCOL_RANGE_JPEG);
  

  // Push the RGB frame into the filtergraph */
  err = av_buffersrc_add_frame_flags(videoFilterSourceCtx, frame, 0);
  if (err < 0) {
    std::cerr << "Error while feeding the filtergraph\n";
    exit (-1);  
  }

  // Pull filtered frames from the filtergraph 
  while (true) {

    AVFrame *filtered_frame = av_frame_alloc();

    if (!filtered_frame) {
      std::cerr << "Error av_frame_alloc\n";
      exit (-1);  
    }
    
    err = av_buffersink_get_frame(videoFilterSinkCtx, filtered_frame);
    if (err==AVERROR(EAGAIN)) {
      // Not an error. No frame available.
      // Try again later.
      break;
    } else if (err==AVERROR_EOF) {
      // There will be no more output frames on this sink.
      // That could happen if a filter like 'trim' is used to
      // stop after a given time. 
      // If that happen, we need to inform the application
      // and it should (1) stop sending frames and (2) flush
      // the encoder.
      // TO BE TESTED
      std::cerr << "Got EOF in av_buffersink_get_frame\n";
      break;
    } else if (err<0) {
      av_frame_free(&filtered_frame);
      std::cerr << "Error in av_buffersink_get_frame\n";
      exit(-1);
    } 
      

    // TODO: Is that necessary? This is done in the
    //       ffmpeg transcoding example but their input
    //       frames are produced by a decoder and so may
    //       have additional flags set. 
    filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;

    // So we have a frame. Encode it!
    
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output;
    avcodec_encode_video2(videoCodecCtx, &pkt, filtered_frame, &got_output);

    if (got_output)
      finish_frame(pkt, true);
    
  }
  
  av_frame_free(&frame);    
  
    
#else
    /* Calculate data after y-inversion */
    int stride[] = {int(4 * params.width)};
    const uint8_t *formatted_pixels = pixels;
    if (y_invert)
    {
        formatted_pixels += stride[0] * (params.height - 1);
        stride[0] *= -1;
    }

    AVFrame *output_frame;
    AVBufferRef *saved_buf0 = NULL;
    if (hw_device_context)
    {
        encoder_frame->data[0] = (uint8_t*)formatted_pixels;
        encoder_frame->linesize[0] = stride[0];

        if (params.to_yuv)
        {
            sws_scale(swsCtx, &formatted_pixels, stride, 0, params.height,
                encoder_frame->data, encoder_frame->linesize);
        }

        if (av_hwframe_transfer_data(hw_frame, encoder_frame, 0))
        {
            std::cerr << "Failed to upload data to the gpu!" << std::endl;
            return;
        }

        output_frame = hw_frame;
    } else if(get_input_format() == videoCodecCtx->pix_fmt)
    {
        output_frame = encoder_frame;
        encoder_frame->data[0] = (uint8_t*)formatted_pixels;
        encoder_frame->linesize[0] = stride[0];
        /* Force ffmpeg to create a copy of the frame, if the codec needs it */
        saved_buf0 = encoder_frame->buf[0];
        encoder_frame->buf[0] = NULL;
    } else
    {
        sws_scale(swsCtx, &formatted_pixels, stride, 0, params.height,
            encoder_frame->data, encoder_frame->linesize);
        /* Force ffmpeg to create a copy of the frame, if the codec needs it */
        saved_buf0 = encoder_frame->buf[0];
        encoder_frame->buf[0] = NULL;
        output_frame = encoder_frame;
    }

    output_frame->pts = usec; // We use time_base = 1/US_RATE

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output;
    avcodec_encode_video2(videoCodecCtx, &pkt, output_frame, &got_output);
    /* Restore frame buffer, so that it can be properly freed in the end */
    if (saved_buf0)
        encoder_frame->buf[0] = saved_buf0;

    if (got_output)
      finish_frame(pkt, true);
#endif
}

#define SRC_RATE 1e6
#define DST_RATE 1e3

static int64_t conv_audio_pts(SwrContext *ctx, int64_t in)
{
    int64_t d = (int64_t) AUDIO_RATE * AUDIO_RATE;

    /* Convert from audio_src_tb to 1/(src_samplerate * dst_samplerate) */
    in = av_rescale_rnd(in, d, SRC_RATE, AV_ROUND_NEAR_INF);

    /* In units of 1/(src_samplerate * dst_samplerate) */
    in = swr_next_pts(ctx, in);

    /* Convert from 1/(src_samplerate * dst_samplerate) to audio_dst_tb */
    return av_rescale_rnd(in, DST_RATE, d, AV_ROUND_NEAR_INF);
}

void FrameWriter::send_audio_pkt(AVFrame *frame)
{
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output;
    avcodec_encode_audio2(audioCodecCtx, &pkt, frame, &got_output);
    if (got_output)
      finish_frame(pkt, false);
}

size_t FrameWriter::get_audio_buffer_size()
{
    return audioCodecCtx->frame_size << 3;
}

void FrameWriter::add_audio(const void* buffer)
{
    AVFrame *inputf = av_frame_alloc();
    inputf->sample_rate    = AUDIO_RATE;
    inputf->format         = AV_SAMPLE_FMT_FLT;
    inputf->channel_layout = AV_CH_LAYOUT_STEREO;
    inputf->nb_samples     = audioCodecCtx->frame_size;

    av_frame_get_buffer(inputf, 0);
    memcpy(inputf->data[0], buffer, get_audio_buffer_size());

    AVFrame *outputf = av_frame_alloc();
    outputf->format         = audioCodecCtx->sample_fmt;
    outputf->sample_rate    = audioCodecCtx->sample_rate;
    outputf->channel_layout = audioCodecCtx->channel_layout;
    outputf->nb_samples     = audioCodecCtx->frame_size;
    av_frame_get_buffer(outputf, 0);

    outputf->pts = conv_audio_pts(swrCtx, INT64_MIN);
    swr_convert_frame(swrCtx, outputf, inputf);

    send_audio_pkt(outputf);

    av_frame_free(&inputf);
    av_frame_free(&outputf);
}

void FrameWriter::finish_frame(AVPacket& pkt, bool is_video)
{
    static std::mutex fmt_mutex, pending_mutex;

    if (is_video)
    {
        // TODO: We use videoCodecCtx->time_base instead of US_RATIONAL                        
        //        av_packet_rescale_ts(&pkt, US_RATIONAL, videoStream->time_base);
      AVRational &packet_time_base = this->videoFilterSinkCtx->inputs[0]->time_base;
        av_packet_rescale_ts(&pkt, packet_time_base, videoStream->time_base);
        pkt.stream_index = videoStream->index;
    } else
    {
        av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000 }, audioStream->time_base);
        pkt.stream_index = audioStream->index;
    }

    /* We use two locks to ensure that if WLOG the audio thread is waiting for
     * the video one, when the video becomes ready the audio thread will be the
     * next one to obtain the lock */
    if (params.enable_audio)
    {
        pending_mutex.lock();
        fmt_mutex.lock();
        pending_mutex.unlock();
    }

    av_interleaved_write_frame(fmtCtx, &pkt);
    av_packet_unref(&pkt);

    if (params.enable_audio)
        fmt_mutex.unlock();
}

FrameWriter::~FrameWriter()
{
    // Writing the delayed frames:
    AVPacket pkt;
    av_init_packet(&pkt);


    // TODO: Should also flush the filter graph but that 
    //       should be ione for now. 
    for (int got_output = 1; got_output;)
    {
      avcodec_encode_video2(videoCodecCtx, &pkt, NULL, &got_output);
        if (got_output)
            finish_frame(pkt, true);
    }

    for (int got_output = 1; got_output && params.enable_audio;)
    {
        avcodec_encode_audio2(audioCodecCtx, &pkt, NULL, &got_output);
        if (got_output)
            finish_frame(pkt, false);
    }

    // Writing the end of the file.
    av_write_trailer(fmtCtx);

    // Closing the file.
    if (!(outputFmt->flags & AVFMT_NOFILE))
        avio_closep(&fmtCtx->pb);

    avcodec_close(videoStream->codec);
    // Freeing all the allocated memory:
    sws_freeContext(swsCtx);

    av_frame_free(&encoder_frame);
    if (params.enable_audio)
        avcodec_close(audioStream->codec);

    // TODO: free all the hw accel
    avformat_free_context(fmtCtx);
}
