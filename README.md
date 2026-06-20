# ffx 

Robust and intuitive ffmpeg command-line wrapper.

This document provides a detailed overview of `main.c`, the entry point for the `ffx` tool. `ffx` is a command-line wrapper built in C that simplifies access to FFmpeg. It abstracts complex FFmpeg arguments, filters, and stream mappings into logical, user-friendly subcommands.

---

## 1. Architectural Overview & Context flow

The execution lifecycle of `ffx` follows a structured sequence:

```
[CLI Arguments (argc/argv)] 
        │
        ▼
 [FlagParser (Root)] ────► [pre_invoke()] ──► [Populate AppCtx & FfxCommonOpts]
        │
        ▼
[Match Subcommand] ──────► [Execute Subcommand Callback (e.g., cmd_convert)]
                                │
                                ▼
                       [Map to ffx_* Library API]
                                │
                                ▼
                       [Execute FFmpeg Commands]
```

### Global Context (`AppCtx`)
The program defines a shared context structure, `AppCtx`, which stores configuration properties that apply globally across all subcommands.

```c
typedef struct {
    bool verbose;               // Enables output logging from the underlying FFmpeg process
    bool dry_run;               // Prints the generated FFmpeg command instead of running it
    bool no_overwrite;          // Instructs FFmpeg not to overwrite existing output files
    const char* hw_accel_str;   // Target hardware acceleration platform string
    FfxCommonOpts common;       // The library-level shared options structure passed to functions
} AppCtx;
```

### Pre-Invocation Configuration
Before a selected subcommand is invoked, `flag_set_pre_invoke` runs the helper function `pre_invoke`. This function initializes the global configuration properties, mapping CLI flags to the internal structures.

```c
static void pre_invoke(void* user_data) {
    AppCtx* ctx = (AppCtx*)user_data;
    ctx->common = ffx_common_opts_default();
    if (ctx->verbose) { 
        ctx->common.log_level = FFX_LOG_INFO; 
    }
    ctx->common.dry_run = ctx->dry_run;
    ctx->common.overwrite = !ctx->no_overwrite;
    ctx->common.hw_accel = parse_hw_accel(ctx->hw_accel_str);
}
```

---

## 2. Global Options and Hardware Acceleration

These options must be provided before the subcommand in the command-line call:

```bash
ffx [global-options] <subcommand> [subcommand-options]
```

| Flag             | Short | Type    | Description                                                        | FFmpeg Equivalence                    |
| :--------------- | :---- | :------ | :----------------------------------------------------------------- | :------------------------------------ |
| `--verbose`      | `-v`  | Boolean | Displays output and logging messages from FFmpeg.                  | Default: quiet; Set: `-loglevel info` |
| `--dry-run`      | `-n`  | Boolean | Prints the constructed command line to `stdout` without execution. | N/A (Internal debug helper)           |
| `--no-overwrite` | None  | Boolean | Prevents writing over files that already exist on disk.            | `-n` flag in standard FFmpeg          |
| `--hw`           | None  | String  | Configures hardware-accelerated processing.                        | `-hwaccel <device>`                   |

### Hardware Acceleration Mapping (`parse_hw_accel`)
The `--hw` flag maps a string value to the `FfxHwAccel` enum:

| Input String   | Library Enum          | Hardware Platform Target                    |
| :------------- | :-------------------- | :------------------------------------------ |
| `nvenc`        | `FFX_HW_NVENC`        | NVIDIA CUDA Hardware Video Encoder          |
| `qsv`          | `FFX_HW_QSV`          | Intel Quick Sync Video                      |
| `vt`           | `FFX_HW_VIDEOTOOLBOX` | Apple VideoToolbox (macOS/iOS)              |
| `vaapi`        | `FFX_HW_VAAPI`        | Video Acceleration API (Linux/Unix systems) |
| `amf`          | `FFX_HW_AMF`          | AMD Advanced Media Framework                |
| *None/Invalid* | `FFX_HW_NONE`         | Standard software-based processing (CPU)    |

---

## 3. Subcommands, Structs, and Flag Mappings

Each subcommand is configured via a dedicated global storage structure and a mapping function.

---

