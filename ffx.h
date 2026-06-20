/**
 * @file ffx.h
 * @brief ffx — A high-level C wrapper around the FFmpeg command-line interface.
 *
 * This library simplifies media operations (transcoding, scaling, trimming,
 * watermarking, etc.) by constructing and invoking appropriate FFmpeg/FFprobe CLI
 * commands. It handles system process spawning and provides structured error handling.
 */

#ifndef FFX_H
#define FFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum custom flags that can be appended to the generated FFmpeg command. */
#define FFX_MAX_EXTRA_FLAGS 32

/** Maximum string length for parsed options and command components. */
#define FFX_MAX_OPT_LEN 512

/**
 * @brief Error and status codes returned by ffx operations.
 */
typedef enum {
    FFX_OK = 0,                    /**< Operation completed successfully. */
    FFX_ERR_INVALID_ARGUMENT = -1, /**< One or more structure options or paths are invalid or NULL. */
    FFX_ERR_FFMPEG_NOT_FOUND = -2, /**< The system could not locate the ffmpeg or ffprobe executable. */
    FFX_ERR_SPAWN_FAILED = -3,     /**< Failed to spawn the child process (process creation or permission error). */
    FFX_ERR_FFMPEG_FAILED = -4,    /**< ffmpeg returned a non-zero exit code during execution. */
    FFX_ERR_IO = -5,               /**< File reading, writing, or accessing error. */
    FFX_ERR_MEMORY = -6,           /**< Memory allocation failure inside wrapper operations. */
    FFX_ERR_UNSUPPORTED = -7,      /**< Attempted an invalid operation or input format configuration. */
    FFX_ERR_PROBE_PARSE = -8,      /**< Failed to parse the output from ffprobe. */
} FfxStatus;

/**
 * @brief Hardware acceleration frameworks available for encoding and decoding.
 */
typedef enum {
    FFX_HW_NONE = 0,     /**< Software processing (CPU-bound). Default option. */
    FFX_HW_NVENC,        /**< NVIDIA NVENC/NVDEC hardware acceleration (CUDA/CUVID). */
    FFX_HW_QSV,          /**< Intel Quick Sync Video hardware acceleration. */
    FFX_HW_VIDEOTOOLBOX, /**< Apple VideoToolbox framework (macOS and iOS hardware codecs). */
    FFX_HW_VAAPI,        /**< Video Acceleration API (commonly used on Linux/Unix systems). */
    FFX_HW_AMF,          /**< AMD Advanced Media Framework. */
} FfxHwAccel;

/**
 * @brief Verbosity levels for the internal ffmpeg execution output.
 */
typedef enum {
    FFX_LOG_QUIET = 0, /**< No output from ffmpeg, completely silent execution. */
    FFX_LOG_ERROR,     /**< Show only critical/fatal errors during execution. */
    FFX_LOG_WARNING,   /**< Show warnings and errors. */
    FFX_LOG_INFO,      /**< Show standard info, including stream layouts and progress. */
    FFX_LOG_VERBOSE,   /**< Detailed configuration, stream, and frame information. */
    FFX_LOG_DEBUG,     /**< Extremely verbose debugging output. */
} FfxLogLevel;

/**
 * @brief Common options shared across almost all high-level ffx actions.
 */
typedef struct {
    /** Sets the verbosity level of the underlying command execution. */
    FfxLogLevel log_level;

    /** If true, overwrites output files without prompting (adds the `-y` flag). Default: true. */
    bool overwrite;

    /** If true, generates and logs the command string without executing it. Default: false. */
    bool dry_run;

    /** Hardware acceleration interface selection. Default: FFX_HW_NONE. */
    FfxHwAccel hw_accel;

    /** Manual, extra command-line flags to append to the end of the argument list. */
    const char* extra_flags[FFX_MAX_EXTRA_FLAGS];

    /** The number of elements populated in the extra_flags array. */
    size_t extra_flags_count;
} FfxCommonOpts;

/**
 * @brief Factory function returning standard default common settings.
 * @return Default FfxCommonOpts structure.
 */
