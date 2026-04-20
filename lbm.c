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

#include <stdio.h>
#include <string.h>

#include "miniff.h"
#include "lbm.h"

#define CYCLE_CAP_INITIAL     16
#define CYCLE_CAP_INCREMENT   16
#define PALETTE_CAP_INITIAL   64
#define PALETTE_CAP_INCREMENT 64
#define BBM_CAP_INITIAL   64
#define BBM_CAP_INCREMENT 64

#define CMAP_ENTRY_TO_COLOR(x) \
    (Color) { .channels = { .a = 0xff, .r = (x).r, .g = (x).g, .b = (x).b } }

typedef struct {
    IffParseState base;
    LbmImage *image;
    bool is_rle_compressed;
    LbmImage *current;
} LbmParseState;

typedef struct {
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
} BitmapHeader;

typedef struct {
    uint16_t padding;
    uint16_t rate;
    uint16_t flags;
    uint8_t low;
    uint8_t high;
} ColorRange;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ColorMapEntry;

static CallbackStatus chunk_callback(IffParseState *state, char *chunk_id, uint32_t length);
static CallbackStatus read_bmhd_chunk(LbmParseState *state, uint32_t length);
static CallbackStatus read_crng_chunk(LbmParseState *state, uint32_t length);
static CallbackStatus read_cmap_chunk(LbmParseState *state, uint32_t length);
static CallbackStatus read_body_chunk(LbmParseState *state, uint32_t length);
static CallbackStatus read_name_chunk(LbmParseState *state, uint32_t length);
static CallbackStatus read_tmln_chunk(LbmParseState *state, uint32_t length);
static void lbm_dump_indented(const LbmImage *image, char *indent);

static CallbackStatus chunk_callback(IffParseState *state, char *chunk_id, uint32_t length)
{
    LbmParseState *lbm_state = (LbmParseState *) state;
    if (strcmp(chunk_id, "FORM:PBM ") == 0) {
        if (lbm_state->current == NULL) {
            // Top-level FORM chunk
            lbm_state->current = lbm_state->image;
        } else {
            // Nested FORM chunk (e.g. for BBM)
            // FIXME: this is incorrect, since it is not recursive
            LbmImage *parent = lbm_state->image;
            if (parent->n_bbms >= parent->s_bbms) {
                uint16_t new_size = parent->s_bbms == 0
                    ? BBM_CAP_INITIAL
                    : parent->s_bbms + BBM_CAP_INCREMENT;
                LbmImage *new_bbms = realloc(parent->bbms, new_size * sizeof(LbmImage));
                if (new_bbms == NULL) {
                    fprintf(stderr, "Failed to allocate memory for BBMs\n");
                    return CALLBACK_ERROR;
                }
                parent->bbms = new_bbms;
                parent->s_bbms = new_size;
            }
            LbmImage *new_image = &parent->bbms[parent->n_bbms++];
            memset(new_image, 0, sizeof(LbmImage));
            lbm_state->current = new_image;
        }
        return CALLBACK_SUCCESS;
    } else if (strcmp(chunk_id, "PROP:PBM ") == 0) {
        lbm_state->current = lbm_state->image;
        return CALLBACK_SUCCESS;
    } else if (strcmp(chunk_id, "LIST:PBM ") == 0) {
        return CALLBACK_SUCCESS;
    } else if (strcmp(chunk_id, "BMHD") == 0) {
        return read_bmhd_chunk(lbm_state, length);
    } else if (strcmp(chunk_id, "CRNG") == 0) {
        return read_crng_chunk(lbm_state, length);
    } else if (strcmp(chunk_id, "NAME") == 0) {
        return read_name_chunk(lbm_state, length);
    } else if (strcmp(chunk_id, "CMAP") == 0) {
        return read_cmap_chunk(lbm_state, length);
    } else if (strcmp(chunk_id, "BODY") == 0) {
        return read_body_chunk(lbm_state, length);
    } else if (strcmp(chunk_id, "TMLN") == 0) {
        return read_tmln_chunk(lbm_state, length);
    } else if (state->verbose_logging) {
        fprintf(stderr, "Unknown chunk type: '%s' (%d bytes)\n",
            chunk_id, length);
    }
    return CALLBACK_UNSUPPORTED;
}

