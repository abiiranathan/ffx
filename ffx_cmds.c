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
 * 
 * ### Executed FFmpeg Flags:
 * - `-i <input>`             Sets the primary input file path.
 * - `-vn`                    Disables the video stream (if no_video is true).
 * - `-an`                    Disables the audio stream (if no_audio is true).
 * - `-c:v copy`              Copies the video stream directly (if stream_copy is true).
 * - `-c:a copy`              Copies the audio stream directly (if stream_copy is true).
 * - `-c:v <codec>`           Sets the video encoder (e.g., "-c:v libx264").
 * - `-preset <preset>`       Sets the encoder speed/compression quality preset.
 * - `-crf <crf>`             Applies a Constant Rate Factor targeting visual fidelity.
 * - `-b:v <bitrate>`         Defines average target video bitrate (e.g., "-b:v 2000k").
 * - `-s <resolution>`        Resizes output frame dimensions (e.g., "-s 1920x1080").
 * - `-r <fps>`               Sets target video frame rate (e.g., "-r 30").
 * - `-pix_fmt <pixfmt>`      Converts color pixel format (e.g., "-pix_fmt yuv420p").
 * - `-c:a <codec>`           Sets the audio encoder (e.g., "-c:a aac").
 * - `-b:a <bitrate>`         Defines average target audio bitrate (e.g., "-b:a 192k").
 * - `-ar <sample_rate>`      Sets audio sampling rate in Hertz (e.g., "-ar 44100").
 * - `-ac <channels>`         Sets number of audio channels (e.g., "-ac 2").
 * - `-f <format>`            Forces container write format overriding file extension.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * - `-ss <timestamp>`        Seeks to the start timestamp.
 *                            - Placed *before* `-i` (input seeking): Fast, but seek points are approximate (keyframe-based).
 *                            - Placed *after* `-i` (output seeking): Slower, but accurate and decodes all frames leading up to the timestamp.
 * - `-i <input>`             Sets primary input file path.
 * - `-t <duration>`          Limits output clip duration (if end_is_duration is true).
 * - `-to <timestamp>`        Stop writing output when this absolute timestamp is reached (if end_is_duration is false).
 * - `-c copy`                Copies video/audio stream data directly without re-encoding to preserve generation quality.
 * - `-avoid_negative_ts make_zero` Shifts negative timestamps generated by stream-copy seeks to zero.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * **Pass 1:**
 * - `-i <input>`             Sets the primary input file path.
 * - `-c:v <vcodec>`          Sets the video encoder (e.g., "-c:v libx264").
 * - `-preset <preset>`       Sets the encoder speed/compression quality preset.
 * - `-b:v <bitrate>`         Defines calculated target video bitrate fitting size constraints.
 * - `-pass 1`                Runs first-pass analysis, logging visual complexity statistics.
 * - `-passlogfile <prefix>`  Saves first-pass statistical logs to an isolated temporary sidecar path.
 * - `-an`                    Disables audio processing to accelerate analysis.
 * - `-f null`                Directs output to a null container format (no file created).
 * - `NUL` / `/dev/null`      Output destination target for pass 1 analysis.
 * 
 * **Pass 2:**
 * - `-pass 2`                Runs second-pass rendering, allocating bits based on pass 1 statistics.
 * - `-c:a aac` / `-c:a copy` Encodes audio to a target bitrate or copies the stream directly.
 * - `-b:a <audio_kbps>`      Sets the target audio bitrate constraint (e.g., "-b:a 128k").
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * **Extraction Mode:**
 * - `-vn`                    Disables the video stream.
 * - `-c:a <codec>`           Sets target audio encoder codec (e.g., "-c:a libmp3lame").
 * - `-b:a <bitrate>`         Applies average audio bitrate limit parameters (e.g., "-b:a 192k").
 * 
 * **Stripping Mode:**
 * - `-c:v copy`              Passes video stream segments direct without decoding.
 * - `-an`                    Disables the audio stream.
 * 
 * **Replacement Mode:**
 * - `-i <input1>` `-i <input2>` Imports primary video/audio and secondary audio replacement inputs.
 * - `-map 0:v:0`             Maps the video stream of the first input file.
 * - `-map 1:a:0`             Maps the audio stream of the second input file.
 * 
 * **Normalization Mode:**
 * - `-af loudnorm=I=...:TP=...:LRA=...` Attaches standard loudness normalization audio filter.
 * 
 * **Mixing Mode:**
 * - `-filter_complex "[0:a]volume=X[a1];[1:a]volume=Y[a2];[a1][a2]amix=inputs=2:duration=longest"`
 *                            Mixes primary and secondary audio tracks with custom volume scaling weights.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * - `-ss <timestamp>`        Seeks to the frame extraction point (e.g., calculated middle point or explicit timestamp).
 * - `-i <input>`             Sets the primary input file path.
 * - `-frames:v 1`            Limits the output to exactly one video frame (equivalent to `-vframes 1`).
 * - `-vf thumbnail=N`        Instructs the thumbnail filter to analyze a batch of N frames (default: 300) 
 *                            and extract the most visually representative frame (used in FFX_THUMB_BEST).
 * - `-vf scale=w:h`          Scales the extracted frame to the specified resolution dimensions.
 * - `-q:v <quality>`         Defines visual quality compression for output JPEG/images (typically 1–31, lower is better).
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * **Pass 1 (palettegen):**
 * - `-i <input>`             Sets the primary input file path.
 * - `-vf "...palettegen=stats_mode=..."` Generates an optimized 256-color palette based on the video frames.
 *                            The `stats_mode` parameter (e.g., "full" or "diff") controls color frequency analysis.
 * 
 * **Pass 2 (paletteuse):**
 * - `-i <input>`             Sets the primary input file path.
 * - `-i <palette_path>`      Loads the 256-color optimized palette file generated in Pass 1.
 * - `-filter_complex "[0:v]...[x];[x][1:v]paletteuse"` Maps the input video through scaling filters 
 *                            and applies the custom color palette for dithering.
 * - `-loop <loop_count>`     Specifies animation looping behaviors (`-1` disables looping; `0` loops indefinitely).
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
    {
        size_t pos = 0;
        if (opts->start != NULL) {
            int n = snprintf(vf_base + pos, sizeof(vf_base) - pos, "trim=start=%s", opts->start);
            if (n > 0) pos += (size_t)n;
            if (opts->duration != NULL) {
                n = snprintf(vf_base + pos, sizeof(vf_base) - pos, ":duration=%s", opts->duration);
                if (n > 0) pos += (size_t)n;
            }
            n = snprintf(vf_base + pos, sizeof(vf_base) - pos, ",setpts=PTS-STARTPTS,");
            if (n > 0) pos += (size_t)n;
        }
        int n = snprintf(vf_base + pos, sizeof(vf_base) - pos, "fps=%d,scale=%d:-1:flags=lanczos", fps, width);
        (void)n;
    }
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
 * 
 * ### Executed FFmpeg Flags:
 * - `-f concat`             Selects the physical concat demuxer driver.
 * - `-safe 0`                Allows the demuxer to accept unsafe or absolute paths in the list file.
 * - `-i <list_path>`         Loads the temporary text file containing the list of files to join.
 * - `-c copy`                Directly copies streams (if reencode is false). Fast and preserves quality.
 * - `-c:v <video_codec>`     Re-encodes the merged video stream (if reencode is true).
 * - `-preset <preset>`       Sets the encoder speed/compression quality preset (if reencode is true).
 * - `-c:a <audio_codec>`     Re-encodes the merged audio stream (if reencode is true).
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * - `-i <input1>`            Sets the primary video input file path.
 * - `-i <input2>`            Sets the watermark asset file path.
 * - `-filter_complex "[1:v]scale=...[wm];[0:v][wm]overlay=X:Y"`
 *                            Scales the watermark asset and blends it over the video stream
 *                            using the calculated coordinate positions (X and Y).
 * - `-c:v <codec>`           Sets target video encoder codec (e.g., "-c:v libx264").
 * - `-preset <preset>`       Sets target video encoder preset.
 * - `-crf <crf>`             Sets target visual quality index.
 * - `-c:a copy`              Copies the audio stream directly (if copy_audio is true).
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * **Video Speed Modification (Silent Mode):**
 * - `-vf setpts=N*PTS`       Adjusts video presentation timestamps (PTS).
 *                            To double speed (2x), PTS values are halved (0.5 * PTS).
 *                            To halve speed (0.5x), PTS values are doubled (2.0 * PTS).
 * - `-an`                    Strips the audio stream (if no_audio is true).
 * 
 * **Combined Video and Audio Speed Modification:**
 * - `-filter_complex "[0:v]setpts=...[v];[0:a]atempo=...[a]"`
 *                            Calculates and chains `setpts` and `atempo` filters (using `build_atempo_chain`)
 *                            to adjust both audio and video playback speed while maintaining synchronization.
 * - `-map "[v]"` `-map "[a]"` Maps the speed-adjusted video and audio streams to the output.
 * - `-c:v <codec>`           Sets target video encoder codec (e.g., "-c:v libx264").
 * - `-c:a <codec>`           Sets target audio encoder codec (e.g., "-c:a aac").
 * - `-preset <preset>`       Sets target video encoder speed preset.
 * - `-crf <crf>`             Sets target visual quality index.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `crop=width:height:x:y`  Extracts a rectangular frame area.
 *                            - `width` and `height` set the dimensions of the crop box.
 *                            - `x` and `y` set the top-left offset coordinates. If negative,
 *                              coordinates default to centered: `(in_w-out_w)/2` and `(in_h-out_h)/2`.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `scale=width:height`     Resizes the video to a target resolution.
 *                            Set to `-1` (e.g., `scale=1920:-1`) to automatically scale height while
 *                            preserving the original aspect ratio.
 * - `scale=...:force_original_aspect_ratio=decrease,pad=w:h:x:y:color`
 *                            Applies pillarboxing or letterboxing for FFX_SCALE_PAD. Scales the video
 *                            to fit within the target dimensions and pads the remaining area with a solid color.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `transpose=1`            Rotates frames 90 degrees Clockwise.
 * - `transpose=2`            Rotates frames 90 degrees Counter-Clockwise.
 * - `transpose=1,transpose=1` Rotates frames 180 degrees.
 * - `hflip`                  Flips the video horizontally.
 * - `vflip`                  Flips the video vertically.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `fade=t=in:st=0:d=duration` Applies a video fade-in starting at second 0 for the specified duration.
 * - `fade=t=out:st=start:d=duration` Applies a video fade-out starting at the calculated timestamp.
 * - `afade=t=in:st=0:d=duration` Applies an audio fade-in starting at second 0.
 * - `afade=t=out:st=start:d=duration` Applies an audio fade-out starting at the calculated timestamp.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `yadif`                  Yet Another Deinterlacing Filter.
 * - `bwdif`                  Bob Weaver Deinterlacing Filter (motion-adaptive deinterlacing).
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `hqdn3d=luma_spatial:chroma_spatial:luma_tmp:chroma_tmp`
 *                            Applies a high-quality 3D spatial-temporal denoiser with scaled noise reduction settings.
 * - `nlmeans=s=strength`     Applies a Non-Local Means video denoiser, adjusting search patch metrics based on strength.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Filters:
 * - `unsharp=luma_amount=val` Sharpens image edges by applying an unsharp mask filter to the luma channel.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
 * 
 * ### Executed FFmpeg Flags:
 * - `-vf <video_filter>`     Applies a custom video filter chain (if video_filter is set).
 * - `-af <audio_filter>`     Applies a custom audio filter chain (if audio_filter is set).
 * - `-filter_complex <expr>` Applies a custom complex multi-input/output filtergraph script (if filter_complex is set).
 * - `-map <stream_label>`    Maps specific filter outputs to the output file (if complex_maps are defined).
 * - `-c:v copy` / `-c:a copy` Copies video or audio streams directly without re-encoding, depending on configuration.
 * 
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or an error status code on failure.
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
