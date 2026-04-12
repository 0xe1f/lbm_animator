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
#include "libretro.h"
#include "scene.pb-c.h"

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

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static Scene *scene = NULL;
static void *pixel_buffer = NULL;
static size_t pixel_buffer_size = 0;
static int pixel_buffer_bpp = 4;
static CycleState *cycle_states = NULL;
static uint32_t *palette = NULL;
static bool color_blending_enabled = true;

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
            "pbbin|pbbin.gz", /* extensions */
            false,   /* need_fullpath */
            false    /* persistent_data */
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
    info->valid_extensions = "pbbin|pbbin.gz";
    info->block_extract = false;
    info->need_fullpath = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->geometry.base_width  = scene->width;
    info->geometry.base_height = scene->height;
    info->geometry.max_width = scene->width;
    info->geometry.max_height = scene->height;
    info->geometry.aspect_ratio  = (float)scene->width / (float)scene->height;
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

    scene = scene__unpack(NULL, buffer_size, buffer);
    if (scene == NULL) {
        log_cb(RETRO_LOG_ERROR, "Protobuf unpacking failed\n");
        return false;
    }

    log_cb(RETRO_LOG_INFO, "Scene loaded successfully\n");
    if (buffer != info->data) {
        free(buffer);
    }

    check_variables();

    unsigned int format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        log_cb(RETRO_LOG_INFO, "Pixel format set to XRGB8888\n");
    } else {
        log_cb(RETRO_LOG_ERROR, "Failed to set pixel format\n");
    }

    // Initialize pixel buffer
    pixel_buffer_size = scene->width * scene->height * pixel_buffer_bpp;
    if ((pixel_buffer = malloc(pixel_buffer_size)) == NULL) {
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for pixel buffer\n");
        return false;
    }

    // Initialize dynamic palette
    palette = malloc(scene->n_palette * sizeof(uint32_t));
    if (palette == NULL) {
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for palette\n");
        free_buffers();
        return false;
    }
    memcpy(palette, scene->palette, scene->n_palette * sizeof(uint32_t));

    // Initialize cycle states
    cycle_states = malloc(scene->n_cycles * sizeof(CycleState));
    if (cycle_states == NULL) { 
        log_cb(RETRO_LOG_ERROR, "Failed to allocate memory for cycle states\n");
        free_buffers();
        return false;
    }
    for (size_t i = 0; i < scene->n_cycles; i++) {
        Scene__Cycle *cycle = scene->cycles[i];
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

    video_cb(pixel_buffer, scene->width, scene->height, scene->width * pixel_buffer_bpp);
}

static void free_buffers()
{
    scene__free_unpacked(scene, NULL);
    scene = NULL;
    free(pixel_buffer);
    pixel_buffer = NULL;
    pixel_buffer_size = 0;
    free(cycle_states);
    cycle_states = NULL;
    free(palette);
    palette = NULL;
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
    for (size_t i = 0; i < scene->n_cycles; i++) {
        Scene__Cycle *cycle = scene->cycles[i];
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
            uint32_t pcolor = scene->palette[cycle->low + (state->poffset + j) % state->length];
            uint32_t color = scene->palette[cycle->low + (state->offset + j) % state->length];
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
    for (i = 0, pp = pixel_buffer; i < scene->width * scene->height; i++, pp++) {
        unsigned int index = scene->pixels.data[i];
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
