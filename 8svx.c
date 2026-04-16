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
static CallbackStatus read_body_chunk(EsvxParseState *state, uint32_t length);

static CallbackStatus chunk_callback(IffParseState *state, char *chunk_id, uint32_t length)
{
    EsvxParseState *esvx_state = (EsvxParseState *) state;
    if (strcmp(chunk_id, "FORM:8SVX") == 0) {
        esvx_state->form_present = true;
        return CALLBACK_SUCCESS;
    } else if (strcmp(chunk_id, "VHDR") == 0) {
        return read_vhdr_chunk(esvx_state, length);
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

    if (state->base.verbose_logging) {
        fprintf(stderr, "Voice header:\n");
        fprintf(stderr, "  One-shot hi samples: %u\n", BE2LE32(vhdr.one_shot_hi_samples));
        fprintf(stderr, "  Repeat hi samples: %u\n", BE2LE32(vhdr.repeat_hi_samples));
        fprintf(stderr, "  Samples per hi cycle: %u\n", BE2LE32(vhdr.samples_per_hi_cycle));
        fprintf(stderr, "  Samples per sec: %u\n", BE2LE16(vhdr.samples_per_sec));
        fprintf(stderr, "  Ct octave: %u\n", vhdr.ct_octave);
        fprintf(stderr, "  Compression: %u\n", vhdr.s_compression);
        fprintf(stderr, "  Volume: %u\n", BE2LE32(vhdr.volume));
    }

    EsvxAudio *audio = state->audio;
    state->compression = vhdr.s_compression;
    audio->sample_rate = BE2LE16(vhdr.samples_per_sec);
    audio->channels = 1;
    audio->bytes_per_sample = 1;

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

static CallbackStatus read_body_chunk(EsvxParseState *state, uint32_t length)
{
    if (state->compression != 0) {
        fprintf(stderr, "Unsupported compression type: %u\n", state->compression);
        return CALLBACK_ERROR;
    } else if (state->audio->samples) {
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

bool esvx_resample(EsvxAudio *audio, uint8_t channels, uint8_t bytes_per_sample, uint16_t target_sample_rate)
{
    if (audio == NULL || audio->samples == NULL) {
        fprintf(stderr, "No audio buffer to resample\n");
        return false;
    }

    uint8_t src_channels = audio->channels;
    uint8_t src_bytes_per_sample = audio->bytes_per_sample;
    uint16_t src_sample_rate = audio->sample_rate;

    if ((src_channels != 1 && src_channels != 2) ||
        (src_bytes_per_sample != 1 && src_bytes_per_sample != 2) ||
        src_sample_rate == 0) {
        fprintf(stderr, "Unsupported source audio format\n");
        return false;
    }

    size_t src_frame_size = (size_t) src_channels * src_bytes_per_sample;
    uint32_t src_frames = (uint32_t) (audio->n_samples / src_frame_size);
    if (src_frames == 0) {
        free(audio->samples);
        audio->samples = NULL;
        audio->n_samples = 0;
        audio->channels = channels;
        audio->bytes_per_sample = bytes_per_sample;
        audio->sample_rate = target_sample_rate;
        return true;
    }

    uint32_t dst_frames = src_frames;
    if (src_sample_rate != target_sample_rate) {
        dst_frames = (uint32_t) ((((uint64_t) src_frames * target_sample_rate) +
            (src_sample_rate / 2)) / src_sample_rate);
        if (dst_frames == 0) {
            dst_frames = 1;
        }
    }

    size_t dst_frame_size = (size_t) channels * bytes_per_sample;
    size_t dst_size = (size_t) dst_frames * dst_frame_size;
    int8_t *dst = malloc(dst_size);
    if (dst == NULL) {
        fprintf(stderr, "Failed to allocate memory for resampled audio\n");
        return false;
    }

    for (uint32_t i = 0; i < dst_frames; i++) {
        uint32_t src_index = i;
        if (src_sample_rate != target_sample_rate) {
            src_index = (uint32_t) (((uint64_t) i * src_sample_rate) / target_sample_rate);
            if (src_index >= src_frames) {
                src_index = src_frames - 1;
            }
        }

        for (uint8_t c = 0; c < channels; c++) {
            int32_t sample16;

            if (src_channels == 1) {
                if (src_bytes_per_sample == 1) {
                    int8_t v = audio->samples[src_index];
                    sample16 = ((int32_t) v) << 8;
                } else {
                    int16_t v;
                    memcpy(&v, &audio->samples[src_index * 2], sizeof(v));
                    sample16 = v;
                }
            } else if (channels == 1) {
                int32_t left, right;
                if (src_bytes_per_sample == 1) {
                    int8_t l = audio->samples[src_index * 2];
                    int8_t r = audio->samples[src_index * 2 + 1];
                    left = ((int32_t) l) << 8;
                    right = ((int32_t) r) << 8;
                } else {
                    int16_t l, r;
                    size_t base = (size_t) src_index * 4;
                    memcpy(&l, &audio->samples[base], sizeof(l));
                    memcpy(&r, &audio->samples[base + 2], sizeof(r));
                    left = l;
                    right = r;
                }
                sample16 = (left + right) / 2;
            } else {
                if (src_bytes_per_sample == 1) {
                    int8_t v = audio->samples[(size_t) src_index * 2 + c];
                    sample16 = ((int32_t) v) << 8;
                } else {
                    int16_t v;
                    size_t base = (size_t) src_index * 4 + (size_t) c * 2;
                    memcpy(&v, &audio->samples[base], sizeof(v));
                    sample16 = v;
                }
            }

            if (bytes_per_sample == 1) {
                dst[(size_t) i * channels + c] = (int8_t) (sample16 >> 8);
            } else {
                int16_t out = (int16_t) sample16;
                size_t base = (size_t) i * channels * 2 + (size_t) c * 2;
                memcpy(&dst[base], &out, sizeof(out));
            }
        }
    }

    free(audio->samples);
    audio->samples = dst;
    audio->n_samples = (uint32_t) dst_frames * channels;
    audio->channels = channels;
    audio->bytes_per_sample = bytes_per_sample;
    audio->sample_rate = target_sample_rate;

    return true;
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
        .base = { .f = f, .callback = chunk_callback },
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
    audio->channels = 0;
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
    printf("Channels: %u\n", audio->channels);
    printf("Number of samples: %u\n", audio->n_samples);
    printf("Bytes per sample: %u\n", audio->bytes_per_sample);
    printf("Sample rate: %u Hz\n", audio->sample_rate);
}
