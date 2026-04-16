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

// Based on https://wiki.amigaos.net/wiki/8SVX_IFF_8-Bit_Sampled_Voice

#include <string.h>

#include "miniff.h"
#include "8svx.h"

typedef struct {
    IffParseState base;
    EsvxAudio *audio;
    bool form_present;
    uint8_t compression;
} EsvxParseState;

typedef struct {
    uint32_t sample_type;
} ChannelData;

typedef struct {
    uint32_t one_shot_hi_samples;
    uint32_t repeat_hi_samples;
    uint32_t samples_per_hi_cycle;
    uint16_t samples_per_sec;
    uint8_t ct_octave;
    uint8_t s_compression;
    uint32_t volume;
} VoiceHeader;

static CallbackStatus chunk_callback(IffParseState *state, char *chunk_id, uint32_t length);
static CallbackStatus read_vhdr_chunk(EsvxParseState *state, uint32_t length);
static CallbackStatus read_name_chunk(EsvxParseState *state, uint32_t length);
static CallbackStatus read_anno_chunk(EsvxParseState *state, uint32_t length);
static CallbackStatus read_chan_chunk(EsvxParseState *state, uint32_t length);
static CallbackStatus read_body_chunk(EsvxParseState *state, uint32_t length);

static CallbackStatus chunk_callback(IffParseState *state, char *chunk_id, uint32_t length)
{
    EsvxParseState *esvx_state = (EsvxParseState *) state;
    if (strcmp(chunk_id, "FORM:8SVX") == 0) {
        esvx_state->form_present = true;
        return CALLBACK_SUCCESS;
    } else if (strcmp(chunk_id, "VHDR") == 0) {
        return read_vhdr_chunk(esvx_state, length);
    } else if (strcmp(chunk_id, "CHAN") == 0) {
        return read_chan_chunk(esvx_state, length);
    } else if (strcmp(chunk_id, "ANNO") == 0) {
        return read_anno_chunk(esvx_state, length);
    } else if (strcmp(chunk_id, "BODY") == 0) {
        return read_body_chunk(esvx_state, length);
    } else if (state->verbose_logging) {
        fprintf(stderr, "Unknown chunk type: '%s' (%d bytes)\n",
            chunk_id, length);
    }
    return CALLBACK_UNSUPPORTED;
}

static CallbackStatus read_vhdr_chunk(EsvxParseState *state, uint32_t length)
{
    VoiceHeader vhdr;
    if (fread(&vhdr, sizeof(VoiceHeader), 1, state->base.f) != 1) {
        fprintf(stderr, "Failed to read voice header\n");
        return CALLBACK_ERROR;
    }

    state->compression = vhdr.s_compression;
    state->audio->sample_rate = BE2LE16(vhdr.samples_per_sec);

    return CALLBACK_SUCCESS;
}

static CallbackStatus read_name_chunk(EsvxParseState *state, uint32_t length)
{
    EsvxAudio *audio = state->audio;
    if (!iff_read_text_chunk(&state->base, length, &audio->name)) {
        fprintf(stderr, "Failed to read NAME chunk\n");
        return CALLBACK_ERROR;
    }
    return CALLBACK_SUCCESS;
}

static CallbackStatus read_anno_chunk(EsvxParseState *state, uint32_t length)
{
    EsvxAudio *audio = state->audio;
    if (!iff_read_text_chunk(&state->base, length, &audio->annotation)) {
        fprintf(stderr, "Failed to read ANNO chunk\n");
        return CALLBACK_ERROR;
    }
    return CALLBACK_SUCCESS;
}

static CallbackStatus read_chan_chunk(EsvxParseState *state, uint32_t length)
{
    EsvxAudio *audio = state->audio;
    ChannelData data;
    if (fread(&data, sizeof(ChannelData), 1, state->base.f) != 1) {
        fprintf(stderr, "Failed to read channel data\n");
        return CALLBACK_ERROR;
    }

    audio->channel = BE2LE32(data.sample_type);

    return CALLBACK_SUCCESS;
}

static CallbackStatus read_body_chunk(EsvxParseState *state, uint32_t length)
{
    if (state->compression != 0) {
        fprintf(stderr, "Unsupported compression type: %u\n", state->compression);
        return CALLBACK_ERROR;
    } else if (state->audio) {
        fprintf(stderr, "Audio data chunk already present\n");
        return CALLBACK_ERROR;
    }

    EsvxAudio *audio = state->audio;
    audio->n_samples = length;
    audio->samples = malloc(audio->n_samples);
    if (audio->samples == NULL) {
        fprintf(stderr, "Failed to allocate memory for samples\n");
        return CALLBACK_ERROR;
    }

    if (fread(audio->samples, 1, length, state->base.f) != length) {
        fprintf(stderr, "Failed to read sample data\n");
        free(audio->samples);
        audio->samples = NULL;
        return CALLBACK_ERROR;
    }

    return CALLBACK_SUCCESS;
}

bool esvx_read_mem(EsvxAudio *audio, const void *data, size_t size)
{
    // Open an in-memory file stream with data
    FILE *mem_file = fmemopen((void *)data, size, "rb");
    if (mem_file == NULL) {
        fprintf(stderr, "Failed to open in-memory file stream\n");
        return false;
    }

    EsvxParseState state = {
        .base = { .f = mem_file, .callback = chunk_callback },
        .audio = audio,
    };
    bool success = iff_read_file((IffParseState *) &state);
    fclose(mem_file);

    if (!state.form_present) {
        fprintf(stderr, "Error: FORM chunk not found\n");
        success = false;
    }

    if (!success) {
        esvx_free(audio);
    }

    return success;
}

bool esvx_read_file(EsvxAudio *audio, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "Failed to open file '%s'\n", path);
        return false;
    }

    EsvxParseState state = {
        .base = { .f = f, .callback = chunk_callback, .verbose_logging = true },
        .audio = audio,
    };
    bool success = iff_read_file((IffParseState *) &state);
    fclose(f);

    if (!state.form_present) {
        fprintf(stderr, "Error: FORM chunk not found\n");
        success = false;
    }

    if (!success) {
        esvx_free(audio);
    }

    return success;
}

void esvx_free(EsvxAudio *audio)
{
    free(audio->name);
    free(audio->annotation);
    free(audio->samples);
    audio->channel = CHANNEL_UNKNOWN;
    audio->name = NULL;
    audio->annotation = NULL;
    audio->samples = NULL;
    audio->n_samples = 0;
    audio->sample_rate = 0;
}

void esvx_dump(const EsvxAudio *audio)
{
    printf("Name: %s\n", audio->name);
    printf("Annotation: %s\n", audio->annotation);
    printf("Channel: %s\n", audio->channel == CHANNEL_LEFT ? "Left" :
           audio->channel == CHANNEL_RIGHT ? "Right" :
           audio->channel == CHANNEL_STEREO ? "Stereo" : "Unknown");
    printf("Number of samples: %u\n", audio->n_samples);
    printf("Sample rate: %u Hz\n", audio->sample_rate);
}