### 3.1. `convert`
Transcodes audio/video files between formats, codecs, resolutions, or bitrates.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;          const char* output;
        const char* video_codec;    const char* preset;
        const char* audio_codec;    const char* resolution;
        const char* format;         int32_t crf;
        int32_t vbitrate;           int32_t abitrate;
        int32_t sample_rate;        int32_t channels;
        float fps;                  bool no_video;
        bool no_audio;              bool stream_copy;
    } s_convert;
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Path to the source file.
    *   `-o`, `--output` (Required, String): Path for the transcoded output.
    *   `-V`, `--video-codec` (String): Specifies the video encoder (e.g., `libx264`, `libx265`, `vp9`, `copy`). Maps to FFmpeg `-c:v`.
    *   `-p`, `--preset` (String): Sets encoder performance/quality trade-off (e.g., `ultrafast`, `medium`, `veryslow`). Maps to FFmpeg `-preset`.
    *   `--crf` (Int32, Default: -1): Constant Rate Factor. Controls quality in variable bitrate encodings (0-51). Maps to FFmpeg `-crf`.
    *   `--video-bitrate` (Int32): Target video bitrate in kbps. Maps to FFmpeg `-b:v`.
    *   `--resolution` (String): Resizes output (e.g., `1920x1080`). Maps to FFmpeg `-s` or `scale` filter.
    *   `--fps` (Float): Changes output framework target frame rate. Maps to FFmpeg `-r` or `fps` filter.
    *   `-A`, `--audio-codec` (String): Target audio encoder (e.g., `aac`, `libmp3lame`, `copy`). Maps to FFmpeg `-c:a`.
    *   `--audio-bitrate` (Int32): Target audio bitrate in kbps. Maps to FFmpeg `-b:a`.
    *   `--sample-rate` (Int32): Audio sample rate in Hz (e.g., `44100`, `48000`). Maps to FFmpeg `-ar`.
    *   `--channels` (Int32): Set layout channel count (e.g. `1` for mono, `2` for stereo). Maps to FFmpeg `-ac`.
    *   `-f`, `--format` (String): Force container format. Maps to FFmpeg `-f`.
    *   `--no-video` (Boolean): Drops the video stream. Maps to FFmpeg `-vn`.
    *   `--no-audio` (Boolean): Drops the audio stream. Maps to FFmpeg `-an`.
    *   `-c`, `--stream-copy` (Boolean): Enables direct stream copying without transcoding. Maps to FFmpeg `-c copy`.

---

### 3.2. `trim`
Extracts a segment of a media file.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;   const char* output;
        const char* start;   const char* end;
        bool duration;       bool accurate;
    } s_trim;
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input file path.
    *   `-o`, `--output` (Required, String): Output file path.
    *   `-s`, `--start` (String): Starting time position (e.g., `00:01:30` or seconds `90`). Maps to FFmpeg `-ss`.
    *   `-e`, `--end` (String): End position or duration. Maps to FFmpeg `-to` (or `-t` if used with `--duration`).
    *   `-d`, `--duration` (Boolean): When active, interprets `--end` as duration instead of an absolute timestamp.
    *   `-a`, `--accurate` (Boolean): Forces precise transcoding-based seek over fast keyframe seek.

---

### 3.3. `compress`
Reduces file size to fit a specific target size in megabytes.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;    const char* output;
        const char* codec;    const char* preset;
        float size_mb;        int32_t abitrate;
        bool copy_audio;
    } s_compress = {.abitrate = 128};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video.
    *   `-o`, `--output` (Required, String): Output video.
    *   `-s`, `--size-mb` (Required, Float): Desired output size target in Megabytes (MiB).
    *   `-c`, `--codec` (String): Target encoder family (e.g., `h264`, `h265`, `vp9`).
    *   `-p`, `--preset` (String): Speed-to-compression ratio preset.
    *   `--audio-bitrate` (Int32, Default: 128): Audio bitrate reservation in kbps.
    *   `--copy-audio` (Boolean): Retains existing audio streams as-is without re-encoding them.

---

### 3.4. `audio`
Performs audio manipulations such as track isolation, striping, replacement, mixing, or normalization.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;       const char* output;
        const char* secondary;   const char* op_str;
        const char* acodec;      int32_t abitrate;
    } s_audio;
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Primary multimedia file.
    *   `-o`, `--output` (Required, String): Target output file path.
    *   `-O`, `--op` (Required, String): The audio operation. Valid choices:
        *   `extract`: Isolates audio to a standalone file (e.g., extracting an MP3 from an MP4).
        *   `strip`: Removes all audio tracks (creates a silent video).
        *   `replace`: Overwrites the audio in the primary input with the audio from `--secondary`.
        *   `normalize`: Applies EBU R128 loudness normalization.
        *   `mix`: Merges primary and secondary audio tracks together.
    *   `-s`, `--secondary` (String): Path to the secondary input file for `replace` or `mix` operations.
    *   `-c`, `--codec` (String): Output audio codec.
    *   `-b`, `--bitrate` (Int32): Target audio bitrate in kbps.

