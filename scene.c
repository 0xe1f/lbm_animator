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

// Based on https://moddingwiki.shikadi.net/wiki/LBM_Format

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene.h"

#define CYCLE_CAP_INITIAL     16
#define CYCLE_CAP_INCREMENT   16
#define PALETTE_CAP_INITIAL   64
#define PALETTE_CAP_INCREMENT 64

#define BE2LE32(num) \
    ((num>>24)&0xff) | \
    ((num<<8)&0xff0000) | \
    ((num>>8)&0xff00) | \
    ((num<<24)&0xff000000)
#define BE2LE16(num) \
    ((num>>8)&0xff) | \
    ((num<<8)&0xff00)
#define CMAP_ENTRY_TO_ARGB(e) \
    0xff000000 | ((e).r << 16) | ((e).g << 8) | ((e).b)

struct ParseState {
    FILE *f;
    Scene *scene;
    bool is_rle_compressed;
};

struct ChunkHeader {
    char chunk_id[4];
    uint32_t chunk_len;
};

struct BitmapHeader {
    uint16_t width;
    uint16_t height;
    int16_t x_origin;
    int16_t y_origin;
    uint8_t num_planes;
    uint8_t mask;
    uint8_t compression;
    uint8_t pad1;
    uint16_t trans_clr;
    uint8_t x_aspect;
    uint8_t y_aspect;
    int16_t page_width;
    int16_t page_height;
};

struct ColorRange {
    uint16_t padding;
    uint16_t rate;
    uint16_t flags;
    uint8_t low;
    uint8_t high;
};

