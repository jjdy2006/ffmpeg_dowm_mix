/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string>
#include <iostream>

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

}

#define LOG_TAG "ffmpeg_jni"
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define LIBRARY_FUNC(RETURN_TYPE, NAME, ...)                              \
  extern "C" {                                                            \
  JNIEXPORT RETURN_TYPE                                                   \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegLibrary_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__);                      \
  }                                                                       \
  JNIEXPORT RETURN_TYPE                                                   \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegLibrary_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define AUDIO_DECODER_FUNC(RETURN_TYPE, NAME, ...)                             \
  extern "C" {                                                                 \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegAudioDecoder_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__);                           \
  }                                                                            \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegAudioDecoder_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define ERROR_STRING_BUFFER_LENGTH 256

// Output format corresponding to AudioFormat.ENCODING_PCM_16BIT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
// Output format corresponding to AudioFormat.ENCODING_PCM_FLOAT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;

static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;

/**
 * Returns the AVCodec with the specified name, or NULL if it is not available.
 */
AVCodec *getCodecByName(JNIEnv *env, jstring codecName);

/**
 * Allocates and opens a new AVCodecContext for the specified codec, passing the
 * provided extraData as initialization data for the decoder if it is non-NULL.
 * Returns the created context.
 */
AVCodecContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                              jboolean outputFloat, jint rawSampleRate,
                              jint rawChannelCount);

/**
 * Decodes the packet into the output buffer, returning the number of bytes
 * written, or a negative AUDIO_DECODER_ERROR constant value in the case of an
 * error.
 */
int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize);

/**
 * Transforms ffmpeg AVERROR into a negative AUDIO_DECODER_ERROR constant value.
 */
int transformError(int errorNumber);

/**
 * Outputs a log message describing the avcodec error number.
 */
void logError(const char *functionName, int errorNumber);

/**
 * Releases the specified context.
 */
void releaseContext(AVCodecContext *context);

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }
    avcodec_register_all();
    av_register_all();//lqnxg
    avfilter_register_all();//lqnxg

    return JNI_VERSION_1_6;
}


LIBRARY_FUNC(jstring, ffmpegGetVersion) {
    return env->NewStringUTF(LIBAVCODEC_IDENT);
}

LIBRARY_FUNC(jint, ffmpegGetInputBufferPaddingSize) {
    return (jint) AV_INPUT_BUFFER_PADDING_SIZE;
}

LIBRARY_FUNC(jboolean, ffmpegHasDecoder, jstring codecName) {
    return getCodecByName(env, codecName) != NULL;
}

AUDIO_DECODER_FUNC(jlong, ffmpegInitialize, jstring codecName,
                   jbyteArray extraData, jboolean outputFloat,
                   jint rawSampleRate, jint rawChannelCount) {
    AVCodec *codec = getCodecByName(env, codecName);
    if (!codec) {
        LOGE("Codec not found.");
        return 0L;
    }
    return (jlong) createContext(env, codec, extraData, outputFloat, rawSampleRate,
                                 rawChannelCount);
}

AUDIO_DECODER_FUNC(jint, ffmpegDecode, jlong context, jobject inputData,
                   jint inputSize, jobject outputData, jint outputSize) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    if (!inputData || !outputData) {
        LOGE("Input and output buffers must be non-NULL.");
        return -1;
    }
    if (inputSize < 0) {
        LOGE("Invalid input buffer size: %d.", inputSize);
        return -1;
    }
    if (outputSize < 0) {
        LOGE("Invalid output buffer length: %d", outputSize);
        return -1;
    }
    uint8_t *inputBuffer = (uint8_t *) env->GetDirectBufferAddress(inputData);
    uint8_t *outputBuffer = (uint8_t *) env->GetDirectBufferAddress(outputData);


    AVPacket packet;
    av_init_packet(&packet);
    packet.data = inputBuffer;
    packet.size = inputSize;
    return decodePacket((AVCodecContext *) context, &packet, outputBuffer,
                        outputSize);
}

