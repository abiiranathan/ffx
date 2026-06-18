/**
 * @file ffx_internal.h
 * @brief Private implementation details shared across ffx translation units.
 *
 * This header defines internal helper structures, dynamic argument vector builders,
 * utility functions, and CLI-execution abstractions. It is not intended for use
 * by external client applications.
 */

#ifndef FFX_INTERNAL_H
#define FFX_INTERNAL_H

#include <solidc/process.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ffx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The allocation step block size when expanding the dynamic argument vector. */
#define FFX_ARGV_GROW_BY 16

/**
 * @brief A dynamic array tracking strings to construct the final command-line argument vector.
 */
typedef struct {
    char** args;     /**< Null-terminated array of string arguments allocated on the heap. */
    size_t count;    /**< Current number of strings stored in the array. */
    size_t capacity; /**< Maximum number of pointers currently allocated for storage. */
} FfxArgv;

/**
 * @brief Initializes an empty dynamic argument array.
 * @param av Pointer to the FfxArgv structure to initialize.
 */
void ffx_argv_init(FfxArgv* av);

/**
 * @brief Deallocates all internal heap memory associated with an argument array.
 * @param av Pointer to the target FfxArgv structure.
 */
void ffx_argv_free(FfxArgv* av);

/**
 * @brief Appends a copy of a literal string to the argument vector.
 * @param av Pointer to the target FfxArgv structure.
 * @param arg Null-terminated string to append.
 * @return true on success, false if memory allocation failed.
 */
bool ffx_argv_push(FfxArgv* av, const char* arg);

/**
 * @brief Formats and appends a string to the argument vector using a printf-like format specifier.
 * @param av Pointer to the target FfxArgv structure.
 * @param fmt Printf-style format string.
 * @return true on success, false if formatting or memory allocation failed.
 */
