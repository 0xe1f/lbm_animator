// Copyright (c) 2026 Akop Karapetyan
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vorbis/vorbisfile.h>
#include "libretro.h"
#include "lbm.h"
#include "svx8.h"

retro_log_printf_t log_cb;

typedef struct {
    const Cycle *cycle;
    unsigned long cycle_rate_us;
    unsigned long last_cycle_us;
    unsigned short length;
    unsigned short poffset;
    unsigned short offset;
} CycleState;

#define SOUND_FREQUENCY 44100
#define FPS             60.0
#define RATE_SCALE_US   (16384UL * 1000000UL)
#define AUDIO_BUFFER_SIZE ((SOUND_FREQUENCY / (int) FPS) * 2 * 2)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static LbmImage image = { 0 };
static void *pixel_buffer = NULL;
static size_t pixel_buffer_size = 0;
static int pixel_buffer_bpp = 4;
static CycleState *cycle_states = NULL;
static uint32_t *base_palette = NULL;
static uint32_t *animated_palette = NULL;
static bool color_blending_enabled = true;
static FILE *audio_file = NULL;
static OggVorbis_File vorbis_file = { 0 };
static Svx8Audio svx8_audio = { 0 };
static uint32_t sample_pos = 0;
static char sound_buffer[AUDIO_BUFFER_SIZE] = { 0 };
static char *system_directory = NULL;
static time_t start_time_real = 0;
static time_t start_time_logical = 0;
static float seconds_per_second = 1.0f;

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_cb;

