#include <stdio.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>

#include <libavcodec/avcodec.h>

#include <libavutil/audio_fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#include <libswresample/swresample.h>

#include "transcoding.h"


// Open input stream and the required decoder.
static int open_input_stream(const BufferData src,
                             AVFormatContext **input_format_context,
                             AVCodecContext **input_codec_context)
{
    AVCodecContext *avctx;
    AVCodec *input_codec;
    AVCodecParameters *codecpar;
    int error;

    *input_format_context = avformat_alloc_context();

    if (NULL == *input_format_context)
    {
        fprintf(stderr, "Could not aloc input format context, allocate memory failed.\n");
        return AVERROR(ENOMEM);
    }

    BufferIO *bio = (BufferIO *)av_malloc(sizeof(BufferIO));
    bio->buf = src.buf;
    bio->curr = 0;
    bio->size = src.size;
    bio->_total = src.size;

    error = init_io_context_default(*input_format_context, 0, bio);
    if (error != 0)
    {
        fprintf(stderr, "Could not init IO context.\n");
        avformat_close_input(input_format_context);
        *input_format_context = NULL;
        return error;
    }

    error = avformat_open_input(input_format_context, NULL, NULL, NULL);
    if (error < 0)
    {
        fprintf(stderr, "Could not open input stream.\n");
        *input_format_context = NULL;
        return error;
    }

    error = avformat_find_stream_info(*input_format_context, NULL);
    if (error < 0)
    {
        fprintf(stderr, "Could not open find stream info.\n");
        avformat_close_input(input_format_context);
        return error;
    }

    // Make sure that there is only one stream in the input file.
    if ((*input_format_context)->nb_streams != 1)
    {
        fprintf(stderr,
                "Expected one audio input stream, but found %d.\n",
                (*input_format_context)->nb_streams);
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    }

    codecpar = (*input_format_context)->streams[0]->codecpar;

    // Find a decoder for the audio stream.
    input_codec = avcodec_find_decoder(codecpar->codec_id);
    if (!input_codec)
    {
        fprintf(stderr, "Could not find input codec.\n");
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    }

    // allocate a new decoding context
    avctx = avcodec_alloc_context3(input_codec);
    if (!avctx)
    {
        fprintf(stderr, "Could not allocate a decoding context.\n");
        avformat_close_input(input_format_context);
        return AVERROR(ENOMEM);
    }

    // initialize the stream parameters with demuxer information
    error = avcodec_parameters_to_context(avctx, codecpar);
    if (error < 0)
    {
        avformat_close_input(input_format_context);
        avcodec_free_context(&avctx);
        return error;
    }

    /*
     Some of audio formats, such as *.wav whose codec is pcm_s16le,
     have no infomation on channel layout, we need to set it manually in case aborting.
    */
    if (avctx->channel_layout == 0)
    {
        avctx->channel_layout = av_get_default_channel_layout(codecpar->channels);
    }

    error = avcodec_open2(avctx, input_codec, NULL);
    if (error < 0)
    {
        fprintf(stderr, "Could not open input codec.\n");
        avcodec_free_context(&avctx);
        avformat_close_input(input_format_context);
        return error;
    }

    *input_codec_context = avctx;

    return 0;
}

