/**
 * @file main.c
 * @brief ffx command-line entry point.
 */

#include <solidc/flags.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffx.h"

/* =========================================================================
 * Shared global state threaded through pre_invoke
 * ========================================================================= */

typedef struct {
    bool verbose;
    bool dry_run;
    bool no_overwrite;
    const char* hw_accel_str;
    FfxCommonOpts common;
} AppCtx;

static FfxHwAccel parse_hw_accel(const char* s) {
    if (s == NULL) return FFX_HW_NONE;
    if (strcmp(s, "nvenc") == 0) return FFX_HW_NVENC;
    if (strcmp(s, "qsv") == 0) return FFX_HW_QSV;
    if (strcmp(s, "vt") == 0) return FFX_HW_VIDEOTOOLBOX;
    if (strcmp(s, "vaapi") == 0) return FFX_HW_VAAPI;
    if (strcmp(s, "amf") == 0) return FFX_HW_AMF;
    fprintf(stderr, "ffx: unknown hw-accel '%s'; ignoring\n", s);
    return FFX_HW_NONE;
}

static void pre_invoke(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    ctx->common = ffx_common_opts_default();
    if (ctx->verbose) { ctx->common.log_level = FFX_LOG_INFO; }
    ctx->common.dry_run = ctx->dry_run;
    ctx->common.overwrite = !ctx->no_overwrite;
    ctx->common.hw_accel = parse_hw_accel(ctx->hw_accel_str);
}

/* =========================================================================
 * Command option storage
 * ========================================================================= */

static struct {
    const char* input;
    const char* output;
    const char* video_codec;
    const char* preset;
    const char* audio_codec;
    const char* resolution;
    const char* format;
    int32_t crf;
    int32_t vbitrate;
    int32_t abitrate;
    int32_t sample_rate;
    int32_t channels;
    float fps;
    bool no_video;
    bool no_audio;
    bool stream_copy;
} s_convert = {.crf = -1};

static struct {
    const char* input;
    const char* output;
    const char* start;
    const char* end;
    bool duration;
    bool accurate;
} s_trim;

static struct {
    const char* input;
    const char* output;
    const char* codec;
    const char* preset;
    float size_mb;
    int32_t abitrate;
    bool copy_audio;
} s_compress = {.abitrate = 128};

static struct {
    const char* input;
    const char* output;
    const char* secondary;
    const char* op_str;
    const char* acodec;
    int32_t abitrate;
} s_audio;

static struct {
    const char* input;
    const char* output;
    const char* ts;
    const char* strat;
    int32_t width;
    int32_t height;
    int32_t quality;
} s_thumbnail = {.quality = 2};

static struct {
    const char* input;
    const char* output;
    const char* start;
    const char* duration;
    const char* stats;
    int32_t fps;
    int32_t width;
    bool loop;
} s_gif = {.fps = 15, .width = 480, .loop = true};

static struct {
    const char* input;
    bool raw_json;
} s_probe;

static struct {
    const char* output;
    const char* vcodec;
    const char* preset;
    const char* acodec;
    bool reencode;
} s_concat;

static struct {
    const char* input;
    const char* watermark;
    const char* output;
    const char* position;
    const char* vcodec;
    const char* preset;
    int32_t margin;
    int32_t scale_w;
    int32_t crf;
    float opacity;
    bool copy_audio;
} s_watermark = {.margin = 10, .crf = 18, .opacity = 1.0f, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    const char* vcodec;
    const char* preset;
    const char* acodec;
    float factor;
    int32_t crf;
    bool no_audio;
} s_speed = {.crf = 18};

static struct {
    const char* input;
    const char* output;
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    bool copy_audio;
    const char* video_codec;
    const char* preset;
    int32_t crf;
} s_crop = {.x = -1, .y = -1, .crf = 18, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    int32_t width;
    int32_t height;
    const char* mode_str;
    const char* pad_color;
    bool copy_audio;
    const char* video_codec;
    const char* preset;
    int32_t crf;
} s_scale = {.crf = 18, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    const char* op_str;
    bool copy_audio;
    const char* video_codec;
    const char* preset;
    int32_t crf;
} s_rotate = {.crf = 18, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    bool fade_in;
    float fade_in_seconds;
    bool fade_out;
    float fade_out_seconds;
    float total_duration;
    bool video;
    bool audio;
    const char* video_codec;
    const char* preset;
    const char* audio_codec;
    int32_t crf;
} s_fade = {.fade_in_seconds = 1.0f, .fade_out_seconds = 1.0f, .video = true, .audio = true, .crf = 18};

static struct {
    const char* input;
    const char* output;
    const char* mode_str;
    bool copy_audio;
    const char* video_codec;
    const char* preset;
    int32_t crf;
} s_deinterlace = {.crf = 18, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    const char* mode_str;
    float strength;
    bool copy_audio;
    const char* video_codec;
    const char* preset;
    int32_t crf;
} s_denoise = {.crf = 18, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    float amount;
    bool copy_audio;
    const char* video_codec;
    const char* preset;
    int32_t crf;
} s_sharpen = {.amount = 1.0f, .crf = 18, .copy_audio = true};