AUDIO_DECODER_FUNC(jint, ffmpegGetChannelCount, jlong context) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    return ((AVCodecContext *) context)->channels;
}

AUDIO_DECODER_FUNC(jint, ffmpegGetSampleRate, jlong context) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    return ((AVCodecContext *) context)->sample_rate;
}

AUDIO_DECODER_FUNC(jlong, ffmpegReset, jlong jContext, jbyteArray extraData) {
    AVCodecContext *context = (AVCodecContext *) jContext;
    if (!context) {
        LOGE("Tried to reset without a context.");
        return 0L;
    }

    AVCodecID codecId = context->codec_id;
    if (codecId == AV_CODEC_ID_TRUEHD) {
        // Release and recreate the context if the codec is TrueHD.
        // TODO: Figure out why flushing doesn't work for this codec.
        releaseContext(context);
        AVCodec *codec = avcodec_find_decoder(codecId);
        if (!codec) {
            LOGE("Unexpected error finding codec %d.", codecId);
            return 0L;
        }
        jboolean outputFloat =
                (jboolean) (context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
        return (jlong) createContext(env, codec, extraData, outputFloat,
                /* rawSampleRate= */ -1,
                /* rawChannelCount= */ -1);
    }

    avcodec_flush_buffers(context);
    return (jlong) context;
}

AUDIO_DECODER_FUNC(void, ffmpegRelease, jlong context) {
    if (context) {
        releaseContext((AVCodecContext *) context);
    }
}

AVCodec *getCodecByName(JNIEnv *env, jstring codecName) {
    if (!codecName) {
        return NULL;
    }
    const char *codecNameChars = env->GetStringUTFChars(codecName, NULL);
    AVCodec *codec = avcodec_find_decoder_by_name(codecNameChars);
    env->ReleaseStringUTFChars(codecName, codecNameChars);
    return codec;
}

int param_channels = 2;//java层传递进来的需要设置的声道数

AVCodecContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                              jboolean outputFloat, jint rawSampleRate,
                              jint rawChannelCount) {
    param_channels = rawChannelCount;
    // 创建音频解码器上下文
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context) {
        LOGE("Failed to allocate context.");
        return NULL;
    }
//    context->sample_fmt = OUTPUT_FORMAT_PCM_16BIT;//lqnxg
    context->request_sample_fmt =
            outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
    if (extraData) {
        jsize size = env->GetArrayLength(extraData);
        context->extradata_size = size;
        context->extradata =
                (uint8_t *) av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!context->extradata) {
            LOGE("Failed to allocate extradata.");
            releaseContext(context);
            return NULL;
        }
        env->GetByteArrayRegion(extraData, 0, size, (jbyte *) context->extradata);
    }
    if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
        context->codec_id == AV_CODEC_ID_PCM_ALAW) {
        context->sample_rate = rawSampleRate;
        context->channels = rawChannelCount;
        context->channel_layout = av_get_default_channel_layout(rawChannelCount);
    }
    context->err_recognition = AV_EF_IGNORE_ERR;
    int result = avcodec_open2(context, codec, NULL);
//    context->sample_fmt = OUTPUT_FORMAT_PCM_16BIT;//lqnxg
    if (result < 0) {
        logError("avcodec_open2", result);
        releaseContext(context);
        return NULL;
    }
    return context;
}


/**
 * 打印所有已经注册的过滤器
 */
void print_all_filters() {
    const AVFilter *filter = NULL;
    void *opaque = NULL;
    while ((filter = av_filter_iterate(&opaque))) {
        if (filter == nullptr || filter == NULL) {
            LOGE("奇怪了，过滤器为空");
        } else {
            LOGE("Filter name: %s", filter->name);
        }
    }
}