static void free_buffers();
static unsigned long micros();
static uint32_t blend_colors(uint32_t c1, uint32_t c2, float delta);
static int find_index_for_timeline_offset(uint32_t sec_offset);
static void cycle_overlay();
static void cycle_palette();
static void update_pixel_buffer();
static void check_variables();
static void fill_audio_buffer();
static void apply_overlay(const LbmImage *overlay);
static void blend_overlay(const LbmImage *overlay1, const LbmImage *overlay2, float ratio);
static uint32_t seconds_since_midnight(float multiplier);

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb)
{
    static const struct retro_controller_info ports[] = {
        { 0 },
    };

    static const struct retro_input_descriptor desc[] = {
        { 0 },
    };

    static const struct retro_system_content_info_override content_overrides[] = {
        {
            "lbm", /* extensions */
            false, /* need_fullpath */
            false  /* persistent_data */
        },
        { NULL, false, false }
    };

    environ_cb = cb;

    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*) ports);
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) desc);
    cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*)content_overrides);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_get_system_info(struct retro_system_info *info)
{
    info->library_name = "LBM Animator Core";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
    info->library_version = "v1.0.0" GIT_VERSION;
    info->valid_extensions = "lbm";
    info->block_extract = false;
    info->need_fullpath = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->geometry.base_width  = image.width;
    info->geometry.base_height = image.height;
    info->geometry.max_width = image.width;
    info->geometry.max_height = image.height;
    info->geometry.aspect_ratio  = (float) image.width / (float) image.height;
    info->timing.fps             = FPS;
    info->timing.sample_rate     = SOUND_FREQUENCY;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

bool retro_load_game(const struct retro_game_info *info)
{
    void *buffer = (void *) info->data;
    int buffer_size = info->size;

    log_cb(RETRO_LOG_INFO, "Loading: %s (size: %d)\n", info->path, buffer_size);

    if ((buffer == NULL || buffer_size == 0) && info->path) {
        // Read the file
        FILE *fp = fopen(info->path, "rb");
        if (fp == NULL) {
            log_cb(RETRO_LOG_ERROR, "Failed to open '%s'\n", info->path);
            return false;
        }
        fseek(fp, 0, SEEK_END);
        buffer_size = ftell(fp);
        rewind(fp);
        if ((buffer = malloc(buffer_size)) == NULL) {
            log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for file: %s\n", info->path);
            fclose(fp);
            return false;
        }
        fread(buffer, buffer_size, 1, fp);
        fclose(fp);
    } else if (buffer == NULL || buffer_size == 0) {
        log_cb(RETRO_LOG_ERROR, "No data and no file path available\n");
        return false;
    }

    if (!lbm_read_mem(&image, buffer, buffer_size)) {
        log_cb(RETRO_LOG_ERROR, "Failed to read LBM data\n");
        return false;
    }

    log_cb(RETRO_LOG_INFO, "Image '%s' loaded successfully (%dx%d, %d colors, %d cycles)\n",
        image.name, image.width, image.height, image.n_palette, image.n_cycles);
    if (buffer != info->data) {
        free(buffer);
    }

    check_variables();

    char audio_path[2048];
    struct retro_game_info_ext *info_ext = NULL;
    sample_pos = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &info_ext)) {
        // Try 8SVX first
        snprintf(audio_path, sizeof(audio_path), "%s/%s.8svx",
            system_directory, info_ext->name);
        if (access(audio_path, F_OK) == 0) {
            if (!svx8_read_file(&svx8_audio, audio_path)) {
                log_cb(RETRO_LOG_WARN, "Error reading 8SVX audio file: %s\n", audio_path);
            } else {
                log_cb(RETRO_LOG_INFO, "8SVX file loaded: %s (%d Hz, %d channels, %d bytes per sample)\n",
                    audio_path, svx8_audio.sample_rate,
                    svx8_audio.channels, svx8_audio.bytes_per_sample);
                svx8_resample(&svx8_audio, 2, 2, SOUND_FREQUENCY);
                log_cb(RETRO_LOG_INFO, "Resampled to %d Hz, %d ch, %d bps (%d KiB)\n",
                    svx8_audio.sample_rate, svx8_audio.channels,
                    svx8_audio.bytes_per_sample,
                    (svx8_audio.n_samples * svx8_audio.bytes_per_sample) / 1024);
                goto done_audio;
            }
        }

        // Try OGG next
        snprintf(audio_path, sizeof(audio_path), "%s/%s.ogg",
            system_directory, info_ext->name);
        if (access(audio_path, F_OK) == 0) {
            audio_file = fopen(audio_path, "rb");
            if (audio_file == NULL) {
                log_cb(RETRO_LOG_WARN, "Couldn't open OGG file: %s\n", audio_path);
            } else if (ov_open_callbacks(audio_file, &vorbis_file, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0) {
                log_cb(RETRO_LOG_WARN, "Error opening OGG stream: %s\n", audio_path);
                fclose(audio_file);
                audio_file = NULL;
            } else {
                log_cb(RETRO_LOG_INFO, "OGG file loaded: %s\n", audio_path);
                goto done_audio;
            }
        }
    }
    done_audio:

    // Init graphics
    unsigned int format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        log_cb(RETRO_LOG_INFO, "Pixel format set to XRGB8888\n");
    } else {
        log_cb(RETRO_LOG_ERROR, "Failed to set pixel format\n");
    }

    // Initialize pixel buffer
    pixel_buffer_size = image.width * image.height * pixel_buffer_bpp;
    if ((pixel_buffer = malloc(pixel_buffer_size)) == NULL) {
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for pixel buffer\n");
        return false;
    }

    // Initialize dynamic palette
    base_palette = malloc(image.n_palette * sizeof(uint32_t));
    if (base_palette == NULL) {
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for base palette\n");
        free_buffers();
        return false;
    }
    animated_palette = malloc(image.n_palette * sizeof(uint32_t));
    if (animated_palette == NULL) {
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for animated palette\n");
        free_buffers();
        return false;
    }

    // Initialize cycle states
    cycle_states = malloc(image.n_cycles * sizeof(CycleState));
    if (cycle_states == NULL) { 
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for cycle states\n");
        free_buffers();
        return false;
    }

    // Set up the base palette
    apply_overlay(&image);

    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    return false;
}