*   **FFmpeg Filter Application:**
    *   **Normalization:** Employs the `loudnorm` filter with parameters:
        `loudnorm=I=-23.0:TP=-2.0:LRA=7.0`.
    *   **Mixing:** Employs the `amix` filter to combine multiple audio inputs into a single stream:
        `amix=inputs=2:duration=first`.

---

### 3.5. `thumbnail`
Extracts a frame from a video file and saves it as an image.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;   const char* output;
        const char* ts;      const char* strat;
        int32_t width;       int32_t height;
        int32_t quality;
    } s_thumbnail = {.quality = 2};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video path.
    *   `-o`, `--output` (Required, String): Target output image path.
    *   `-t`, `--timestamp` (String): Specific time position to extract (e.g., `00:05:00`). Maps to FFmpeg `-ss`.
    *   `-s`, `--strategy` (String): Automatic frame-selection algorithm:
        *   `time`: Selects a frame at the timestamp specified by `-t`.
        *   `middle`: Calculates video duration and extracts a frame from the exact midpoint.
        *   `best`: Scans keyframes to extract a high-clarity scene.
    *   `-W`, `--width` (Int32): Resizes the extracted frame's width.
    *   `-H`, `--height` (Int32): Resizes the extracted frame's height.
    *   `-q`, `--quality` (Int32, Default: 2): Set JPEG compression quality level 
    *   (range 1-31, where lower is higher quality). Maps to FFmpeg `-qscale:v` or `-q:v`.

---

### 3.6. `gif`
Converts a segment of a video file into an animated GIF.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;      const char* output;
        const char* start;      const char* duration;
        const char* stats;      int32_t fps;
        int32_t width;          bool loop;
    } s_gif = {.fps = 15, .width = 480, .loop = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Path to the source video.
    *   `-o`, `--output` (Required, String): Target GIF file path.
    *   `-s`, `--start` (String): Starting timestamp offset.
    *   `-d`, `--duration` (String): Playback duration of the output GIF.
    *   `--fps` (Int32, Default: 15): Frames per second rate of the output GIF.
    *   `-w`, `--width` (Int32, Default: 480): Width scale of the GIF. Aspect ratio is maintained.
    *   `--stats` (String): Controls palette generation optimization.
    *   `--loop` (Boolean, Default: true): Loops playback continuously. Maps to `-loop 0` (enabled) or `-loop -1` (disabled).

*   **FFmpeg Filter Application:**
    To achieve high-quality results with standard 256-color limits, this subcommand maps to a dual-stage filterchain:
    `[0:v] fps=15,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse`

---

### 3.7. `probe`
Inspects a media file and displays its metadata.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;
        bool raw_json;
    } s_probe;
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Path to the target media file.
    *   `-j`, `--json` (Boolean): Outputs the raw technical details in JSON format. Maps to `ffprobe -v quiet -print_format json -show_format -show_streams`.

---