int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize) {
    LOGE("##############################decodePacket START####################################");
    int result = 0;
    // Queue input data.
    result = avcodec_send_packet(context, packet);//真正的解码操作，负责将数据发送给解码器
    if (result) {
        logError("avcodec_send_packet", result);
        return transformError(result);
    }


//    print_all_filters();

    //这块贼牛逼，param_channels=1是双声道，声道数是2.param_channels=2是5.1，声道数是6
    LOGE("检查设置的声道参数，以便确认是否需要过滤器：%d", param_channels);



    // Dequeue output data until it runs out.
    int outSize = 0;
    while (true) {
//        LOGE("&&&&&&&&&&&&&&&&&while循环开始&&&&&&&&&&&&&&&&&");
        AVFrame *frame = av_frame_alloc();//分配一个帧结构体的内存空间
        if (!frame) {
            LOGE("Failed to allocate output frame.");
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }


        result = avcodec_receive_frame(context, frame);//提取解码后的数据，用于从解码器中提取出解码后的帧数据
//        LOGE("从解码器中提取出解码后的帧数据的情况：result===%d", result);

        if (result) {
            av_frame_free(&frame);
//            LOGE("执行了释放帧的方法");
            if (result == AVERROR(EAGAIN)) {
                break;
            }
            logError("avcodec_receive_frame", result);
            return transformError(result);
        }


        //param_channels=1是双声道，声道数是2.param_channels=2是5.1，声道数是6
        if (param_channels == 1) {//只有java层设置了双声道，才需要走过滤器来做声道融合


            LOGE("帧数据：sample_rate===%d", frame->sample_rate);
//            LOGE("帧数据：nb_samples===%d", frame->nb_samples);
            LOGE("帧数据：声道数量===%d", frame->channels);
//            LOGE("帧数据：音频格式===%s",av_get_sample_fmt_name((AVSampleFormat) frame->format));//8=fltp,1=s16
//            LOGE("帧数据：音频数据===%d", frame->data);


            // 创建过滤器图和上下文
//            const char *filter_descr = "pan=stereo|FL<FL+0.5*FC+0.6*LFE+0.6*SL|FR<FR+0.5*FC+0.6*LFE+0.6*SR";//如果通道规范中的“=”被“<”替换，则该规范的增益将被重新规范化，使总数为1，从而避免削波噪声。
            const char *filter_descr = "pan=stereo|c0=FL+FC+LFE+SL|c1=FR+FC+LFE+SR";//音量与不使用滤波器相同，但是看FFMPEG官方文档中说，这样可能会造成削波噪声
//            const char *filter_descr = "pan=stereo|FL<FL+0.5*FC+0.6*BL+0.6*SL|FR<FR+0.5*FC+0.6*BR+0.6*SR";//FFMPEG官方文档中给的downmix到立体声的示例
//        const char *filter_descr = "pan=stereo|c0=FL|c1=FR";//只保留左右声道声音
            AVFilterGraph *filter_graph = avfilter_graph_alloc();
            AVFilterContext *pan_ctx;
            AVFilterContext *buffersrc_ctx = nullptr;
            AVFilterContext *buffersink_ctx = nullptr;
            AVFilterInOut *inputs = avfilter_inout_alloc();
            AVFilterInOut *outputs = avfilter_inout_alloc();
            const AVFilter *buffersrc = avfilter_get_by_name("abuffer");
            const AVFilter *buffersink = avfilter_get_by_name("abuffersink");
            int ret;

            if (!buffersrc) {
//                LOGE("流程：buffersrc创建失败");
            } else {
//                LOGE("流程：buffersrc创建成功");
            }

            char args[512];
            //重点来了，PRIx64太重要了，这里的channel_layout只接受一个这样类型的数据，代表5.1声道或者立体声啥的，必备
            snprintf(args, sizeof(args),
                     "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
                     1, frame->sample_rate, frame->sample_rate,
                     av_get_sample_fmt_name(context->sample_fmt),
                     frame->channel_layout);


            if (avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args,
                                             nullptr, filter_graph) < 0) {
//                LOGE("流程：avfilter_graph_create_filter  in 失败");
            } else {
//                LOGE("流程：avfilter_graph_create_filter  in 成功");
            }


            // 创建一个缓冲区接收器过滤器

            if (!buffersink) {
//                LOGE("流程：buffersink创建失败");
            } else {
//                LOGE("流程：buffersink创建成功");
            }
            if (avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr, nullptr,
                                             filter_graph) < 0) {
//                LOGE("流程：avfilter_graph_create_filter  out 失败");
            } else {
//                LOGE("流程：avfilter_graph_create_filter  out 成功");
            }
//    // 创建一个音频过滤器并连接到缓冲区源和接收器

            outputs->name = av_strdup("in");
            outputs->filter_ctx = buffersrc_ctx;
            outputs->pad_idx = 0;
            outputs->next = nullptr;

            inputs->name = av_strdup("out");
            inputs->filter_ctx = buffersink_ctx;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            // 将输入输出连接到过滤器图
            ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, nullptr);
            //过滤器输入输出直连，测试用，这样能测试一下代码大体正常否，目前直连的时候，一切正常，能正常输出PCM的音频