void retro_unload_game(void)
{
    free_buffers();
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    return 0;
}

void retro_init(void)
{
    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
    } else {
        log_cb = NULL;
    }

    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory)) {
        log_cb(RETRO_LOG_WARN, "System directory not available.\n");
    }

    start_time_real = time(NULL);
    start_time_logical = start_time_real;
}

void retro_deinit(void)
{
}

void retro_reset(void)
{
}

void retro_run(void)
{
    cycle_overlay();
    cycle_palette();
    update_pixel_buffer();
    fill_audio_buffer();
    video_cb(pixel_buffer, image.width, image.height, image.width * pixel_buffer_bpp);
}

static void free_buffers()
{
    lbm_free(&image);
    free(pixel_buffer);
    pixel_buffer = NULL;
    pixel_buffer_size = 0;
    free(cycle_states);
    cycle_states = NULL;
    free(base_palette);
    base_palette = NULL;
    free(animated_palette);
    animated_palette = NULL;
    ov_clear(&vorbis_file);
    svx8_free(&svx8_audio);
    if (audio_file) {
        fclose(audio_file);
        audio_file = NULL;
    }
}

static unsigned long micros()
{
    struct timeval tp;
    gettimeofday(&tp, NULL);
    return tp.tv_sec * 1000000UL + tp.tv_usec;
}

static uint32_t blend_colors(uint32_t c1, uint32_t c2, float ratio)
{
    uint8_t r1 = (c1 >> 16) & 0xFF;
    uint8_t g1 = (c1 >> 8) & 0xFF;
    uint8_t b1 = c1 & 0xFF;

    uint8_t r2 = (c2 >> 16) & 0xFF;
    uint8_t g2 = (c2 >> 8) & 0xFF;
    uint8_t b2 = c2 & 0xFF;

    uint8_t r = (uint8_t)(r1 * (1.0f - ratio) + r2 * ratio);
    uint8_t g = (uint8_t)(g1 * (1.0f - ratio) + g2 * ratio);
    uint8_t b = (uint8_t)(b1 * (1.0f - ratio) + b2 * ratio);

    return (r << 16) | (g << 8) | b;
}

static int find_index_for_timeline_offset(uint32_t sec_offset)
{
    if (image.n_timelines == 0 || image.n_bbms == 0) {
        return -1;
    } else if (sec_offset <= image.timelines[0].offset_secs) {
        return image.timelines[0].index;
    } else if (sec_offset >= image.timelines[image.n_timelines - 1].offset_secs) {
        return image.timelines[image.n_timelines - 1].index;
    }

    for (size_t i = image.n_timelines - 1; i >= 0; i--) {
        if (sec_offset >= image.timelines[i].offset_secs) {
            return image.timelines[i].index;
        }
    }

    return -1;
}

static bool find_indices_for_timeline_offset(uint32_t sec_offset, int *out_index1, int *out_index2, float *out_ratio)
{
    if (image.n_timelines == 0 || image.n_bbms == 0) {
        return false;
    } else if (sec_offset <= image.timelines[0].offset_secs) {
        *out_index1 = image.timelines[0].index;
        *out_index2 = image.timelines[0].index;
        *out_ratio = 0.0f;
        return true;
    } else if (sec_offset >= image.timelines[image.n_timelines - 1].offset_secs) {
        *out_index1 = image.timelines[image.n_timelines - 1].index;
        *out_index2 = image.timelines[image.n_timelines - 1].index;
        *out_ratio = 0.0f;
        return true;
    }

    for (size_t i = image.n_timelines - 1; i >= 0; i--) {
        if (sec_offset >= image.timelines[i].offset_secs) {
            *out_index1 = image.timelines[i].index;
            *out_index2 = image.timelines[i + 1].index;
            *out_ratio = (sec_offset - image.timelines[i].offset_secs) / (float)(image.timelines[i + 1].offset_secs - image.timelines[i].offset_secs);
            return true;
        }
    }

    return false;
}

