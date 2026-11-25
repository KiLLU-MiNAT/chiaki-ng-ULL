// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video-decoder.h"

#include <jni.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window_jni.h>

#include <string.h>

#define INPUT_BUFFER_TIMEOUT_MS 10

static void *android_chiaki_video_decoder_output_thread_func(void *user);

ChiakiErrorCode android_chiaki_video_decoder_init(AndroidChiakiVideoDecoder *decoder, ChiakiLog *log, int32_t target_width, int32_t target_height, ChiakiCodec codec)
{
	decoder->log = log;
	decoder->codec = NULL;
	decoder->timestamp_cur = 0;
	decoder->target_width = target_width;
	decoder->target_height = target_height;
	decoder->target_codec = codec;
	__atomic_store_n(&decoder->shutdown_output, false, __ATOMIC_RELEASE);
	decoder->window = NULL;
	return chiaki_mutex_init(&decoder->codec_mutex, false);
}

// Caller owns codec_mutex. Output thread never takes that mutex.
static void kill_decoder(AndroidChiakiVideoDecoder *decoder)
{
	__atomic_store_n(&decoder->shutdown_output, true, __ATOMIC_RELEASE);
	chiaki_thread_join(&decoder->output_thread, NULL);
	AMediaCodec_stop(decoder->codec);
	AMediaCodec_delete(decoder->codec);
	decoder->codec = NULL;
	if(decoder->window)
		ANativeWindow_release(decoder->window);
	decoder->window = NULL;
	__atomic_store_n(&decoder->shutdown_output, false, __ATOMIC_RELEASE);
}

void android_chiaki_video_decoder_fini(AndroidChiakiVideoDecoder *decoder)
{
	chiaki_mutex_lock(&decoder->codec_mutex);
	if(decoder->codec)
		kill_decoder(decoder);
	chiaki_mutex_unlock(&decoder->codec_mutex);
	chiaki_mutex_fini(&decoder->codec_mutex);
}

void android_chiaki_video_decoder_set_surface(AndroidChiakiVideoDecoder *decoder, JNIEnv *env, jobject surface)
{
	chiaki_mutex_lock(&decoder->codec_mutex);

	if(!surface)
	{
		if(decoder->codec)
		{
			kill_decoder(decoder);
			CHIAKI_LOGI(decoder->log, "Decoder shut down after surface was removed");
		}
		goto beach;
	}

	if(decoder->codec)
	{
#if __ANDROID_API__ >= 23
		CHIAKI_LOGI(decoder->log, "Video decoder already initialized, swapping surface");
		ANativeWindow *new_window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
		if(new_window && AMediaCodec_setOutputSurface(decoder->codec, new_window) == AMEDIA_OK)
		{
			ANativeWindow_release(decoder->window);
			decoder->window = new_window;
		}
		else if(new_window)
			ANativeWindow_release(new_window);
#else
		CHIAKI_LOGE(decoder->log, "Video Decoder already initialized");
#endif
		goto beach;
	}

	decoder->window = ANativeWindow_fromSurface(env, surface);
	if(!decoder->window)
		goto beach;

	const char *mime = chiaki_codec_is_h265(decoder->target_codec) ? "video/hevc" : "video/avc";
	CHIAKI_LOGI(decoder->log, "Initializing decoder with mime %s", mime);

	decoder->codec = AMediaCodec_createDecoderByType(mime);
	if(!decoder->codec)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create AMediaCodec for mime type %s", mime);
		goto error_surface;
	}

	AMediaFormat *format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->target_width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->target_height);

	// Android 11+ decoder hint. Older or unsupported codecs may ignore it.
	AMediaFormat_setInt32(format, "low-latency", 1);
	media_status_t r = AMediaCodec_configure(decoder->codec, format, decoder->window, NULL, 0);
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGW(decoder->log, "ULL: retrying normal decoder configuration");
		AMediaCodec_delete(decoder->codec);
		decoder->codec = AMediaCodec_createDecoderByType(mime);
		if(!decoder->codec)
		{
			AMediaFormat_delete(format);
			goto error_surface;
		}
		AMediaFormat_setInt32(format, "low-latency", 0);
		r = AMediaCodec_configure(decoder->codec, format, decoder->window, NULL, 0);
	}
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGE(decoder->log, "AMediaCodec_configure() failed: %d", (int)r);
		AMediaFormat_delete(format);
		goto error_codec;
	}

	r = AMediaCodec_start(decoder->codec);
	AMediaFormat_delete(format);
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGE(decoder->log, "AMediaCodec_start() failed: %d", (int)r);
		goto error_codec;
	}

	ChiakiErrorCode err = chiaki_thread_create(&decoder->output_thread, android_chiaki_video_decoder_output_thread_func, decoder);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create output thread for AMediaCodec");
		AMediaCodec_stop(decoder->codec);
		goto error_codec;
	}

	goto beach;