static CallbackStatus read_bmhd_chunk(LbmParseState *state, uint32_t length)
{
    BitmapHeader bmhd;
    if (fread(&bmhd, sizeof(BitmapHeader), 1, state->base.f) != 1) {
        fprintf(stderr, "Failed to read bitmap header\n");
        return CALLBACK_ERROR;
    }

    if (state->base.verbose_logging) {
        printf("Bitmap header:\n");
        printf("  Width: %u\n", BE2LE16(bmhd.width));
        printf("  Height: %u\n", BE2LE16(bmhd.height));
        printf("  X origin: %d\n", BE2LE16(bmhd.x_origin));
        printf("  Y origin: %d\n", BE2LE16(bmhd.y_origin));
        printf("  Number of planes: %u\n", bmhd.num_planes);
        printf("  Masking method: %u\n", bmhd.mask);
        printf("  Compression method: %u\n", bmhd.compression);
        printf("  Transparent color: %u\n", BE2LE16(bmhd.trans_clr));
        printf("  X aspect ratio: %u\n", bmhd.x_aspect);
        printf("  Y aspect ratio: %u\n", bmhd.y_aspect);
        printf("  Page width: %d\n", BE2LE16(bmhd.page_width));
        printf("  Page height: %d\n", BE2LE16(bmhd.page_height));
    }

    if (bmhd.num_planes != 8) {
        fprintf(stderr, "Unsupported number of planes: %u\n", bmhd.num_planes);
        return CALLBACK_ERROR;
    }

    LbmImage *image = state->current;
    if (image == NULL) {
        fprintf(stderr, "BMHD chunk found before image context is set\n");
        return CALLBACK_ERROR;
    }
    image->width = BE2LE16(bmhd.width);
    image->height = BE2LE16(bmhd.height);
    state->is_rle_compressed = (bmhd.compression == 1);

    return CALLBACK_SUCCESS;
}

static CallbackStatus read_crng_chunk(LbmParseState *state, uint32_t length)
{
    ColorRange crng;
    if (fread(&crng, sizeof(ColorRange), 1, state->base.f) != 1) {
        fprintf(stderr, "Failed to read color range\n");
        return CALLBACK_ERROR;
    }

    LbmImage *image = state->current;
    if (image == NULL) {
        fprintf(stderr, "CRNG chunk found before image context is set\n");
        return CALLBACK_ERROR;
    }
    if (image->n_cycles >= image->s_cycles) {
        uint16_t new_size = image->s_cycles == 0
            ? CYCLE_CAP_INITIAL
            : image->s_cycles + CYCLE_CAP_INCREMENT;
        Cycle *new_cycles = realloc(image->cycles, new_size * sizeof(Cycle));
        if (new_cycles == NULL) {
            fprintf(stderr, "Failed to allocate memory for cycles\n");
            return CALLBACK_ERROR;
        }
        image->cycles = new_cycles;
        image->s_cycles = new_size;
    }
    image->cycles[image->n_cycles++] = (Cycle) {
        // TODO: documentation specifies that this is a BE value;
        //       however, that does not match up with bit positions
        .flags = crng.flags,
        .rate = BE2LE16(crng.rate),
        .low = crng.low,
        .high = crng.high
    };

    return CALLBACK_SUCCESS;
}

