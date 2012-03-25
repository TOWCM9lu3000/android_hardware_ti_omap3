/*
 * Copyright (C) 2011 Texas Instruments
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
 *
 * This module is a derived work from the original contribution of
 * the /device/samsung/tuna/audio/audio_hw.c by Simon Wilson
 *
 */

#define LOG_TAG "audio_hw_primary"
//#define LOG_NDEBUG 0
//#define LOG_NDEBUG_FUNCTION
#ifndef LOG_NDEBUG_FUNCTION
#define LOGFUNC(...) ((void)0)
#else
#define LOGFUNC(...) (LOGV(__VA_ARGS__))
#endif


#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>


/*omap3 alsa controls*/
#define MIXER_HEADSETR_AUDIO_R2                "HeadsetR Mixer AudioR2"
#define MIXER_HEADSETL_AUDIO_L2                "HeadsetL Mixer AudioL2"
#define MIXER_ANALOG_LEFT_AUXL_CAPTURE_SWITCH  "Analog Left AUXL Capture Switch"
#define MIXER_ANALOG_RIGHT_AUXL_CAPTURE_SWITCH "Analog Right AUXR Capture Switch"
#define MIXER_ANALOG_CAPTURE_VOLUME             "Analog Capture Volume"

#ifdef AM335XEVM
#define MIXER_HEADSET_PLAYBACK_VOLUME 	       "PCM Playback Volume"
#else
#define MIXER_HEADSET_PLAYBACK_VOLUME 	       "Headset Playback Volume"
#endif

/* Mixer control gain and route values */
#define MIXER_ABE_GAIN_0DB                  120

#define CARD_OMAP3_DEFAULT 0
#define PORT_OMAP3 0

/* constraint imposed by ABE for CBPr mode: all period sizes must be multiples of 24 */
#define ABE_BASE_FRAME_COUNT 24
/* number of base blocks in a short period (low latency) */
#define SHORT_PERIOD_MULTIPLIER 80  /* 40 ms */
/* number of frames per short period (low latency) */
#define SHORT_PERIOD_SIZE (ABE_BASE_FRAME_COUNT * SHORT_PERIOD_MULTIPLIER)
/* number of short periods in a long period (low power) */
#define LONG_PERIOD_MULTIPLIER 1  /* 40 ms */
/* number of frames per long period (low power) */
#define LONG_PERIOD_SIZE (SHORT_PERIOD_SIZE * LONG_PERIOD_MULTIPLIER)
/* number of periods for playback */
#define PLAYBACK_PERIOD_COUNT 4
/* number of periods for capture */
#define CAPTURE_PERIOD_COUNT 2
/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 5000

#define RESAMPLER_BUFFER_FRAMES (SHORT_PERIOD_SIZE * 2)
#define RESAMPLER_BUFFER_SIZE (4 * RESAMPLER_BUFFER_FRAMES)

#define DEFAULT_OUT_SAMPLING_RATE 44100

/* sampling rate when using MM low power port */
#define MM_LOW_POWER_SAMPLING_RATE 44100
/* sampling rate when using MM full power port */
#define MM_FULL_POWER_SAMPLING_RATE 48000
/* sampling rate when using VX port for narrow band */
#define VX_NB_SAMPLING_RATE 8000
/* sampling rate when using VX port for wide band */
#define VX_WB_SAMPLING_RATE 16000

/* conversions from dB to ABE and codec gains */
#define DB_TO_ABE_GAIN(x) ((x) + MIXER_ABE_GAIN_0DB)
#define DB_TO_CAPTURE_PREAMPLIFIER_VOLUME(x) (((x) + 6) / 6)
#define DB_TO_CAPTURE_VOLUME(x) (((x) - 6) / 6)
#define DB_TO_HEADSET_VOLUME(x) (((x) + 30) / 2)
#define DB_TO_SPEAKER_VOLUME(x) (((x) + 52) / 2)
#define DB_TO_EARPIECE_VOLUME(x) (((x) + 24) / 2)


#define HEADSET_VOLUME                        0
#define HEADPHONE_VOLUME                      0 /* allow louder output for headphones */

/* product-specific defines */
#define PRODUCT_DEVICE_PROPERTY "ro.product.device"
#define PRODUCT_DEVICE_TYPE    "omap3evm"