static struct {
    const char* input;
    const char* output;
    const char* video_filter;
    const char* audio_filter;
    const char* filter_complex;
    const char* complex_maps_str;
    const char* video_codec;
    const char* preset;
    const char* audio_codec;
    int32_t crf;
} s_filter = {.crf = 18};

/* =========================================================================
 * Subcommands invocation callbacks
 * ========================================================================= */

static void cmd_convert(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    FfxConvertOpts opts = {
        .input = s_convert.input,
        .output = s_convert.output,
        .video_codec = s_convert.video_codec,
        .video_preset = s_convert.preset,
        .crf = (int)s_convert.crf,
        .video_bitrate_kbps = (int)s_convert.vbitrate,
        .resolution = s_convert.resolution,
        .fps = s_convert.fps,
        .pix_fmt = FFX_PIX_FMT_SAME,
        .audio_codec = s_convert.audio_codec,
        .audio_bitrate_kbps = (int)s_convert.abitrate,
        .audio_sample_rate = (int)s_convert.sample_rate,
        .audio_channels = (int)s_convert.channels,
        .format = s_convert.format,
        .no_video = s_convert.no_video,
        .no_audio = s_convert.no_audio,
        .stream_copy = s_convert.stream_copy,
        .common = ctx->common,
    };
    if (opts.input == NULL || opts.output == NULL) {
        fprintf(stderr, "ffx convert: --input and --output are required\n");
        return;
    }
    FfxStatus st = ffx_convert(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx convert: %s\n", ffx_status_str(st)); }
}

static void cmd_trim(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_trim.input == NULL || s_trim.output == NULL) {
        fprintf(stderr, "ffx trim: --input and --output are required\n");
        return;
    }
    FfxTrimOpts opts = {
        .input = s_trim.input,
        .output = s_trim.output,
        .start = s_trim.start,
        .end = s_trim.end,
        .end_is_duration = s_trim.duration,
        .accurate_seek = s_trim.accurate,
        .common = ctx->common,
    };
    FfxStatus st = ffx_trim(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx trim: %s\n", ffx_status_str(st)); }
}

static void cmd_compress(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_compress.input == NULL || s_compress.output == NULL || s_compress.size_mb <= 0.0f) {
        fprintf(stderr, "ffx compress: --input, --output, and --size-mb are required\n");
        return;
    }
    FfxCompressOpts opts = {
        .input = s_compress.input,
        .output = s_compress.output,
        .target_size_mb = s_compress.size_mb,
        .codec_family = s_compress.codec,
        .video_preset = s_compress.preset,
        .copy_audio = s_compress.copy_audio,
        .audio_bitrate_kbps = (int)s_compress.abitrate,
        .common = ctx->common,
    };
    FfxStatus st = ffx_compress(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx compress: %s\n", ffx_status_str(st)); }
}

static void cmd_audio(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_audio.input == NULL || s_audio.output == NULL || s_audio.op_str == NULL) {
        fprintf(stderr, "ffx audio: --input, --output, and --op are required\n");
        return;
    }
    FfxAudioOp op;
    if (strcmp(s_audio.op_str, "extract") == 0)
        op = FFX_AUDIO_EXTRACT;
    else if (strcmp(s_audio.op_str, "strip") == 0)
        op = FFX_AUDIO_STRIP;
    else if (strcmp(s_audio.op_str, "replace") == 0)
        op = FFX_AUDIO_REPLACE;
    else if (strcmp(s_audio.op_str, "normalize") == 0)
        op = FFX_AUDIO_NORMALIZE;
    else if (strcmp(s_audio.op_str, "mix") == 0)
        op = FFX_AUDIO_MIX;
    else {
        fprintf(stderr, "ffx audio: unknown op '%s'\n", s_audio.op_str);
        return;
    }
    FfxAudioOpts opts = {
        .op = op,
        .input = s_audio.input,
        .output = s_audio.output,
        .secondary_input = s_audio.secondary,
        .audio_codec = s_audio.acodec,
        .audio_bitrate_kbps = (int)s_audio.abitrate,
        .loudnorm_i = -23.0f,
        .loudnorm_tp = -2.0f,
        .loudnorm_lra = 7.0f,
        .mix_weight_primary = 1.0f,
        .mix_weight_secondary = 1.0f,
        .common = ctx->common,
    };
    FfxStatus st = ffx_audio(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx audio: %s\n", ffx_status_str(st)); }
}

static void cmd_thumbnail(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_thumbnail.input == NULL || s_thumbnail.output == NULL) {
        fprintf(stderr, "ffx thumbnail: --input and --output are required\n");
        return;
    }
    FfxThumbStrategy strategy = FFX_THUMB_AT_TIME;
    if (s_thumbnail.strat != NULL) {
        if (strcmp(s_thumbnail.strat, "best") == 0)
            strategy = FFX_THUMB_BEST;
        else if (strcmp(s_thumbnail.strat, "middle") == 0)
            strategy = FFX_THUMB_MIDDLE;
        else if (strcmp(s_thumbnail.strat, "time") == 0)
            strategy = FFX_THUMB_AT_TIME;
        else {
            fprintf(stderr, "ffx thumbnail: unknown strategy '%s'\n", s_thumbnail.strat);
            return;
        }
    }
    FfxThumbnailOpts opts = {
        .input = s_thumbnail.input,
        .output = s_thumbnail.output,
        .strategy = strategy,
        .timestamp = s_thumbnail.ts,
        .width = (int)s_thumbnail.width,
        .height = (int)s_thumbnail.height,
        .quality = (int)s_thumbnail.quality,
        .common = ctx->common,
    };
    FfxStatus st = ffx_thumbnail(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx thumbnail: %s\n", ffx_status_str(st)); }
}

static void cmd_gif(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_gif.input == NULL || s_gif.output == NULL) {
        fprintf(stderr, "ffx gif: --input and --output are required\n");
        return;
    }
    FfxGifOpts opts = {
        .input = s_gif.input,
        .output = s_gif.output,
        .start = s_gif.start,
        .duration = s_gif.duration,
        .fps = (int)s_gif.fps,
        .width = (int)s_gif.width,
        .stats_mode = s_gif.stats,
        .loop = s_gif.loop,
        .common = ctx->common,
    };
    FfxStatus st = ffx_gif(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx gif: %s\n", ffx_status_str(st)); }
}

static void cmd_probe(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_probe.input == NULL) {
        fprintf(stderr, "ffx probe: --input is required\n");
        return;
    }
    FfxProbeOpts opts = {
        .input = s_probe.input,
        .print_json = s_probe.raw_json,
        .common = ctx->common,
    };
    FfxProbeResult result = {0};
    FfxStatus st = ffx_probe(&opts, &result);
    if (st != FFX_OK) {
        fprintf(stderr, "ffx probe: %s\n", ffx_status_str(st));
        return;
    }
    if (!s_probe.raw_json) { ffx_probe_print(&result); }
}

