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
#include <unistd.h>
#include <vorbis/vorbisfile.h>
#include "libretro.h"
#include "lbm.h"

retro_log_printf_t log_cb;

typedef struct {
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
static uint32_t *palette = NULL;
static bool color_blending_enabled = true;
static FILE *audio_file = NULL;
static OggVorbis_File vorbis_file = { 0 };
static char sound_buffer[AUDIO_BUFFER_SIZE] = { 0 };
static char *system_directory = NULL;

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_cb;

static void free_buffers();
static unsigned long micros();
static uint32_t blend_colors(uint32_t c1, uint32_t c2, float delta);
static void cycle_palette();
static void update_pixel_buffer();
static void check_variables();
static void fill_audio_buffer();

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

    log_cb(RETRO_LOG_INFO, "Loading: %s (size: %d, buffer: %p)\n",
        info->path, buffer_size, buffer);

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

    log_cb(RETRO_LOG_INFO, "Image loaded successfully\n");
    if (buffer != info->data) {
        free(buffer);
    }

    check_variables();

    char audio_path[2048];
    struct retro_game_info_ext *info_ext = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &info_ext)) {
        snprintf(audio_path, sizeof(audio_path), "%s/%s.ogg",
            system_directory, info_ext->name);
    }

    // Check to see if file exists; if not, disable audio
    if (*audio_path && access(audio_path, F_OK) != 0) {
        log_cb(RETRO_LOG_INFO, "Audio file not found: %s\n", audio_path);
        *audio_path = '\0';
    }

    // Init audio
    if (*audio_path) {
        audio_file = fopen(audio_path, "rb");
        if (audio_file == NULL) {
            log_cb(RETRO_LOG_WARN, "Audio file not found: %s\n", audio_path);
        } else if (ov_open_callbacks(audio_file, &vorbis_file, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0) {
            log_cb(RETRO_LOG_WARN, "Error opening audio stream: %s\n", audio_path);
            fclose(audio_file);
            audio_file = NULL;
        } else {
            log_cb(RETRO_LOG_INFO, "Audio file loaded: %s\n", audio_path);
        }
    }

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
    palette = malloc(image.n_palette * sizeof(uint32_t));
    if (palette == NULL) {
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for palette\n");
        free_buffers();
        return false;
    }
    memcpy(palette, image.palette, image.n_palette * sizeof(uint32_t));

    // Initialize cycle states
    cycle_states = malloc(image.n_cycles * sizeof(CycleState));
    if (cycle_states == NULL) { 
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for cycle states\n");
        free_buffers();
        return false;
    }
    for (size_t i = 0; i < image.n_cycles; i++) {
        Cycle *cycle = &image.cycles[i];
        int cycle_size = (cycle->high - cycle->low) + 1;
        cycle_states[i] = (CycleState) {
            .cycle_rate_us = (unsigned long)(RATE_SCALE_US / (FPS * cycle->rate)),
            .last_cycle_us = 0,
            .length = (unsigned short) cycle_size,
            .offset = 0,
        };
    }

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
}

void retro_deinit(void)
{
}

void retro_reset(void)
{
}

void retro_run(void)
{
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
    free(palette);
    palette = NULL;
    ov_clear(&vorbis_file);
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

    uint8_t r = (uint8_t)(r1 * ratio + r2 * (1.0f - ratio));
    uint8_t g = (uint8_t)(g1 * ratio + g2 * (1.0f - ratio));
    uint8_t b = (uint8_t)(b1 * ratio + b2 * (1.0f - ratio));

    return (r << 16) | (g << 8) | b;
}

static void cycle_palette()
{
    unsigned long current_time = micros();
    for (size_t i = 0; i < image.n_cycles; i++) {
        Cycle *cycle = &image.cycles[i];
        if (cycle->rate == 0) {
            continue; // No cycling
        }

        // Advance the cycle state if enough time has passed
        CycleState *state = &cycle_states[i];
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
            uint32_t pcolor = image.palette[cycle->low + (state->poffset + j) % state->length].argb;
            uint32_t color = image.palette[cycle->low + (state->offset + j) % state->length].argb;
            uint32_t blended_color = color_blending_enabled
                ? blend_colors(color, pcolor, ratio)
                : color;

            if (cycle->flags & 0x02) {
                palette[cycle->low + j] = blended_color;
            } else {
                palette[cycle->high - j] = blended_color;
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
        *pp = palette[index];
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
}

static void fill_audio_buffer()
{
    if (audio_file == NULL) {
        return; // No audio
    }

    // Read enough to fill the audio buffer
    static int current_section = 0;
    int play = sizeof(sound_buffer);
    while (play > 0) {
        long ret = ov_read(&vorbis_file, sound_buffer, sizeof(sound_buffer), 0, 2, 1, &current_section);
        if (ret == 0) {
            // End of stream, loop back to start
            ov_pcm_seek(&vorbis_file, 0);
        } else if (ret < 0) {
            // Error in audio stream; ignore
        } else {
            // Convert bytes read to frames and send to audio callback
            unsigned int frames = ret / 4; // Assuming 16-bit stereo audio
            audio_cb((const int16_t *) sound_buffer, frames); // Assuming 16-bit stereo audio
        }
        play -= ret;
    }
}