static int set_encoder_params(const TranscodingArgs args,
                              AVCodecContext *encoder_ctx,
                              AVCodec *encoder,
                              AVCodecContext *input_ctx)
{

    int idx, found;

    // // Allow the use of the experimental encoders, such as AAC
    // encoder_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    encoder_ctx->channels = input_ctx->channels;
    encoder_ctx->channel_layout = av_get_default_channel_layout(encoder_ctx->channels);
    encoder_ctx->sample_fmt = input_ctx->sample_fmt;

    if (encoder->channel_layouts != NULL)
    {
        idx = 0;
        found = 0;
        while (encoder->channel_layouts[idx] != 0)
        {
            if (encoder->channel_layouts[idx] == encoder_ctx->channel_layout)
            {
                found = 1;
                break;
            }
            idx = idx + 1;
        }
        if (found == 0)
        {
            char ch_layout_str[128];
            av_get_channel_layout_string(ch_layout_str, 128,
                                         encoder_ctx->channels,
                                         encoder_ctx->channel_layout);
            fprintf(stderr, "channel layout is not supported : %s", ch_layout_str);
            return -1;
        }
    }

    if (encoder->sample_fmts != NULL)
    {
        idx = 0;
        found = 0;
        while (encoder->sample_fmts[idx] != -1)
        {
            if (encoder->sample_fmts[idx] == input_ctx->sample_fmt)
            {
                found = 1;
                break;
            }
            idx = idx + 1;
        }
        if (found == 0)
        {
            encoder_ctx->sample_fmt = encoder->sample_fmts[0];
        }
    }
    else
    {
        fprintf(stdout,
                "Warning: don't know the supported sample formats of the encoder %s, "
                "using the input sample format by default, though it may crash.\n",
                encoder->name);
    }

    int sample_rate = args.sample_rate > 0 ? args.sample_rate : input_ctx->sample_rate;

    if (encoder->supported_samplerates != NULL)
    {
        idx = 0;
        found = 0;
        while (encoder->supported_samplerates[idx] != 0)
        {
            if (encoder->supported_samplerates[idx] == sample_rate)
            {
                found = 1;
                break;
            }
            idx = idx + 1;
        }
        if (found == 1)
        {
            encoder_ctx->sample_rate = sample_rate;
        }
        else
        {
            encoder_ctx->sample_rate = encoder->supported_samplerates[0];
            if (args.sample_rate > 0)
            {
                fprintf(stdout,
                        "The encoder %s doesn't support the sample rate you specified"
                        ", so use %d instead.\n",
                        encoder->name, encoder->supported_samplerates[0]);
            }
        }
    }
    else
    {
        encoder_ctx->sample_rate = sample_rate;
    }

    // For opus, it's encouraged to always use 48kHz
    if (encoder->id == AV_CODEC_ID_OPUS)
    {
        encoder_ctx->sample_rate = 48000;
    }

    if (args.bit_rate > 0)
    {
        encoder_ctx->bit_rate = args.bit_rate;
    }

    return 0;
}