bool ffx_argv_pushf(FfxArgv* av, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief Helper utility to push both a flag and its corresponding value to the argument list.
 * @details Equivalent to calling ffx_argv_push(av, flag) followed by ffx_argv_push(av, value).
 * @param av Pointer to the target FfxArgv structure.
 * @param flag The CLI option name (e.g., "-c:v").
 * @param value The value associated with the option (e.g., "libx264").
 * @return true if both strings were successfully appended, false on allocation failure.
 */
bool ffx_argv_push2(FfxArgv* av, const char* flag, const char* value);

/**
 * @brief Appends global options like log level, hardware acceleration, and override flags to the arguments.
 * @param av Pointer to the target FfxArgv structure.
 * @param co Pointer to the common options parameters.
 */
void ffx_argv_push_common(FfxArgv* av, const FfxCommonOpts* co);

/**
 * @brief Maps hardware acceleration settings and a requested codec to appropriate CLI flags.
 * @param av Pointer to the target FfxArgv structure.
 * @param codec Name of the video codec to select.
 * @param hw Hardware acceleration interface requested.
 */
void ffx_argv_push_video_codec(FfxArgv* av, const char* codec, FfxHwAccel hw);

/**
 * @brief Appends a flag paired with an integer value to the argument vector.
 * @param av Pointer to the target FfxArgv structure.
 * @param flag The CLI option name.
 * @param value Integer value to append.
 * @return true on success, false on allocation failure.
 */
bool ffx_argv_push_int(FfxArgv* av, const char* flag, int value);

/**
 * @brief Appends a flag paired with a bitrate format (e.g., "128k") to the argument vector.
 * @param av Pointer to the target FfxArgv structure.
 * @param flag The CLI option name.
 * @param value_kbps Bitrate value in kilobits per second.
 * @return true on success, false on allocation failure.
 */
bool ffx_argv_push_kbps(FfxArgv* av, const char* flag, int value_kbps);

/**
 * @brief Appends a flag paired with a floating-point value to the argument vector.
 * @param av Pointer to the target FfxArgv structure.
 * @param flag The CLI option name.
 * @param value Double-precision value to append.
 * @return true on success, false on allocation failure.
 */
bool ffx_argv_push_float(FfxArgv* av, const char* flag, double value);

/**
 * @brief Parameters for helper routines running single-video-filter commands.
 * @details Groups options for operations that share basic command-line construction patterns
 * (such as deinterlacing, sharpening, cropping, or rotating).
 */
typedef struct {
    const char* input;           /**< Path to input video file. Required. */
    const char* output;          /**< Path to output file destination. Required. */
    const char* vf_filter;       /**< The constructed video filter string (e.g., "unsharp=5:5:1.0"). */
    bool copy_audio;             /**< If true, copies the audio stream instead of re-encoding. */
    const char* audio_codec;     /**< Target audio codec, if re-encoding. */
    const char* video_codec;     /**< Target video codec. */
    const char* video_preset;    /**< Performance preset configuration standard. */
    int crf;                     /**< Constant Rate Factor quality target. */
    const FfxCommonOpts* common; /**< Common options parameters block. */
} FfxSingleVfOpts;

/**
 * @brief Assembles and executes an FFmpeg command with a single `-vf` entry.
 * @param opts Configuration block specifying filters, paths, and transcoding properties.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_run_single_vf(const FfxSingleVfOpts* opts);

/**
 * @brief Appends a suffix to an output file name to create an auxiliary or sidecar path.
 * @details Helps construct paths for two-pass encoding logs, temp files, or palettes.
 * @param output Original output file path.
 * @param suffix String suffix to insert (e.g., "_palette").
 * @param buf Output buffer to receive the generated path.
 * @param buflen Maximum size of the destination buffer.
 * @return true on success, false if the path exceeded the buffer length.
 */
bool ffx_build_sidecar_path(const char* output, const char* suffix, char* buf, size_t buflen);

/**
 * @brief Queries the duration of an input file in seconds using ffprobe.
 * @param input Path to target media file.
 * @param common Common options parameters block.
 * @param out_seconds Pointer to double where the retrieved duration will be written.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_probe_duration(const char* input, const FfxCommonOpts* common, double* out_seconds);

/**
 * @brief Spawns and awaits the execution of an FFmpeg/FFprobe process without capturing stdout.
 * @param av Populated dynamic argument vector configuration.
 * @param co Common options configuration parameters.
 * @return FFX_OK on successful exit (code 0), or a negative status code on failure.
 */
FfxStatus ffx_run(const FfxArgv* av, const FfxCommonOpts* co);

/**
 * @brief Spawns a process and captures its standard output stream in a buffer.
 * @param av Populated dynamic argument vector configuration.
 * @param co Common options configuration parameters.
 * @param out_buf Buffer to receive the standard output stream data.
 * @param out_buf_size Capacity of the provided capture buffer.
 * @param out_len Optional pointer populated with the actual bytes written to out_buf.
 * @return FFX_OK on success, or a negative status code on failure.
 */
FfxStatus ffx_run_capture(const FfxArgv* av, const FfxCommonOpts* co, char* out_buf, size_t out_buf_size,
                          size_t* out_len);

/**
 * @brief Internal inline initializer helper that locates the required binary and sets up the base command line.
 * @param av Pointer to the target FfxArgv structure.
 * @param binary_name Name of the target binary to find and execute ("ffmpeg" or "ffprobe").
 * @param co Common options structure configuration.
 * @return FFX_OK on success, or a negative status code on failure.
 */
static inline FfxStatus ffx_argv_begin(FfxArgv* av, const char* binary_name, const FfxCommonOpts* co) {
    char bin_path[FFX_MAX_OPT_LEN];
    bool found = false;

    if (strcmp(binary_name, "ffprobe") == 0) {
        found = ffx_find_ffprobe(bin_path, sizeof(bin_path));
    } else {
        found = ffx_find_ffmpeg(bin_path, sizeof(bin_path));
    }

    if (!found) {
        fprintf(stderr, "ffx: '%s' not found in PATH\n", binary_name);
        return FFX_ERR_FFMPEG_NOT_FOUND;
    }

    if (!ffx_argv_push(av, bin_path)) { return FFX_ERR_MEMORY; }

    if (co != NULL) {
        if (strcmp(binary_name, "ffmpeg") == 0) {
            ffx_argv_push_common(av, co);
        } else {
            /* For ffprobe and other binaries: only inject the caller's extra_flags.
             * ffprobe does not accept ffmpeg's -y, -stats, or -hwaccel options. */
            for (size_t i = 0; i < co->extra_flags_count; ++i) {
                if (co->extra_flags[i] != NULL) { ffx_argv_push(av, co->extra_flags[i]); }
            }
        }
    }
    return FFX_OK;
}

#ifdef __cplusplus
}
#endif
#endif /* FFX_INTERNAL_H */
