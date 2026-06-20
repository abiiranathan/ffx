/**
 * @file ffx_cmds.c
 * @brief Implementations of all ffx subcommands.
 *
 * This file maps high-level structure options into concrete command line argument
 * strings executed via FFmpeg or FFprobe. It manages custom video and audio filter
 * configurations, multi-pass encoding, sidecar file lifecycle management, and
 * mathematical coordinate transformations.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffx_internal.h"

/**
 * @brief Utility translating FfxPixFmt values to native FFmpeg -pix_fmt flag strings.
 * @param fmt Pixel format enumeration.
 * @return Static string representation, or NULL if invalid or FFX_PIX_FMT_SAME.
 */
static const char* pixfmt_str(FfxPixFmt fmt) {
    switch (fmt) {
        case FFX_PIX_FMT_YUV420P:
            return "yuv420p";
        case FFX_PIX_FMT_YUV444P:
            return "yuv444p";
        case FFX_PIX_FMT_YUV422P:
            return "yuv422p";
        case FFX_PIX_FMT_NV12:
            return "nv12";
        case FFX_PIX_FMT_RGB24:
            return "rgb24";
        default:
            return NULL;
    }
}

/**
 * @brief Writes input files to a text file using FFmpeg's concat list format.
 * @details Writes lines formatted as `file '/absolute/path/to/media'` to support the
 * concat demuxer backend.
 * @param path Destination text file path (e.g., "list.txt").
 * @param inputs Array of file path strings to write.
 * @param count Number of elements in the inputs array.
 * @return true if successful, false if file creation or writes failed.
 */
static bool write_concat_list(const char* path, const char* const* inputs, size_t count) {
    FILE* fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "ffx: cannot create concat list '%s': %s\n", path, strerror(errno));
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (fprintf(fp, "file '%s'\n", inputs[i]) < 0) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);
    return true;
}

/**
 * @brief Builds a chain of 'atempo' filters to achieve speed factors outside native limits.
 * @details FFmpeg's 'atempo' filter restricts changes to factors between 0.5 and 2.0.
 * To achieve extreme speed changes (e.g., 4.0x or 0.125x), multiple atempo filters
 * are chained together sequentially (e.g., "atempo=2.0,atempo=2.0").
 * @param factor Speed modification factor (multiplier).
 * @param buf Destination character buffer for the filter string.
 * @param buflen Total size of the destination character buffer.
 * @return true if string construction succeeded, false if parameters were invalid or overflowed.
 */
static bool build_atempo_chain(double factor, char* buf, size_t buflen) {
    if (factor <= 0.0 || buf == NULL || buflen == 0) { return false; }
    const double per_stage_max = 2.0;
    const double per_stage_min = 0.5;
    size_t pos = 0;
    double remaining = factor;
    bool first = true;
    while (fabs(remaining - 1.0) > 1e-9) {
        double stage;
        if (remaining > 1.0) {
            stage = remaining > per_stage_max ? per_stage_max : remaining;
            remaining /= stage;
        } else {
            stage = remaining < per_stage_min ? per_stage_min : remaining;
            remaining /= stage;
        }
        int n = snprintf(buf + pos, buflen - pos, "%satempo=%.6g", first ? "" : ",", stage);
        if (n < 0 || (size_t)n >= buflen - pos) { return false; }
        pos += (size_t)n;
        first = false;
        if (pos > buflen - 32) { break; }
    }
    if (pos == 0) {
        int n = snprintf(buf, buflen, "atempo=1.0");
        return n > 0 && (size_t)n < buflen;
    }
    return true;
}

/**
 * @brief Implementation of `ffx_convert` (Transcoding, scaling, and file conversions).
 */