// Open an output stream and the required encoder. Also set some basic encoder parameters.
static int open_output_stream(const TranscodingArgs args, BufferIO * bio,
                              AVCodecContext *input_codec_context,
                              AVFormatContext **output_format_context,
                              AVCodecContext **output_codec_context)
{
    int error;
    AVStream *stream      = NULL;
    AVCodecContext *avctx = NULL;
    AVCodec *output_codec = NULL;
    enum AVCodecID encoder_id = AV_CODEC_ID_NONE;

    // Create a new format context for the output container format.
    char outname[16] = "o.";
    av_strlcpy(outname+2, args.format_name, 14);

    error = avformat_alloc_output_context2(output_format_context, NULL, NULL, outname);
    if (error < 0)
    {
        fprintf(stderr, "Could not allocate output format context.\n");
        return error;
    }

    error = init_io_context_default(*output_format_context, 1, bio);
    if (error != 0 )
    {
        fprintf(stderr, "Could not init output format context.\n");
        avformat_free_context(*output_format_context);
        *output_format_context = NULL;
        return error;
    }

    // Find the encoder to be used by its name.
    // av_get_pcm_codec(enum AVSampleFormat fmt, int be)
    encoder_id = av_guess_codec((*output_format_context)->oformat,
                                NULL, NULL, NULL, AVMEDIA_TYPE_AUDIO);
    output_codec = avcodec_find_encoder(encoder_id);
    if (!output_codec)
    {
        fprintf(stderr, "Could not find encoder.\n");
        goto cleanup;
    }

    // Create a new audio stream in the output container.
    stream = avformat_new_stream(*output_format_context, NULL);
    if (!stream)
    {
        fprintf(stderr, "Could not create new stream.\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    avctx = avcodec_alloc_context3(output_codec);
    if (!avctx)
    {
        fprintf(stderr, "Could not allocate an encoding context.\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    error = set_encoder_params(args, avctx, output_codec, input_codec_context);
    if (error != 0)
    {
        goto cleanup;
    }

    // Set the sample rate for the container.
    stream->time_base.num = 1;
    stream->time_base.den = avctx->sample_rate;

    /*
     Some container formats (like MP4) require global headers to be present
     Mark the encoder so that it behaves accordingly.
    */
    if ((*output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
    {
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open the encoder for the audio stream to use it later.
    error = avcodec_open2(avctx, output_codec, NULL);
    if (error < 0)
    {
        fprintf(stderr, "Could not open output codec.\n");
        goto cleanup;
    }

    error = avcodec_parameters_from_context(stream->codecpar, avctx);
    if (error < 0)
    {
        fprintf(stderr, "Could not initialize stream parameters.\n");
        goto cleanup;
    }

    *output_codec_context = avctx;

    return 0;

cleanup:
    avcodec_free_context(&avctx);
    avformat_free_context(*output_format_context);
    *output_format_context = NULL;
    return error < 0 ? error : AVERROR_EXIT;
}

// Initialize one data packet for reading or writing.
static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    // Set the packet data and size so that it is recognized as being empty.
    packet->data = NULL;
    packet->size = 0;
}

// Initialize one audio frame for reading from the input file
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc()))
    {
        fprintf(stderr, "Could not allocate input frame.\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/*
 Initialize the audio resampler based on the input and output codec settings.
 If the input and output sample formats differ, a conversion is required
 libswresample takes care of this, but requires initialization.
 */
static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context)
{
    int error;

    /*
     Create a resampler context for the conversion.
     Set the conversion parameters.
     Default channel layouts based on the number of channels
     are assumed for simplicity (they are sometimes not detected
     properly by the demuxer and/or decoder).
     */

    *resample_context = swr_alloc();

    if (!(*resample_context))
    {
        fprintf(stderr, "Could not allocate resample context.\n");
        return AVERROR(ENOMEM);
    }


    av_opt_set_int(*resample_context, "in_sample_rate", input_codec_context->sample_rate, 0);
    av_opt_set_sample_fmt(*resample_context, "in_sample_fmt", input_codec_context->sample_fmt, 0);
    av_opt_set_channel_layout(*resample_context, "in_channel_layout",
                              av_get_default_channel_layout(input_codec_context->channels), 0);

    av_opt_set_int(*resample_context, "out_sample_rate", output_codec_context->sample_rate, 0);
    av_opt_set_sample_fmt(*resample_context, "out_sample_fmt", output_codec_context->sample_fmt, 0);
    av_opt_set_channel_layout(*resample_context, "out_channel_layout",
                              output_codec_context->channel_layout, 0);

    // Open the resampler with the specified parameters.
    error = swr_init(*resample_context);
    if (error < 0)
    {
        fprintf(stderr, "Could not open resample context.\n");
        swr_free(resample_context);
        return error;
    }
    return 0;
}

// Initialize a FIFO buffer for the audio samples to be encoded.
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    // Create the FIFO buffer based on the specified output sample format.
    *fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                output_codec_context->channels, 1);

    if (!fifo)
    {
        fprintf(stderr, "Could not allocate FIFO.\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

// Write the header of the output file container.
static int write_output_file_header(AVFormatContext *output_format_context)
{
    int error;
    error = avformat_write_header(output_format_context, NULL);
    if (error < 0)
    {
        fprintf(stderr, "Could not write output file header.\n");
        return error;
    }
    return 0;
}

// Decode one audio frame from the input file.
static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished)
{
    int error;
    AVPacket input_packet; // Packet used for temporary storage.

    init_packet(&input_packet);

    // Read one audio frame from the input file into a temporary packet.
    error = av_read_frame(input_format_context, &input_packet);
    if (error < 0)
    {
        // If we are at the end of the file, flush the decoder below.
        if (error == AVERROR_EOF)
        {
            *finished = 1;
        }
        else
        {
            fprintf(stderr, "Could not read frame.\n");
            return error;
        }
    }

    /*
     Decode the audio frame stored in the temporary packet.
     The input audio stream decoder is used to do this.
     If we are at the end of the file, pass an empty packet to the decoder
     to flush it.
     */
    error = avcodec_decode_audio4(input_codec_context, frame, data_present, &input_packet);
    if (error < 0)
    {
        fprintf(stderr, "Could not decode frame.\n");
        av_packet_unref(&input_packet);
        return error;
    }

    /*
     If the decoder has not been flushed completely, we are not finished,
     so that this function has to be called again.
     */
    if (*finished && *data_present)
    {
        *finished = 0;
    }

    av_packet_unref(&input_packet);

    return 0;
}

/*
 Initialize a temporary storage for the specified number of audio samples.
 The conversion requires temporary storage due to the different format.
 The number of audio samples to be allocated is specified in frame_size.
 */
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    int error;

    /*
     Allocate as many pointers as there are audio channels.
     Each pointer will later point to the audio samples of the corresponding
     channels (although it may be NULL for interleaved formats).
     */
    *converted_input_samples = calloc(output_codec_context->channels,
                                      sizeof(**converted_input_samples));
    if (!(*converted_input_samples))
    {
        fprintf(stderr, "Could not allocate converted input sample pointers.\n");
        return AVERROR(ENOMEM);
    }

    /*
     Allocate memory for the samples of all channels in one consecutive
     block for convenience.
     */
    error = av_samples_alloc(*converted_input_samples, NULL,
                             output_codec_context->channels, frame_size,
                             output_codec_context->sample_fmt, 0);
    if (error < 0)
    {
        fprintf(stderr, "Could not allocate converted input samples.\n");
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);

        return error;
    }
    else
    {
        return 0;
    }

}

// Add converted input audio samples to the FIFO buffer for later processing.
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /*
     Make the FIFO as large as it needs to be to hold both,
     the old and the new samples.
     */
    if (frame_size <= 0)
    {
        return 0;
    }
    error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size);
    if (error < 0)
    {
        fprintf(stderr, "Could not reallocate FIFO.\n");
        return error;
    }

    // Store the new samples in the FIFO buffer.
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples, frame_size) < frame_size)
    {
        fprintf(stderr, "Could not write data to FIFO.\n");
        return AVERROR_EXIT;
    }
    else
    {
        return 0;
    }
}