struct pcm_config pcm_config_mm = {
    .channels = 2,
    .rate = DEFAULT_OUT_SAMPLING_RATE,
    .period_size = LONG_PERIOD_SIZE,
#ifdef AM335XEVM
    .period_count = 16,
#else
    .period_count = PLAYBACK_PERIOD_COUNT,
#endif
    .format = PCM_FORMAT_S16_LE,
};


struct pcm_config pcm_config_mm_ul = {
    .channels = 2,
    .rate = 8000,
    .period_size = 1024,
#ifdef AM335XEVM
    .period_count = 16,
#else
    .period_count = 16,
#endif
    .format = PCM_FORMAT_S16_LE,
};


#define MIN(x, y) ((x) > (y) ? (y) : (x))

struct route_setting
{
    char *ctl_name;
    int intval;
    char *strval;
};

struct route_setting defaults[] = {
    /* general */

    {
        .ctl_name = MIXER_HEADSETR_AUDIO_R2,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_HEADSETL_AUDIO_L2,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_HEADSET_PLAYBACK_VOLUME,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_ANALOG_LEFT_AUXL_CAPTURE_SWITCH,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_ANALOG_RIGHT_AUXL_CAPTURE_SWITCH,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_ANALOG_CAPTURE_VOLUME,
        .intval = 0,
    },
    {
        .ctl_name = NULL,
    },

};

struct route_setting speaker[] = {

    {
        .ctl_name = MIXER_HEADSETR_AUDIO_R2,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_HEADSETL_AUDIO_L2,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_HEADSET_PLAYBACK_VOLUME,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_ANALOG_LEFT_AUXL_CAPTURE_SWITCH,
        .intval = 1,
    },
    {
        .ctl_name = NULL,
    },

};

struct route_setting builtin_mic[] = {

    {
        .ctl_name = MIXER_ANALOG_LEFT_AUXL_CAPTURE_SWITCH,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_ANALOG_RIGHT_AUXL_CAPTURE_SWITCH,
        .intval = 1,
    },
    {
        .ctl_name = MIXER_ANALOG_CAPTURE_VOLUME,
        .intval = 0,
    },
    {
        .ctl_name = NULL,
    },

};



struct mixer_ctls
{
    struct mixer_ctl *headset_volume;
};

struct omap3_audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct mixer *mixer;
    struct mixer_ctls mixer_ctls;
    int mode;
    int devices;
    float voice_volume;
    struct omap3_stream_in *active_input;
    struct omap3_stream_out *active_output;
    bool mic_mute;

};

struct omap3_stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    char *buffer;
    int standby;
    int write_threshold;

    struct omap3_audio_device *dev;
};

#define MAX_PREPROCESSORS 3 /* maximum one AGC + one NS + one AEC per input stream */

struct omap3_stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    int device;
    int16_t *buffer;
    size_t frames_in;
    unsigned int requested_rate;
    int standby;
    int source;

    struct omap3_audio_device *dev;
};

/**
 * NOTE: when multiple mutexes have to be acquired, always respect the following order:
 *        hw device > in stream > out stream
 */


static void select_output_device(struct omap3_audio_device *adev);
static void select_input_device(struct omap3_audio_device *adev);
static int adev_set_voice_volume(struct audio_hw_device *dev, float volume);
static int do_input_standby(struct omap3_stream_in *in);
static int do_output_standby(struct omap3_stream_out *out);

static int get_boardtype(struct omap3_audio_device *adev)
{
    char board[PROPERTY_VALUE_MAX];
    int status = 0;

    LOGFUNC("%s(%p)", __FUNCTION__, adev);

    return 0;
}
/* The enable flag when 0 makes the assumption that enums are disabled by
 * "Off" and integers/booleans by 0 */

static int set_route_by_array(struct mixer *mixer, struct route_setting *route,
                              int enable)
{
    struct mixer_ctl *ctl;
    unsigned int i, j;

    LOGFUNC("%s(%p, %p, %d)", __FUNCTION__, mixer, route, enable);

    /* Go through the route array and set each value */
    i = 0;
    while (route[i].ctl_name) {
        ctl = mixer_get_ctl_by_name(mixer, route[i].ctl_name);
        if (!ctl)
            return -EINVAL;
	LOGE("Route MIXER CTRL name: %s", route[i].ctl_name);
        if (route[i].strval) {
            if (enable)
                mixer_ctl_set_enum_by_string(ctl, route[i].strval);
            else
                mixer_ctl_set_enum_by_string(ctl, "Off");
        } else {
            /* This ensures multiple (i.e. stereo) values are set jointly */
            for (j = 0; j < mixer_ctl_get_num_values(ctl); j++) {
                if (enable)
                    mixer_ctl_set_value(ctl, j, route[i].intval);
                else
                    mixer_ctl_set_value(ctl, j, 0);
            }
        }
        i++;
    }

    return 0;
}