static CallbackStatus read_cmap_chunk(LbmParseState *state, uint32_t length)
{
    if (length % 3 != 0) {
        fprintf(stderr, "Invalid CMAP chunk length: %u\n", length);
        return CALLBACK_ERROR;
    }

    uint16_t n_entries = length / 3;
    for (uint16_t i = 0; i < n_entries; i++) {
        ColorMapEntry entry;
        if (fread(&entry, sizeof(entry), 1, state->base.f) != 1) {
            fprintf(stderr, "Failed to read palette entry\n");
            return CALLBACK_ERROR;
        }

        LbmImage *image = state->current;
        if (image == NULL) {
            fprintf(stderr, "CMAP chunk found before image context is set\n");
            return CALLBACK_ERROR;
        }
        if (image->n_palette + n_entries > image->s_palette) {
            uint16_t new_size = image->s_palette == 0
                ? PALETTE_CAP_INITIAL
                : image->s_palette + PALETTE_CAP_INCREMENT;
            Color *new_palette = realloc(image->palette, new_size * sizeof(Color));
            if (new_palette == NULL) {
                fprintf(stderr, "Failed to allocate memory for palette\n");
                return CALLBACK_ERROR;
            }
            image->palette = new_palette;
            image->s_palette = new_size;
        }

        image->palette[image->n_palette++] = CMAP_ENTRY_TO_COLOR(entry);
    }

    return CALLBACK_SUCCESS;
}

static CallbackStatus read_body_chunk(LbmParseState *state, uint32_t length)
{
    LbmImage *image = state->current;
    if (image == NULL) {
        fprintf(stderr, "BODY chunk found before image context is set\n");
        return CALLBACK_ERROR;
    }
    image->n_pixels = image->width * image->height;
    image->pixels = malloc(image->n_pixels);
    if (image->pixels == NULL) {
        fprintf(stderr, "Failed to allocate memory for pixels\n");
        return CALLBACK_ERROR;
    }

    long pos = ftell(state->base.f);
    if (state->is_rle_compressed) {
        if (!iff_decompress_rle(&state->base, image->pixels, image->n_pixels)) {
            fprintf(stderr, "Failed to decompress RLE data\n");
            free(image->pixels);
            image->pixels = NULL;
            return CALLBACK_ERROR;
        }
    } else if (length != image->n_pixels) {
        fprintf(stderr, "BODY chunk length does not match expected pixel data length\n");
        free(image->pixels);
        image->pixels = NULL;
        return CALLBACK_ERROR;
    } else {
        if (fread(image->pixels, 1, length, state->base.f) != length) {
            fprintf(stderr, "Failed to read pixel data\n");
            free(image->pixels);
            image->pixels = NULL;
            return CALLBACK_ERROR;
        }
    }

    int bytes_left_over = length - (ftell(state->base.f) - pos);
    if (bytes_left_over > 0) {
        fseek(state->base.f, bytes_left_over, SEEK_CUR);
    }

    return CALLBACK_SUCCESS;
}

static CallbackStatus read_name_chunk(LbmParseState *state, uint32_t length)
{
    LbmImage *image = state->current;
    if (image == NULL) {
        fprintf(stderr, "NAME chunk found before image context is set\n");
        return CALLBACK_ERROR;
    }
    if (!iff_read_text_chunk(&state->base, length, &image->name)) {
        fprintf(stderr, "Failed to read NAME chunk\n");
        return CALLBACK_ERROR;
    }
    return CALLBACK_SUCCESS;
}

static CallbackStatus read_tmln_chunk(LbmParseState *state, uint32_t length)
{
    size_t struct_size = sizeof(TimelineEntry);
    if (length % struct_size != 0) {
        fprintf(stderr, "Invalid TMLN chunk length: %u\n", length);
        return CALLBACK_ERROR;
    }

    LbmImage *image = state->current;
    image->timelines = malloc(length);
    if (image->timelines == NULL) {
        fprintf(stderr, "Failed to allocate memory for timelines\n");
        return CALLBACK_ERROR;
    }

    image->n_timelines = length / struct_size;
    for (uint16_t i = 0; i < image->n_timelines; i++) {
        if (fread(&image->timelines[i], struct_size, 1, state->base.f) != 1) {
            fprintf(stderr, "Failed to read timeline entry\n");
            return CALLBACK_ERROR;
        }
        image->timelines[i].offset_secs = BE2LE32(image->timelines[i].offset_secs);
    }

    if (state->base.verbose_logging) {
        printf("Read %u timeline entries\n", image->n_timelines);
    }

    return CALLBACK_SUCCESS;
}

