/**
 * @file ffx_core.c
 * @brief Internal helpers shared by all ffx subcommand implementations.
 *
 * This translation unit manages:
 *   - FfxArgv: A growable argv builder providing memory-safe dynamic array helpers.
 *   - Binary discovery: Robust checks across path environments and platform fallbacks.
 *   - Execution layers: Interfaces to spawn FFmpeg/FFprobe and process results safely.
 *   - Common helper routines: Translating structures into CLI parameters to minimize
 *     redundancy in commands.
 */

#include "ffx_internal.h"

#include <errno.h>   // errno, ENOENT
#include <stdarg.h>  // va_list
#include <stdio.h>   // fprintf, snprintf
#include <stdlib.h>  // malloc, free, getenv
#include <string.h>  // strlen, strncpy, memset

/* =========================================================================
 * Binary discovery
 * ========================================================================= */

/** 
 * @brief Candidate search directories used when the binary is not located in the system PATH.
 * Includes typical installation paths for Unix, macOS (MacPorts and Homebrew Apple Silicon).
 */
static const char* const k_extra_dirs[] = {"/usr/bin", "/usr/local/bin",
                                           "/opt/homebrew/bin",  // Apple Silicon Homebrew
                                           "/opt/local/bin",     // MacPorts
                                           NULL};

/**
 * @brief Verifies whether a given file path points to an executable.
 * @details Leverages access(2) with X_OK on POSIX environments and _access on Windows systems.
 * @param path The absolute or relative file path to verify.
 * @return true if the path points to an executable, false otherwise.
 */
static bool is_executable(const char* path) {
#ifdef _WIN32
    return _access(path, 4) == 0;  // 4 indicates read permission, sufficient for verification on Windows
#else
    return access(path, X_OK) == 0;
#endif
}

/**
 * @brief Searches the environment PATH and fallback directories to locate a target binary.
 * @param binary The name of the binary to search for (e.g., "ffmpeg").
 * @param buf The output buffer to store the absolute path of the located binary.
 * @param buflen The allocated length of the output path buffer.
 * @return true if the binary is found and verified as executable, false otherwise.
 */
