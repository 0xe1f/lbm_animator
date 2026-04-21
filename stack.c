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

// #include <stdio.h>
// #include <string.h>

#include <stdlib.h>

#include "stack.h"

struct StackEl {
    void *data;
    struct StackEl *prev;
    struct StackEl *next;
};

typedef struct StackEl StackEl;

bool stack_push(Stack *stack, void *data)
{
    StackEl *new_el = malloc(sizeof(StackEl));
    if (!new_el) {
        return false;
    }
    new_el->data = data;
    new_el->prev = stack->tail;
    new_el->next = NULL;

    if (stack->tail) {
        stack->tail->next = new_el;
    } else {
        stack->head = new_el;
    }
    stack->tail = new_el;
    stack->count++;

    return true;
}

bool stack_pop(Stack *stack, void **out_data)
{
    if (stack->tail == NULL) {
        return false;
    }

    StackEl *tail = stack->tail;
    void *data = tail->data;

    stack->tail = tail->prev;
    if (stack->tail) {
        stack->tail->next = NULL;
    } else {
        stack->head = NULL;
    }
    free(tail);
    stack->count--;

    if (out_data) {
        *out_data = data;
    }

    return true;
}

bool stack_peek(const Stack *stack, void **out_data)
{
    if (stack->tail == NULL) {
        return false;
    } else if (out_data) {
        *out_data = stack->tail->data;
    }

    return true;
}

void stack_free(Stack *stack)
{
    StackEl *current = stack->head;
    while (current) {
        StackEl *next = current->next;
        free(current);
        current = next;
    }

    stack->head = NULL;
    stack->tail = NULL;
    stack->count = 0;
}