static void set_eq_filter(struct omap3_audio_device *adev)
{
    LOGFUNC("%s(%p)", __FUNCTION__, adev);
}


static void set_output_volumes(struct omap3_audio_device *adev)
{
    unsigned int channel;
    int headset_volume;

    headset_volume = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADSET ?
                                                        HEADSET_VOLUME :
                                                        HEADPHONE_VOLUME;

    for (channel = 0; channel < 2; channel++) {
        mixer_ctl_set_value(adev->mixer_ctls.headset_volume, channel,
            DB_TO_HEADSET_VOLUME(headset_volume));
    }
}

static void force_all_standby(struct omap3_audio_device *adev)
{
    struct omap3_stream_in *in;
    struct omap3_stream_out *out;

    LOGFUNC("%s(%p)", __FUNCTION__, adev);

    if (adev->active_output) {
        out = adev->active_output;
        pthread_mutex_lock(&out->lock);
        do_output_standby(out);
        pthread_mutex_unlock(&out->lock);
    }
    if (adev->active_input) {
        in = adev->active_input;
        pthread_mutex_lock(&in->lock);
        do_input_standby(in);
        pthread_mutex_unlock(&in->lock);
    }
}

static void select_output_device(struct omap3_audio_device *adev)
{
    int headset_on;
    int headphone_on;
    int speaker_on;
    int earpiece_on;
    int bt_on;
    int dl1_on;
    int sidetone_capture_on = 0;
    unsigned int channel, voice_ul_volume[2];

    LOGFUNC("%s(%p)", __FUNCTION__, adev);

    speaker_on = adev->devices & AUDIO_DEVICE_OUT_SPEAKER;

    set_route_by_array(adev->mixer, speaker, speaker_on);

}

static void select_input_device(struct omap3_audio_device *adev)
{
    int main_mic_on = 0;

    LOGFUNC("%s(%p)", __FUNCTION__, adev);

    main_mic_on = adev->devices & AUDIO_DEVICE_IN_BUILTIN_MIC;
    set_route_by_array(adev->mixer, builtin_mic , main_mic_on);

}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct omap3_stream_out *out)
{
    struct omap3_audio_device *adev = out->dev;
    unsigned int card = CARD_OMAP3_DEFAULT;
    unsigned int port = PORT_OMAP3;

    LOGFUNC("%s(%p)", __FUNCTION__, adev);

    adev->active_output = out;

    select_output_device(adev);

    out->write_threshold = PLAYBACK_PERIOD_COUNT * LONG_PERIOD_SIZE;
    out->config.start_threshold = SHORT_PERIOD_SIZE * 2;
    out->config.avail_min = LONG_PERIOD_SIZE,


    out->pcm = pcm_open(card, port, PCM_OUT | PCM_MMAP, &out->config);

    if (!pcm_is_ready(out->pcm)) {
        LOGE("cannot open pcm_out driver: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        adev->active_output = NULL;
        return -ENOMEM;
    }

    return 0;
}

static int check_input_parameters(uint32_t sample_rate, int format, int channel_count)
{
    LOGFUNC("%s(%d, %d, %d)", __FUNCTION__, sample_rate, format, channel_count);

    if (format != AUDIO_FORMAT_PCM_16_BIT) {
        return -EINVAL;
    }

    if ((channel_count < 1) || (channel_count > 2)) {
        return -EINVAL;
    }

    switch(sample_rate) {
    case 8000:
    case 11025:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate, int format, int channel_count)
{
    size_t size;
    size_t device_rate;

    LOGFUNC("%s(%d, %d, %d)", __FUNCTION__, sample_rate, format, channel_count);

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size = (pcm_config_mm_ul.period_size * sample_rate) / pcm_config_mm_ul.rate;
    size = ((size + 15) / 16) * 16;

    LOGFUNC("%s:size : %d ", __FUNCTION__, size * channel_count * sizeof(short));
    return size * channel_count * sizeof(short);
}


static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return DEFAULT_OUT_SAMPLING_RATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, rate);

    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct omap3_stream_out *out = (struct omap3_stream_out *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size_t size = (SHORT_PERIOD_SIZE * DEFAULT_OUT_SAMPLING_RATE) / out->config.rate;
    size = ((size + 15) / 16) * 16;
    return size * audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return AUDIO_CHANNEL_OUT_STEREO;
}

static int out_get_format(const struct audio_stream *stream)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, int format)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return 0;
}

