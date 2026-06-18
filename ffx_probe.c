/**
 * @file ffx_probe.c
 * @brief ffx_probe() — stream metadata via ffprobe's JSON output.
 *
 * ffprobe is invoked with -print_format json -show_format -show_streams.
 * Rather than pulling in a full JSON library we walk the flat JSON output
 * with targeted strstr() / sscanf() searches.  This is intentionally
 * simple and brittle only for keys we recognise; unknown keys are silently
 * ignored.
 */

#include "ffx_internal.h"

#include <stdio.h>   // fprintf, snprintf, printf
#include <stdlib.h>  // strtoll, strtod
#include <string.h>  // strstr, sscanf

/* =========================================================================
 * Minimal JSON value extractor
 * ========================================================================= */

/**
 * Copy string safely with guaranteed null-termination, avoiding strncpy warnings.
 */
static void safe_copy(char* dest, const char* src, size_t dest_size) {
    if (dest_size == 0) return;
    size_t i = 0;
    for (; i < dest_size - 1 && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/**
 * Find the value string for @p key inside a flat JSON object fragment.
 * Copies the extracted (unescaped) value into @p out_buf.
 *
 * Only handles values that are JSON strings ("...") or bare numbers.
 * This is deliberately minimal — ffprobe's output is regular enough.
 *
 * @param json     The JSON text to search.
 * @param key      Exact key name to find.
 * @param out_buf  Destination buffer.
 * @param buflen   Size of @p out_buf.
 * @return true if the key was found and its value fit in @p out_buf.
 */
static bool json_find_str(const char* json, const char* key, char* out_buf, size_t buflen) {
    if (json == NULL || key == NULL || out_buf == NULL || buflen == 0) { return false; }

    /* Build the search pattern: "key" : */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (p == NULL) { return false; }

    /* Advance past the key and the colon. */
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p == ':') { ++p; }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }

    if (*p == '"') {
        /* String value. */
        ++p;
        size_t i = 0;
        while (*p && *p != '"' && i < buflen - 1) {
            if (*p == '\\') { ++p; /* Skip escape character; copy the next char raw. */ }
            out_buf[i++] = *p++;
        }
        out_buf[i] = '\0';
        return i > 0 || *p == '"'; /* Empty string is valid. */
    } else {
        /* Bare number or keyword (null, true, false). */
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && *p != '\n' && i < buflen - 1) {
            out_buf[i++] = *p++;
        }
        /* Trim trailing whitespace. */
        while (i > 0 && (out_buf[i - 1] == ' ' || out_buf[i - 1] == '\r' || out_buf[i - 1] == '\n')) {
            --i;
        }
        out_buf[i] = '\0';
        return i > 0;
    }
}

/** Extract a double from a JSON key. Returns @p def_val if not found. */
static double json_get_double(const char* json, const char* key, double def_val) {
    char buf[64];
    if (!json_find_str(json, key, buf, sizeof(buf))) { return def_val; }
    char* end = NULL;
    double v = strtod(buf, &end);
    return (end != buf) ? v : def_val;
}

/** Extract a long long from a JSON key. Returns @p def_val if not found. */
static long long json_get_ll(const char* json, const char* key, long long def_val) {
    char buf[32];
    if (!json_find_str(json, key, buf, sizeof(buf))) { return def_val; }
    char* end = NULL;
    long long v = strtoll(buf, &end, 10);
    return (end != buf) ? v : def_val;
}

/**
 * Parse the AVRational string "num/den" (ffprobe's r_frame_rate) to a double.
 * Returns 0.0 if parsing fails.
 */
static double parse_rational(const char* s) {
    if (s == NULL || *s == '\0') { return 0.0; }
    long long num = 0, den = 0;
    if (sscanf(s, "%lld/%lld", &num, &den) == 2 && den != 0) { return (double)num / (double)den; }
    /* It might be a plain decimal. */
    char* end = NULL;
    double v = strtod(s, &end);
    return (end != s) ? v : 0.0;
}

/* =========================================================================
 * Stream block parser
 * ========================================================================= */

/**
 * Find the n-th stream object block inside the "streams" JSON array.
 * Returns a pointer into @p json, or NULL if there is no n-th stream.
 */
static const char* find_nth_stream(const char* json, int n) {
    const char* streams_start = strstr(json, "\"streams\"");
    if (streams_start == NULL) { return NULL; }
    /* Advance to the opening '[' of the streams array. */
    const char* p = strchr(streams_start + 9, '[');
    if (p == NULL) { return NULL; }
    ++p; /* Skip '['. */

    int found = 0;
    while (*p) {
        while (*p && *p != '{') {
            ++p;
        }
        if (*p == '\0') { break; }
        if (found == n) { return p; }
        /* Skip this block by matching braces. */
        int depth = 0;
        do {
            if (*p == '{') {
                ++depth;
            } else if (*p == '}') {
                --depth;
            }
            ++p;
        } while (*p && depth > 0);
        ++found;
    }
    return NULL;
}

/**
 * Parse one stream JSON block starting at @p block into @p si.
 */