FfxStatus ffx_convert(const FfxConvertOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    ffx_argv_push2(&av, "-i", opts->input);

    if (opts->no_video) {
        ffx_argv_push(&av, "-vn");
    } else if (opts->stream_copy) {
        ffx_argv_push2(&av, "-c:v", "copy");
    } else {
        ffx_argv_push_video_codec(&av, opts->video_codec, opts->common.hw_accel);
        if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
        if (opts->crf >= 0) { ffx_argv_push_int(&av, "-crf", opts->crf); }
        if (opts->video_bitrate_kbps > 0) { ffx_argv_push_kbps(&av, "-b:v", opts->video_bitrate_kbps); }
        if (opts->resolution != NULL) { ffx_argv_push2(&av, "-s", opts->resolution); }
        if (opts->fps > 0.0f) { ffx_argv_push_float(&av, "-r", (double)opts->fps); }
        const char* pf = pixfmt_str(opts->pix_fmt);
        if (pf != NULL) { ffx_argv_push2(&av, "-pix_fmt", pf); }
    }

    if (opts->no_audio) {
        ffx_argv_push(&av, "-an");
    } else if (opts->stream_copy) {
        ffx_argv_push2(&av, "-c:a", "copy");
    } else {
        if (opts->audio_codec != NULL) { ffx_argv_push2(&av, "-c:a", opts->audio_codec); }
        if (opts->audio_bitrate_kbps > 0) { ffx_argv_push_kbps(&av, "-b:a", opts->audio_bitrate_kbps); }
        if (opts->audio_sample_rate > 0) { ffx_argv_push_int(&av, "-ar", opts->audio_sample_rate); }
        if (opts->audio_channels > 0) { ffx_argv_push_int(&av, "-ac", opts->audio_channels); }
    }
    if (opts->format != NULL) { ffx_argv_push2(&av, "-f", opts->format); }
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_trim` (Trimming segments from media).
 */
FfxStatus ffx_trim(const FfxTrimOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    if (opts->accurate_seek && opts->start != NULL) { ffx_argv_push2(&av, "-ss", opts->start); }
    ffx_argv_push2(&av, "-i", opts->input);
    if (!opts->accurate_seek && opts->start != NULL) { ffx_argv_push2(&av, "-ss", opts->start); }
    if (opts->end != NULL) {
        const char* end_flag = opts->end_is_duration ? "-t" : "-to";
        ffx_argv_push2(&av, end_flag, opts->end);
    }
    ffx_argv_push2(&av, "-c", "copy");
    ffx_argv_push2(&av, "-avoid_negative_ts", "make_zero");
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_compress` (Constrained two-pass file size reduction).
 */
FfxStatus ffx_compress(const FfxCompressOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    if (opts->target_size_mb <= 0.0f) {
        fprintf(stderr, "ffx compress: target_size_mb must be > 0\n");
        return FFX_ERR_INVALID_ARGUMENT;
    }
    double duration_seconds = 0.0;
    FfxStatus st = ffx_probe_duration(opts->input, &opts->common, &duration_seconds);
    if (st != FFX_OK) {
        fprintf(stderr, "ffx compress: could not determine duration of '%s'\n", opts->input);
        return st;
    }
    const int audio_kbps = (opts->audio_bitrate_kbps > 0) ? opts->audio_bitrate_kbps : 128;
    double total_kbps = ((double)opts->target_size_mb * 8192.0) / duration_seconds;
    double video_kbps = total_kbps - (double)audio_kbps;
    if (video_kbps <= 0.0) {
        fprintf(stderr, "ffx compress: target size too small for audio + video bitrate budget\n");
        return FFX_ERR_INVALID_ARGUMENT;
    }
    const char* codec_family = (opts->codec_family != NULL) ? opts->codec_family : "h264";
    const char* vcodec = NULL;
    if (strcmp(codec_family, "h265") == 0) {
        vcodec = "libx265";
    } else if (strcmp(codec_family, "vp9") == 0) {
        vcodec = "libvpx-vp9";
    } else {
        vcodec = "libx264";
    }
    char passlog[FFX_MAX_OPT_LEN + 32];
    if (!ffx_build_sidecar_path(opts->output, "_ffx2pass", passlog, sizeof(passlog))) { return FFX_ERR_IO; }

    /* ---- Pass 1 ---------------------------------------------------- */
    {
        FfxArgv av;
        ffx_argv_init(&av);
        st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
        if (st != FFX_OK) {
            ffx_argv_free(&av);
            return st;
        }
        ffx_argv_push2(&av, "-i", opts->input);
        ffx_argv_push2(&av, "-c:v", vcodec);
        if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
        ffx_argv_push_kbps(&av, "-b:v", (int)(video_kbps + 0.5));
        ffx_argv_push2(&av, "-pass", "1");
        ffx_argv_push2(&av, "-passlogfile", passlog);
        ffx_argv_push(&av, "-an");
        ffx_argv_push2(&av, "-f", "null");
#ifdef _WIN32
        ffx_argv_push(&av, "NUL");
#else
        ffx_argv_push(&av, "/dev/null");
#endif
        st = ffx_run(&av, &opts->common);
        ffx_argv_free(&av);
        if (st != FFX_OK) { return st; }
    }

    /* ---- Pass 2 ---------------------------------------------------- */
    {
        FfxArgv av;
        ffx_argv_init(&av);
        st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
        if (st != FFX_OK) {
            ffx_argv_free(&av);
            return st;
        }
        ffx_argv_push2(&av, "-i", opts->input);
        ffx_argv_push2(&av, "-c:v", vcodec);
        if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
        ffx_argv_push_kbps(&av, "-b:v", (int)(video_kbps + 0.5));
        ffx_argv_push2(&av, "-pass", "2");
        ffx_argv_push2(&av, "-passlogfile", passlog);
        if (opts->copy_audio) {
            ffx_argv_push2(&av, "-c:a", "copy");
        } else {
            ffx_argv_push2(&av, "-c:a", "aac");
            ffx_argv_push_kbps(&av, "-b:a", audio_kbps);
        }
        ffx_argv_push(&av, opts->output);
        st = ffx_run(&av, &opts->common);
        ffx_argv_free(&av);
    }
    if (!opts->common.dry_run) {
        char logfile[FFX_MAX_OPT_LEN + 64];
        if (ffx_build_sidecar_path(passlog, "-0.log", logfile, sizeof(logfile))) { remove(logfile); }
        if (ffx_build_sidecar_path(passlog, "-0.log.mbtree", logfile, sizeof(logfile))) { remove(logfile); }
    }
    return st;
}

/**
 * @brief Implementation of `ffx_audio` (Audio extraction, stripping, replacement, mixing, or normalization).
 */
FfxStatus ffx_audio(const FfxAudioOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    switch (opts->op) {
        case FFX_AUDIO_EXTRACT:
            ffx_argv_push2(&av, "-i", opts->input);
            ffx_argv_push(&av, "-vn");
            if (opts->audio_codec != NULL) { ffx_argv_push2(&av, "-c:a", opts->audio_codec); }
            if (opts->audio_bitrate_kbps > 0) { ffx_argv_push_kbps(&av, "-b:a", opts->audio_bitrate_kbps); }
            ffx_argv_push(&av, opts->output);
            break;
        case FFX_AUDIO_STRIP:
            ffx_argv_push2(&av, "-i", opts->input);
            ffx_argv_push2(&av, "-c:v", "copy");
            ffx_argv_push(&av, "-an");
            ffx_argv_push(&av, opts->output);
            break;
        case FFX_AUDIO_REPLACE:
            if (opts->secondary_input == NULL) {
                fprintf(stderr, "ffx audio replace: secondary_input is required\n");
                st = FFX_ERR_INVALID_ARGUMENT;
                goto done;
            }
            ffx_argv_push2(&av, "-i", opts->input);
            ffx_argv_push2(&av, "-i", opts->secondary_input);
            ffx_argv_push2(&av, "-map", "0:v:0");
            ffx_argv_push2(&av, "-map", "1:a:0");
            ffx_argv_push2(&av, "-c:v", "copy");
            if (opts->audio_codec != NULL) {
                ffx_argv_push2(&av, "-c:a", opts->audio_codec);
            } else {
                ffx_argv_push2(&av, "-c:a", "aac");
            }
            if (opts->audio_bitrate_kbps > 0) { ffx_argv_push_kbps(&av, "-b:a", opts->audio_bitrate_kbps); }
            ffx_argv_push(&av, opts->output);
            break;
        case FFX_AUDIO_NORMALIZE: {
            float i = (opts->loudnorm_i != 0.0f) ? opts->loudnorm_i : -23.0f;
            float tp = (opts->loudnorm_tp != 0.0f) ? opts->loudnorm_tp : -2.0f;
            float lra = (opts->loudnorm_lra != 0.0f) ? opts->loudnorm_lra : 7.0f;
            char filter[256];
            snprintf(filter, sizeof(filter), "loudnorm=I=%.1f:TP=%.1f:LRA=%.1f", (double)i, (double)tp, (double)lra);
            ffx_argv_push2(&av, "-i", opts->input);
            ffx_argv_push2(&av, "-af", filter);
            if (opts->audio_codec != NULL) { ffx_argv_push2(&av, "-c:a", opts->audio_codec); }
            ffx_argv_push(&av, "-vn");
            ffx_argv_push(&av, opts->output);
            break;
        }
        case FFX_AUDIO_MIX: {
            if (opts->secondary_input == NULL) {
                fprintf(stderr, "ffx audio mix: secondary_input is required\n");
                st = FFX_ERR_INVALID_ARGUMENT;
                goto done;
            }
            float w1 = (opts->mix_weight_primary > 0.0f) ? opts->mix_weight_primary : 1.0f;
            float w2 = (opts->mix_weight_secondary > 0.0f) ? opts->mix_weight_secondary : 1.0f;
            char filter[256];
            snprintf(filter, sizeof(filter),
                     "[0:a]volume=%.3f[a1];[1:a]volume=%.3f[a2];"
                     "[a1][a2]amix=inputs=2:duration=longest",
                     (double)w1, (double)w2);
            ffx_argv_push2(&av, "-i", opts->input);
            ffx_argv_push2(&av, "-i", opts->secondary_input);
            ffx_argv_push2(&av, "-filter_complex", filter);
            if (opts->audio_codec != NULL) { ffx_argv_push2(&av, "-c:a", opts->audio_codec); }
            ffx_argv_push(&av, opts->output);
            break;
        }
        default:
            fprintf(stderr, "ffx audio: unknown operation %d\n", (int)opts->op);
            st = FFX_ERR_INVALID_ARGUMENT;
            goto done;
    }
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_thumbnail` (Generating image previews from video files).
 */
FfxStatus ffx_thumbnail(const FfxThumbnailOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    const char* timestamp = opts->timestamp;
    char mid_ts[32] = {0};
    if (opts->strategy == FFX_THUMB_MIDDLE) {
        double duration_seconds = 0.0;
        FfxStatus pst = ffx_probe_duration(opts->input, &opts->common, &duration_seconds);
        if (pst != FFX_OK) {
            fprintf(stderr, "ffx thumbnail: could not determine duration, using 0:00:05\n");
            snprintf(mid_ts, sizeof(mid_ts), "5");
        } else {
            snprintf(mid_ts, sizeof(mid_ts), "%.3f", duration_seconds / 2.0);
        }
        timestamp = mid_ts;
    }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    if (timestamp != NULL) { ffx_argv_push2(&av, "-ss", timestamp); }
    ffx_argv_push2(&av, "-i", opts->input);
    ffx_argv_push2(&av, "-frames:v", "1");
    if (opts->strategy == FFX_THUMB_BEST) {
        if (opts->width > 0 || opts->height > 0) {
            int w = opts->width > 0 ? opts->width : -1;
            int h = opts->height > 0 ? opts->height : -1;
            char vf[64];
            snprintf(vf, sizeof(vf), "thumbnail=300,scale=%d:%d", w, h);
            ffx_argv_push2(&av, "-vf", vf);
        } else {
            ffx_argv_push2(&av, "-vf", "thumbnail=300");
        }
    } else if (opts->width > 0 || opts->height > 0) {
        int w = opts->width > 0 ? opts->width : -1;
        int h = opts->height > 0 ? opts->height : -1;
        char vf[64];
        snprintf(vf, sizeof(vf), "scale=%d:%d", w, h);
        ffx_argv_push2(&av, "-vf", vf);
    }
    {
        int q = (opts->quality > 0 && opts->quality <= 31) ? opts->quality : 2;
        ffx_argv_push_int(&av, "-q:v", q);
    }
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_gif` (Optimized, high-quality animated GIF generation).
 */
FfxStatus ffx_gif(const FfxGifOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    const int fps = (opts->fps > 0) ? opts->fps : 15;
    const int width = (opts->width != 0) ? opts->width : 480;
    const char* stats = (opts->stats_mode != NULL) ? opts->stats_mode : "full";
    char palette_path[FFX_MAX_OPT_LEN + 32];
    if (!ffx_build_sidecar_path(opts->output, "_palette.png", palette_path, sizeof(palette_path))) {
        return FFX_ERR_IO;
    }

    char vf_base[256];
    snprintf(vf_base, sizeof(vf_base), "fps=%d,scale=%d:-1:flags=lanczos", fps, width);

    FfxStatus st = FFX_OK;

    /* ---- Pass 1: palettegen ---------------------------------------- */
    {
        FfxArgv av;
        ffx_argv_init(&av);
        st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
        if (st != FFX_OK) {
            ffx_argv_free(&av);
            return st;
        }

        if (opts->start != NULL) { ffx_argv_push2(&av, "-ss", opts->start); }
        if (opts->duration != NULL) { ffx_argv_push2(&av, "-t", opts->duration); }
        ffx_argv_push2(&av, "-i", opts->input);

        char vf[512];
        snprintf(vf, sizeof(vf), "%s,palettegen=stats_mode=%s", vf_base, stats);
        ffx_argv_push2(&av, "-vf", vf);
        ffx_argv_push(&av, palette_path);
        st = ffx_run(&av, &opts->common);
        ffx_argv_free(&av);
        if (st != FFX_OK) { return st; }
    }

    /* ---- Pass 2: paletteuse ---------------------------------------- */
    {
        FfxArgv av;
        ffx_argv_init(&av);
        st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
        if (st != FFX_OK) {
            ffx_argv_free(&av);
            goto cleanup_palette;
        }
        if (opts->start != NULL) { ffx_argv_push2(&av, "-ss", opts->start); }
        if (opts->duration != NULL) { ffx_argv_push2(&av, "-t", opts->duration); }
        ffx_argv_push2(&av, "-i", opts->input);
        ffx_argv_push2(&av, "-i", palette_path);

        char vf[512];
        snprintf(vf, sizeof(vf), "[0:v] %s [x]; [x][1:v] paletteuse", vf_base);
        ffx_argv_push2(&av, "-filter_complex", vf);
        if (!opts->loop) { ffx_argv_push2(&av, "-loop", "-1"); }
        ffx_argv_push(&av, opts->output);
        st = ffx_run(&av, &opts->common);
        ffx_argv_free(&av);
    }
cleanup_palette:
    if (!opts->common.dry_run) { remove(palette_path); }
    return st;
}

/**
 * @brief Implementation of `ffx_concat` (Merging multiple media files together).
 */
FfxStatus ffx_concat(const FfxConcatOpts* opts) {
    if (opts == NULL || opts->inputs == NULL || opts->input_count < 2 || opts->output == NULL) {
        return FFX_ERR_INVALID_ARGUMENT;
    }
    char list_path[FFX_MAX_OPT_LEN + 32];
    if (!ffx_build_sidecar_path(opts->output, "_concat_list.txt", list_path, sizeof(list_path))) { return FFX_ERR_IO; }
    if (!opts->common.dry_run) {
        if (!write_concat_list(list_path, opts->inputs, opts->input_count)) { return FFX_ERR_IO; }
    } else {
        fprintf(stderr, "[ffx dry-run] would write concat list to: %s\n", list_path);
    }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    ffx_argv_push2(&av, "-f", "concat");
    ffx_argv_push2(&av, "-safe", "0");
    ffx_argv_push2(&av, "-i", list_path);
    if (!opts->reencode) {
        ffx_argv_push2(&av, "-c", "copy");
    } else {
        const char* vc = (opts->video_codec != NULL) ? opts->video_codec : "libx264";
        const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
        ffx_argv_push2(&av, "-c:v", vc);
        if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
        ffx_argv_push2(&av, "-c:a", ac);
    }
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    if (!opts->common.dry_run) { remove(list_path); }
    return st;
}

/**
 * @brief Implementation of `ffx_watermark` (Overlaying an image or video logo).
 */
FfxStatus ffx_watermark(const FfxWatermarkOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->watermark == NULL || opts->output == NULL) {
        return FFX_ERR_INVALID_ARGUMENT;
    }
    const int margin = (opts->margin > 0) ? opts->margin : 10;
    const float opacity = (opts->opacity > 0.0f && opts->opacity <= 1.0f) ? opts->opacity : 1.0f;
    const int crf = (opts->crf > 0) ? opts->crf : 18;
    char x_expr[64], y_expr[64];
    switch (opts->position) {
        case FFX_WM_TOP_LEFT:
            snprintf(x_expr, sizeof(x_expr), "%d", margin);
            snprintf(y_expr, sizeof(y_expr), "%d", margin);
            break;
        case FFX_WM_TOP_RIGHT:
            snprintf(x_expr, sizeof(x_expr), "W-w-%d", margin);
            snprintf(y_expr, sizeof(y_expr), "%d", margin);
            break;
        case FFX_WM_BOTTOM_LEFT:
            snprintf(x_expr, sizeof(x_expr), "%d", margin);
            snprintf(y_expr, sizeof(y_expr), "H-h-%d", margin);
            break;
        case FFX_WM_BOTTOM_RIGHT:
            snprintf(x_expr, sizeof(x_expr), "W-w-%d", margin);
            snprintf(y_expr, sizeof(y_expr), "H-h-%d", margin);
            break;
        case FFX_WM_CENTER:
            snprintf(x_expr, sizeof(x_expr), "(W-w)/2");
            snprintf(y_expr, sizeof(y_expr), "(H-h)/2");
            break;
        case FFX_WM_CUSTOM:
            if (opts->x_expr == NULL || opts->y_expr == NULL) {
                fprintf(stderr, "ffx watermark: custom position requires x_expr and y_expr\n");
                return FFX_ERR_INVALID_ARGUMENT;
            }
            snprintf(x_expr, sizeof(x_expr), "%s", opts->x_expr);
            snprintf(y_expr, sizeof(y_expr), "%s", opts->y_expr);
            break;
        default:
            snprintf(x_expr, sizeof(x_expr), "W-w-%d", margin);
            snprintf(y_expr, sizeof(y_expr), "H-h-%d", margin);
            break;
    }
    char filter[512];
    if (opts->scale_width > 0) {
        snprintf(filter, sizeof(filter),
                 "[1:v]scale=%d:-1,format=rgba,"
                 "colorchannelmixer=aa=%.3f[wm];"
                 "[0:v][wm]overlay=%s:%s",
                 opts->scale_width, (double)opacity, x_expr, y_expr);
    } else {
        snprintf(filter, sizeof(filter),
                 "[1:v]format=rgba,"
                 "colorchannelmixer=aa=%.3f[wm];"
                 "[0:v][wm]overlay=%s:%s",
                 (double)opacity, x_expr, y_expr);
    }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    ffx_argv_push2(&av, "-i", opts->input);
    ffx_argv_push2(&av, "-i", opts->watermark);
    ffx_argv_push2(&av, "-filter_complex", filter);
    const char* vc = (opts->video_codec != NULL) ? opts->video_codec : "libx264";
    ffx_argv_push2(&av, "-c:v", vc);
    if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
    ffx_argv_push_int(&av, "-crf", crf);
    if (opts->copy_audio) { ffx_argv_push2(&av, "-c:a", "copy"); }
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_speed` (Changing video and audio playback speed).
 */
FfxStatus ffx_speed(const FfxSpeedOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    if (opts->factor <= 0.0f) {
        fprintf(stderr, "ffx speed: factor must be > 0 (got %.4g)\n", (double)opts->factor);
        return FFX_ERR_INVALID_ARGUMENT;
    }
    const int crf = (opts->crf > 0) ? opts->crf : 18;
    const char* vc = (opts->video_codec != NULL) ? opts->video_codec : "libx264";
    const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
    double pts_factor = 1.0 / (double)opts->factor;
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    ffx_argv_push2(&av, "-i", opts->input);
    if (opts->no_audio) {
        char vf[64];
        snprintf(vf, sizeof(vf), "setpts=%.6g*PTS", pts_factor);
        ffx_argv_push2(&av, "-vf", vf);
        ffx_argv_push(&av, "-an");
    } else {
        char atempo[256];
        if (!build_atempo_chain((double)opts->factor, atempo, sizeof(atempo))) {
            fprintf(stderr, "ffx speed: could not build atempo chain for factor %.4g\n", (double)opts->factor);
            st = FFX_ERR_UNSUPPORTED;
            goto done;
        }
        char filter_complex[512];
        snprintf(filter_complex, sizeof(filter_complex), "[0:v]setpts=%.6g*PTS[v];[0:a]%s[a]", pts_factor, atempo);
        ffx_argv_push2(&av, "-filter_complex", filter_complex);
        ffx_argv_push2(&av, "-map", "[v]");
        ffx_argv_push2(&av, "-map", "[a]");
        ffx_argv_push2(&av, "-c:a", ac);
    }
    ffx_argv_push2(&av, "-c:v", vc);
    if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
    ffx_argv_push_int(&av, "-crf", crf);
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_crop` (Cropping video frames to a bounding box).
 */
FfxStatus ffx_crop(const FfxCropOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    if (opts->width <= 0 || opts->height <= 0) {
        fprintf(stderr, "ffx crop: width and height must be > 0\n");
        return FFX_ERR_INVALID_ARGUMENT;
    }
    char x_expr[32], y_expr[32];
    if (opts->x < 0) {
        snprintf(x_expr, sizeof(x_expr), "(in_w-out_w)/2");
    } else {
        snprintf(x_expr, sizeof(x_expr), "%d", opts->x);
    }
    if (opts->y < 0) {
        snprintf(y_expr, sizeof(y_expr), "(in_h-out_h)/2");
    } else {
        snprintf(y_expr, sizeof(y_expr), "%d", opts->y);
    }
    char vf[128];
    snprintf(vf, sizeof(vf), "crop=%d:%d:%s:%s", opts->width, opts->height, x_expr, y_expr);
    FfxSingleVfOpts single = {
        .input = opts->input,
        .output = opts->output,
        .vf_filter = vf,
        .copy_audio = opts->copy_audio,
        .audio_codec = NULL,
        .video_codec = opts->video_codec,
        .video_preset = opts->video_preset,
        .crf = opts->crf,
        .common = &opts->common,
    };
    return ffx_run_single_vf(&single);
}

/**
 * @brief Implementation of `ffx_scale` (Scaling video frames with aspect ratio preservation).
 */
FfxStatus ffx_scale(const FfxScaleOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    if (opts->width <= 0 && opts->height <= 0) {
        fprintf(stderr, "ffx scale: at least one of width or height must be > 0\n");
        return FFX_ERR_INVALID_ARGUMENT;
    }
    char vf[256];
    switch (opts->mode) {
        case FFX_SCALE_STRETCH: {
            if (opts->width <= 0 || opts->height <= 0) {
                fprintf(stderr, "ffx scale: stretch mode requires both width and height\n");
                return FFX_ERR_INVALID_ARGUMENT;
            }
            snprintf(vf, sizeof(vf), "scale=%d:%d", opts->width, opts->height);
            break;
        }
        case FFX_SCALE_PAD: {
            if (opts->width <= 0 || opts->height <= 0) {
                fprintf(stderr, "ffx scale: pad mode requires both width and height\n");
                return FFX_ERR_INVALID_ARGUMENT;
            }
            const char* pad_color = (opts->pad_color != NULL) ? opts->pad_color : "black";
            snprintf(vf, sizeof(vf),
                     "scale=%d:%d:force_original_aspect_ratio=decrease,"
                     "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:%s",
                     opts->width, opts->height, opts->width, opts->height, pad_color);
            break;
        }
        default: {
            int w = (opts->width > 0) ? opts->width : -1;
            int h = (opts->height > 0) ? opts->height : -1;
            snprintf(vf, sizeof(vf), "scale=%d:%d", w, h);
            break;
        }
    }
    FfxSingleVfOpts single = {
        .input = opts->input,
        .output = opts->output,
        .vf_filter = vf,
        .copy_audio = opts->copy_audio,
        .audio_codec = NULL,
        .video_codec = opts->video_codec,
        .video_preset = opts->video_preset,
        .crf = opts->crf,
        .common = &opts->common,
    };
    return ffx_run_single_vf(&single);
}

/**
 * @brief Implementation of `ffx_rotate` (Rotating or mirroring video frames).
 */
FfxStatus ffx_rotate(const FfxRotateOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    const char* vf = NULL;
    switch (opts->op) {
        case FFX_ROTATE_90_CW:
            vf = "transpose=1";
            break;
        case FFX_ROTATE_90_CCW:
            vf = "transpose=2";
            break;
        case FFX_ROTATE_180:
            vf = "transpose=1,transpose=1";
            break;
        case FFX_FLIP_HORIZONTAL:
            vf = "hflip";
            break;
        case FFX_FLIP_VERTICAL:
            vf = "vflip";
            break;
        default:
            fprintf(stderr, "ffx rotate: unknown operation %d\n", (int)opts->op);
            return FFX_ERR_INVALID_ARGUMENT;
    }
    FfxSingleVfOpts single = {
        .input = opts->input,
        .output = opts->output,
        .vf_filter = vf,
        .copy_audio = opts->copy_audio,
        .audio_codec = NULL,
        .video_codec = opts->video_codec,
        .video_preset = opts->video_preset,
        .crf = opts->crf,
        .common = &opts->common,
    };
    return ffx_run_single_vf(&single);
}

/**
 * @brief Implementation of `ffx_fade` (Applying linear fade-in or fade-out effects).
 */
FfxStatus ffx_fade(const FfxFadeOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    if (!opts->fade_in && !opts->fade_out) {
        fprintf(stderr, "ffx fade: at least one of fade_in or fade_out must be set\n");
        return FFX_ERR_INVALID_ARGUMENT;
    }
    const bool want_video = (opts->video || (!opts->video && !opts->audio));
    const bool want_audio = (opts->audio || (!opts->video && !opts->audio));
    const float in_secs = (opts->fade_in_seconds > 0.0f) ? opts->fade_in_seconds : 1.0f;
    const float out_secs = (opts->fade_out_seconds > 0.0f) ? opts->fade_out_seconds : 1.0f;
    double fade_out_start = 0.0;
    if (opts->fade_out) {
        double duration = opts->total_duration_seconds;
        if (duration <= 0.0) {
            FfxStatus pst = ffx_probe_duration(opts->input, &opts->common, &duration);
            if (pst != FFX_OK) {
                fprintf(stderr, "ffx fade: could not determine duration for fade-out positioning\n");
                return pst;
            }
        }
        fade_out_start = duration - (double)out_secs;
        if (fade_out_start < 0.0) { fade_out_start = 0.0; }
    }
    char vf[256] = {0};
    char af[256] = {0};
    size_t vpos = 0, apos = 0;
    if (want_video) {
        if (opts->fade_in) {
            int n = snprintf(vf + vpos, sizeof(vf) - vpos, "fade=t=in:st=0:d=%.3f", (double)in_secs);
            if (n > 0) vpos += (size_t)n;
        }
        if (opts->fade_out) {
            int n = snprintf(vf + vpos, sizeof(vf) - vpos, "%sfade=t=out:st=%.3f:d=%.3f", (vpos > 0) ? "," : "",
                             fade_out_start, (double)out_secs);
            if (n > 0) vpos += (size_t)n;
        }
    }
    if (want_audio) {
        if (opts->fade_in) {
            int n = snprintf(af + apos, sizeof(af) - apos, "afade=t=in:st=0:d=%.3f", (double)in_secs);
            if (n > 0) apos += (size_t)n;
        }
        if (opts->fade_out) {
            int n = snprintf(af + apos, sizeof(af) - apos, "%safade=t=out:st=%.3f:d=%.3f", (apos > 0) ? "," : "",
                             fade_out_start, (double)out_secs);
            if (n > 0) apos += (size_t)n;
        }
    }
    const int crf = (opts->crf > 0) ? opts->crf : 18;
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    ffx_argv_push2(&av, "-i", opts->input);
    if (vpos > 0) { ffx_argv_push2(&av, "-vf", vf); }
    if (apos > 0) { ffx_argv_push2(&av, "-af", af); }
    if (vpos > 0) {
        ffx_argv_push_video_codec(&av, opts->video_codec, opts->common.hw_accel);
        if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
        ffx_argv_push_int(&av, "-crf", crf);
    } else {
        ffx_argv_push2(&av, "-c:v", "copy");
    }
    if (apos > 0) {
        const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
        ffx_argv_push2(&av, "-c:a", ac);
    } else {
        ffx_argv_push2(&av, "-c:a", "copy");
    }
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Implementation of `ffx_deinterlace` (Removing interlacing artifacts).
 */
FfxStatus ffx_deinterlace(const FfxDeinterlaceOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    const char* vf = (opts->mode == FFX_DEINTERLACE_BWDIF) ? "bwdif" : "yadif";
    FfxSingleVfOpts single = {
        .input = opts->input,
        .output = opts->output,
        .vf_filter = vf,
        .copy_audio = opts->copy_audio,
        .audio_codec = NULL,
        .video_codec = opts->video_codec,
        .video_preset = opts->video_preset,
        .crf = opts->crf,
        .common = &opts->common,
    };
    return ffx_run_single_vf(&single);
}

/**
 * @brief Implementation of `ffx_denoise` (Denoising and artifact reduction).
 */
FfxStatus ffx_denoise(const FfxDenoiseOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    char vf[128];
    if (opts->mode == FFX_DENOISE_NLMEANS) {
        if (opts->strength > 0.0f) {
            double s = 1.0 + ((double)opts->strength / 100.0) * 29.0;
            snprintf(vf, sizeof(vf), "nlmeans=s=%.3f", s);
        } else {
            snprintf(vf, sizeof(vf), "nlmeans");
        }
    } else {
        if (opts->strength > 0.0f) {
            double luma_spatial = ((double)opts->strength / 100.0) * 16.0;
            double chroma_spatial = luma_spatial * 0.75;
            snprintf(vf, sizeof(vf), "hqdn3d=%.2f:%.2f:%.2f:%.2f", luma_spatial, chroma_spatial, luma_spatial * 1.5,
                     chroma_spatial * 1.5);
        } else {
            snprintf(vf, sizeof(vf), "hqdn3d");
        }
    }
    FfxSingleVfOpts single = {
        .input = opts->input,
        .output = opts->output,
        .vf_filter = vf,
        .copy_audio = opts->copy_audio,
        .audio_codec = NULL,
        .video_codec = opts->video_codec,
        .video_preset = opts->video_preset,
        .crf = opts->crf,
        .common = &opts->common,
    };
    return ffx_run_single_vf(&single);
}

/**
 * @brief Implementation of `ffx_sharpen` (Sharpening edges and enhancing focus).
 */
FfxStatus ffx_sharpen(const FfxSharpenOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    const double amount = (opts->amount != 0.0f) ? (double)opts->amount : 1.0;
    char vf[64];
    snprintf(vf, sizeof(vf), "unsharp=luma_amount=%.3f", amount);
    FfxSingleVfOpts single = {
        .input = opts->input,
        .output = opts->output,
        .vf_filter = vf,
        .copy_audio = opts->copy_audio,
        .audio_codec = NULL,
        .video_codec = opts->video_codec,
        .video_preset = opts->video_preset,
        .crf = opts->crf,
        .common = &opts->common,
    };
    return ffx_run_single_vf(&single);
}

/**
 * @brief Implementation of `ffx_filter` (Applying custom video, audio, or complex filters).
 */
FfxStatus ffx_filter(const FfxFilterOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    const int set_count = (opts->video_filter != NULL) + (opts->audio_filter != NULL) + (opts->filter_complex != NULL);
    if (set_count != 1) {
        fprintf(stderr, "ffx filter: exactly one of video_filter, audio_filter, or filter_complex must be set\n");
        return FFX_ERR_INVALID_ARGUMENT;
    }
    const int crf = (opts->crf > 0) ? opts->crf : 18;
    if (opts->video_filter != NULL) {
        FfxSingleVfOpts single = {
            .input = opts->input,
            .output = opts->output,
            .vf_filter = opts->video_filter,
            .copy_audio = (opts->audio_codec == NULL),
            .audio_codec = opts->audio_codec,
            .video_codec = opts->video_codec,
            .video_preset = opts->video_preset,
            .crf = crf,
            .common = &opts->common,
        };
        return ffx_run_single_vf(&single);
    }
    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }
    ffx_argv_push2(&av, "-i", opts->input);
    if (opts->audio_filter != NULL) {
        ffx_argv_push2(&av, "-af", opts->audio_filter);
        ffx_argv_push2(&av, "-c:v", "copy");
        const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
        ffx_argv_push2(&av, "-c:a", ac);
    } else {
        ffx_argv_push2(&av, "-filter_complex", opts->filter_complex);
        if (opts->complex_maps != NULL) {
            for (size_t i = 0; opts->complex_maps[i] != NULL; ++i) {
                ffx_argv_push2(&av, "-map", opts->complex_maps[i]);
            }
        }
        ffx_argv_push_video_codec(&av, opts->video_codec, opts->common.hw_accel);
        if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }
        ffx_argv_push_int(&av, "-crf", crf);
        const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
        ffx_argv_push2(&av, "-c:a", ac);
    }
    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, &opts->common);
done:
    ffx_argv_free(&av);
    return st;
}

