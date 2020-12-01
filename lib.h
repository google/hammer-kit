/* Copyright 2020 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIB_H
#define LIB_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "params.h"
#include "util.h"

void init(struct params *p, const char *file);

uintptr_t physical_address(void *virtual_address);

uint64_t hammer(struct params *p, volatile uint8_t **addr,
		int count, int loops, int timing_loops);

uint64_t parallel_hammer(struct params *p, volatile uint8_t **addr,
		int count, int loops, int timing_loops);

void fill(struct params *p, uint8_t *base, uint8_t *mem, size_t len);
int check(struct params *p, uint8_t *base, uint8_t *mem, size_t len);

uint64_t ns(void);

#ifdef __aarch64__
#define ARM64_ISB() { asm volatile("isb"); }
#endif

uint8_t *linearize_alloc(struct params *p,
			 uint8_t *mem, uint64_t len);

/* Arch-specific helpers. */
#ifdef __aarch64__
static inline void flush(volatile void *addr)
{
	asm volatile("dc civac, %0\n\t" : : "r" (addr) : "memory");
}
#elif __x86_64__
static inline void flush(volatile void *addr)
{
	asm volatile("clflush (%0)\n\t" : : "r" (addr) : "memory");
}
#endif

#endif /* LIB_H */