/* must be called with hw device and output stream mutexes locked */
static int do_output_standby(struct omap3_stream_out *out)
{
    struct omap3_audio_device *adev = out->dev;

    LOGFUNC("%s(%p)", __FUNCTION__, out);

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_output = 0;
    }
        out->standby = 1;
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct omap3_stream_out *out = (struct omap3_stream_out *)stream;
    int status;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, fd);

    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct omap3_stream_out *out = (struct omap3_stream_out *)stream;

    LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, kvpairs);

    return 0;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, keys);

    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct omap3_stream_out *out = (struct omap3_stream_out *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);
    return (SHORT_PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * 1000) / out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    LOGFUNC("%s(%p, %f, %f)", __FUNCTION__, stream, left, right);

    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret;
    struct omap3_stream_out *out = (struct omap3_stream_out *)stream;
    struct omap3_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_frame_size(&out->stream.common);
    size_t in_frames = bytes / frame_size;
    size_t out_frames = RESAMPLER_BUFFER_SIZE / frame_size;
    struct omap3_stream_in *in;
    int kernel_frames;
    void *buf;

    LOGFUNC("%s(%p, %p, %d)", __FUNCTION__, stream, buffer, bytes);

do_over:

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (out->config.rate != DEFAULT_OUT_SAMPLING_RATE) {
	LOGFUNC(" %d Sampleing rate not supported\n", out->config.rate)	;
    }

    out_frames = in_frames;
    buf = (void *)buffer;


    /* do not allow more than out->write_threshold frames in kernel pcm driver buffer */
    do {
        struct timespec time_stamp;

        if (pcm_get_htimestamp(out->pcm, (unsigned int *)&kernel_frames, &time_stamp) < 0)
            break;
        kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;
        if (kernel_frames > out->write_threshold) {
            unsigned long time = (unsigned long)
                    (((int64_t)(kernel_frames - out->write_threshold) * 1000000) /
                            MM_FULL_POWER_SAMPLING_RATE);
            if (time < MIN_WRITE_SLEEP_US)
                time = MIN_WRITE_SLEEP_US;
            usleep(time);
        }
    } while (kernel_frames > out->write_threshold);

    ret = pcm_mmap_write(out->pcm, (void *)buf, out_frames * frame_size);

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret > 0) {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    if (ret == -EPIPE) {
	    /* Recover from an underrun */
	    LOGE("XRUN detected");
	    pthread_mutex_lock(&adev->lock);
	    pthread_mutex_lock(&out->lock);
	    do_output_standby(out);
	    pthread_mutex_unlock(&out->lock);
	    pthread_mutex_unlock(&adev->lock);
	    goto do_over;
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    LOGFUNC("%s(%p, %p)", __FUNCTION__, stream, dsp_frames);

    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    LOGFUNC("%s(%p, %p)", __FUNCTION__, stream, effect);

    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    LOGFUNC("%s(%p, %p)", __FUNCTION__, stream, effect);

    return 0;
}

/** audio_stream_in implementation **/

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct omap3_stream_in *in)
{
    int ret = 0;
    unsigned int card = CARD_OMAP3_DEFAULT;
    unsigned int device = PORT_OMAP3;
    struct omap3_audio_device *adev = in->dev;

    LOGFUNC("%s(%p)", __FUNCTION__, in);

    adev->active_input = in;

    LOGFUNC("%s: opening pcm device\n", __FUNCTION__);

    in->pcm = pcm_open(card, device, PCM_IN, &in->config);


    if (!pcm_is_ready(in->pcm)) {
        LOGE("cannot open pcm_in driver: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        adev->active_input = NULL;
        return -ENOMEM;
    }

    LOGFUNC("%s: opened pcm device:  %s\n", __FUNCTION__, pcm_get_error (in->pcm));
    return 0;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);
    LOGFUNC("%s(%d)", __FUNCTION__, in->requested_rate);

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, rate);

    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 in->config.channels);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);
    LOGFUNC("%s(%d)", __FUNCTION__, in->config.channels);

    if (in->config.channels == 1) {
        return AUDIO_CHANNEL_IN_MONO;
    } else {
        return AUDIO_CHANNEL_IN_STEREO;
    }
}