/**
 * @brief Parses FFmpeg's `silencedetect` stderr log lines into structured intervals.
 * @details Dynamically resizes the results storage array using realloc as new intervals are found.
 * @param log Captured stderr text from an FFmpeg `silencedetect` run.
 * @param result Pointer to pre-zeroed results structure to populate.
 */
static void parse_silence_log(const char* log, FfxSilenceResult* result) {
    const char* p = log;
    bool have_open_start = false;
    double open_start = 0.0;

    while (p != NULL && *p != '\0') {
        const char* start_marker = strstr(p, "silence_start:");
        const char* end_marker = strstr(p, "silence_end:");

        /* Pick whichever marker occurs first in the remaining text. */
        const char* next = NULL;
        bool is_start = false;
        if (start_marker != NULL && (end_marker == NULL || start_marker < end_marker)) {
            next = start_marker;
            is_start = true;
        } else if (end_marker != NULL) {
            next = end_marker;
            is_start = false;
        } else {
            break;
        }

        double value = 0.0;
        if (is_start) {
            if (sscanf(next, "silence_start: %lf", &value) == 1) {
                open_start = value;
                have_open_start = true;
            }
            p = next + strlen("silence_start:");
        } else {
            double duration = 0.0;
            int matched = sscanf(next, "silence_end: %lf | silence_duration: %lf", &value, &duration);
            if (matched >= 1 && have_open_start) {
                /* Dynamically grow the intervals array if we hit capacity */
                if (result->interval_count >= result->interval_capacity) {
                    size_t new_cap = result->interval_capacity == 0 ? 128 : result->interval_capacity * 2;
                    FfxSilenceInterval* new_arr = realloc(result->intervals, new_cap * sizeof(FfxSilenceInterval));
                    if (new_arr == NULL) {
                        fprintf(stderr, "ffx: out of memory allocating silence intervals\n");
                        break;
                    }
                    result->intervals = new_arr;
                    result->interval_capacity = new_cap;
                }

                FfxSilenceInterval* iv = &result->intervals[result->interval_count++];
                iv->start_seconds = open_start;
                iv->end_seconds = value;
                iv->duration_seconds = (matched == 2) ? duration : (value - open_start);
                have_open_start = false;
            }
            p = next + strlen("silence_end:");
        }
    }

    result->trailing_silence_open = have_open_start;
}