### 3.8. `concat`
Appends multiple media files together in sequence.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* output;   const char* vcodec;
        const char* preset;   const char* acodec;
        bool reencode;
    } s_concat;
    ```

*   **Flag Mappings:**
    *   Positional Arguments: Paths to the input files to be joined, up to a maximum of 64 files.
    *   `-o`, `--output` (Required, String): Target concatenated output file path.
    *   `-r`, `--reencode` (Boolean): Re-encodes the files during concatenation. This is necessary if the source videos have different resolutions, codecs, or frame rates.
    *   `--vcodec` (String): Target video encoder to use if re-encoding.
    *   `-p`, `--preset` (String): Video encoder speed/efficiency preset.
    *   `--acodec` (String): Target audio encoder to use if re-encoding.

*   **FFmpeg Application Mechanics:**
    *   **Direct Join (Default):** Generates a text file listing the input files and uses the FFmpeg `concat` demuxer:
        `-f concat -safe 0 -i inputs.txt -c copy`
    *   **Filter-based Join (`--reencode`):** Uses the `concat` video filter:
        `[0:v][0:a][1:v][1:a] concat=n=2:v=1:a=1 [v][a]`

---

### 3.9. `watermark`
Overlays an image (such as a logo) on top of a video.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;       const char* watermark;
        const char* output;      const char* position;
        const char* vcodec;      const char* preset;
        int32_t margin;          int32_t scale_w;
        int32_t crf;             float opacity;
        bool copy_audio;
    } s_watermark = {.margin = 10, .crf = 18, .opacity = 1.0f, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video.
    *   `-w`, `--watermark` (Required, String): Path to the image file to overlay.
    *   `-o`, `--output` (Required, String): Target output video file path.
    *   `-p`, `--position` (String, Default: `br`): Anchor corner for the overlay:
        *   `tl`: Top-Left
        *   `tr`: Top-Right
        *   `bl`: Bottom-Left
        *   `br`: Bottom-Right
        *   `c`: Center
    *   `--margin` (Int32, Default: 10): Distance in pixels from the edge of the video frame.
    *   `--opacity` (Float, Default: 1.0): Opacity level of the watermark (0.0 to 1.0).
    *   `--scale-width` (Int32): Resizes the watermark's width in pixels while maintaining its aspect ratio.
    *   `--vcodec` (String): Target output video encoder.
    *   `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Target CRF quality level.
    *   `--copy-audio` (Boolean, Default: true): Retains the original audio streams without re-encoding.

*   **FFmpeg Filter Application:**
    Uses the `overlay` and `scale` filters.
    *   **Opacity Filter:** Applied via format manipulation or the `colorchannelmixer` or `lut` filters.
    *   **Position Equations:**
        *   `tl`: `x=margin:y=margin`
        *   `tr`: `x=main_w-overlay_w-margin:y=margin`
        *   `bl`: `x=margin:y=main_h-overlay_h-margin`
        *   `br`: `x=main_w-overlay_w-margin:y=main_h-overlay_h-margin`
        *   `c`: `x=(main_w-overlay_w)/2:y=(main_h-overlay_h)/2`

---