/*
 Read one audio frame from the input file, decodes, converts and stores
 it in the FIFO buffer.
 */
static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resample_context,
                                         int *finished)
{
    // Temporary storage of the input samples of the frame read from the file.
    AVFrame *input_frame = NULL;
    // Temporary storage for the converted input samples.
    uint8_t **converted_input_samples = NULL;
    int data_present;
    int ret = AVERROR_EXIT;

    // Initialize temporary storage for one input frame.
    if (init_input_frame(&input_frame))
    {
        goto cleanup;
    }

    // Decode one frame worth of audio samples.
    if (decode_audio_frame(input_frame, input_format_context, input_codec_context,
                           &data_present, finished))
    {
        goto cleanup;
    }

    /*
     If we are at the end of the file and there are no more samples
     in the decoder which are delayed, we are actually finished.
     This must not be treated as an error.
     */
    if (*finished && !data_present)
    {
        ret = 0;
        goto cleanup;
    }

    // If there is decoded data, convert and store it
    if (data_present)
    {
        int64_t delay;
        int desired_nb_samples, converted_nb_samples;
        delay = swr_get_delay(resample_context, input_codec_context->sample_rate);

        desired_nb_samples = (int)av_rescale_rnd(delay + input_frame->nb_samples,
                                                 output_codec_context->sample_rate,
                                                 input_codec_context->sample_rate,
                                                 AV_ROUND_UP);

        // Initialize the temporary storage for the converted input samples.
        if (init_converted_samples(&converted_input_samples,
                                   output_codec_context,
                                   desired_nb_samples))
        {
            goto cleanup;
        }

        /*
         Convert the input samples to the output sample format using the resampler.
         This requires a temporary storage provided by converted_input_samples.
         */
        converted_nb_samples = swr_convert(resample_context,
                                           converted_input_samples, desired_nb_samples,
                                           (const uint8_t**)input_frame->extended_data,
                                           input_frame->nb_samples);

        if (converted_nb_samples < 0)
        {
            fprintf(stderr, "Could not convert input samples.\n");
            goto cleanup;
        }

        // Add the converted samples to the FIFO buffer for later processing.
        if (add_samples_to_fifo(fifo, converted_input_samples, converted_nb_samples))
        {
            goto cleanup;
        }

        ret = 0;
    }

    ret = 0;

cleanup:
    if (converted_input_samples)
    {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }
    av_frame_free(&input_frame);

    return ret;
}