/** Buffer size used to capture the stderr log from a silencedetect analysis pass. */
#define FFX_SILENCE_LOG_BUF_SIZE (2 * 1024 * 1024)

FfxStatus ffx_silence_detect(const FfxSilenceDetectOpts* opts, FfxSilenceResult* result) {
    if (opts == NULL || opts->input == NULL || result == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    memset(result, 0, sizeof(*result));

    const double noise_db = (opts->noise_floor_db != 0.0f) ? (double)opts->noise_floor_db : -30.0;
    const double min_dur = (opts->min_duration_seconds > 0.0f) ? (double)opts->min_duration_seconds : 0.5;

    char af[128];
    int n = snprintf(af, sizeof(af), "silencedetect=noise=%.2fdB:d=%.3f", noise_db, min_dur);
    if (n < 0 || (size_t)n >= sizeof(af)) { return FFX_ERR_INVALID_ARGUMENT; }

    FfxArgv av;
    ffx_argv_init(&av);
    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", &opts->common);
    if (st != FFX_OK) { goto done; }

    ffx_argv_push2(&av, "-i", opts->input);
    ffx_argv_push2(&av, "-af", af);
    ffx_argv_push(&av, "-vn");
    ffx_argv_push2(&av, "-f", "null");
#ifdef _WIN32
    ffx_argv_push(&av, "NUL");
#else
    ffx_argv_push(&av, "/dev/null");
#endif

    ffx_argv_push2(&av, "-loglevel", "info");
    ffx_argv_push(&av, "-nostats");

    char* log_buf = malloc(FFX_SILENCE_LOG_BUF_SIZE);
    if (log_buf == NULL) {
        st = FFX_ERR_MEMORY;
        goto done;
    }

    size_t log_len = 0;
    st = ffx_run_capture_stderr(&av, &opts->common, log_buf, FFX_SILENCE_LOG_BUF_SIZE, &log_len);
    if (st == FFX_OK && !opts->common.dry_run) {
        parse_silence_log(log_buf, result);
    } else {
        free(result->intervals);
        result->intervals = NULL;
    }
    free(log_buf);

done:
    ffx_argv_free(&av);
    return st;
}

void ffx_silence_print(const FfxSilenceResult* result) {
    if (result == NULL) { return; }
    for (size_t i = 0; i < result->interval_count; ++i) {
        const FfxSilenceInterval* iv = &result->intervals[i];
        printf("%.3f %.3f %.3f\n", iv->start_seconds, iv->end_seconds, iv->duration_seconds);
    }
    if (result->trailing_silence_open) {
        fprintf(stderr, "ffx silence-detect: warning: trailing silence ran to end of file (unterminated)\n");
    }
}

/**
 * @brief A single non-silent (kept) time span computed from a silence detection result.
 */
typedef struct {
    double start_seconds;
    double end_seconds;
} FfxKeptSpan;

/**
 * @brief Computes the padded, non-overlapping "kept" (non-silent) spans from detected
 * silence intervals.
 */
static bool compute_kept_spans(const FfxSilenceResult* result, double total_duration_seconds, double pad_seconds,
                               FfxKeptSpan** out_spans, size_t* out_count) {
    *out_spans = NULL;
    *out_count = 0;

    size_t max_kept = result->interval_count + 1;
    FfxKeptSpan* spans = malloc(max_kept * sizeof(FfxKeptSpan));
    if (spans == NULL) { return false; }

    size_t kept_count = 0;
    double cursor = 0.0;
    for (size_t i = 0; i < result->interval_count; ++i) {
        const FfxSilenceInterval* iv = &result->intervals[i];
        if (iv->start_seconds > cursor + 1e-6) {
            double padded_end = iv->start_seconds + pad_seconds;
            double cap = iv->end_seconds;
            spans[kept_count].start_seconds = cursor;
            spans[kept_count].end_seconds = (padded_end < cap) ? padded_end : cap;
            kept_count++;
        }
        cursor = (iv->end_seconds > cursor) ? iv->end_seconds : cursor;
    }

    if (!result->trailing_silence_open && cursor < total_duration_seconds - 1e-6) {
        spans[kept_count].start_seconds = cursor;
        spans[kept_count].end_seconds = total_duration_seconds;
        kept_count++;
    }

    if (kept_count == 0) {
        free(spans);
        return false;
    }

    /* Pull each span's start backward for lead-in padding, clamped so it can never
     * overlap the previous (already-padded) span's end. */
    for (size_t i = 1; i < kept_count; ++i) {
        double padded_start = spans[i].start_seconds - pad_seconds;
        if (padded_start < spans[i - 1].end_seconds) { padded_start = spans[i - 1].end_seconds; }
        if (padded_start < 0.0) { padded_start = 0.0; }
        spans[i].start_seconds = padded_start;
    }

    *out_spans = spans;
    *out_count = kept_count;
    return true;
}

/**
 * @brief Escapes path strings for the FFmpeg concat demuxer.
 * @details Backslashes and single quotes are escaped with a backslash.
 */
static void write_escaped_path(FILE* fp, const char* path) {
    fprintf(fp, "file '");
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\'' || *p == '\\') { fputc('\\', fp); }
        fputc(*p, fp);
    }
    fprintf(fp, "'\n");
}