### 3.10. `speed`
Adjusts the playback speed of video and audio.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;    const char* output;
        const char* vcodec;   const char* preset;
        const char* acodec;   float factor;
        int32_t crf;          bool no_audio;
    } s_speed = {.crf = 18};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input media file.
    *   `-o`, `--output` (Required, String): Output media file.
    *   `-x`, `--factor` (Required, Float): Speed multiplier. For example, `2.0` doubles the speed, while `0.5` plays at half-speed.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--acodec` (String): Output audio encoder.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.
    *   `--no-audio` (Boolean): Removes audio tracks entirely.

*   **FFmpeg Filter Application:**
    Adjusting speed requires changing presentation timestamps (PTS) for video frames and modifying audio samples via a pitch-preserving filter.
    *   **Video Filter:** `setpts=PTS/factor` (e.g., `setpts=PTS/2.0` makes video frames play twice as fast).
    *   **Audio Filter:** `atempo=factor` (e.g., `atempo=2.0`). FFmpeg's `atempo` filter accepts values between `0.5` and `100.0`.

---

### 3.11. `crop`
Crops a video to a smaller rectangular region.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;         const char* output;
        int32_t width;             int32_t height;
        int32_t x;                 int32_t y;
        bool copy_audio;           const char* video_codec;
        const char* preset;        int32_t crf;
    } s_crop = {.x = -1, .y = -1, .crf = 18, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video.
    *   `-o`, `--output` (Required, String): Output video.
    *   `-w`, `--width` (Required, Int32): Width of the cropped region in pixels.
    *   `-h`, `--height` (Required, Int32): Height of the cropped region in pixels.
    *   `-x`, `--x` (Int32, Default: -1): Horizontal offset from the top-left corner. If negative, crops from the center.
    *   `-y`, `--y` (Int32, Default: -1): Vertical offset from the top-left corner. If negative, crops from the center.
    *   `--copy-audio` (Boolean, Default: true): Copies audio streams without transcoding.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    Maps directly to FFmpeg's `crop` filter:
    `crop=w:h:x:y` (e.g., `crop=640:480:100:100`).
    If `x` or `y` are unspecified (or negative), they default to centering: `(in_w-out_w)/2` and `(in_h-out_h)/2`.

---

### 3.12. `scale`
Resizes a video, offering options to handle different aspect ratios.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;         const char* output;
        int32_t width;             int32_t height;
        const char* mode_str;      const char* pad_color;
        bool copy_audio;           const char* video_codec;
        const char* preset;        int32_t crf;
    } s_scale = {.crf = 18, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video.
    *   `-o`, `--output` (Required, String): Output video.
    *   `-w`, `--width` (Int32): Target width in pixels.
    *   `-h`, `--height` (Int32): Target height in pixels.
    *   `-m`, `--mode` (String, Default: `fit`): Aspect-ratio handling mode:
        *   `fit`: Scales to fit inside the target resolution while preserving aspect ratio (e.g., using `-1` for the other dimension).
        *   `stretch`: Forces scaling to the exact target dimensions, ignoring aspect ratio.
        *   `pad`: Centers the scaled video within the target resolution and fills empty areas (letterboxing or pillarboxing).
    *   `-c`, `--pad-color` (String): Background fill color for padding (e.g., `black`, `white`, `0xFF0000`).
    *   `--copy-audio` (Boolean, Default: true): Copies audio streams without transcoding.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    *   **Fit Mode:** `scale=width:-2` or `scale=-2:height` (uses even dimensions for compatibility with most encoders).
    *   **Stretch Mode:** `scale=width:height`.
    *   **Pad Mode:** Combines `scale` and `pad` filters:
        `scale=w=target_w:h=target_h:force_original_aspect_ratio=decrease,pad=w=target_w:h=target_h:x=(ow-iw)/2:y=(oh-ih)/2:color=pad_color`

---

### 3.13. `rotate`
Rotates a video or flips it horizontally or vertically.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;     const char* output;
        const char* op_str;    bool copy_audio;
        const char* video_codec; const char* preset;
        int32_t crf;
    } s_rotate = {.crf = 18, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video.
    *   `-o`, `--output` (Required, String): Output video.
    *   `-r`, `--op` (Required, String): Rotation or flip operation:
        *   `90cw`: Rotates 90 degrees clockwise.
        *   `90ccw`: Rotates 90 degrees counter-clockwise.
        *   `180`: Flips the video 180 degrees.
        *   `hflip`: Flips the video horizontally (mirror effect).
        *   `vflip`: Flips the video vertically.
    *   `--copy-audio` (Boolean, Default: true): Copies audio streams without transcoding.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    Maps rotation operations to the `transpose` and `hflip`/`vflip` filters:
    *   `90cw`: `transpose=1` (or `transpose=clock`)
    *   `90ccw`: `transpose=2` (or `transpose=cclock`)
    *   `180`: `transpose=1,transpose=1` (or combining `hflip,vflip`)
    *   `hflip`: `hflip`
    *   `vflip`: `vflip`

---

### 3.14. `fade`
Applies fade-in or fade-out effects to video, audio, or both.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;               const char* output;
        bool fade_in;                    float fade_in_seconds;
        bool fade_out;                   float fade_out_seconds;
        float total_duration;            bool video;
        bool audio;                      const char* video_codec;
        const char* preset;              const char* audio_codec;
        int32_t crf;
    } s_fade = {.fade_in_seconds = 1.0f, .fade_out_seconds = 1.0f, .video = true, .audio = true, .crf = 18};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input video/audio.
    *   `-o`, `--output` (Required, String): Output video/audio.
    *   `--fade-in` (Boolean): Enables a fade-in effect.
    *   `--fade-in-duration` (Float, Default: 1.0): Length of the fade-in in seconds.
    *   `--fade-out` (Boolean): Enables a fade-out effect.
    *   `--fade-out-duration` (Float, Default: 1.0): Length of the fade-out in seconds.
    *   `--duration` (Float): Total duration of the input in seconds (required to calculate the start time for the fade-out).
    *   `--video` (Boolean, Default: true): Applies the fade effect to the video track.
    *   `--audio` (Boolean, Default: true): Applies the fade effect to the audio track.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--acodec` (String): Output audio encoder.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    *   **Video Fade:** Uses the `fade` filter.
        *   *Fade-In:* `fade=t=in:st=0:d=fade_in_seconds`
        *   *Fade-Out:* `fade=t=out:st=(total_duration-fade_out_seconds):d=fade_out_seconds`
    *   **Audio Fade:** Uses the `afade` filter.
        *   *Fade-In:* `afade=t=in:ss=0:d=fade_in_seconds`
        *   *Fade-Out:* `afade=t=out:st=(total_duration-fade_out_seconds):d=fade_out_seconds`

---

