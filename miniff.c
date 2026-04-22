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

// https://wiki.amigaos.net/wiki/IFF_FORM_and_Chunk_Registry
// https://moddingwiki.shikadi.net/wiki/LBM_Format

#include <stdlib.h>
#include <string.h>

#include "miniff.h"

typedef struct {
    char chunk_id[4];
    uint32_t chunk_len;
} ChunkHeader;

static CallbackStatus form_callback(IffParseState *state, char *chunk_id, uint32_t length);
static CallbackStatus list_callback(IffParseState *state, char *chunk_id, uint32_t length);
static CallbackStatus read_chunks(IffParseState *state, const char *parent_chunk_id, uint32_t length);

bool iff_read_file(IffParseState *state)
{
    // Get file size
    fseek(state->f, 0, SEEK_END);
    int file_size = ftell(state->f);
    fseek(state->f, 0, SEEK_SET);

    if (state->on_enter_group == NULL) {
        fprintf(stderr, "No callback provided to handle chunk group entry\n");
        return CALLBACK_ERROR;
    } else if (state->on_read_chunk == NULL) {
        fprintf(stderr, "No callback provided to handle chunk reads\n");
        return CALLBACK_ERROR;
    } else if (state->on_exit_group == NULL) {
        fprintf(stderr, "No callback provided to handle chunk group exit\n");
        return CALLBACK_ERROR;
    }

    if (read_chunks(state, NULL, file_size) != CALLBACK_SUCCESS) {
        return false;
    }

    // Ensure we've read the entire file
    if (ftell(state->f) != file_size) {
        fprintf(stderr, "Still more to read (%ld/%d)\n",
            ftell(state->f), file_size);
        return false;
    }

    return true;
}

bool iff_decompress_rle(IffParseState *state, uint8_t *dest, uint32_t dest_len)
{
    uint32_t decompressed_len = 0;
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
            uint32_t count = 257 - value;
            memset(dest + decompressed_len, next_byte, count);
            decompressed_len += count;
        } else if (value < 128) {
            uint32_t count = value + 1;
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
        fprintf(stderr, "Decompressed data length mismatch: expected %u, got %u\n",
            dest_len, decompressed_len);
        return false;
    }

    return true;
}

bool iff_read_text_chunk(IffParseState *state, uint32_t chunk_length, char **dest)
{
    *dest = malloc(chunk_length + 1);
    if (*dest == NULL) {
        fprintf(stderr, "Failed to allocate memory\n");
        return false;
    }

    if (fread(*dest, 1, chunk_length, state->f) != chunk_length) {
        fprintf(stderr, "Failed to read text\n");
        free(*dest);
        *dest = NULL;
        return false;
    }
    (*dest)[chunk_length] = '\0';

    return true;
}

static CallbackStatus group_callback(IffParseState *state, char *chunk_id, uint32_t length)
{
    if (length < 4) {
        fprintf(stderr, "%.4s chunk too short to contain type\n", chunk_id);
        return CALLBACK_ERROR;
    }

    char format_id[4];
    if (fread(format_id, 1, 4, state->f) != 4) {
        fprintf(stderr, "Failed to read %.4s type\n", chunk_id);
        return CALLBACK_ERROR;
    }

    length -= 4; // Account for format_id bytes

    char tag[10];
    snprintf(tag, sizeof(tag), "%.4s:%.4s", chunk_id, format_id);
    CallbackStatus status = state->on_enter_group(state, tag);
    if (status == CALLBACK_ERROR) {
        fprintf(stderr, "Failed to parse %s\n", tag);
        return CALLBACK_ERROR;
    } else if (status == CALLBACK_UNSUPPORTED) {
        if (state->verbose_logging) {
            fprintf(stderr, "Unsupported: %s (%d bytes)\n", tag, length);
        }

        // Skip unsupported chunk
        fseek(state->f, length, SEEK_CUR);
        return CALLBACK_SUCCESS;
    }

    // Read the group's child chunks
    CallbackStatus read_status = read_chunks(state, tag, length);

    // Call exit group callback
    if (state->on_exit_group(state, tag) != CALLBACK_SUCCESS) {
        fprintf(stderr, "Failed to exit group %s\n", tag);
    }

    return read_status;
}

static CallbackStatus read_chunks(IffParseState *state, const char *format_id, uint32_t length)
{
    ChunkHeader header;
    int32_t left = length;
    while (left > 0) {
        if (fread(&header, sizeof(ChunkHeader), 1, state->f) != 1) {
            fprintf(stderr, "Failed to read chunk header\n");
            return CALLBACK_ERROR;
        }

        char chunk_id[5];
        snprintf(chunk_id, sizeof(chunk_id), "%.4s", header.chunk_id);
        header.chunk_len = BE2LE32(header.chunk_len);
        if (state->verbose_logging) {
            printf("%.4s: %u bytes\n", chunk_id, header.chunk_len);
        }

        state->format_id = format_id;
        CallbackStatus status;
        if (
            strcmp(chunk_id, "FORM") == 0 ||
            strcmp(chunk_id, "LIST") == 0 ||
            strcmp(chunk_id, "PROP") == 0
        ) {
            status = group_callback(state, chunk_id, header.chunk_len);
        } else {
            status = state->on_read_chunk(state, chunk_id, header.chunk_len);
        }

        if (status == CALLBACK_ERROR) {
            fprintf(stderr, "Failed to parse chunk '%s'\n", chunk_id);
            return CALLBACK_ERROR;
        } else if (status == CALLBACK_UNSUPPORTED) {
            if (state->verbose_logging) {
                fprintf(stderr, "Unsupported chunk type: '%s' (%d bytes)\n",
                    chunk_id, header.chunk_len);
            }
            // Skip unsupported chunk
            fseek(state->f, header.chunk_len, SEEK_CUR);
        }

        if (header.chunk_len % 2 == 1) {
            // Skip padding byte
            fseek(state->f, 1, SEEK_CUR);
            left--;
        }

        left -= sizeof(ChunkHeader) + header.chunk_len;
    }

    return CALLBACK_SUCCESS;
}
