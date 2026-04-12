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

#ifndef SCENE_H
#define SCENE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t flags;
    uint16_t rate;
    uint16_t low;
    uint16_t high;
} Cycle;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
    uint32_t n_pixels;
    uint32_t *palette;
    uint16_t n_palette;
    uint16_t s_palette;
    Cycle *cycles;
    uint16_t n_cycles;
    uint16_t s_cycles;
    char *name;
} Scene;

bool scene_read_lbm_mem(Scene *scene, const void *data, size_t size);
bool scene_read_lbm(Scene *scene, const char *path);
void scene_free(Scene *scene);
void scene_dump(const Scene *scene);

#endif // SCENE_H