static void cycle_overlay()
{
    if (image.n_timelines == 0 || image.n_bbms == 0) {
        return; // No timelines or overlays to apply
    }

    static unsigned long last_time = 0;
    unsigned long current_time = micros();
    if (current_time - last_time >= 1UL) {
        last_time = current_time;

        int index1, index2;
        float ratio;
        uint32_t secs = seconds_since_midnight(seconds_per_second);
        if (find_indices_for_timeline_offset(secs, &index1, &index2, &ratio)) {
            if (index1 < 0 || index2 >= image.n_bbms) {
                log_cb(RETRO_LOG_WARN, "Invalid index1 for offset: %u secs\n", secs);
                return;
            } else if (index2 < 0 || index2 >= image.n_bbms) {
                log_cb(RETRO_LOG_WARN, "Invalid index2 for offset: %u secs\n", secs);
                return;
            }
            log_cb(RETRO_LOG_INFO, "secs: %u, (%d, %d, %.2f)\n",
                secs, index1, index2, ratio);
            blend_overlay(&image.bbms[index1], &image.bbms[index2], ratio);
        }
    }
}

static void cycle_palette()
{
    unsigned long current_time = micros();
    for (size_t i = 0; i < image.n_cycles; i++) {
        CycleState *state = &cycle_states[i];
        const Cycle *cycle = state->cycle;
        if (cycle->rate == 0 || (cycle->flags & 0x01) == 0) {
            continue; // No cycling
        }

        // Advance the cycle state if enough time has passed
        if (current_time - state->last_cycle_us >= state->cycle_rate_us) {
            // Advance the offset and wrap around the cycle range
            state->poffset = state->offset;
            state->offset = (state->offset + 1) % state->length;

            // Update the last cycle time
            state->last_cycle_us = current_time;
        }
        float ratio = MIN((current_time - state->last_cycle_us) / (float)state->cycle_rate_us, 1.0f);

        // Update the palette entries for this cycle
        for (int j = 0; j < state->length; j++) {
            uint32_t pcolor = base_palette[cycle->low + (state->poffset + j) % state->length];
            uint32_t color = base_palette[cycle->low + (state->offset + j) % state->length];
            uint32_t blended_color = color_blending_enabled
                ? blend_colors(pcolor, color, ratio)
                : color;

            if (cycle->flags & 0x02) {
                animated_palette[cycle->low + j] = blended_color;
            } else {
                animated_palette[cycle->high - j] = blended_color;
            }
        }
    }
}

static void update_pixel_buffer()
{
    int i;
    unsigned int *pp;
    for (i = 0, pp = pixel_buffer; i < image.width * image.height; i++, pp++) {
        unsigned int index = image.pixels[i];
        *pp = animated_palette[index];
    }
}

static void check_variables()
{
    struct retro_variable var = {0};

    var.key = "lbm_animator_color_blending";
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (var.value && strcmp(var.value, "disabled") == 0) {
        color_blending_enabled = false;
    }

    var.key = "lbm_animator_seconds_per_second";
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (var.value) {
        seconds_per_second = atof(var.value);
        if (seconds_per_second <= 0.0f) {
            log_cb(RETRO_LOG_WARN, "Invalid value for seconds_per_second: %s\n", var.value);
            seconds_per_second = 1.0f;
        }
    }
}