static void parse_stream(const char* block, FfxStreamInfo* si) {
    char buf[128];

    si->index = (int)json_get_ll(block, "index", -1);

    if (json_find_str(block, "codec_name", buf, sizeof(buf))) {
        safe_copy(si->codec_name, buf, sizeof(si->codec_name));
    }
    if (json_find_str(block, "codec_type", buf, sizeof(buf))) {
        safe_copy(si->codec_type, buf, sizeof(si->codec_type));
    }
    if (json_find_str(block, "pix_fmt", buf, sizeof(buf))) {
        safe_copy(si->pixel_format, buf, sizeof(si->pixel_format));
    }
    if (json_find_str(block, "channel_layout", buf, sizeof(buf))) {
        safe_copy(si->channel_layout, buf, sizeof(si->channel_layout));
    }

    si->width = (int)json_get_ll(block, "width", 0);
    si->height = (int)json_get_ll(block, "height", 0);
    si->bit_rate = json_get_ll(block, "bit_rate", 0);
    si->sample_rate = (int)json_get_ll(block, "sample_rate", 0);
    si->channels = (int)json_get_ll(block, "channels", 0);

    si->duration_seconds = json_get_double(block, "duration", 0.0);

    /* r_frame_rate is more reliable than avg_frame_rate for constant fps. */
    if (json_find_str(block, "r_frame_rate", buf, sizeof(buf))) { si->fps = parse_rational(buf); }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

#define PROBE_BUF_SIZE (4 * 1024 * 1024)

FfxStatus ffx_probe(const FfxProbeOpts* opts, FfxProbeResult* result) {
    if (opts == NULL || opts->input == NULL || result == NULL) { return FFX_ERR_INVALID_ARGUMENT; }

    memset(result, 0, sizeof(*result));

    FfxArgv av;
    ffx_argv_init(&av);

    FfxStatus st = ffx_argv_begin(&av, "ffprobe", &opts->common);
    if (st != FFX_OK) {
        ffx_argv_free(&av);
        return st;
    }

    ffx_argv_push2(&av, "-v", "quiet");
    ffx_argv_push2(&av, "-print_format", "json");
    ffx_argv_push(&av, "-show_format");
    ffx_argv_push(&av, "-show_streams");
    ffx_argv_push(&av, opts->input);

    char* buf = malloc(PROBE_BUF_SIZE);
    if (buf == NULL) {
        ffx_argv_free(&av);
        return FFX_ERR_MEMORY;
    }

    size_t out_len = 0;
    FfxStatus run_st = ffx_run_capture(&av, &opts->common, buf, PROBE_BUF_SIZE, &out_len);
    ffx_argv_free(&av);

    if (run_st != FFX_OK) {
        free(buf);
        return run_st;
    }

    if (opts->print_json) {
        fwrite(buf, 1, out_len, stdout);
        fputc('\n', stdout);
    }

    /* ---- Parse format block --------------------------------------- */
    const char* fmt_block = strstr(buf, "\"format\"");
    if (fmt_block != NULL) {
        char tmp[128];
        if (json_find_str(fmt_block, "format_name", tmp, sizeof(tmp))) {
            safe_copy(result->format_name, tmp, sizeof(result->format_name));
        }
        result->duration_seconds = json_get_double(fmt_block, "duration", 0.0);
        result->size_bytes = json_get_ll(fmt_block, "size", 0);
        result->bit_rate = json_get_ll(fmt_block, "bit_rate", 0);
    }

    /* ---- Parse each stream --------------------------------------- */
    int idx = 0;
    while (idx < FFX_MAX_STREAMS) {
        const char* stream_block = find_nth_stream(buf, idx);
        if (stream_block == NULL) { break; }
        parse_stream(stream_block, &result->streams[idx]);
        ++idx;
    }
    result->stream_count = idx;

    free(buf);
    return (result->stream_count > 0 || result->duration_seconds > 0.0) ? FFX_OK : FFX_ERR_PROBE_PARSE;
}

void ffx_probe_print(const FfxProbeResult* result) {
    if (result == NULL) { return; }

    printf("Container : %s\n", result->format_name[0] ? result->format_name : "(unknown)");
    printf("Duration  : %.3f s\n", result->duration_seconds);
    printf("Size      : %.2f MiB (%lld bytes)\n", (double)result->size_bytes / (1024.0 * 1024.0), result->size_bytes);
    printf("Bitrate   : %.0f kbps\n", (double)result->bit_rate / 1000.0);
    printf("Streams   : %d\n\n", result->stream_count);

    for (int i = 0; i < result->stream_count; ++i) {
        const FfxStreamInfo* s = &result->streams[i];
        printf("  [%d] %s — %s\n", s->index, s->codec_type, s->codec_name);

        if (strcmp(s->codec_type, "video") == 0) {
            printf("       %dx%d @ %.2f fps  pix_fmt=%s\n", s->width, s->height, s->fps,
                   s->pixel_format[0] ? s->pixel_format : "?");
        } else if (strcmp(s->codec_type, "audio") == 0) {
            printf("       %d Hz  %s  %d ch\n", s->sample_rate, s->channel_layout[0] ? s->channel_layout : "?",
                   s->channels);
        }
        if (s->bit_rate > 0) { printf("       bitrate: %.0f kbps\n", (double)s->bit_rate / 1000.0); }
        printf("       duration: %.3f s\n", s->duration_seconds);
    }
}