static int in_get_format(const struct audio_stream *stream)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, int format)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, format);

    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int do_input_standby(struct omap3_stream_in *in)
{
    struct omap3_audio_device *adev = in->dev;

    LOGFUNC("%s(%p)", __FUNCTION__, in);

    if (!in->standby ) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_input = 0;
	LOGFUNC("%s: close pcm device", __FUNCTION__);
        in->standby = 1;
    }
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;
    int status = 0;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    status = do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, fd);

    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;

    LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, kvpairs);

    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, keys);

    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    LOGFUNC("%s(%p, %f)", __FUNCTION__, stream, gain);

    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;
    struct omap3_audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_frame_size(&stream->common);

    LOGFUNC("%s(%p, %p, %d)", __FUNCTION__, stream, buffer, bytes);
    LOGE("%s(%p, %p, %d)", __FUNCTION__, stream, buffer, bytes);

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the input stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret == 0)
            in->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

        ret = pcm_read(in->pcm, buffer, bytes);

    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
    {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               in_get_sample_rate(&stream->common));

    memset(buffer, 0, bytes);
    }
    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, stream, effect);
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;
    return 0;
}


static int adev_open_output_stream(struct audio_hw_device *dev,
                                   uint32_t devices, int *format,
                                   uint32_t *channels, uint32_t *sample_rate,
                                   struct audio_stream_out **stream_out)
{
    struct omap3_audio_device *ladev = (struct omap3_audio_device *)dev;
    struct omap3_stream_out *out;
    int ret;

    LOGFUNC("%s(%p, 0x%04x,%d, 0x%04x, %d, %p)", __FUNCTION__, dev, devices,
                        *format, *channels, *sample_rate, stream_out);

    out = (struct omap3_stream_out *)calloc(1, sizeof(struct omap3_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;

    out->config = pcm_config_mm;

    out->dev = ladev;
    out->standby = 1;

    /* FIXME: when we support multiple output devices, we will want to
     * do the following:
     * adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
     * adev->devices |= out->device;
     * select_output_device(adev);
     * This is because out_set_parameters() with a route is not
     * guaranteed to be called after an output stream is opened. */

    *format = out_get_format(&out->stream.common);
    *channels = out_get_channels(&out->stream.common);
    *sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct omap3_stream_out *out = (struct omap3_stream_out *)stream;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, dev, stream);

    out_standby(&stream->common);
    if (out->buffer)
        free(out->buffer);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct omap3_audio_device *adev = (struct omap3_audio_device *)dev;

    LOGFUNC("%s(%p, %s)", __FUNCTION__, dev, kvpairs);

    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    LOGFUNC("%s(%p, %s)", __FUNCTION__, dev, keys);

    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    LOGFUNC("%s(%p)", __FUNCTION__, dev);
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct omap3_audio_device *adev = (struct omap3_audio_device *)dev;

    LOGFUNC("%s(%p, %f)", __FUNCTION__, dev, volume);
    adev->voice_volume = volume;
    return 0;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    LOGFUNC("%s(%p, %f)", __FUNCTION__, dev, volume);

    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, int mode)
{
    struct omap3_audio_device *adev = (struct omap3_audio_device *)dev;

    LOGFUNC("%s(%p, %d)", __FUNCTION__, dev, mode);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct omap3_audio_device *adev = (struct omap3_audio_device *)dev;

    LOGFUNC("%s(%p, %d)", __FUNCTION__, dev, state);

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct omap3_audio_device *adev = (struct omap3_audio_device *)dev;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, dev, state);

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         uint32_t sample_rate, int format,
                                         int channel_count)
{
    size_t size;

    LOGFUNC("%s(%p, %d, %d, %d)", __FUNCTION__, dev, sample_rate,
                                format, channel_count);

    if (check_input_parameters(sample_rate, format, channel_count) != 0) {
        return 0;
    }

    return get_input_buffer_size(sample_rate, format, channel_count);
}

static int adev_open_input_stream(struct audio_hw_device *dev, uint32_t devices,
                                  int *format, uint32_t *channel_mask,
                                  uint32_t *sample_rate,
                                  audio_in_acoustics_t acoustics,
                                  struct audio_stream_in **stream_in)
{
    struct omap3_audio_device *ladev = (struct omap3_audio_device *)dev;
    struct omap3_stream_in *in;
    int ret;
//    int channel_count = popcount(*channel_mask);
    int channel_count = 2;
	*channel_mask = 0x3;
    /*audioflinger expects return variable to be NULL incase of failure */
    *stream_in = NULL;
    LOGFUNC("%s(%p, 0x%04x, %d, 0x%04x, %d, 0x%04x, %p)", __FUNCTION__, dev,
        devices, *format, *channel_mask, *sample_rate, acoustics, stream_in);

