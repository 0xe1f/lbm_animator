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

#ifndef STACK_H
#define STACK_H

#include <stdbool.h>
#include <stdint.h>

struct StackEl;

typedef struct {
    struct StackEl *head;
    struct StackEl *tail;
    uint32_t count;
} Stack;

bool stack_push(Stack *stack, void *data);
bool stack_pop(Stack *stack, void **out_data);
bool stack_peek(const Stack *stack, void **out_data);
void stack_free(Stack *stack);

#endif // STACK_H