### 3.15. `deinterlace`
Removes interlacing artifacts from video.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;         const char* output;
        const char* mode_str;      bool copy_audio;
        const char* video_codec;   const char* preset;
        int32_t crf;
    } s_deinterlace = {.crf = 18, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input interlaced video.
    *   `-o`, `--output` (Required, String): Output deinterlaced video.
    *   `-m`, `--mode` (String, Default: `yadif`): Deinterlacing filter algorithm:
        *   `yadif`: Yet Another Deinterlacing Filter (standard, computationally efficient option).
        *   `bwdif`: Bob Weaver Deinterlacing Filter (produces higher visual quality at a higher processing cost).
    *   `--copy-audio` (Boolean, Default: true): Copies audio streams without transcoding.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    Maps to either the `yadif` or `bwdif` filter in the FFmpeg filterchain:
    *   `yadif` mode maps to: `yadif=mode=0:parity=-1:deint=0`
    *   `bwdif` mode maps to: `bwdif=mode=0:parity=-1:deint=0`

---

### 3.16. `denoise`
Reduces visual noise and graininess in a video.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;         const char* output;
        const char* mode_str;      float strength;
        bool copy_audio;           const char* video_codec;
        const char* preset;        int32_t crf;
    } s_denoise = {.crf = 18, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input noisy video.
    *   `-o`, `--output` (Required, String): Output denoised video.
    *   `-m`, `--mode` (String, Default: `hqdn3d`): Denoising filter algorithm:
        *   `hqdn3d`: High-Quality 3D Denoising (fast, spatial-temporal denoiser).
        *   `nlmeans`: Non-Local Means (highly precise, but computationally heavy).
    *   `-s`, `--strength` (Float): Denoising strength. Controls the aggressive scaling parameters of the selected filter.
    *   `--copy-audio` (Boolean, Default: true): Copies audio streams without transcoding.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    *   **hqdn3d mode:** Maps to standard spatial/temporal limits, scaled by strength:
        `hqdn3d=luma_spatial=strength`
    *   **nlmeans mode:** Maps to standard patch-based searches, scaled by strength:
        `nlmeans=s=strength`

---

### 3.17. `sharpen`
Adjusts video sharpness.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;         const char* output;
        float amount;              bool copy_audio;
        const char* video_codec;   const char* preset;
        int32_t crf;
    } s_sharpen = {.amount = 1.0f, .crf = 18, .copy_audio = true};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input blurry/unprocessed video.
    *   `-o`, `--output` (Required, String): Output sharpened video.
    *   `-a`, `--amount` (Float, Default: 1.0): Sharpening intensity. Negative values can be used to blur the video.
    *   `--copy-audio` (Boolean, Default: true): Copies audio streams without transcoding.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **FFmpeg Filter Application:**
    Maps directly to FFmpeg's `unsharp` (unsharp mask) filter:
    `unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=amount`

---

### 3.18. `filter`
Provides direct access to raw FFmpeg filtergraphs for advanced users.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;             const char* output;
        const char* video_filter;      const char* audio_filter;
        const char* filter_complex;    const char* complex_maps_str;
        const char* video_codec;       const char* preset;
        const char* audio_codec;       int32_t crf;
    } s_filter = {.crf = 18};
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input file.
    *   `-o`, `--output` (Required, String): Output file.
    *   `--vf` (String): Raw video filter string. Maps directly to FFmpeg `-vf`.
    *   `--af` (String): Raw audio filter string. Maps directly to FFmpeg `-af`.
    *   `--filter-complex` (String): Complex filtergraph expression. Maps directly to FFmpeg `-filter_complex`.
    *   `--maps` (String): Comma-separated output maps (e.g., `[v],[a]`). Maps to multiple `-map` options in FFmpeg.
    *   `--vcodec` (String): Output video encoder.
    *   `-p`, `--preset` (String): Video encoder preset.
    *   `--acodec` (String): Output audio encoder.
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

---

### 3.19. `silence-detect`
Scans a media file's audio stream for silent periods below a given decibel threshold and outputs the timestamps.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;
        float noise_floor;
        float min_duration;
    } s_silence_detect;
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Path to the source file.
    *   `-n`, `--noise` (Float, Default: -30.0): Decibel limit below which signal is evaluated as silence. Maps to `silencedetect=noise`.
    *   `-d`, `--duration` (Float, Default: 0.5): Minimum length in seconds for a quiet interval to be recognized as silence. Maps to `silencedetect=d`.