#define FFX_DESILENCE_MAX_WORKERS 4

/**
 * @brief Removes silent intervals from media, joining non-silent blocks with optional leveling filters.
 * @details Leverages multi-process timeline-partitioned chunks with strict thread-limiting 
 * to prevent CPU thread contention and desktop hangs.
 */
FfxStatus ffx_desilence(const FfxDesilenceOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL) { return FFX_ERR_INVALID_ARGUMENT; }

    /* ---- Pass 1: detect silence intervals -------------------------- */
    FfxSilenceDetectOpts detect_opts = {
        .input = opts->input,
        .noise_floor_db = opts->noise_floor_db,
        .min_duration_seconds = opts->min_duration_seconds,
        .common = opts->common,
    };
    detect_opts.common.dry_run = false;

    FfxSilenceResult silence = {0};
    FfxStatus st = ffx_silence_detect(&detect_opts, &silence);
    if (st != FFX_OK) {
        fprintf(stderr, "ffx desilence: silence detection pass failed: %s\n", ffx_status_str(st));
        return st;
    }

    double total_duration = 0.0;
    st = ffx_probe_duration(opts->input, &opts->common, &total_duration);
    if (st != FFX_OK) {
        fprintf(stderr, "ffx desilence: could not determine input duration\n");
        free(silence.intervals);
        return st;
    }

    const double pad_seconds = (opts->pad_seconds > 0.0f) ? (double)opts->pad_seconds : 0.1;

    FfxKeptSpan* spans = NULL;
    size_t span_count = 0;
    if (!compute_kept_spans(&silence, total_duration, pad_seconds, &spans, &span_count)) {
        fprintf(stderr, "ffx desilence: no non-silent audio detected\n");
        free(silence.intervals);
        return FFX_ERR_UNSUPPORTED;
    }
    free(silence.intervals);

    printf("Number of silences to be trimmed: %zu\n", span_count > 0 ? span_count - 1 : 0);

    /* ---- Pass 2: Configure Concurrency Timeline Limits ------------- */
    size_t num_workers = FFX_DESILENCE_MAX_WORKERS;
    if (span_count < num_workers) { num_workers = span_count; }

    ProcessHandle* handles[FFX_DESILENCE_MAX_WORKERS] = {0};
    char* chunk_paths[FFX_DESILENCE_MAX_WORKERS] = {0};
    FfxArgv argv_list[FFX_DESILENCE_MAX_WORKERS];
    for (size_t i = 0; i < num_workers; ++i) {
        ffx_argv_init(&argv_list[i]);
    }

    bool spawn_failed = false;
    size_t spans_per_worker = span_count / num_workers;
    size_t remainder = span_count % num_workers;

    char ffmpeg_path[FFX_MAX_OPT_LEN];
    if (!ffx_find_ffmpeg(ffmpeg_path, sizeof(ffmpeg_path))) {
        st = FFX_ERR_FFMPEG_NOT_FOUND;
        goto cleanup;
    }

    FfxProbeOpts probe_opts = {
        .input = opts->input,
        .print_json = false,
        .common = opts->common,
    };
    probe_opts.common.log_level = FFX_LOG_QUIET;
    probe_opts.common.dry_run = false;

    FfxProbeResult probe_res = {0};
    double fps = 30.0;
    int sample_rate = 44100;

    FfxStatus probe_st = ffx_probe(&probe_opts, &probe_res);
    if (probe_st == FFX_OK) {
        for (int i = 0; i < probe_res.stream_count; ++i) {
            if (strcmp(probe_res.streams[i].codec_type, "video") == 0) {
                if (probe_res.streams[i].fps > 0.0) fps = probe_res.streams[i].fps;
            } else if (strcmp(probe_res.streams[i].codec_type, "audio") == 0) {
                if (probe_res.streams[i].sample_rate > 0) sample_rate = probe_res.streams[i].sample_rate;
            }
        }
    }

    /* ---- Pass 3: Spawn Parallel timeline Slice Processes ---------- */
    for (size_t j = 0; j < num_workers; ++j) {
        size_t start_idx = j * spans_per_worker + (j < remainder ? j : remainder);
        size_t end_idx = start_idx + spans_per_worker + (j < remainder ? 1 : 0);

        if (start_idx >= end_idx) { continue; }

        double t_start = spans[start_idx].start_seconds;
        double t_end = spans[end_idx - 1].end_seconds;
        double duration = t_end - t_start;
        if (duration <= 0.0) { duration = 0.001; }

        char suffix[64];
        snprintf(suffix, sizeof(suffix), "_ffxchunk_%zu.mp4", j);
        chunk_paths[j] = malloc(FFX_MAX_OPT_LEN + 32);
        if (chunk_paths[j] == NULL ||
            !ffx_build_sidecar_path(opts->output, suffix, chunk_paths[j], FFX_MAX_OPT_LEN + 32)) {
            spawn_failed = true;
            st = FFX_ERR_MEMORY;
            break;
        }

        size_t filter_len = (end_idx - start_idx) * 80 + 512;
        char* filter_complex = malloc(filter_len);
        if (filter_complex == NULL) {
            spawn_failed = true;
            st = FFX_ERR_MEMORY;
            break;
        }

        size_t f_pos = 0;
        int fn = snprintf(filter_complex + f_pos, filter_len - f_pos, "[0:v]select='");
        if (fn > 0) f_pos += (size_t)fn;

        for (size_t i = start_idx; i < end_idx; ++i) {
            fn = snprintf(filter_complex + f_pos, filter_len - f_pos, "%sbetween(t,%.4f,%.4f)",
                          (i == start_idx) ? "" : "+", spans[i].start_seconds - t_start,
                          spans[i].end_seconds - t_start);
            if (fn > 0) f_pos += (size_t)fn;
        }

        fn = snprintf(filter_complex + f_pos, filter_len - f_pos, "',setpts=N/(%.6g*TB)[v];", fps);
        if (fn > 0) f_pos += (size_t)fn;

        fn = snprintf(filter_complex + f_pos, filter_len - f_pos, "[0:a]aselect='");
        if (fn > 0) f_pos += (size_t)fn;

        for (size_t i = start_idx; i < end_idx; ++i) {
            fn = snprintf(filter_complex + f_pos, filter_len - f_pos, "%sbetween(t,%.4f,%.4f)",
                          (i == start_idx) ? "" : "+", spans[i].start_seconds - t_start,
                          spans[i].end_seconds - t_start);
            if (fn > 0) f_pos += (size_t)fn;
        }

        fn = snprintf(filter_complex + f_pos, filter_len - f_pos, "',asetpts='(st(0,ld(0)+S)-S)/%d/TB'[a]",
                      sample_rate);
        if (fn > 0) f_pos += (size_t)fn;

        FfxArgv* av = &argv_list[j];
        st = ffx_argv_begin(av, "ffmpeg", &opts->common);
        if (st != FFX_OK) {
            free(filter_complex);
            spawn_failed = true;
            break;
        }

        /* 
         * OPTIMIZATION: Limit processing threads strictly per worker to prevent 
         * context-switching lockups.
         */
        ffx_argv_push2(av, "-threads", "4");
        ffx_argv_push2(av, "-filter_threads", "2");

        char start_time_str[32];
        char duration_str[32];
        snprintf(start_time_str, sizeof(start_time_str), "%.6f", t_start);
        snprintf(duration_str, sizeof(duration_str), "%.6f", duration);

        ffx_argv_push2(av, "-ss", start_time_str);
        ffx_argv_push2(av, "-t", duration_str);
        ffx_argv_push2(av, "-i", opts->input);
        ffx_argv_push2(av, "-filter_complex", filter_complex);
        ffx_argv_push2(av, "-map", "[v]");
        ffx_argv_push2(av, "-map", "[a]");

        ffx_argv_push_video_codec(av, opts->video_codec, opts->common.hw_accel);
        if (opts->video_preset != NULL) { ffx_argv_push2(av, "-preset", opts->video_preset); }
        int crf = (opts->crf > 0) ? opts->crf : 18;
        ffx_argv_push_int(av, "-crf", crf);

        const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
        ffx_argv_push2(av, "-c:a", ac);

        ffx_argv_push(av, chunk_paths[j]);
        free(filter_complex);

        if (opts->common.dry_run) {
            ffx_run(av, &opts->common);
        } else {
            ProcessOptions options = {
                .working_directory = NULL,
                .inherit_environment = true,
                .environment = NULL,
                .detached = false,
                .io =
                    {
                        .stdin_pipe = NULL,
                        .stdout_pipe = NULL,
                        .stderr_pipe = NULL,
                        .merge_stderr = false,
                    },
            };

            ProcessError pe = process_create(&handles[j], ffmpeg_path, (const char* const*)av->args, &options);
            if (pe != PROCESS_SUCCESS) {
                fprintf(stderr, "ffx desilence: failed to spawn worker %zu: %s\n", j, process_error_string(pe));
                spawn_failed = true;
                st = FFX_ERR_SPAWN_FAILED;
                break;
            }
        }
    }

    /* Wait for concurrent executions to terminate */
    if (!opts->common.dry_run) {
        for (size_t j = 0; j < num_workers; ++j) {
            if (handles[j] != NULL) {
                ProcessResult result = {0};
                ProcessError pe = process_wait(handles[j], &result, -1);
                process_free(handles[j]);
                handles[j] = NULL;
                if (pe != PROCESS_SUCCESS || !result.exited_normally || result.exit_code != 0) {
                    fprintf(stderr, "ffx desilence: worker %zu failed or exited with code %d\n", j, result.exit_code);
                    st = FFX_ERR_FFMPEG_FAILED;
                }
            }
        }
    }

    if (spawn_failed || st != FFX_OK) { goto cleanup; }

    /* ---- Pass 4: Final Stream-Copy Concat of the Chunks ------------- */
    char list_path[FFX_MAX_OPT_LEN + 32];
    if (!ffx_build_sidecar_path(opts->output, "_desilence_list.txt", list_path, sizeof(list_path))) {
        st = FFX_ERR_IO;
        goto cleanup;
    }

    if (!opts->common.dry_run) {
        FILE* fp = fopen(list_path, "w");
        if (fp == NULL) {
            st = FFX_ERR_IO;
            goto cleanup;
        }
        for (size_t j = 0; j < num_workers; ++j) {
            if (chunk_paths[j] != NULL) { write_escaped_path(fp, chunk_paths[j]); }
        }
        fclose(fp);
    } else {
        fprintf(stderr, "[ffx dry-run] would write merge concat list to: %s\n", list_path);
    }

    FfxArgv merge_av;
    ffx_argv_init(&merge_av);
    st = ffx_argv_begin(&merge_av, "ffmpeg", &opts->common);
    if (st == FFX_OK) {
        ffx_argv_push2(&merge_av, "-f", "concat");
        ffx_argv_push2(&merge_av, "-safe", "0");
        ffx_argv_push2(&merge_av, "-i", list_path);

        char audio_chain[256] = {0};
        size_t apos = 0;
        if (opts->smooth_audio) {
            int n = snprintf(audio_chain + apos, sizeof(audio_chain) - apos, "dynaudnorm=f=150:g=15");
            if (n > 0) apos += (size_t)n;
        }
        if (opts->target_lufs != 0.0f) {
            int n = snprintf(audio_chain + apos, sizeof(audio_chain) - apos, "%sloudnorm=I=%.1f:TP=-1.5:LRA=11",
                             (apos > 0) ? "," : "", (double)opts->target_lufs);
            if (n > 0) apos += (size_t)n;
        }

        if (apos > 0) {
            ffx_argv_push2(&merge_av, "-af", audio_chain);
            ffx_argv_push2(&merge_av, "-c:v", "copy");
            const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
            ffx_argv_push2(&merge_av, "-c:a", ac);
        } else {
            ffx_argv_push2(&merge_av, "-c", "copy");
        }

        ffx_argv_push(&merge_av, opts->output);
        st = ffx_run(&merge_av, &opts->common);
    }
    ffx_argv_free(&merge_av);

    if (!opts->common.dry_run) { remove(list_path); }

cleanup:
    if (!opts->common.dry_run) {
        for (size_t j = 0; j < num_workers; ++j) {
            if (chunk_paths[j] != NULL) { remove(chunk_paths[j]); }
        }
    }
    for (size_t j = 0; j < num_workers; ++j) {
        free(chunk_paths[j]);
        ffx_argv_free(&argv_list[j]);
    }
    free(spans);
    return st;
}
