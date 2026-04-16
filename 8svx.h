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

#ifndef EIGHTSVX_H
#define EIGHTSVX_H

// #include <stdbool.h>
// #include <stdint.h>
// #include <stdio.h>
#include <stdlib.h>

typedef enum {
    CHANNEL_UNKNOWN = 0,
    CHANNEL_LEFT  = 0x2,
    CHANNEL_RIGHT = 0x4,
    CHANNEL_STEREO = (CHANNEL_LEFT | CHANNEL_RIGHT),
} Channel;

typedef struct {
    int8_t *samples;
    uint32_t n_samples;
    uint16_t sample_rate;
    Channel channel;
    char *annotation;
    char *name;
} EsvxAudio;

bool esvx_read_mem(EsvxAudio *audio, const void *data, size_t size);
bool esvx_read_file(EsvxAudio *audio, const char *path);
void esvx_free(EsvxAudio *audio);
void esvx_dump(const EsvxAudio *audio);

#endif // EIGHTSVX_H