struct ColorMapEntry {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

static bool read_lbm(struct ParseState *state);
static bool read_form_chunk(struct ParseState *state, uint32_t length);
static bool read_pbm_chunk(struct ParseState *state, uint32_t length);
static bool read_bmhd_chunk(struct ParseState *state, uint32_t length);
static bool read_crng_chunk(struct ParseState *state, uint32_t length);
static bool read_cmap_chunk(struct ParseState *state, uint32_t length);
static bool read_body_chunk(struct ParseState *state, uint32_t length);
static bool read_name_chunk(struct ParseState *state, uint32_t length);
static bool decompress_rle(struct ParseState *state, uint8_t *dest, uint32_t dest_len);

static bool read_lbm(struct ParseState *state)
{
    // Get file size
    fseek(state->f, 0, SEEK_END);
    int file_size = ftell(state->f);
    fseek(state->f, 0, SEEK_SET);

    bool success = read_form_chunk(state, file_size);
    if (!success) {
        return false;
    }

    if (ftell(state->f) != file_size) {
        fprintf(stderr, "Still more to read (%ld/%d)\n",
            ftell(state->f), file_size);
        return false;
    }

    return true;
}

static bool read_form_chunk(struct ParseState *state, uint32_t length)
{
    struct ChunkHeader header;
    if (fread(&header, sizeof(struct ChunkHeader), 1, state->f) != 1) {
        fprintf(stderr, "Failed to read chunk header\n");
        return false;
    }

    if (strncmp(header.chunk_id, "FORM", 4) != 0) {
        fprintf(stderr, "FORM chunk not found\n");
        return false;
    }

    header.chunk_len = BE2LE32(header.chunk_len);
    printf("%.4s: %u bytes\n", header.chunk_id, header.chunk_len);

    char form_type[4];
    if (fread(form_type, 1, 4, state->f) != 4) {
        fprintf(stderr, "Failed to read FORM type\n");
        return false;
    }

    if (strncmp(form_type, "PBM ", 4) == 0) {
        return read_pbm_chunk(state, header.chunk_len - 4);
    } else if (strncmp(form_type, "ILBM", 4) == 0) {
        fprintf(stderr, "Unsupported FORM type: '%.4s'\n", form_type);
        return false;
    } else {
        fprintf(stderr, "Unsupported FORM type: '%.4s'\n", form_type);
        return false;
    }

    return true;
}

static bool read_pbm_chunk(struct ParseState *state, uint32_t length)
{
    struct ChunkHeader header;
    uint32_t left = length;
    while (left > 0) {
        if (fread(&header, sizeof(struct ChunkHeader), 1, state->f) != 1) {
            fprintf(stderr, "Failed to read chunk header\n");
            return false;
        }

        header.chunk_len = BE2LE32(header.chunk_len);
        printf("%.4s: %u bytes\n", header.chunk_id, header.chunk_len);
        if (strncmp(header.chunk_id, "BMHD", 4) == 0) {
            if (!read_bmhd_chunk(state, header.chunk_len)) {
                return false;
            }
        } else if (strncmp(header.chunk_id, "CRNG", 4) == 0) {
            if (!read_crng_chunk(state, header.chunk_len)) {
                return false;
            }
        } else if (strncmp(header.chunk_id, "CMAP", 4) == 0) {
            if (!read_cmap_chunk(state, header.chunk_len)) {
                return false;
            }
        } else if (strncmp(header.chunk_id, "BODY", 4) == 0) {
            if (!read_body_chunk(state, header.chunk_len)) {
                return false;
            }
        } else if (strncmp(header.chunk_id, "NAME", 4) == 0) {
            if (!read_name_chunk(state, header.chunk_len)) {
                return false;
            }
        } else {
            fprintf(stderr, "Unsupported chunk type: '%.4s' (%d bytes)\n",
                header.chunk_id, header.chunk_len);
            fseek(state->f, header.chunk_len, SEEK_CUR);
        }

        left -= sizeof(struct ChunkHeader) + header.chunk_len;
    }

    return true;
}

static bool read_bmhd_chunk(struct ParseState *state, uint32_t length)
{
    struct BitmapHeader bmhd;
    if (fread(&bmhd, sizeof(struct BitmapHeader), 1, state->f) != 1) {
        fprintf(stderr, "Failed to read bitmap header\n");
        return false;
    }

    if (bmhd.num_planes != 8) {
        fprintf(stderr, "Unsupported number of planes: %u\n", bmhd.num_planes);
        return false;
    }

    Scene *scene = state->scene;
    scene->width = BE2LE16(bmhd.width);
    scene->height = BE2LE16(bmhd.height);
    state->is_rle_compressed = (bmhd.compression == 1);

    return true;
}

static bool read_crng_chunk(struct ParseState *state, uint32_t length)
{
    struct ColorRange crng;
    if (fread(&crng, sizeof(struct ColorRange), 1, state->f) != 1) {
        fprintf(stderr, "Failed to read color range\n");
        return false;
    }

    Scene *scene = state->scene;
    if (scene->n_cycles >= scene->s_cycles) {
        uint16_t new_size = scene->s_cycles == 0
            ? CYCLE_CAP_INITIAL
            : scene->s_cycles + CYCLE_CAP_INCREMENT;
        Cycle *new_cycles = realloc(scene->cycles, new_size * sizeof(Cycle));
        if (new_cycles == NULL) {
            fprintf(stderr, "Failed to allocate memory for cycles\n");
            return false;
        }
        scene->cycles = new_cycles;
        scene->s_cycles = new_size;
    }
    scene->cycles[scene->n_cycles++] = (Cycle) {
        // TODO: documentation specifies that this is a BE value;
        //       however, that does not match up with bit positions
        .flags = crng.flags,
        .rate = BE2LE16(crng.rate),
        .low = crng.low,
        .high = crng.high
    };

    return true;
}

static bool read_cmap_chunk(struct ParseState *state, uint32_t length)
{
    if (length % 3 != 0) {
        fprintf(stderr, "Invalid CMAP chunk length: %u\n", length);
        return false;
    }

    uint16_t n_entries = length / 3;
    for (uint16_t i = 0; i < n_entries; i++) {
        struct ColorMapEntry entry;
        if (fread(&entry, sizeof(struct ColorMapEntry), 1, state->f) != 1) {
            fprintf(stderr, "Failed to read palette entry\n");
            return false;
        }

        Scene *scene = state->scene;
        if (scene->n_palette + n_entries > scene->s_palette) {
            uint16_t new_size = scene->s_palette == 0
                ? PALETTE_CAP_INITIAL
                : scene->s_palette + PALETTE_CAP_INCREMENT;
            uint32_t *new_palette = realloc(scene->palette, new_size * sizeof(uint32_t));
            if (new_palette == NULL) {
                fprintf(stderr, "Failed to allocate memory for palette\n");
                return false;
            }
            scene->palette = new_palette;
            scene->s_palette = new_size;
        }

        scene->palette[scene->n_palette++] = CMAP_ENTRY_TO_ARGB(entry);
    }

    return true;
}

static bool read_body_chunk(struct ParseState *state, uint32_t length)
{
    Scene *scene = state->scene;
    scene->n_pixels = scene->width * scene->height;
    scene->pixels = malloc(scene->n_pixels);
    if (scene->pixels == NULL) {
        fprintf(stderr, "Failed to allocate memory for pixels\n");
        return false;
    }

    long pos = ftell(state->f);
    if (state->is_rle_compressed) {
        if (!decompress_rle(state, scene->pixels, scene->n_pixels)) {
            return false;
        }
    } else if (length != scene->n_pixels) {
        fprintf(stderr, "BODY chunk length does not match expected pixel data length\n");
        return false;
    } else {
        if (fread(scene->pixels, 1, length, state->f) != length) {
            fprintf(stderr, "Failed to read pixel data\n");
            return false;
        }
    }

    int bytes_left_over = length - (ftell(state->f) - pos);
    if (bytes_left_over > 0) {
        fseek(state->f, bytes_left_over, SEEK_CUR);
    }

    return true;
}

static bool read_name_chunk(struct ParseState *state, uint32_t length)
{
    Scene *scene = state->scene;
    scene->name = malloc(length + 1);
    if (scene->name == NULL) {
        fprintf(stderr, "Failed to allocate memory for NAME\n");
        return false;
    }

    if (fread(scene->name, 1, length, state->f) != length) {
        fprintf(stderr, "Failed to read NAME\n");
        return false;
    }
    scene->name[length] = '\0';

    return true;
}

static bool decompress_rle(struct ParseState *state, uint8_t *dest, uint32_t dest_len)
{
    int decompressed_len = 0;
    while (decompressed_len < dest_len) {
        uint8_t value;
        if (fread(&value, 1, 1, state->f) != 1) {
            fprintf(stderr, "Failed to read compressed data\n");
            return false;
        }

        if (value > 128) {
            uint8_t next_byte;
            if (fread(&next_byte, 1, 1, state->f) != 1) {
                fprintf(stderr, "Failed to read compressed data\n");
                return false;
            }
            int count = 257 - value;
            memset(dest + decompressed_len, next_byte, count);
            decompressed_len += count;
        } else if (value < 128) {
            int count = value + 1;
            if (fread(dest + decompressed_len, 1, count, state->f) != count) {
                fprintf(stderr, "Failed to read compressed data\n");
                return false;
            }
            decompressed_len += count;
        } else {
            break; // value == 128
        }
    }

    if (decompressed_len != dest_len) {
        fprintf(stderr, "Decompressed data length mismatch: expected %d, got %d\n",
            dest_len, decompressed_len);
        return false;
    }

    return true;
}

void scene_dump(const Scene *scene)
{
    printf("Dimensions: %ux%u\n", scene->width, scene->height);
    printf("Name: %s\n", scene->name);
    for (uint16_t i = 0; i < scene->n_palette; i++) {
        uint32_t color = scene->palette[i];
        printf("Color %d: 0x%08x\n", i, color);
    }
    for (uint16_t i = 0; i < scene->n_cycles; i++) {
        Cycle *c = &scene->cycles[i];
        printf("Cycle %u: flags=0x%02x, rate=%u, low=%u, high=%u\n",
            i, c->flags, c->rate, c->low, c->high);
    }
}

void scene_free(Scene *scene)
{
    free(scene->pixels);
    free(scene->palette);
    free(scene->cycles);
    free(scene->name);

    scene->pixels = NULL;
    scene->n_pixels = 0;
    scene->palette = NULL;
    scene->n_palette = 0;
    scene->s_palette = 0;
    scene->cycles = NULL;
    scene->n_cycles = 0;
    scene->s_cycles = 0;
    scene->name = NULL;
}

bool scene_read_lbm(Scene *scene, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "Failed to open file '%s'\n", path);
        return false;
    }

    struct ParseState state = { .scene = scene, .f = f };
    bool success = read_lbm(&state);
    if (!success) {
        scene_free(scene);
    }
    fclose(f);

    return success;
}

bool scene_read_lbm_mem(Scene *scene, const void *data, size_t size)
{
    // Create a temporary file and write the data to it
    FILE *temp_file = tmpfile();
    if (temp_file == NULL) {
        fprintf(stderr, "Failed to create temporary file\n");
        return false;
    }

    if (fwrite(data, 1, size, temp_file) != size) {
        fprintf(stderr, "Failed to write data to temporary file\n");
        fclose(temp_file);
        return false;
    }
    rewind(temp_file);

    struct ParseState state = { .scene = scene, .f = temp_file };
    bool success = read_lbm(&state);
    if (!success) {
        scene_free(scene);
    }
    fclose(temp_file);

    return success;
}