//        ret = avfilter_link(buffersrc_ctx, 0, buffersink_ctx, 0);
            if (ret < 0) {
//                LOGE("流程：这里加上pan过滤命令avfilter_graph_parse_ptr 失败:%d", ret);
            } else {
//                LOGE("流程：这里加上pan过滤命令avfilter_graph_parse_ptr 成功:%d", ret);
            }
            // 打开过滤器图
            ret = avfilter_graph_config(filter_graph, nullptr);
            if (ret < 0) {
//                LOGE("流程：avfilter_graph_config 失败:%d", ret);
            } else {
//                LOGE("流程：avfilter_graph_config 成功:%d", ret);
            }

            ret = av_buffersrc_add_frame(buffersrc_ctx, frame);
//        ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
//        ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_PUSH);
            if (ret < 0) {
                // 错误处理
//                LOGE("流程：av_buffersrc_add_frame 错误：%d", ret);
            } else {
//                LOGE("流程：av_buffersrc_add_frame 成功");
            }
            ret = av_buffersink_get_frame(buffersink_ctx, frame);
            if (ret < 0) {
                // 错误处理
//                LOGE("流程：av_buffersink_get_frame 错误%d", ret);
            } else {
//                LOGE("流程：av_buffersink_get_frame 成功");
            }

            // 释放资源
            avfilter_graph_free(&filter_graph);

            LOGE("过滤后的帧数据：sample_rate===%d", frame->sample_rate);
//            LOGE("过滤后的帧数据：nb_samples===%d", frame->nb_samples);
            LOGE("过滤后的帧数据：声道数量===%d", frame->channels);
//            LOGE("过滤后的帧数据：音频格式===%s",
//                 av_get_sample_fmt_name((AVSampleFormat) frame->format));//8=fltp,1=s16
//            LOGE("过滤后的帧数据：音频数据===%d", frame->data);
        }




        // Resample output.
        AVSampleFormat in_sampleFormat = context->sample_fmt;//fltp,一般情况都是这个，因为ffmpeg的储存格式就是这个
        AVSampleFormat out_sampleFormat = context->request_sample_fmt;//输出的格式，目前是s16