#define CONCAT_MAX_INPUTS 64

static void cmd_concat(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_concat.output == NULL) {
        fprintf(stderr, "ffx concat: --output is required\n");
        return;
    }
    int pos_count = flag_positional_count(NULL);
    if (pos_count < 2) {
        fprintf(stderr, "ffx concat: at least two input files required as positional arguments\n");
        return;
    }
    const char* inputs[CONCAT_MAX_INPUTS];
    int n = (pos_count < CONCAT_MAX_INPUTS) ? pos_count : CONCAT_MAX_INPUTS;
    for (int i = 0; i < n; ++i) {
        inputs[i] = flag_positional_at(NULL, i);
    }
    FfxConcatOpts opts = {
        .inputs = inputs,
        .input_count = (size_t)n,
        .output = s_concat.output,
        .reencode = s_concat.reencode,
        .video_codec = s_concat.vcodec,
        .video_preset = s_concat.preset,
        .audio_codec = s_concat.acodec,
        .common = ctx->common,
    };
    FfxStatus st = ffx_concat(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx concat: %s\n", ffx_status_str(st)); }
}

static void cmd_watermark(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_watermark.input == NULL || s_watermark.watermark == NULL || s_watermark.output == NULL) {
        fprintf(stderr, "ffx watermark: --input, --watermark, and --output are required\n");
        return;
    }
    FfxWatermarkPos pos = FFX_WM_BOTTOM_RIGHT;
    if (s_watermark.position != NULL) {
        if (strcmp(s_watermark.position, "tl") == 0)
            pos = FFX_WM_TOP_LEFT;
        else if (strcmp(s_watermark.position, "tr") == 0)
            pos = FFX_WM_TOP_RIGHT;
        else if (strcmp(s_watermark.position, "bl") == 0)
            pos = FFX_WM_BOTTOM_LEFT;
        else if (strcmp(s_watermark.position, "br") == 0)
            pos = FFX_WM_BOTTOM_RIGHT;
        else if (strcmp(s_watermark.position, "c") == 0)
            pos = FFX_WM_CENTER;
        else {
            fprintf(stderr, "ffx watermark: unknown position '%s'\n", s_watermark.position);
            return;
        }
    }
    FfxWatermarkOpts opts = {
        .input = s_watermark.input,
        .watermark = s_watermark.watermark,
        .output = s_watermark.output,
        .position = pos,
        .margin = (int)s_watermark.margin,
        .opacity = s_watermark.opacity,
        .scale_width = (int)s_watermark.scale_w,
        .x_expr = NULL,
        .y_expr = NULL,
        .copy_audio = s_watermark.copy_audio,
        .video_codec = s_watermark.vcodec,
        .video_preset = s_watermark.preset,
        .crf = (int)s_watermark.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_watermark(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx watermark: %s\n", ffx_status_str(st)); }
}

static void cmd_speed(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_speed.input == NULL || s_speed.output == NULL || s_speed.factor <= 0.0f) {
        fprintf(stderr, "ffx speed: --input, --output, and --factor are required\n");
        return;
    }
    FfxSpeedOpts opts = {
        .input = s_speed.input,
        .output = s_speed.output,
        .factor = s_speed.factor,
        .no_audio = s_speed.no_audio,
        .video_codec = s_speed.vcodec,
        .video_preset = s_speed.preset,
        .audio_codec = s_speed.acodec,
        .crf = (int)s_speed.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_speed(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx speed: %s\n", ffx_status_str(st)); }
}

static void cmd_crop(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_crop.input == NULL || s_crop.output == NULL) {
        fprintf(stderr, "ffx crop: --input and --output are required\n");
        return;
    }
    if (s_crop.width <= 0 || s_crop.height <= 0) {
        fprintf(stderr, "ffx crop: --width and --height must be > 0\n");
        return;
    }
    FfxCropOpts opts = {
        .input = s_crop.input,
        .output = s_crop.output,
        .width = (int)s_crop.width,
        .height = (int)s_crop.height,
        .x = (int)s_crop.x,
        .y = (int)s_crop.y,
        .copy_audio = s_crop.copy_audio,
        .video_codec = s_crop.video_codec,
        .video_preset = s_crop.preset,
        .crf = (int)s_crop.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_crop(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx crop: %s\n", ffx_status_str(st)); }
}

static void cmd_scale(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_scale.input == NULL || s_scale.output == NULL) {
        fprintf(stderr, "ffx scale: --input and --output are required\n");
        return;
    }
    FfxScaleMode mode = FFX_SCALE_FIT;
    if (s_scale.mode_str != NULL) {
        if (strcmp(s_scale.mode_str, "fit") == 0)
            mode = FFX_SCALE_FIT;
        else if (strcmp(s_scale.mode_str, "stretch") == 0)
            mode = FFX_SCALE_STRETCH;
        else if (strcmp(s_scale.mode_str, "pad") == 0)
            mode = FFX_SCALE_PAD;
        else {
            fprintf(stderr, "ffx scale: unknown mode '%s'\n", s_scale.mode_str);
            return;
        }
    }
    FfxScaleOpts opts = {
        .input = s_scale.input,
        .output = s_scale.output,
        .width = (int)s_scale.width,
        .height = (int)s_scale.height,
        .mode = mode,
        .pad_color = s_scale.pad_color,
        .copy_audio = s_scale.copy_audio,
        .video_codec = s_scale.video_codec,
        .video_preset = s_scale.preset,
        .crf = (int)s_scale.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_scale(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx scale: %s\n", ffx_status_str(st)); }
}

static void cmd_rotate(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_rotate.input == NULL || s_rotate.output == NULL || s_rotate.op_str == NULL) {
        fprintf(stderr, "ffx rotate: --input, --output, and --op are required\n");
        return;
    }
    FfxRotateOp op;
    if (strcmp(s_rotate.op_str, "90cw") == 0)
        op = FFX_ROTATE_90_CW;
    else if (strcmp(s_rotate.op_str, "90ccw") == 0)
        op = FFX_ROTATE_90_CCW;
    else if (strcmp(s_rotate.op_str, "180") == 0)
        op = FFX_ROTATE_180;
    else if (strcmp(s_rotate.op_str, "hflip") == 0)
        op = FFX_FLIP_HORIZONTAL;
    else if (strcmp(s_rotate.op_str, "vflip") == 0)
        op = FFX_FLIP_VERTICAL;
    else {
        fprintf(stderr, "ffx rotate: unknown op '%s'\n", s_rotate.op_str);
        return;
    }
    FfxRotateOpts opts = {
        .input = s_rotate.input,
        .output = s_rotate.output,
        .op = op,
        .copy_audio = s_rotate.copy_audio,
        .video_codec = s_rotate.video_codec,
        .video_preset = s_rotate.preset,
        .crf = (int)s_rotate.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_rotate(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx rotate: %s\n", ffx_status_str(st)); }
}

static void cmd_fade(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_fade.input == NULL || s_fade.output == NULL) {
        fprintf(stderr, "ffx fade: --input and --output are required\n");
        return;
    }
    FfxFadeOpts opts = {
        .input = s_fade.input,
        .output = s_fade.output,
        .fade_in = s_fade.fade_in,
        .fade_in_seconds = s_fade.fade_in_seconds,
        .fade_out = s_fade.fade_out,
        .fade_out_seconds = s_fade.fade_out_seconds,
        .total_duration_seconds = (double)s_fade.total_duration,
        .video = s_fade.video,
        .audio = s_fade.audio,
        .video_codec = s_fade.video_codec,
        .video_preset = s_fade.preset,
        .audio_codec = s_fade.audio_codec,
        .crf = (int)s_fade.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_fade(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx fade: %s\n", ffx_status_str(st)); }
}

static void cmd_deinterlace(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_deinterlace.input == NULL || s_deinterlace.output == NULL) {
        fprintf(stderr, "ffx deinterlace: --input and --output are required\n");
        return;
    }
    FfxDeinterlaceMode mode = FFX_DEINTERLACE_YADIF;
    if (s_deinterlace.mode_str != NULL) {
        if (strcmp(s_deinterlace.mode_str, "yadif") == 0)
            mode = FFX_DEINTERLACE_YADIF;
        else if (strcmp(s_deinterlace.mode_str, "bwdif") == 0)
            mode = FFX_DEINTERLACE_BWDIF;
        else {
            fprintf(stderr, "ffx deinterlace: unknown mode '%s'\n", s_deinterlace.mode_str);
            return;
        }
    }
    FfxDeinterlaceOpts opts = {
        .input = s_deinterlace.input,
        .output = s_deinterlace.output,
        .mode = mode,
        .copy_audio = s_deinterlace.copy_audio,
        .video_codec = s_deinterlace.video_codec,
        .video_preset = s_deinterlace.preset,
        .crf = (int)s_deinterlace.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_deinterlace(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx deinterlace: %s\n", ffx_status_str(st)); }
}

static void cmd_denoise(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_denoise.input == NULL || s_denoise.output == NULL) {
        fprintf(stderr, "ffx denoise: --input and --output are required\n");
        return;
    }
    FfxDenoiseMode mode = FFX_DENOISE_HQDN3D;
    if (s_denoise.mode_str != NULL) {
        if (strcmp(s_denoise.mode_str, "hqdn3d") == 0)
            mode = FFX_DENOISE_HQDN3D;
        else if (strcmp(s_denoise.mode_str, "nlmeans") == 0)
            mode = FFX_DENOISE_NLMEANS;
        else {
            fprintf(stderr, "ffx denoise: unknown mode '%s'\n", s_denoise.mode_str);
            return;
        }
    }
    FfxDenoiseOpts opts = {
        .input = s_denoise.input,
        .output = s_denoise.output,
        .mode = mode,
        .strength = s_denoise.strength,
        .copy_audio = s_denoise.copy_audio,
        .video_codec = s_denoise.video_codec,
        .video_preset = s_denoise.preset,
        .crf = (int)s_denoise.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_denoise(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx denoise: %s\n", ffx_status_str(st)); }
}

static void cmd_sharpen(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_sharpen.input == NULL || s_sharpen.output == NULL) {
        fprintf(stderr, "ffx sharpen: --input and --output are required\n");
        return;
    }
    FfxSharpenOpts opts = {
        .input = s_sharpen.input,
        .output = s_sharpen.output,
        .amount = s_sharpen.amount,
        .copy_audio = s_sharpen.copy_audio,
        .video_codec = s_sharpen.video_codec,
        .video_preset = s_sharpen.preset,
        .crf = (int)s_sharpen.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_sharpen(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx sharpen: %s\n", ffx_status_str(st)); }
}

static void cmd_filter(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    if (s_filter.input == NULL || s_filter.output == NULL) {
        fprintf(stderr, "ffx filter: --input and --output are required\n");
        return;
    }
    char* maps[FFX_MAX_EXTRA_FLAGS] = {0};
    char* map_buf = NULL;
    if (s_filter.complex_maps_str != NULL) {
        map_buf = strdup(s_filter.complex_maps_str);
        if (map_buf != NULL) {
            int idx = 0;
            char* token = strtok(map_buf, ",");
            while (token != NULL && idx < FFX_MAX_EXTRA_FLAGS - 1) {
                maps[idx++] = token;
                token = strtok(NULL, ",");
            }
        }
    }
    FfxFilterOpts opts = {
        .input = s_filter.input,
        .output = s_filter.output,
        .video_filter = s_filter.video_filter,
        .audio_filter = s_filter.audio_filter,
        .filter_complex = s_filter.filter_complex,
        .complex_maps = (s_filter.complex_maps_str != NULL) ? (const char* const*)maps : NULL,
        .video_codec = s_filter.video_codec,
        .video_preset = s_filter.preset,
        .audio_codec = s_filter.audio_codec,
        .crf = (int)s_filter.crf,
        .common = ctx->common,
    };
    FfxStatus st = ffx_filter(&opts);
    if (st != FFX_OK) { fprintf(stderr, "ffx filter: %s\n", ffx_status_str(st)); }
    free(map_buf);
}

/* =========================================================================
 * Root parser construction and subcommand registration
 * ========================================================================= */

static void register_subcommands(FlagParser* root, AppCtx* ctx) {
    (void)ctx;

    /* ---- convert -------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "convert",
                                            "Transcode a media file to a different container or codec.", cmd_convert);
        flag_req_string(p, "input", 'i', "Input file path.", &s_convert.input);
        flag_req_string(p, "output", 'o', "Output file path.", &s_convert.output);
        flag_string(p, "video-codec", 'V', "Video codec (e.g. libx264, libx265, copy).", &s_convert.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset (e.g. fast, slow).", &s_convert.preset);
        flag_int32(p, "crf", 0, "Constant Rate Factor 0-51.", &s_convert.crf);
        flag_int32(p, "video-bitrate", 0, "Video target bitrate kbps.", &s_convert.vbitrate);
        flag_string(p, "resolution", 0, "Output resolution e.g. 1920x1080.", &s_convert.resolution);
        flag_float(p, "fps", 0, "Output frame rate.", &s_convert.fps);
        flag_string(p, "audio-codec", 'A', "Audio codec.", &s_convert.audio_codec);
        flag_int32(p, "audio-bitrate", 0, "Audio bitrate kbps.", &s_convert.abitrate);
        flag_int32(p, "sample-rate", 0, "Audio sample rate Hz.", &s_convert.sample_rate);
        flag_int32(p, "channels", 0, "Audio channels.", &s_convert.channels);
        flag_string(p, "format", 'f', "Force output container format.", &s_convert.format);
        flag_bool(p, "no-video", 0, "Strip video stream.", &s_convert.no_video);
        flag_bool(p, "no-audio", 0, "Strip audio stream.", &s_convert.no_audio);
        flag_bool(p, "stream-copy", 'c', "Copy all streams.", &s_convert.stream_copy);
    }

    /* ---- trim ----------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "trim", "Cut a time range losslessly.", cmd_trim);
        flag_req_string(p, "input", 'i', "Input file.", &s_trim.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_trim.output);
        flag_string(p, "start", 's', "Start timestamp.", &s_trim.start);
        flag_string(p, "end", 'e', "End timestamp.", &s_trim.end);
        flag_bool(p, "duration", 'd', "Treat --end as duration.", &s_trim.duration);
        flag_bool(p, "accurate", 'a', "Use accurate seek.", &s_trim.accurate);
    }

    /* ---- compress ------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "compress", "Shrink a video to a target size.", cmd_compress);
        flag_req_string(p, "input", 'i', "Input file.", &s_compress.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_compress.output);
        flag_req_float(p, "size-mb", 's', "Target output size in MiB.", &s_compress.size_mb);
        flag_string(p, "codec", 'c', "Codec family: h264 h265 vp9.", &s_compress.codec);
        flag_string(p, "preset", 'p', "Encoder preset (e.g. fast, slow).", &s_compress.preset);
        flag_int32(p, "audio-bitrate", 0, "Audio bitrate budget kbps.", &s_compress.abitrate);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_compress.copy_audio);
    }

    /* ---- audio ---------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "audio", "Audio track manipulation.", cmd_audio);
        flag_req_string(p, "input", 'i', "Primary input file.", &s_audio.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_audio.output);
        flag_req_string(p, "op", 'O', "Operation: extract strip replace normalize mix.", &s_audio.op_str);
        flag_string(p, "secondary", 's', "Secondary input.", &s_audio.secondary);
        flag_string(p, "codec", 'c', "Audio output codec.", &s_audio.acodec);
        flag_int32(p, "bitrate", 'b', "Audio bitrate kbps.", &s_audio.abitrate);
    }

    /* ---- thumbnail ------------------------------------------------ */
    {
        FlagParser* p = flag_add_subcommand(root, "thumbnail", "Extract a representative frame.", cmd_thumbnail);
        flag_req_string(p, "input", 'i', "Input video file.", &s_thumbnail.input);
        flag_req_string(p, "output", 'o', "Output image.", &s_thumbnail.output);
        flag_string(p, "timestamp", 't', "Seek to this time.", &s_thumbnail.ts);
        flag_string(p, "strategy", 's', "Frame strategy: time best middle.", &s_thumbnail.strat);
        flag_int32(p, "width", 'W', "Scale width.", &s_thumbnail.width);
        flag_int32(p, "height", 'H', "Scale height.", &s_thumbnail.height);
        flag_int32(p, "quality", 'q', "JPEG quality.", &s_thumbnail.quality);
    }

    /* ---- gif ------------------------------------------------------ */
    {
        FlagParser* p = flag_add_subcommand(root, "gif", "Convert a video segment to a GIF.", cmd_gif);
        flag_req_string(p, "input", 'i', "Input video.", &s_gif.input);
        flag_req_string(p, "output", 'o', "Output GIF.", &s_gif.output);
        flag_string(p, "start", 's', "Start timestamp.", &s_gif.start);
        flag_string(p, "duration", 'd', "Duration.", &s_gif.duration);
        flag_int32(p, "fps", 0, "GIF frame rate.", &s_gif.fps);
        flag_int32(p, "width", 'w', "Width.", &s_gif.width);
        flag_string(p, "stats", 0, "Palette stats mode.", &s_gif.stats);
        flag_bool(p, "loop", 0, "Loop GIF.", &s_gif.loop);
    }

    /* ---- probe ---------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "probe", "Print media file metadata.", cmd_probe);
        flag_req_string(p, "input", 'i', "File to probe.", &s_probe.input);
        flag_bool(p, "json", 'j', "Dump raw ffprobe JSON.", &s_probe.raw_json);
    }

    /* ---- concat --------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "concat", "Losslessly concatenate same-codec files.", cmd_concat);
        flag_req_string(p, "output", 'o', "Output file.", &s_concat.output);
        flag_bool(p, "reencode", 'r', "Re-encode.", &s_concat.reencode);
        flag_string(p, "vcodec", 0, "Video codec.", &s_concat.vcodec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_concat.preset);
        flag_string(p, "acodec", 0, "Audio codec.", &s_concat.acodec);
    }

    /* ---- watermark ----------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "watermark", "Overlay an image on video.", cmd_watermark);
        flag_req_string(p, "input", 'i', "Input video file.", &s_watermark.input);
        flag_req_string(p, "watermark", 'w', "Overlay image file.", &s_watermark.watermark);
        flag_req_string(p, "output", 'o', "Output video file.", &s_watermark.output);
        flag_string(p, "position", 'p', "Position: tl tr bl br c.", &s_watermark.position);
        flag_int32(p, "margin", 0, "Edge margin.", &s_watermark.margin);
        flag_float(p, "opacity", 0, "Overlay opacity.", &s_watermark.opacity);
        flag_int32(p, "scale-width", 0, "Scale watermark width.", &s_watermark.scale_w);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_watermark.vcodec);
        flag_string(p, "preset", 0, "Encoder preset (no short flag).",
                    &s_watermark.preset); /* Conflicted short flag 'p' omitted */
        flag_int32(p, "crf", 0, "CRF.", &s_watermark.crf);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_watermark.copy_audio);
    }

    /* ---- speed ---------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "speed", "Change playback speed.", cmd_speed);
        flag_req_string(p, "input", 'i', "Input file.", &s_speed.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_speed.output);
        flag_req_float(p, "factor", 'x', "Speed multiplier.", &s_speed.factor);
        flag_string(p, "vcodec", 0, "Video codec.", &s_speed.vcodec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_speed.preset);
        flag_string(p, "acodec", 0, "Audio codec.", &s_speed.acodec);
        flag_int32(p, "crf", 0, "CRF.", &s_speed.crf);
        flag_bool(p, "no-audio", 0, "Drop audio.", &s_speed.no_audio);
    }

    /* ---- crop ----------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "crop", "Crop a video to a rectangular region.", cmd_crop);
        flag_req_string(p, "input", 'i', "Input file.", &s_crop.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_crop.output);
        flag_int32(p, "width", 'w', "Crop width.", &s_crop.width);
        flag_int32(p, "height", 'h', "Crop height.", &s_crop.height);
        flag_int32(p, "x", 'x', "X offset.", &s_crop.x);
        flag_int32(p, "y", 'y', "Y offset.", &s_crop.y);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_crop.copy_audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_crop.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_crop.preset);
        flag_int32(p, "crf", 0, "CRF.", &s_crop.crf);
    }

    /* ---- scale ---------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "scale", "Resize video with aspect-ratio control.", cmd_scale);
        flag_req_string(p, "input", 'i', "Input file.", &s_scale.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_scale.output);
        flag_int32(p, "width", 'w', "Target width.", &s_scale.width);
        flag_int32(p, "height", 'h', "Target height.", &s_scale.height);
        flag_string(p, "mode", 'm', "Scale mode: fit stretch pad.", &s_scale.mode_str);
        flag_string(p, "pad-color", 'c', "Pad color.", &s_scale.pad_color);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_scale.copy_audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_scale.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_scale.preset);
        flag_int32(p, "crf", 0, "CRF.", &s_scale.crf);
    }

    /* ---- rotate --------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "rotate", "Rotate or flip a video.", cmd_rotate);
        flag_req_string(p, "input", 'i', "Input file.", &s_rotate.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_rotate.output);
        flag_req_string(p, "op", 'r', "Operation: 90cw 90ccw 180 hflip vflip.", &s_rotate.op_str);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_rotate.copy_audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_rotate.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_rotate.preset);
        flag_int32(p, "crf", 0, "CRF.", &s_rotate.crf);
    }

    /* ---- fade ----------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "fade", "Apply fade-in and/or fade-out to video/audio.", cmd_fade);
        flag_req_string(p, "input", 'i', "Input file.", &s_fade.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_fade.output);
        flag_bool(p, "fade-in", 0, "Apply fade-in.", &s_fade.fade_in);
        flag_float(p, "fade-in-duration", 0, "Fade-in duration.", &s_fade.fade_in_seconds);
        flag_bool(p, "fade-out", 0, "Apply fade-out.", &s_fade.fade_out);
        flag_float(p, "fade-out-duration", 0, "Fade-out duration.", &s_fade.fade_out_seconds);
        flag_float(p, "duration", 0, "Total duration.", &s_fade.total_duration);
        flag_bool(p, "video", 0, "Apply fade to video.", &s_fade.video);
        flag_bool(p, "audio", 0, "Apply fade to audio.", &s_fade.audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_fade.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_fade.preset);
        flag_string(p, "acodec", 0, "Output audio codec.", &s_fade.audio_codec);
        flag_int32(p, "crf", 0, "CRF.", &s_fade.crf);
    }

    /* ---- deinterlace ---------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "deinterlace", "Deinterlace an interlaced video.", cmd_deinterlace);
        flag_req_string(p, "input", 'i', "Input file.", &s_deinterlace.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_deinterlace.output);
        flag_string(p, "mode", 'm', "Deinterlacing filter: yadif bwdif.", &s_deinterlace.mode_str);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_deinterlace.copy_audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_deinterlace.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_deinterlace.preset);
        flag_int32(p, "crf", 0, "CRF.", &s_deinterlace.crf);
    }

    /* ---- denoise -------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "denoise", "Reduce noise in video.", cmd_denoise);
        flag_req_string(p, "input", 'i', "Input file.", &s_denoise.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_denoise.output);
        flag_string(p, "mode", 'm', "Denoising filter: hqdn3d nlmeans.", &s_denoise.mode_str);
        flag_float(p, "strength", 's', "Denoising strength.", &s_denoise.strength);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_denoise.copy_audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_denoise.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_denoise.preset);
        flag_int32(p, "crf", 0, "CRF.", &s_denoise.crf);
    }

    /* ---- sharpen -------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "sharpen", "Sharpen or blur video.", cmd_sharpen);
        flag_req_string(p, "input", 'i', "Input file.", &s_sharpen.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_sharpen.output);
        flag_float(p, "amount", 'a', "Luma sharpening amount.", &s_sharpen.amount);
        flag_bool(p, "copy-audio", 0, "Stream-copy audio.", &s_sharpen.copy_audio);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_sharpen.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_sharpen.preset);
        flag_int32(p, "crf", 0, "CRF.", &s_sharpen.crf);
    }

    /* ---- filter --------------------------------------------------- */
    {
        FlagParser* p = flag_add_subcommand(root, "filter", "Apply raw filtergraphs.", cmd_filter);
        flag_req_string(p, "input", 'i', "Input file.", &s_filter.input);
        flag_req_string(p, "output", 'o', "Output file.", &s_filter.output);
        flag_string(p, "vf", 0, "Raw -vf string.", &s_filter.video_filter);
        flag_string(p, "af", 0, "Raw -af string.", &s_filter.audio_filter);
        flag_string(p, "filter-complex", 0, "Raw -filter_complex.", &s_filter.filter_complex);
        flag_string(p, "maps", 0, "Comma-separated complex output maps.", &s_filter.complex_maps_str);
        flag_string(p, "vcodec", 0, "Output video codec.", &s_filter.video_codec);
        flag_string(p, "preset", 'p', "Encoder preset.", &s_filter.preset);
        flag_string(p, "acodec", 0, "Output audio codec.", &s_filter.audio_codec);
        flag_int32(p, "crf", 0, "CRF.", &s_filter.crf);
    }
}

/* =========================================================================
 * main()
 * ========================================================================= */

int main(int argc, char* argv[]) {
    AppCtx ctx = {
        .verbose = false,
        .dry_run = false,
        .no_overwrite = false,
        .hw_accel_str = NULL,
    };

    FlagParser* root = flag_parser_new("ffx", "A wrapper around FFmpeg. Run 'ffx <subcommand> --help' for details.");
    if (root == NULL) {
        fprintf(stderr, "ffx: failed to create argument parser\n");
        return EXIT_FAILURE;
    }

    flag_parser_set_footer(root,
                           "Examples:\n"
                           "  ffx convert -i input.mov -o output.mp4 --preset fast\n"
                           "  ffx trim -i input.mp4 -o clip.mp4 --start 0:01:00 --end 0:02:30\n"
                           "  ffx compress -i big.mp4 -o small.mp4 --size-mb 50\n"
                           "  ffx gif -i video.mp4 -o out.gif --start 5 --duration 3 --width 640\n"
                           "  ffx probe -i movie.mkv\n"
                           "  ffx speed -i input.mp4 -o fast.mp4 --factor 2.0\n");

    flag_bool(root, "verbose", 'v', "Show FFmpeg output.", &ctx.verbose);
    flag_bool(root, "dry-run", 'n', "Print assembled command.", &ctx.dry_run);
    flag_bool(root, "no-overwrite", 0, "Do not overwrite existing files.", &ctx.no_overwrite);
    flag_string(root, "hw", 0, "HW accel option.", &ctx.hw_accel_str);

    flag_add_completion_cmd(root);
    flag_set_pre_invoke(root, pre_invoke);
    register_subcommands(root, &ctx);

    FlagStatus fs = flag_parse_and_invoke(root, argc, argv, &ctx);
    if (fs != FLAG_OK) {
        fprintf(stderr, "ffx: %s\n", flag_get_error(root));
        flag_print_usage(root);
        flag_parser_free(root);
        return EXIT_FAILURE;
    }

    if (flag_active_subcommand(root) == NULL) { flag_print_usage(root); }
    flag_parser_free(root);
    return EXIT_SUCCESS;
}