static void lbm_dump_indented(const LbmImage *image, char *indent)
{
    printf("%sDimensions: %ux%u\n", indent, image->width, image->height);
    printf("%sName: %s\n", indent, image->name);
    printf("%sPalette (%u colors)\n", indent, image->n_palette);
    // for (uint16_t i = 0; i < image->n_palette; i++) {
    //     printf("%s  %d: 0x%08x\n", indent, i, image->palette[i].argb);
    // }
    printf("%sCycles (%u)\n", indent, image->n_cycles);
    // for (uint16_t i = 0; i < image->n_cycles; i++) {
    //     Cycle *c = &image->cycles[i];
    //     printf("%s  %u: flags=0x%02x, rate=%u, low=%u, high=%u\n",
    //         indent, i, c->flags, c->rate, c->low, c->high);
    // }
    printf("%sTimelines (%u)\n", indent, image->n_timelines);
    // for (uint16_t i = 0; i < image->n_timelines; i++) {
    //     TimelineEntry *e = &image->timelines[i];
    //     printf("%s  %u: offset=%u secs, index=%u\n",
    //         indent, i, e->offset_secs, e->index);
    // }

    printf("%sBBMs (%u)\n", indent, image->n_bbms);
    char next_indent[256];
    snprintf(next_indent, sizeof(next_indent), "%s  ", indent);

    for (uint16_t i = 0; i < image->n_bbms; i++) {
        printf("%s%u:\n", indent, i);
        lbm_dump_indented(&image->bbms[i], next_indent);
    }
}

bool lbm_read_mem(LbmImage *image, const void *data, size_t size)
{
    // Open an in-memory file stream with data
    FILE *mem_file = fmemopen((void *)data, size, "rb");
    if (mem_file == NULL) {
        fprintf(stderr, "Failed to open in-memory file stream\n");
        return false;
    }

    LbmParseState state = {
        .base = { .f = mem_file, .callback = chunk_callback },
        .image = image,
        .current = NULL,
    };
    bool success = iff_read_file((IffParseState *) &state);
    fclose(mem_file);

    if (state.current == NULL) {
        fprintf(stderr, "Error: FORM chunk not found\n");
        success = false;
    }

    if (!success) {
        lbm_free(image);
    }

    return success;
}

bool lbm_read_file(LbmImage *image, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "Failed to open file '%s'\n", path);
        return false;
    }

    LbmParseState state = {
        .base = { .f = f, .callback = chunk_callback },
        .image = image,
        .current = NULL,
    };
    bool success = iff_read_file((IffParseState *) &state);
    fclose(f);

    if (state.current == NULL) {
        fprintf(stderr, "Error: FORM chunk not found\n");
        success = false;
    }

    if (!success) {
        lbm_free(image);
    }

    return success;
}

void lbm_free(LbmImage *image)
{
    free(image->pixels);
    free(image->palette);
    free(image->cycles);
    free(image->name);
    free(image->bbms);
    free(image->timelines);

    image->pixels = NULL;
    image->n_pixels = 0;
    image->palette = NULL;
    image->n_palette = 0;
    image->s_palette = 0;
    image->cycles = NULL;
    image->n_cycles = 0;
    image->s_cycles = 0;
    image->name = NULL;
    image->bbms = NULL;
    image->n_bbms = 0;
    image->s_bbms = 0;
    image->timelines = NULL;
    image->n_timelines = 0;
}

void lbm_dump(const LbmImage *image)
{
    lbm_dump_indented(image, "");
}
