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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    int8_t *samples;
    uint32_t n_samples;
    uint16_t sample_rate;
    uint8_t channels;
    uint8_t bytes_per_sample;
    char *annotation;
    char *name;
} Svx8Audio;

bool svx8_resample(Svx8Audio *audio, uint8_t channels, uint8_t bytes_per_sample, uint16_t target_sample_rate);
bool svx8_read_mem(Svx8Audio *audio, const void *data, size_t size);
bool svx8_read_file(Svx8Audio *audio, const char *path);
void svx8_free(Svx8Audio *audio);
void svx8_dump(const Svx8Audio *audio);

#endif // EIGHTSVX_H