//        LOGE("音频输入格式in_sampleFormat：%s,%d", av_get_sample_fmt_name(in_sampleFormat),
//             in_sampleFormat);
//        LOGE("音频输出格式out_sampleFormat：%s,%d", av_get_sample_fmt_name(out_sampleFormat),
//             out_sampleFormat);


        int channelCount = frame->channels;
        int channelLayout = frame->channel_layout;
        int sampleRate = frame->sample_rate;
        int sampleCount = frame->nb_samples;
        int dataSize = av_samples_get_buffer_size(NULL, channelCount, sampleCount,
                                                  in_sampleFormat, 1);

        SwrContext *resampleContext;
        if (context->opaque) {
            resampleContext = (SwrContext *) context->opaque;
        } else {
            resampleContext = swr_alloc();
//            AV_CH_LAYOUT_STEREO,,,AV_CH_LAYOUT_STEREO_DOWNMIX
            av_opt_set_channel_layout(resampleContext, "in_channel_layout", channelLayout, 0);
            av_opt_set_channel_layout(resampleContext, "out_channel_layout",
                                      context->channel_layout, 0);
            av_opt_set_int(resampleContext, "in_sample_rate", sampleRate, 0);
            av_opt_set_int(resampleContext, "out_sample_rate", sampleRate, 0);
            av_opt_set_sample_fmt(resampleContext, "in_sample_fmt", in_sampleFormat, 0);
            av_opt_set_sample_fmt(resampleContext, "out_sample_fmt", out_sampleFormat, 0);
            result = swr_init(resampleContext);

            if (result < 0) {
                logError("swr_init", result);
                av_frame_free(&frame);
                return transformError(result);
            }
            context->opaque = resampleContext;
        }


        int inSampleSize = av_get_bytes_per_sample(in_sampleFormat);//每帧音频数据量的大小
        int outSampleSize = av_get_bytes_per_sample(out_sampleFormat);
        int outSamples = swr_get_out_samples(resampleContext, sampleCount);
        int bufferOutSize = outSampleSize * context->channels * outSamples;

        LOGE("**********************");
        LOGE("最终使用的数据：channelCount===%d", channelCount);
//        LOGE("最终使用的数据：channelLayout===%d", channelLayout);
        LOGE("最终使用的数据：sampleRate===%d", sampleRate);
//        LOGE("最终使用的数据：sampleCount===%d", sampleCount);
//        LOGE("最终使用的数据：dataSize===%d", dataSize);
//        LOGE("最终使用的数据：inSampleSize====%d", inSampleSize);
//        LOGE("最终使用的数据：outSampleSize====%d", outSampleSize);
//        LOGE("最终使用的数据：outSamples====%d", outSamples);
//        LOGE("最终使用的数据：bufferOutSize====%d", bufferOutSize);
        LOGE("**********************");


        if (outSize + bufferOutSize > outputSize) {
            LOGE("Output buffer size (%d) too small for output data (%d).",
                 outputSize, outSize + bufferOutSize);
            av_frame_free(&frame);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        if (av_sample_fmt_is_planar(context->sample_fmt)) {
//            LOGE("pcm planar模式");
        } else {
//            LOGE("pcm Pack模式");
        }
        if (av_sample_fmt_is_planar(context->request_sample_fmt)) {
//            LOGE("request_sample_fmt pcm planar模式");
        } else {
//            LOGE("request_sample_fmt pcm Pack模式");
        }

        //执行真正的重采样操作
        result = swr_convert(resampleContext, &outputBuffer, bufferOutSize,
                             (const uint8_t **) frame->data, sampleCount);
        av_frame_free(&frame);
        if (result < 0) {
            logError("swr_convert", result);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        int available = swr_get_out_samples(resampleContext, 0);
        if (available != 0) {
            LOGE("Expected no samples remaining after resampling, but found %d.",
                 available);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        outputBuffer += bufferOutSize;
        outSize += bufferOutSize;
//        LOGE("&&&&&&&&&&&&&&&&&while循环结束&&&&&&&&&&&&&&&&&");
    }
    LOGE("##############################decodePacket END####################################");
    return outSize;
}

int transformError(int errorNumber) {
    return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                              : AUDIO_DECODER_ERROR_OTHER;
}

void logError(const char *functionName, int errorNumber) {
    char *buffer = (char *) malloc(ERROR_STRING_BUFFER_LENGTH * sizeof(char));
    av_strerror(errorNumber, buffer, ERROR_STRING_BUFFER_LENGTH);
    LOGE("Error in %s: %s", functionName, buffer);
    free(buffer);
}

void releaseContext(AVCodecContext *context) {
    if (!context) {
        return;
    }
    SwrContext *swrContext;
    if ((swrContext = (SwrContext *) context->opaque)) {
        swr_free(&swrContext);
        context->opaque = NULL;
    }
    avcodec_free_context(&context);
}