/*
 Initialize one input frame for writing to the output file.
 The frame will be exactly frame_size samples large.
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    // Create a new frame to store the audio samples.
    *frame = av_frame_alloc();
    if (!(*frame))
    {
        fprintf(stderr, "Could not allocate output frame.\n");
        return AVERROR_EXIT;
    }

    /*
     Set the frame's parameters, especially its size and format.
     av_frame_get_buffer needs this to allocate memory for the
     audio samples of the frame.
     Default channel layouts based on the number of channels
     are assumed for simplicity.
     */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /*
     Allocate the samples of the created frame. This call will make
     sure that the audio frame can hold as many samples as specified.
     */
    error = av_frame_get_buffer(*frame, 0);
    if (error < 0)
    {
        fprintf(stderr, "Could allocate output frame samples.\n");
        av_frame_free(frame);

        return error;
    }
    else
    {
        return 0;
    }
}

// Encode one frame worth of audio to the output file.
static int encode_audio_frame(int64_t *pts, AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    int error;
    // Packet used for temporary storage.
    AVPacket output_packet;
    init_packet(&output_packet);

    // Set a timestamp based on the sample rate for the container.
    if (frame)
    {
        frame->pts = *pts;
        *pts = *pts + frame->nb_samples;
    }

    /*
     Encode the audio frame and store it in the temporary packet.
     The output audio stream encoder is used to do this.
     */
    error = avcodec_encode_audio2(output_codec_context, &output_packet, frame, data_present);
    if (error < 0)
    {

        fprintf(stderr, "Could not encode frame.\n");
        av_packet_unref(&output_packet);

        return error;
    }

    // Write one audio frame from the temporary packet to the output file.
    if (*data_present)
    {
        error = av_write_frame(output_format_context, &output_packet);
        if (error < 0)
        {
            fprintf(stderr, "Could not write frame.\n");
            av_packet_unref(&output_packet);

            return error;
        }

        av_packet_unref(&output_packet);
    }

    return 0;
}