---

### 3.20. `desilence`
Detects silent sections, removes them, and rejoins non-silent segments in a single rendering pass. Supports optional audio smoothing and leveling filters.

*   **Subcommand Storage Structure:**
    ```c
    static struct {
        const char* input;         const char* output;
        float noise_floor;         float min_duration;
        bool smooth;               float target_lufs;
        const char* video_codec;   const char* preset;
        const char* audio_codec;   int32_t crf;
    } s_desilence;
    ```

*   **Flag Mappings:**
    *   `-i`, `--input` (Required, String): Input file path.
    *   `-o`, `--output` (Required, String): Output file path.
    *   `-n`, `--noise` (Float, Default: -30.0): Noise floor threshold in decibels (dB).
    *   `-d`, `--duration` (Float, Default: 0.5): Minimum silence duration in seconds.
    *   `-s`, `--smooth` (Boolean): Applies dynamic audio normalization (`dynaudnorm`) to balance quiet/loud elements.
    *   `-l`, `--target-lufs` (Float, Default: 0.0): Target LUFS loudness normalization level.
    *   `-V`, `--vcodec` (String): Output video encoder codec choice.
    *   `-p`, `--preset` (String): Speed-to-compression ratio preset.
    *   `-A`, `--acodec` (String): Output audio encoder codec choice (default: `aac`).
    *   `--crf` (Int32, Default: 18): Constant Rate Factor.

*   **Filter Application Mechanics:**
    1.  **Interval Complement Calculation**: Parses silence segments from a detection pass, computing the non-silent "kept" timelines.
    2.  **Filtergraph Generation**: Uses `select` and `aselect` with boolean time checks to keep the video and audio aligned during removal:
        `[0:v]select='between(t,...)',setpts=...[vout];[0:a]aselect='between(t,...)',asetpts=...[aout]`
    3.  **Volume Leveling**: Chains `dynaudnorm` (dynamic range compression) and `loudnorm` filters directly onto the audio track when enabled to smooth out sound inconsistencies.

## 4. Key Implementation Mechanics

The tool handles command generation, dynamic memory management, and error handling through several notable patterns:

### String Tokenization of Complex Maps
In `cmd_filter`, maps specified with the `--maps` flag are parsed on-the-fly. The input string is split by commas using `strtok` to extract individual mapping targets.

```c
char* maps[FFX_MAX_EXTRA_FLAGS] = {0};
char* map_buf = NULL;
if (s_filter.complex_maps_str != NULL) {
    map_buf = strdup(s_filter.complex_maps_str); // Duplicated to preserve the original CLI flag string
    if (map_buf != NULL) {
        int idx = 0;
        char* token = strtok(map_buf, ",");
        while (token != NULL && idx < FFX_MAX_EXTRA_FLAGS - 1) {
            maps[idx++] = token;
            token = strtok(NULL, ",");
        }
    }
}
```

### Direct Positional Flag Retrieval
The `concat` subcommand reads input files directly from the command line's positional arguments rather than using standard options. It accesses these arguments sequentially using the `flag_positional_count` and `flag_positional_at` APIs:

```c
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
```

---

## 5. Quick-Start Command Examples

### Transcoding with Preset Limits
Converts a QuickTime `.mov` file to `.mp4` using the standard H.264 video codec and high-quality AAC audio:
```bash
ffx convert -i vacation.mov -o target.mp4 -V libx264 -p slow --crf 20 -A aac --audio-bitrate 192
```

### Splitting and Saving Segments (Lossless Cut)
Extracts a precise 30-second clip starting at the 1-minute mark without re-encoding:
```bash
ffx trim -i movie.mp4 -o clip.mp4 --start 00:01:00 --end 30 --duration --accurate
```

### Overlaying a Watermark Logo
Overlays a logo in the bottom-right corner of a video with 50% opacity and a 15-pixel margin from the edge:
```bash
ffx watermark -i source.mp4 -w logo.png -o output.mp4 --position br --margin 15 --opacity 0.5
```

### Doubling Playback Speed (Fast-Forward)
Plays both video and audio tracks at twice their normal speed:
```bash
ffx speed -i demo.mp4 -o fast.mp4 --factor 2.0
```

### Merging Multiple Videos (Concatenation)
Stitches three video segments together sequentially into a single file:
```bash
ffx concat -o merged.mp4 clip1.mp4 clip2.mp4 clip3.mp4
```