static inline FfxCommonOpts ffx_common_opts_default(void) {
    FfxCommonOpts o = {
        .log_level = FFX_LOG_ERROR,
        .overwrite = true,
        .dry_run = false,
        .hw_accel = FFX_HW_NONE,
        .extra_flags_count = 0,
    };
    return o;
}

/**
 * @brief Supported pixel format conversions.
 */
typedef enum {
    FFX_PIX_FMT_SAME = 0, /**< Preserve the source media pixel format. */
    FFX_PIX_FMT_YUV420P,  /**< Planar YUV 4:2:0, 12bpp (Highly compatible with standard players). */
    FFX_PIX_FMT_YUV444P,  /**< Planar YUV 4:4:4, 24bpp (High-fidelity color space). */
    FFX_PIX_FMT_YUV422P,  /**< Planar YUV 4:2:2, 16bpp. */
    FFX_PIX_FMT_NV12,     /**< Semi-planar YUV 4:2:0 (Commonly used in hardware encoders). */
    FFX_PIX_FMT_RGB24,    /**< Packed RGB 8:8:8, 24bpp. */
} FfxPixFmt;

/**
 * @brief Options structure for standard transcoding and format conversion.
 */
typedef struct {
    const char* input;  /**< Path to the input media file (e.g., "input.mov"). Required. */
    const char* output; /**< Path to the output file destination (e.g., "output.mp4"). Required. */

    /** Video codec name (e.g., "libx264", "libx265", "libvpx-vp9", "h264_nvenc", "copy" for no re-encoding). */
    const char* video_codec;

    /** Video preset for encoding speed/efficiency trade-off (e.g., "ultrafast", "medium", "veryslow"). */
    const char* video_preset;

    /** Constant Rate Factor 
    (typically 0–51, where lower is higher quality. Default: ~23 for x264. Set <= 0 to ignore). */
    int crf;

    /** Target video bitrate in kilobits per second 
    (e.g., 2000 for 2 Mbps). Ignored if crf is specified. */
    int video_bitrate_kbps;

    /** Target video resolution string (e.g., "1920x1080", "1280x720", or NULL to preserve source). */
    const char* resolution;

    /** Target frame rate (e.g., 30.0, 60.0, 23.976, or 0.0 to preserve source). */
    float fps;

    /** Output pixel format (e.g., FFX_PIX_FMT_YUV420P). Default: FFX_PIX_FMT_SAME. */
    FfxPixFmt pix_fmt;

    /** Audio codec name (e.g., "aac", "libmp3lame", "opus", "flac", "copy" for no re-encoding). */
    const char* audio_codec;

    /** Target audio bitrate in kilobits per second (e.g., 128, 192, 320). */
    int audio_bitrate_kbps;

    /** Audio sampling rate in Hertz (e.g., 44100, 48000). */
    int audio_sample_rate;

    /** Output audio channel count (e.g., 1 for mono, 2 for stereo, 6 for 5.1). */
    int audio_channels;

    /** Explicitly force output format container (e.g., "mp4", "mkv", "mp3"). Normally inferred from output path. */
    const char* format;

    /** If true, discards the video track entirely (equivalent to `-vn`). */
    bool no_video;

    /** If true, discards the audio track entirely (equivalent to `-an`). */
    bool no_audio;

    /** Shortcut configuration setting both video and audio codecs to "copy". */
    bool stream_copy;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxConvertOpts;

/**
 * @brief Converts a media file from one format/encoding standard to another.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_convert(const FfxConvertOpts* opts);

/**
 * @brief Options structure for cutting or trimming a media segment.
 */
typedef struct {
    const char* input;  /**< Path to the input media file. Required. */
    const char* output; /**< Path to the output file destination. Required. */

    /** Start timestamp. Format: "HH:MM:SS" or raw seconds (e.g., "00:01:30" or "90"). 
    NULL starts at beginning. */
    const char* start;

    /** End timestamp or duration segment. Format matching 'start'. NULL runs to end of file. */
    const char* end;

    /** If true, treats the 'end' string as a duration segment 
    (adds `-t`) instead of an absolute timestamp (`-to`). */
    bool end_is_duration;

    /** If true, applies input-level seeking which is 
    frame-accurate but slower. If false, uses faster stream-level seeking. */
    bool accurate_seek;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxTrimOpts;

/**
 * @brief Trims or slices a segment out of a media file.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_trim(const FfxTrimOpts* opts);

/**
 * @brief Options structure for compressing a media file to a target size limit.
 */
typedef struct {
    const char* input;  /**< Path to the input media file. Required. */
    const char* output; /**< Path to the output file destination. Required. */

    /** Target output file size in Megabytes (MB). 
    Encoding engine adjusts bitrates to fit this constraint. */
    float target_size_mb;

    /** Codec family standard used for compression (e.g., "h264", "h265", "vp9", "av1"). */
    const char* codec_family;

    /** Codec preset parameter regulating compression ratio versus runtime (e.g., "medium", "slow"). */
    const char* video_preset;

    /** If true, passes original audio streams directly through without re-encoding to save compression time. */
    bool copy_audio;

    /** Fallback audio bitrate in kilobits per second if re-encoding is necessary. Default: ~128. */
    int audio_bitrate_kbps;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxCompressOpts;

/**
 * @brief Compresses media using automatic target-bitrate calculating routines.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_compress(const FfxCompressOpts* opts);

/**
 * @brief Supported operations on audio tracks.
 */
typedef enum {
    FFX_AUDIO_EXTRACT = 0, /**< Demux/extract audio stream to a raw audio container. */
    FFX_AUDIO_STRIP,       /**< Strip the audio tracks, yielding silent video output. */
    FFX_AUDIO_REPLACE,     /**< Replace the audio track of a video with another media's track. */
    FFX_AUDIO_NORMALIZE,   /**< Apply EBU R128 standard loudness normalization. */
    FFX_AUDIO_MIX,         /**< Mix primary and secondary audio inputs into a single track. */
} FfxAudioOp;

/**
 * @brief Options structure for processing or adjusting audio tracks.
 */
typedef struct {
    FfxAudioOp op;      /**< Specific operational behavior selected. Required. */
    const char* input;  /**< Path to primary input file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Secondary input path, required only for FFX_AUDIO_REPLACE and FFX_AUDIO_MIX. */
    const char* secondary_input;

    /** Output audio codec choice (e.g., "aac", "libmp3lame", "opus"). */
    const char* audio_codec;

    /** Target audio stream bitrate in kilobits per second. */
    int audio_bitrate_kbps;

    /** EBU R128 loudness normalization target in LUFS (Range: -70.0 to -5.0. Default: -24.0). */
    float loudnorm_i;

    /** Loudness normalization maximum True Peak level in dBTP (Range: -9.0 to 0.0. Default: -2.0). */
    float loudnorm_tp;

    /** Loudness Range (LRA) target in LU (Range: 1.0 to 20.0. Default: 7.0). */
    float loudnorm_lra;

    /** Volume weight of primary input during mixing operations (Range: 0.0 to 1.0. Default: 1.0). */
    float mix_weight_primary;

    /** Volume weight of secondary input during mixing operations (Range: 0.0 to 1.0. Default: 1.0). */
    float mix_weight_secondary;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxAudioOpts;

/**
 * @brief Performs operations on audio tracks, including stripping, extracting, mixing, and normalizing.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_audio(const FfxAudioOpts* opts);

/**
 * @brief Strategy choices for generating preview thumbnails.
 */
typedef enum {
    FFX_THUMB_AT_TIME = 0, /**< Capture frame at an exact specified duration timestamp. */
    FFX_THUMB_BEST,        /**< Attempt to isolate the highest-fidelity keyframe. */
    FFX_THUMB_MIDDLE,      /**< Capture a frame precisely in the middle of the file's duration. */
} FfxThumbStrategy;

/**
 * @brief Options structure for generating image thumbnails.
 */
typedef struct {
    const char* input;  /**< Path to source video file. Required. */
    const char* output; /**< Output image path (e.g., "thumb.jpg", "thumb.png"). Required. */

    /** Thumbnail frame search strategy. */
    FfxThumbStrategy strategy;

    /** Time position of frame if strategy is set to FFX_THUMB_AT_TIME (e.g., "00:02:15" or "135"). */
    const char* timestamp;

    /** Target width in pixels. Set to -1 to automatically scale preserving aspect ratio. */
    int width;

    /** Target height in pixels. Set to -1 to automatically scale preserving aspect ratio. */
    int height;

    /** Output compression quality (Typically 1 to 31 for JPEG; lower value produces higher quality). */
    int quality;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxThumbnailOpts;

/**
 * @brief Extracts a high-quality thumbnail image frame from a video.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_thumbnail(const FfxThumbnailOpts* opts);

/**
 * @brief Options structure for generating animated GIFs from a video clip.
 */
typedef struct {
    const char* input;  /**< Path to source video file. Required. */
    const char* output; /**< Output GIF file destination (e.g., "animated.gif"). Required. */

    /** Start time within the source video (e.g., "00:00:10.5" or "10.5"). */
    const char* start;

    /** Duration of the animated GIF segment (e.g., "5" or "5.5" seconds). */
    const char* duration;

    /** Animated GIF frame rate (e.g., 10, 15, or 24 FPS. Typical range: 5–30). */
    int fps;

    /** Width in pixels. Height scale will automatically maintain original aspect ratio. */
    int width;

    /** Color-palette optimization mode ("full" or "diff"). 
    Diff tracks movement overlays to minimize size. */
    const char* stats_mode;

    /** If true, the output GIF loops indefinitely. If false, loops only once. */
    bool loop;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxGifOpts;

/**
 * @brief Generates high-quality animated GIFs from video streams using customized palette systems.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_gif(const FfxGifOpts* opts);

/**
 * @brief Structure containing parsed parameters of a specific media stream.
 */
typedef struct {
    int index;               /**< Internal index identifier of the stream (0, 1, etc.). */
    char codec_name[64];     /**< Parsed codec designation (e.g., "h264", "aac", "subrip"). */
    char codec_type[16];     /**< Stream type category (e.g., "video", "audio", "subtitle"). */
    char pixel_format[32];   /**< Video pixel format representation (e.g., "yuv420p"). Video only. */
    int width;               /**< Display width of frames in pixels. Video only. */
    int height;              /**< Display height of frames in pixels. Video only. */
    double fps;              /**< Frame rate representation. Video only. */
    double duration_seconds; /**< Computed stream duration in seconds. */
    long long bit_rate;      /**< Parsed stream data bitrate in bits per second. */
    int sample_rate;         /**< Audio sampling frequency in Hz. Audio only. */
    int channels;            /**< Number of audio channels. Audio only. */
    char channel_layout[32]; /**< Audio spatial speaker configuration (e.g., "stereo", "5.1"). */
} FfxStreamInfo;

/** Maximum streams recorded from file probe. */
#define FFX_MAX_STREAMS 16

/**
 * @brief Combined metadata output structure returned from a media analysis operation.
 */
typedef struct {
    char format_name[64];                   /**< General wrapper container format (e.g., "mov,mp4,m4a,3gp"). */
    double duration_seconds;                /**< Overall file duration in seconds. */
    long long size_bytes;                   /**< File size on disk in bytes. */
    long long bit_rate;                     /**< Computed combined bitrate in bits per second. */
    FfxStreamInfo streams[FFX_MAX_STREAMS]; /**< Array containing parsed specifications for active streams. */
    int stream_count;                       /**< Total streams detected in input. */
} FfxProbeResult;

/**
 * @brief Options structure for analyzing files via the probe utility.
 */
typedef struct {
    const char* input;    /**< Path to target media file. Required. */
    bool print_json;      /**< If true, formats and dumps raw discovery metadata to stdout. */
    FfxCommonOpts common; /**< Common options configuration block. */
} FfxProbeOpts;

/**
 * @brief Analyzes a media file, extracting format and stream metadata.
 * @param opts Configuration options structure.
 * @param result Pointer to pre-allocated results structure populated on success.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_probe(const FfxProbeOpts* opts, FfxProbeResult* result);

/**
 * @brief Formats and prints a summary of FfxProbeResult structures to stdout.
 * @param result Pointer to populated analysis results.
 */
void ffx_probe_print(const FfxProbeResult* result);

/**
 * @brief Options structure for concatenating multiple media streams together.
 */
typedef struct {
    const char** inputs; /**< Array of file paths to join together. Required. */
    size_t input_count;  /**< Number of file paths in inputs array. Must be > 1. */
    const char* output;  /**< Output file destination path. Required. */

    /** If false, uses stream copy (requires identical codecs/formats). 
    If true, transcodes through filtering. */
    bool reencode;

    /** Video codec for re-encoding (e.g., "libx264"). Ignored if 'reencode' is false. */
    const char* video_codec;

    /** Video preset parameter for encoding optimization. Ignored if 'reencode' is false. */
    const char* video_preset;

    /** Audio codec for re-encoding (e.g., "aac"). Ignored if 'reencode' is false. */
    const char* audio_codec;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxConcatOpts;

/**
 * @brief Concatenates multiple media tracks into a single output file.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_concat(const FfxConcatOpts* opts);

/**
 * @brief Standard placement coordinate models for watermarks.
 */
typedef enum {
    FFX_WM_TOP_LEFT = 0, /**< Top-left corner placement. */
    FFX_WM_TOP_RIGHT,    /**< Top-right corner placement. */
    FFX_WM_BOTTOM_LEFT,  /**< Bottom-left corner placement. */
    FFX_WM_BOTTOM_RIGHT, /**< Bottom-right corner placement. */
    FFX_WM_CENTER,       /**< Dead-center placement. */
    FFX_WM_CUSTOM,       /**< Position dynamically calculated using x_expr and y_expr. */
} FfxWatermarkPos;

/**
 * @brief Options structure for overlaying watermarks (images or videos).
 */
typedef struct {
    const char* input;     /**< Path to source video file. Required. */
    const char* watermark; /**< Path to watermark image/video asset (e.g., "logo.png"). Required. */
    const char* output;    /**< Output file destination. Required. */

    /** General position scheme. Set to FFX_WM_CUSTOM to use math expressions. */
    FfxWatermarkPos position;

    /** Boundary distance margin in pixels. Applied to top, bottom, left, or right margins. */
    int margin;

    /** Watermark blending factor (Range: 0.0 to 1.0, where 1.0 is completely opaque). */
    float opacity;

    /** Target overlay scale width. Aspect ratio is preserved. Set <= 0 to keep native size. */
    int scale_width;

    /** Custom horizontal coordinate mathematical expression 
    (e.g., "main_w-overlay_w-10"). Used for FFX_WM_CUSTOM. */
    const char* x_expr;

    /** Custom vertical coordinate mathematical expression 
    (e.g., "main_h-overlay_h-10"). Used for FFX_WM_CUSTOM. */
    const char* y_expr;

    /** If true, copies the original audio stream directly. 
    If false, processes/re-encodes. */
    bool copy_audio;

    /** Target video codec choice (e.g., "libx264"). */
    const char* video_codec;

    /** Encoding preset selected (e.g., "medium"). */
    const char* video_preset;

    /** Constant Rate Factor quality target. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxWatermarkOpts;

/**
 * @brief Overlays a watermark image or video on top of a primary video track.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_watermark(const FfxWatermarkOpts* opts);

/**
 * @brief Options structure for accelerating or slowing down media playback.
 */
typedef struct {
    const char* input;  /**< Path to input media file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Multiplier factor. Values > 1.0 speed up playback; 
    values < 1.0 slow down playback (e.g., 2.0 or 0.5). */
    float factor;

    /** If true, audio track is completely removed. 
    If false, speed adjustments are made to audio pitch matching video. */
    bool no_audio;

    /** Video encoder codec selection (e.g., "libx264"). */
    const char* video_codec;

    /** Encoding optimization preset selection (e.g., "fast"). */
    const char* video_preset;

    /** Target audio encoder codec if audio is processed (e.g., "aac"). */
    const char* audio_codec;

    /** Quality index target parameter for video stream. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxSpeedOpts;

/**
 * @brief Alters the execution and rendering frame rate speed of video and audio streams.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_speed(const FfxSpeedOpts* opts);

/**
 * @brief Options structure for cropping video frames.
 */
typedef struct {
    const char* input;  /**< Path to input video file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Width of the target crop bounding box in pixels. */
    int width;

    /** Height of the target crop bounding box in pixels. */
    int height;

    /** Horizontal pixel coordinate offset for the top-left corner of the crop frame. */
    int x;

    /** Vertical pixel coordinate offset for the top-left corner of the crop frame. */
    int y;

    /** If true, preserves and copies original audio tracks without processing. */
    bool copy_audio;

    /** Video encoder codec selection. */
    const char* video_codec;

    /** Codec performance preset standard. */
    const char* video_preset;

    /** Quality factor index for transcoding. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxCropOpts;

/**
 * @brief Crops video frames to specified coordinate boundaries.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_crop(const FfxCropOpts* opts);

/**
 * @brief Scaling strategies when resizing media to fit destination constraints.
 */
typedef enum {
    FFX_SCALE_FIT = 0, /**< Resize maintaining aspect ratio to fit inside target boundaries. */
    FFX_SCALE_STRETCH, /**< Stretch/squeeze directly to target width/height boundaries. */
    FFX_SCALE_PAD,     /**< Resize preserving aspect ratio, filling leftover margins with background solid colors. */
} FfxScaleMode;

/**
 * @brief Options structure for scaling and padding video resolutions.
 */
typedef struct {
    const char* input;  /**< Path to input video file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Target frame resolution width in pixels. */
    int width;

    /** Target frame resolution height in pixels. */
    int height;

    /** Frame mapping scale mode strategy. */
    FfxScaleMode mode;

    /** Padding background solid color designator (e.g., "black", "white", or hex values like "0x000000"). */
    const char* pad_color;

    /** If true, copies original audio stream data without processing. */
    bool copy_audio;

    /** Video encoder codec designation (e.g., "libx265"). */
    const char* video_codec;

    /** Codec performance optimization preset. */
    const char* video_preset;

    /** Quality factor index target parameter. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxScaleOpts;

/**
 * @brief Resizes video frame dimensions using designated scaling and padding strategies.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_scale(const FfxScaleOpts* opts);

/**
 * @brief Spatial re-orientation and mirroring transformations.
 */
typedef enum {
    FFX_ROTATE_90_CW = 0, /**< Rotate orientation 90 degrees Clockwise. */
    FFX_ROTATE_90_CCW,    /**< Rotate orientation 90 degrees Counter-Clockwise. */
    FFX_ROTATE_180,       /**< Rotate orientation 180 degrees. */
    FFX_FLIP_HORIZONTAL,  /**< Mirror image horizontally (left to right). */
    FFX_FLIP_VERTICAL,    /**< Mirror image vertically (upside down). */
} FfxRotateOp;

/**
 * @brief Options structure for transforming frame rotation and mirror orientation.
 */
typedef struct {
    const char* input;  /**< Path to input video file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Rotation or flip action option. */
    FfxRotateOp op;

    /** If true, passes original audio streams directly through. */
    bool copy_audio;

    /** Target video encoder codec designator. */
    const char* video_codec;

    /** Performance preset optimization option. */
    const char* video_preset;

    /** Transcoding quality index value. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxRotateOpts;

/**
 * @brief Rotates or flips video frames using spatial transpose transformations.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_rotate(const FfxRotateOpts* opts);

/**
 * @brief Options structure for configuring media fade-in/fade-out transitions.
 */
typedef struct {
    const char* input;  /**< Path to input media file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** If true, applies a linear fade-in transition starting at the beginning of the file. */
    bool fade_in;

    /** Duration of the fade-in effect in seconds. */
    float fade_in_seconds;

    /** If true, applies a linear fade-out transition ending at the output duration limit. */
    bool fade_out;

    /** Duration of the fade-out effect in seconds. */
    float fade_out_seconds;

    /** Total length of the input clip in seconds. Required to compute the starting point of the fade-out. */
    double total_duration_seconds;

    /** If true, applies fade adjustments to the video track. */
    bool video;

    /** If true, applies fade adjustments to the audio track. */
    bool audio;

    /** Video encoder codec selection. */
    const char* video_codec;

    /** Codec encoding speed/efficiency preset. */
    const char* video_preset;

    /** Target audio encoder codec. */
    const char* audio_codec;

    /** Video quality factor constraint. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxFadeOpts;

/**
 * @brief Appends linear audio and/or video fade-in and fade-out transitions.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_fade(const FfxFadeOpts* opts);

/**
 * @brief Deinterlacing filter algorithms.
 */
typedef enum {
    FFX_DEINTERLACE_YADIF = 0, /**< Yet Another Deinterlacing Filter (standard, fast, adaptive). */
    FFX_DEINTERLACE_BWDIF,     /**< Bob Weaver Deinterlacing Filter (improved motion-adaptive edge accuracy). */
} FfxDeinterlaceMode;

/**
 * @brief Options structure for deinterlacing analog-source video frames.
 */
typedef struct {
    const char* input;  /**< Path to input video file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Selection of deinterlacing engine. */
    FfxDeinterlaceMode mode;

    /** If true, copies original audio stream directly. */
    bool copy_audio;

    /** Target video encoder codec. */
    const char* video_codec;

    /** Codec preset level selected. */
    const char* video_preset;

    /** Quality factor parameters index. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxDeinterlaceOpts;

/**
 * @brief Deinterlaces video fields into progressive video frames.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_deinterlace(const FfxDeinterlaceOpts* opts);

/**
 * @brief Spatial and temporal noise reduction filter options.
 */
typedef enum {
    FFX_DENOISE_HQDN3D = 0, /**< High-quality 3D (spatial-temporal) denoiser (Fast, lightweight). */
    FFX_DENOISE_NLMEANS, /**< Non-Local Means denoiser (Highly accurate edge preservation, computationally intensive). */
} FfxDenoiseMode;

/**
 * @brief Options structure for configuring noise-reduction filters.
 */
typedef struct {
    const char* input;  /**< Path to input video file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Noise filtering algorithms selected. */
    FfxDenoiseMode mode;

    /** Filter strength multiplier (Typically 0.0 to 10.0; higher limits filter more aggressively). */
    float strength;

    /** If true, copies original audio stream data without processing. */
    bool copy_audio;

    /** Target video encoder codec standard. */
    const char* video_codec;

    /** Codec efficiency presets standard. */
    const char* video_preset;

    /** Quality target index value. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxDenoiseOpts;

/**
 * @brief Reduces digital video noise and compression artifacts.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_denoise(const FfxDenoiseOpts* opts);

/**
 * @brief Options structure for video sharpening.
 */
typedef struct {
    const char* input;  /**< Path to input video file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Contrast sharpening intensity amount (Typically 0.0 to 2.0. Recommended standard is ~0.5–1.0). */
    float amount;

    /** If true, preserves and copies original audio tracks without processing. */
    bool copy_audio;

    /** Target video encoder codec standard. */
    const char* video_codec;

    /** Codec performance preset standard. */
    const char* video_preset;

    /** Transcoding quality index standard. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxSharpenOpts;

/**
 * @brief Sharpens blurry video frames to enhance edge contrast.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_sharpen(const FfxSharpenOpts* opts);

/**
 * @brief Options structure for passing custom filter configurations.
 */
typedef struct {
    const char* input;  /**< Path to input media file. Required. */
    const char* output; /**< Path to output file destination. Required. */

    /** Custom video filter chain string (e.g., "hqdn3d=1.5,unsharp=5:5:1.0:5:5:1.0"). Adds `-vf`. */
    const char* video_filter;

    /** Custom audio filter chain string (e.g., "volume=1.5,lowpass=f=3000"). Adds `-af`. */
    const char* audio_filter;

    /** Complex filtergraph script block 
    (e.g., "[0:v][1:v]overlay=10:10[outv]"). Adds `-filter_complex`. */
    const char* filter_complex;

    /** Complex filtergraph output map targets 
    (NULL-terminated array of strings, e.g., {"[outv]", "[outa]", NULL}). */
    const char* const* complex_maps;

    /** Target video encoder codec designation. */
    const char* video_codec;

    /** Codec optimization preset standards. */
    const char* video_preset;

    /** Target audio encoder codec. */
    const char* audio_codec;

    /** Video quality factor index. */
    int crf;

    /** Common options configuration block. */
    FfxCommonOpts common;
} FfxFilterOpts;

/**
 * @brief Applies custom simple filters or multi-input complex filtergraphs.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_filter(const FfxFilterOpts* opts);

/**
 * @brief Converts an FfxStatus error code into a human-readable string.
 * @param status The status code to convert.
 * @return A static string explaining the status code.
 */
const char* ffx_status_str(FfxStatus status);

/**
 * @brief Locates the ffmpeg executable on the system path or environment variables.
 * @param buf Output buffer to store the absolute path of the executable.
 * @param buflen Length of the destination buffer.
 * @return true if found, false otherwise.
 */
bool ffx_find_ffmpeg(char* buf, size_t buflen);

/**
 * @brief Locates the ffprobe executable on the system path or environment variables.
 * @param buf Output buffer to store the absolute path of the executable.
 * @param buflen Length of the destination buffer.
 * @return true if found, false otherwise.
 */
bool ffx_find_ffprobe(char* buf, size_t buflen);

/**
 * @brief Structure containing parsed parameters of a detected silent interval.
 */
typedef struct {
    double start_seconds;    /**< Start timestamp of the silence in seconds. */
    double end_seconds;      /**< End timestamp of the silence in seconds. */
    double duration_seconds; /**< Duration of the silent period in seconds. */
} FfxSilenceInterval;

/**
 * @brief Combined silence tracking result returned from analysis operations.
 */
typedef struct {
    FfxSilenceInterval* intervals; /**< Dynamically allocated array of detected silent spans. */
    size_t interval_count;         /**< Total number of intervals recorded. */
    size_t interval_capacity;      /**< Current allocated capacity of the array. */
    bool trailing_silence_open;    /**< True if the file ended in a silent state. */
} FfxSilenceResult;

/**
 * @brief Options structure for detecting silent sections using the silence filter.
 */
typedef struct {
    const char* input;          /**< Path to target media file. Required. */
    float noise_floor_db;       /**< Noise floor threshold in decibels (default: -30.0 dB). */
    float min_duration_seconds; /**< Minimum silence duration threshold in seconds (default: 0.5 s). */
    FfxCommonOpts common;       /**< Common options configuration block. */
} FfxSilenceDetectOpts;

/**
 * @brief Options structure for removing silent periods and re-encoding.
 */
typedef struct {
    const char* input;          /**< Path to input media file. Required. */
    const char* output;         /**< Path to output file destination. Required. */
    float noise_floor_db;       /**< Noise floor threshold in decibels (default: -30.0 dB). */
    float min_duration_seconds; /**< Minimum silence duration threshold in seconds (default: 0.5 s). */

    /**< Silence padding retained at each cut, in seconds (default: 0.1). Keeps speech onsets/offsets from sounding clipped. */
    float pad_seconds;
    bool smooth_audio; /**< Apply dynamic audio normalization (dynaudnorm) to smooth volume levels. */
    float target_lufs; /**< Target LUFS loudness normalization value. Set to 0.0f to disable. */

    /**< Target video encoder codec standard. Only used if smooth_audio or target_lufs requires a final re-encode pass. */
    const char* video_codec;

    /**< Performance preset configuration standard. Only used if a final re-encode pass occurs. */
    const char* video_preset;
    const char* audio_codec; /**< Target audio encoder codec standard. Only used if a final re-encode pass occurs. */
    int crf;                 /**< Constant Rate Factor quality target. Only used if a final re-encode pass occurs. */
    FfxCommonOpts common;    /**< Common options configuration block. */
} FfxDesilenceOpts;

/**
 * @brief Scans a media file's audio track for silent sections below a given decibel floor.
 * @param opts Configuration options structure.
 * @param result Pointer to pre-allocated results structure populated on success.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_silence_detect(const FfxSilenceDetectOpts* opts, FfxSilenceResult* result);

/**
 * @brief Formats and prints a space-separated list of silence intervals to stdout.
 * @param result Pointer to populated silence detection results.
 */
void ffx_silence_print(const FfxSilenceResult* result);

/**
 * @brief Removes silent intervals from media, joining non-silent blocks with optional leveling filters.
 * @param opts Configuration options structure.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_desilence(const FfxDesilenceOpts* opts);

#ifdef __cplusplus
}
#endif
#endif /* FFX_H */