// Load one audio frame from the FIFO buffer, encode and write it to the output file.
static int load_encode_and_write(int64_t *pts, AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    // Temporary storage of the output samples of the frame written to the file.
    AVFrame *output_frame;
    /*
     Use the maximum number of possible samples per frame.
     If there is less than the maximum possible frame size in the FIFO
     buffer use this number. Otherwise, use the maximum possible frame size
     */
    const int frame_size = FFMIN(av_audio_fifo_size(fifo), output_codec_context->frame_size);
    int data_written;

    // Initialize temporary storage for one output frame.
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
    {
        return AVERROR_EXIT;
    }

    /*
     Read as many samples from the FIFO buffer as required to fill the frame.
     The samples are stored in the frame temporarily.
     */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size)
    {
        fprintf(stderr, "Could not read data from FIFO.\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    // Encode one frame worth of audio samples.
    if (encode_audio_frame(pts,
                           output_frame, output_format_context, output_codec_context,
                           &data_written))
    {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    av_frame_free(&output_frame);

    return 0;
}

static int write_output_file_trailer(AVFormatContext *output_format_context)
{
    int error;

    error = av_write_trailer(output_format_context);
    if (error < 0)
    {
        fprintf(stderr, "Could not write output file trailer.\n");
        return error;
    }
    else
    {
        return 0;
    }
}

int transcoding(BufferData *p_dst_buf, int *out_bit_rate, float *out_duration, const TranscodingArgs args, const BufferData src_buf)
{
    int ret = AVERROR(AVERROR_EXIT);
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
    AVCodecContext  *input_codec_context = NULL,  *output_codec_context = NULL;
    SwrContext      *resample_context = NULL;
    AVAudioFifo     *fifo = NULL;
    int64_t pts = 0; // Global timestamp for the audio frames

    av_register_all();

    if (open_input_stream(src_buf, &input_format_context, &input_codec_context))
    {
        goto cleanup;
    }

    // Estimate output buffer size in bytes
    size_t estimated_bytes;
    if (args.bit_rate > 0)
    {
        AVStream *audio_stream = input_format_context->streams[0];
        double duration = audio_stream->duration * av_q2d(audio_stream->time_base);
        estimated_bytes= args.bit_rate * duration / 8;
    }
    else
    {
        estimated_bytes = src_buf.size / 18;
    }

    BufferIO * bio = (BufferIO *)av_malloc(sizeof(BufferIO));

    bio->buf = (uint8_t *)av_malloc(estimated_bytes);
    if (bio->buf == NULL)
    {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    bio->curr   = 0;
    bio->size   = 0;
    bio->_total = estimated_bytes;

    if (open_output_stream(args, bio, input_codec_context,
                           &output_format_context, &output_codec_context))
    {
        goto cleanup;
    }

    // Initialize the resampler to be able to convert audio sample formats.
    if (init_resampler(input_codec_context, output_codec_context, &resample_context))
    {
        goto cleanup;
    }

    // Initialize the FIFO buffer to store audio samples to be encoded.
    if (init_fifo(&fifo, output_codec_context))
    {
        goto cleanup;
    }

    // Write the header of the output file container.
    if (write_output_file_header(output_format_context))
    {
        goto cleanup;
    }

    /*
     Loop as long as we have input samples to read or
     output samples to write; abort as soon as we have neither.
     */
    while (1)
    {
        // Use the encoder's desired frame size for processing.
        const int output_frame_size = output_codec_context->frame_size;
        int finished = 0;

        /*
         Make sure that there is one frame worth of samples in the FIFO
         buffer so that the encoder can do its work.
         Since the decoder's and the encoder's frame size may differ, we
         need to FIFO buffer to store as many frames worth of input samples
         that they make up at least one frame worth of output samples.
        */
        while (av_audio_fifo_size(fifo) < output_frame_size)
        {
            /*
              Decode one frame worth of audio samples, convert it to the
              output sample format and put it into the FIFO buffer.
             */
            if (read_decode_convert_and_store(fifo, input_format_context,
                                              input_codec_context,
                                              output_codec_context,
                                              resample_context, &finished))
            {
                goto cleanup;
            }

            /*
             If we are at the end of the input file, we continue
             encoding the remaining audio samples to the output file.
             */
            if (finished)
            {
                break;
            }
        }

        /*
         If we have enough samples for the encoder, we encode them.
         At the end of the file, we pass the remaining samples to
         the encoder.
         */
        while (av_audio_fifo_size(fifo) >= output_frame_size ||
               (finished && av_audio_fifo_size(fifo) > 0))
        {
            /*
            Take one frame worth of audio samples from the FIFO buffer,
            encode it and write it to the output file.
            */
            if (load_encode_and_write(&pts, fifo, output_format_context, output_codec_context))
            {
                goto cleanup;
            }
        }

        /*
         If we are at the end of the input file and have encoded
         all remaining samples, we can exit this loop and finish.
         */
        if (finished)
        {
            int data_written;
            // Flush the encoder as it may have delayed frames.
            do
            {
                if (encode_audio_frame(&pts, NULL,
                                       output_format_context, output_codec_context,
                                       &data_written))
                {
                    goto cleanup;
                }
            } while (data_written);

            break;
        }
    }

    // Write the trailer of the output file container.
    if (write_output_file_trailer(output_format_context))
    {
        goto cleanup;
    }

    p_dst_buf->buf = bio->buf;
    p_dst_buf->size = bio->size;

    *out_duration = (float)pts / output_codec_context->sample_rate;

    *out_bit_rate = 8 * bio->size / *out_duration;
    *out_bit_rate = *out_bit_rate - *out_bit_rate % 1000;

    ret = 0;

cleanup:
    if (fifo)
    {
        av_audio_fifo_free(fifo);
    }
    swr_free(&resample_context);
    if (output_codec_context)
    {
        avcodec_free_context(&output_codec_context);
    }
    if (output_format_context)
    {
        // avio_closep(&(output_format_context->pb)); // DEBUG: exc bad access
        avformat_free_context(output_format_context);
    }
    if (input_codec_context)
    {
        avcodec_free_context(&input_codec_context);
    }
    if (input_format_context)
    {
        avformat_close_input(&input_format_context);
    }

    return ret;
}