static void fill_audio_buffer()
{
    int bytes_left = sizeof(sound_buffer);
    if (svx8_audio.n_samples) {
        int bytes_per_frame = svx8_audio.channels * svx8_audio.bytes_per_sample;
        while (bytes_left > 0) {
            if (sample_pos >= svx8_audio.n_samples) {
                sample_pos = 0; // Loop back to start
            }
            sound_buffer[sizeof(sound_buffer) - bytes_left] = svx8_audio.samples[sample_pos++];
            bytes_left--;
        }
        int frames = sizeof(sound_buffer) / bytes_per_frame;
        audio_cb((const int16_t *) sound_buffer, frames);
    } else if (audio_file != NULL) {
        int bytes_per_frame = 4;
        int offset = 0;
        while (bytes_left > 0) {
            long bytes_read = ov_read(&vorbis_file,
                sound_buffer + offset, bytes_left,
                0, 2, 1, NULL);
            if (bytes_read == 0) {
                // End of stream, loop back to start
                ov_pcm_seek(&vorbis_file, 0);
            } else if (bytes_read < 0) {
                // Error in audio stream; abort
                break;
            } else {
                offset += bytes_read;
                bytes_left -= bytes_read;
            }
        }
        int frames = offset / bytes_per_frame;
        if (frames > 0) {
            audio_cb((const int16_t *) sound_buffer, frames);
        }
    }
}

static void apply_overlay(const LbmImage *overlay)
{
    // Initialize dynamic palette
    if (animated_palette == NULL) {
        log_cb(RETRO_LOG_ERROR, "Palette not initialized\n");
        return;
    } else if (overlay->n_palette != image.n_palette) {
        log_cb(RETRO_LOG_ERROR, "Overlay palette size mismatch (%u != %u)\n",
            overlay->n_palette, image.n_palette);
        return;
    }

    // Initialize cycle states
    if (cycle_states == NULL) {
        log_cb(RETRO_LOG_ERROR, "Cycle states not initialized\n");
        return;
    } else if (overlay->n_cycles != image.n_cycles) {
        log_cb(RETRO_LOG_ERROR, "Overlay cycle size mismatch (%u != %u)\n",
            overlay->n_cycles, image.n_cycles);
        return;
    }

    // Apply overlays
    memcpy(base_palette, overlay->palette, overlay->n_palette * sizeof(uint32_t));
    memcpy(animated_palette, overlay->palette, overlay->n_palette * sizeof(uint32_t));
    for (size_t i = 0; i < overlay->n_cycles; i++) {
        const Cycle *cycle = &overlay->cycles[i];
        int cycle_size = (cycle->high - cycle->low) + 1;
        cycle_states[i] = (CycleState) {
            .cycle = cycle,
            .cycle_rate_us = (unsigned long)(RATE_SCALE_US / (FPS * cycle->rate)),
            .last_cycle_us = 0,
            .length = (unsigned short) cycle_size,
            .offset = 0,
        };
    }

    log_cb(RETRO_LOG_INFO, "Overlay applied\n");
}

static void blend_overlay(const LbmImage *overlay1, const LbmImage *overlay2, float ratio)
{
    // Initialize dynamic palette
    if (animated_palette == NULL) {
        log_cb(RETRO_LOG_ERROR, "Palette not initialized\n");
        return;
    } else if (overlay1->n_palette != overlay2->n_palette) {
        log_cb(RETRO_LOG_ERROR, "Overlay palette size mismatch (%u != %u)\n",
            overlay1->n_palette, overlay2->n_palette);
        return;
    } else if (overlay1->n_palette != image.n_palette) {
        log_cb(RETRO_LOG_ERROR, "Base palette size mismatch (%u != %u)\n",
            overlay1->n_palette, image.n_palette);
        return;
    }

    // Apply blended palette
    for (size_t i = 0; i < overlay1->n_palette; i++) {
        base_palette[i] = blend_colors(overlay1->palette[i].argb,
            overlay2->palette[i].argb, ratio);
    }
    // Apply to animated palette
    memcpy(animated_palette, base_palette, image.n_palette * sizeof(uint32_t));
}

static uint32_t seconds_since_midnight(float multiplier)
{
    time_t now = time(NULL);
    time_t logical_now = start_time_logical + (time_t)((now - start_time_real) * multiplier);
    struct tm *lt = localtime(&logical_now);
    uint32_t seconds_since_midnight = lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;

    return seconds_since_midnight;
}