    if (check_input_parameters(*sample_rate, *format, channel_count) != 0)
    {
	    LOGFUNC("%s : checkparameter failed\n", __FUNCTION__);

	return -EINVAL;
    }
    in = (struct omap3_stream_in *)calloc(1, sizeof(struct omap3_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->requested_rate = *sample_rate;
    memcpy(&in->config, &pcm_config_mm_ul, sizeof(pcm_config_mm_ul));
//    in->config.channels = channel_count;

    in->buffer = malloc(2 * in->config.period_size * audio_stream_frame_size(&in->stream.common));
    if (!in->buffer) {
        ret = -ENOMEM;
        goto err;
    }

    if (in->requested_rate != in->config.rate)
    {
	    LOGFUNC("%s: %d sampling rate not supported \n", __FUNCTION__, in->requested_rate);

    }
    in->dev = ladev;
    in->standby = 1;
    in->device = devices;

    *stream_in = &in->stream;
    LOGFUNC("%s : open input stream successful : channels : %d\n", __FUNCTION__, popcount(*channel_mask));
    return 0;

err:

    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct omap3_stream_in *in = (struct omap3_stream_in *)stream;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, dev, stream);

    in_standby(&stream->common);
    free(stream);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, device, fd);

    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct omap3_audio_device *adev = (struct omap3_audio_device *)device;

    LOGFUNC("%s(%p)", __FUNCTION__, device);


    mixer_close(adev->mixer);
    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *dev)
{
    LOGFUNC("%s(%p)", __FUNCTION__, dev);

    return (/* OUT */
            AUDIO_DEVICE_OUT_EARPIECE |
            AUDIO_DEVICE_OUT_SPEAKER |
            AUDIO_DEVICE_OUT_WIRED_HEADSET |
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
            AUDIO_DEVICE_OUT_AUX_DIGITAL |
            AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET |
            AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET |
            AUDIO_DEVICE_OUT_ALL_SCO |
            AUDIO_DEVICE_OUT_DEFAULT |
            /* IN */
            AUDIO_DEVICE_IN_COMMUNICATION |
            AUDIO_DEVICE_IN_AMBIENT |
            AUDIO_DEVICE_IN_BUILTIN_MIC |
            AUDIO_DEVICE_IN_WIRED_HEADSET |
            AUDIO_DEVICE_IN_AUX_DIGITAL |
            AUDIO_DEVICE_IN_BACK_MIC |
            AUDIO_DEVICE_IN_ALL_SCO |
            AUDIO_DEVICE_IN_DEFAULT);
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct omap3_audio_device *adev;
    int ret;
    pthread_mutexattr_t mta;

    LOGFUNC("%s(%p, %s, %p)", __FUNCTION__, module, name, device);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
    {
        LOGE("ERROR .....................");
        return -EINVAL;
    }

    adev = calloc(1, sizeof(struct omap3_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = 0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.get_supported_devices = adev_get_supported_devices;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->mixer = mixer_open(0);
    if (!adev->mixer) {
        free(adev);
        LOGE("Unable to open the mixer, aborting.");
        return -EINVAL;
    }

    adev->mixer_ctls.headset_volume = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_HEADSET_PLAYBACK_VOLUME);

    pthread_mutexattr_init(&mta);
    pthread_mutexattr_settype(&mta, PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(&adev->lock, &mta);
    pthread_mutexattr_destroy(&mta);

    /* Set the default route before the PCM stream is opened */
    pthread_mutex_lock(&adev->lock);

    set_route_by_array(adev->mixer, defaults, 1);
    adev->mode = AUDIO_MODE_NORMAL;
    adev->devices = AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_IN_BUILTIN_MIC;
    select_output_device(adev);

    adev->voice_volume = 1.0f;
    if(get_boardtype(adev)) {
        pthread_mutex_unlock(&adev->lock);
        mixer_close(adev->mixer);
        free(adev);
        LOGE("Unsupported boardtype, aborting.");
        return -EINVAL;
    }

    pthread_mutex_unlock(&adev->lock);
    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Omap3 audio HW HAL",
        .author = "Texas Instruments",
        .methods = &hal_module_methods,
    },
};