static bool find_binary(const char* binary, char* buf, size_t buflen) {
    if (binary == NULL || buf == NULL || buflen == 0) { return false; }

    /* 1. Retrieve and parse the system PATH environment variable. */
    const char* path_env = getenv("PATH");
    if (path_env != NULL) {
        /* Duplicate path env to perform safe thread-safe tokenization. */
        char path_copy[4096];
        strncpy(path_copy, path_env, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

#ifdef _WIN32
        const char* delim = ";";
#else
        const char* delim = ":";
#endif
        char* save = NULL;
        char* dir = strtok_r(path_copy, delim, &save);
        while (dir != NULL) {
            int n = snprintf(buf, buflen, "%s/%s", dir, binary);
            if (n > 0 && (size_t)n < buflen && is_executable(buf)) { return true; }
            dir = strtok_r(NULL, delim, &save);
        }
    }

    /* 2. Search predefined fallback installation locations. */
    for (size_t i = 0; k_extra_dirs[i] != NULL; ++i) {
        int n = snprintf(buf, buflen, "%s/%s", k_extra_dirs[i], binary);
        if (n > 0 && (size_t)n < buflen && is_executable(buf)) { return true; }
    }

    buf[0] = '\0';
    return false;
}

bool ffx_find_ffmpeg(char* buf, size_t buflen) {
    return find_binary("ffmpeg", buf, buflen);
}

bool ffx_find_ffprobe(char* buf, size_t buflen) {
    return find_binary("ffprobe", buf, buflen);
}

/* =========================================================================
 * FfxArgv — growable argv builder
 * ========================================================================= */

void ffx_argv_init(FfxArgv* av) {
    memset(av, 0, sizeof(*av));
}

void ffx_argv_free(FfxArgv* av) {
    if (av == NULL) { return; }
    for (size_t i = 0; i < av->count; ++i) {
        free(av->args[i]);
    }
    free(av->args);
    memset(av, 0, sizeof(*av));
}

/**
 * @brief Expands the memory capacity of the dynamic argument vector list.
 * @details Allocates memory in blocks defined by FFX_ARGV_GROW_BY. Tracks the capacity 
 * index and appends an extra element to maintain a trailing NULL sentinel.
 * @param av Pointer to the target FfxArgv structure to grow.
 * @return true if memory re-allocation succeeded, false on allocation failure.
 */
static bool argv_grow(FfxArgv* av) {
    size_t new_cap = av->capacity + FFX_ARGV_GROW_BY;
    /* Allocate space for new capacity pointers + 1 for trailing NULL sentinel */
    char** new_args = realloc(av->args, (new_cap + 1) * sizeof(char*));
    if (new_args == NULL) { return false; }
    av->args = new_args;
    av->capacity = new_cap;
    return true;
}

bool ffx_argv_push(FfxArgv* av, const char* arg) {
    if (av == NULL || arg == NULL) { return false; }
    if (av->count >= av->capacity) {
        if (!argv_grow(av)) { return false; }
    }
    av->args[av->count] = strdup(arg);
    if (av->args[av->count] == NULL) { return false; }
    av->count++;
    av->args[av->count] = NULL;  // Keep the NULL sentinel updated
    return true;
}

bool ffx_argv_pushf(FfxArgv* av, const char* fmt, ...) {
    if (av == NULL || fmt == NULL) { return false; }

    char buf[FFX_MAX_OPT_LEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(buf)) {
        /* Truncated or encoding error — treat as a failure to prevent
         * passing malformed or partial flag strings to the sub-process. */
        return false;
    }
    return ffx_argv_push(av, buf);
}

bool ffx_argv_push2(FfxArgv* av, const char* flag, const char* value) {
    return ffx_argv_push(av, flag) && ffx_argv_push(av, value);
}

/* =========================================================================
 * Numeric/flag push helpers
 *
 * These encapsulate the conversion, buffer formatting, and array injection
 * patterns for numeric parameters such as CRF, bitrate, and frame rates.
 * ========================================================================= */

bool ffx_argv_push_int(FfxArgv* av, const char* flag, int value) {
    if (av == NULL || flag == NULL) { return false; }
    return ffx_argv_push(av, flag) && ffx_argv_pushf(av, "%d", value);
}

bool ffx_argv_push_kbps(FfxArgv* av, const char* flag, int value_kbps) {
    if (av == NULL || flag == NULL) { return false; }
    return ffx_argv_push(av, flag) && ffx_argv_pushf(av, "%dk", value_kbps);
}

bool ffx_argv_push_float(FfxArgv* av, const char* flag, double value) {
    if (av == NULL || flag == NULL) { return false; }
    return ffx_argv_push(av, flag) && ffx_argv_pushf(av, "%.6g", value);
}

/* =========================================================================
 * Common flag helpers
 * ========================================================================= */

/**
 * @brief Maps internal FfxLogLevel values to corresponding FFmpeg loglevel strings.
 * @param lvl The requested library log level.
 * @return A static string mapped to the expected CLI value.
 */
static const char* loglevel_str(FfxLogLevel lvl) {
    switch (lvl) {
        case FFX_LOG_QUIET:
            return "quiet";
        case FFX_LOG_ERROR:
            return "error";
        case FFX_LOG_WARNING:
            return "warning";
        case FFX_LOG_INFO:
            return "info";
        case FFX_LOG_VERBOSE:
            return "verbose";
        case FFX_LOG_DEBUG:
            return "debug";
        default:
            return "error";
    }
}

void ffx_argv_push_common(FfxArgv* av, const FfxCommonOpts* co) {
    if (av == NULL || co == NULL) { return; }

    ffx_argv_push2(av, "-loglevel", loglevel_str(co->log_level));

    /* The -stats option controls FFmpeg's interactive console output (such as frame,
     * fps, and progress). It is appended whenever log_level is not FFX_LOG_QUIET,
     * allowing progress feedback on interactive terminals. */
    if (co->log_level != FFX_LOG_QUIET) { ffx_argv_push(av, "-stats"); }

    if (co->overwrite) { ffx_argv_push(av, "-y"); }

    /* Configure decoder hardware acceleration. Custom encoders handle encoder-side acceleration. */
    switch (co->hw_accel) {
        case FFX_HW_NVENC:
            ffx_argv_push2(av, "-hwaccel", "cuda");
            break;
        case FFX_HW_QSV:
            ffx_argv_push2(av, "-hwaccel", "qsv");
            break;
        case FFX_HW_VIDEOTOOLBOX:
            ffx_argv_push2(av, "-hwaccel", "videotoolbox");
            break;
        case FFX_HW_VAAPI:
            ffx_argv_push2(av, "-hwaccel", "vaapi");
            break;
        case FFX_HW_AMF:
            ffx_argv_push2(av, "-hwaccel", "d3d11va");
            break;
        case FFX_HW_NONE:
        default:
            break;
    }

    /* Inject user-specified custom flags directly. */
    for (size_t i = 0; i < co->extra_flags_count; ++i) {
        if (co->extra_flags[i] != NULL) { ffx_argv_push(av, co->extra_flags[i]); }
    }
}

/**
 * @brief Appends appropriate hardware or software encoder codec strings based on hardware selections.
 * @details Defaults to "libx264" if no hardware target is configured and no specific codec is requested.
 * @param av The dynamic argument builder to mutate.
 * @param codec Optional string identifier of a explicit encoder. If NULL, auto-detects based on hw.
 * @param hw The hardware backend selected.
 */
void ffx_argv_push_video_codec(FfxArgv* av, const char* codec, FfxHwAccel hw) {
    if (av == NULL) { return; }

    /* If an explicit codec is specified, defer to user choice. */
    if (codec != NULL) {
        ffx_argv_push2(av, "-c:v", codec);
        return;
    }

    /* Automatically select a codec based on the hardware acceleration backend. */
    switch (hw) {
        case FFX_HW_NVENC:
            ffx_argv_push2(av, "-c:v", "h264_nvenc");
            break;
        case FFX_HW_QSV:
            ffx_argv_push2(av, "-c:v", "h264_qsv");
            break;
        case FFX_HW_VIDEOTOOLBOX:
            ffx_argv_push2(av, "-c:v", "h264_videotoolbox");
            break;
        case FFX_HW_VAAPI:
            ffx_argv_push2(av, "-c:v", "h264_vaapi");
            break;
        case FFX_HW_AMF:
            ffx_argv_push2(av, "-c:v", "h264_amf");
            break;
        case FFX_HW_NONE:
        default:
            ffx_argv_push2(av, "-c:v", "libx264");
            break;
    }
}

/* =========================================================================
 * Sidecar path helper
 * ========================================================================= */

bool ffx_build_sidecar_path(const char* output, const char* suffix, char* buf, size_t buflen) {
    if (output == NULL || suffix == NULL || buf == NULL || buflen == 0) { return false; }
    int n = snprintf(buf, buflen, "%s%s", output, suffix);
    return n > 0 && (size_t)n < buflen;
}

/* =========================================================================
 * Probe-for-duration helper
 * ========================================================================= */

FfxStatus ffx_probe_duration(const char* input, const FfxCommonOpts* common, double* out_seconds) {
    if (input == NULL || out_seconds == NULL) { return FFX_ERR_INVALID_ARGUMENT; }
    *out_seconds = 0.0;

    FfxProbeOpts probe_opts = {
        .input = input,
        .print_json = false,
        .common = (common != NULL) ? *common : ffx_common_opts_default(),
    };
    /* Always silence metadata probing to prevent polling output from clobbering progress metrics. */
    probe_opts.common.log_level = FFX_LOG_QUIET;

    FfxProbeResult result = {0};
    FfxStatus st = ffx_probe(&probe_opts, &result);
    if (st != FFX_OK) { return st; }
    if (result.duration_seconds <= 0.0) { return FFX_ERR_PROBE_PARSE; }

    *out_seconds = result.duration_seconds;
    return FFX_OK;
}

/* =========================================================================
 * Shared single-filter command runner
 * ========================================================================= */

FfxStatus ffx_run_single_vf(const FfxSingleVfOpts* opts) {
    if (opts == NULL || opts->input == NULL || opts->output == NULL || opts->vf_filter == NULL ||
        opts->common == NULL) {
        return FFX_ERR_INVALID_ARGUMENT;
    }

    FfxArgv av;
    ffx_argv_init(&av);

    FfxStatus st = ffx_argv_begin(&av, "ffmpeg", opts->common);
    if (st != FFX_OK) { goto done; }

    ffx_argv_push2(&av, "-i", opts->input);
    ffx_argv_push2(&av, "-vf", opts->vf_filter);

    ffx_argv_push_video_codec(&av, opts->video_codec, opts->common->hw_accel);
    if (opts->video_preset != NULL) { ffx_argv_push2(&av, "-preset", opts->video_preset); }

    const int crf = (opts->crf > 0) ? opts->crf : 18;
    ffx_argv_push_int(&av, "-crf", crf);

    if (opts->copy_audio) {
        ffx_argv_push2(&av, "-c:a", "copy");
    } else {
        const char* ac = (opts->audio_codec != NULL) ? opts->audio_codec : "aac";
        ffx_argv_push2(&av, "-c:a", ac);
    }

    ffx_argv_push(&av, opts->output);
    st = ffx_run(&av, opts->common);

done:
    ffx_argv_free(&av);
    return st;
}

/* =========================================================================
 * Process runner
 * ========================================================================= */

/**
 * @brief Prints the constructed arguments list to stderr for debug tracking or dry-run evaluation.
 * @param args Array of argument strings.
 * @param count Number of arguments stored in the array.
 */
static void print_argv(const char* const* args, size_t count) {
    fprintf(stderr, "[ffx dry-run]");
    for (size_t i = 0; i < count; ++i) {
        /* Add enclosing quotes for arguments containing white spaces to improve readability. */
        if (args[i] != NULL && strchr(args[i], ' ') != NULL) {
            fprintf(stderr, " '%s'", args[i]);
        } else {
            fprintf(stderr, " %s", args[i] != NULL ? args[i] : "(null)");
        }
    }
    fputc('\n', stderr);
}

FfxStatus ffx_run(const FfxArgv* av, const FfxCommonOpts* co) {
    if (av == NULL || av->args == NULL || av->count == 0) { return FFX_ERR_INVALID_ARGUMENT; }

    if (co != NULL && co->dry_run) {
        print_argv((const char* const*)av->args, av->count);
        return FFX_OK;
    }

    /* Locate the executable binary. */
    const char* binary = av->args[0];

    ProcessHandle* handle = NULL;
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

    ProcessError pe = process_create(&handle, binary, (const char* const*)av->args, &options);
    if (pe != PROCESS_SUCCESS) {
        fprintf(stderr, "ffx: failed to spawn '%s': %s\n", binary, process_error_string(pe));
        return FFX_ERR_SPAWN_FAILED;
    }

    ProcessResult result = {0};
    pe = process_wait(handle, &result, -1 /* wait indefinitely */);
    process_free(handle);

    if (pe != PROCESS_SUCCESS) {
        fprintf(stderr, "ffx: process_wait failed: %s\n", process_error_string(pe));
        return FFX_ERR_SPAWN_FAILED;
    }

    if (!result.exited_normally || result.exit_code != 0) {
        fprintf(stderr, "ffx: ffmpeg exited with code %d\n", result.exit_code);
        return FFX_ERR_FFMPEG_FAILED;
    }

    return FFX_OK;
}

FfxStatus ffx_run_capture(const FfxArgv* av, const FfxCommonOpts* co, char* out_buf, size_t out_buf_size,
                          size_t* out_len) {
    if (av == NULL || out_buf == NULL || out_buf_size == 0) { return FFX_ERR_INVALID_ARGUMENT; }

    if (co != NULL && co->dry_run) {
        print_argv((const char* const*)av->args, av->count);
        out_buf[0] = '\0';
        if (out_len != NULL) { *out_len = 0; }
        return FFX_OK;
    }

    PipeHandle* stdout_pipe = NULL;
    ProcessHandle* handle = NULL;

    /* Initialize a pipe structure to redirect and intercept standard output. */
    if (pipe_create(&stdout_pipe) != PROCESS_SUCCESS) { return FFX_ERR_IO; }

    ProcessOptions options = {
        .working_directory = NULL,
        .inherit_environment = true,
        .environment = NULL,
        .detached = false,
        .io =
            {
                .stdin_pipe = NULL,
                .stdout_pipe = stdout_pipe, /* Map child output to write end of pipe */
                .stderr_pipe = NULL,
                .merge_stderr = false,
            },
    };

    FfxStatus status = FFX_OK;

    ProcessError pe = process_create(&handle, av->args[0], (const char* const*)av->args, &options);
    if (pe != PROCESS_SUCCESS) {
        status = FFX_ERR_SPAWN_FAILED;
        goto cleanup_pipe;
    }

    /* Close write end in the parent context to trigger EOF once child process terminates. */
    pipe_close_write_end(stdout_pipe);

    /* Read and capture stdout stream output directly into the provided output memory buffer. */
    size_t total = 0;
    while (total < out_buf_size - 1) {
        size_t nread = 0;
        ProcessError re = pipe_read(stdout_pipe, out_buf + total, out_buf_size - 1 - total, &nread, -1);
        if (re == PROCESS_ERROR_PIPE_CLOSED || nread == 0) { break; }
        if (re != PROCESS_SUCCESS) {
            status = FFX_ERR_IO;
            break;
        }
        total += nread;
    }
    out_buf[total] = '\0';
    if (out_len != NULL) { *out_len = total; }

    ProcessResult result = {0};
    process_wait(handle, &result, -1);
    process_free(handle);

    if (!result.exited_normally || result.exit_code != 0) { status = FFX_ERR_FFMPEG_FAILED; }

cleanup_pipe:
    pipe_close(stdout_pipe);
    return status;
}

/* =========================================================================
 * Status strings
 * ========================================================================= */

const char* ffx_status_str(FfxStatus status) {
    switch (status) {
        case FFX_OK:
            return "success";
        case FFX_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case FFX_ERR_FFMPEG_NOT_FOUND:
            return "ffmpeg binary not found";
        case FFX_ERR_SPAWN_FAILED:
            return "failed to spawn process";
        case FFX_ERR_FFMPEG_FAILED:
            return "ffmpeg exited with an error";
        case FFX_ERR_IO:
            return "I/O error";
        case FFX_ERR_MEMORY:
            return "memory allocation failed";
        case FFX_ERR_UNSUPPORTED:
            return "unsupported option combination";
        case FFX_ERR_PROBE_PARSE:
            return "failed to parse ffprobe output";
        default:
            return "unknown error";
    }
}