error_codec:
	AMediaCodec_delete(decoder->codec);
	decoder->codec = NULL;

error_surface:
	ANativeWindow_release(decoder->window);
	decoder->window = NULL;

beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
}

bool android_chiaki_video_decoder_video_sample(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	bool r = true;
	AndroidChiakiVideoDecoder *decoder = user;
	chiaki_mutex_lock(&decoder->codec_mutex);

	if(!decoder->codec)
	{
		CHIAKI_LOGE(decoder->log, "Received video data, but decoder is not initialized!");
		goto beach;
	}

	while(buf_size > 0)
	{
		ssize_t codec_buf_index = AMediaCodec_dequeueInputBuffer(decoder->codec, INPUT_BUFFER_TIMEOUT_MS * 1000);
		if(codec_buf_index < 0)
		{
			CHIAKI_LOGE(decoder->log, "Failed to get input buffer");
			r = false;
			goto beach;
		}

		size_t codec_buf_size;
		uint8_t *codec_buf = AMediaCodec_getInputBuffer(decoder->codec, (size_t)codec_buf_index, &codec_buf_size);
		size_t codec_sample_size = buf_size;
		if(codec_sample_size > codec_buf_size)
		{
			//CHIAKI_LOGD(decoder->log, "Sample is bigger than buffer, splitting");
			codec_sample_size = codec_buf_size;
		}
		memcpy(codec_buf, buf, codec_sample_size);
		media_status_t r = AMediaCodec_queueInputBuffer(decoder->codec, (size_t)codec_buf_index, 0, codec_sample_size, decoder->timestamp_cur++, 0); // timestamp just raised by 1 for maximum realtime
		if(r != AMEDIA_OK)
		{
			CHIAKI_LOGE(decoder->log, "AMediaCodec_queueInputBuffer() failed: %d", (int)r);
		}
		buf += codec_sample_size;
		buf_size -= codec_sample_size;

	}

beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
	return r;
}

static void *android_chiaki_video_decoder_output_thread_func(void *user)
{
	AndroidChiakiVideoDecoder *decoder = user;

	while(!__atomic_load_n(&decoder->shutdown_output, __ATOMIC_ACQUIRE))
	{
		AMediaCodecBufferInfo info;
		ssize_t status = AMediaCodec_dequeueOutputBuffer(decoder->codec, &info, 10000);
		if(status >= 0)
		{
			// Drain only immediately available decoded pictures, with a strict bound.
			// Never discard compressed reference frames or wait for a future picture.
			bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			for(unsigned int i = 0; i < 3 && !eos; ++i)
			{
				AMediaCodecBufferInfo next_info;
				ssize_t next = AMediaCodec_dequeueOutputBuffer(decoder->codec, &next_info, 0);
				if(next < 0)
					break;
				eos = (next_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
				if(next_info.size == 0 || (next_info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
				{
					AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)next, false);
					continue;
				}
				AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, false);
				status = next;
				info = next_info;
			}
			bool render = info.size != 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)
				&& !__atomic_load_n(&decoder->shutdown_output, __ATOMIC_ACQUIRE);
			AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, render);
			if(eos)
				break;
		}
		else if(status != AMEDIACODEC_INFO_TRY_AGAIN_LATER && status != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
			&& status != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
		{
			CHIAKI_LOGE(decoder->log, "ULL: output decoder error %d", (int)status);
			break;
		}
	}

	CHIAKI_LOGI(decoder->log, "Video Decoder Output Thread exiting");

	return NULL;
}