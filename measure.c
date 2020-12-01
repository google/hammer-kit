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

#include "lib.h"
#include "mm.h"

static void measure(struct params *p, void *mem, int step, size_t len)
{
	uint8_t *buf = mem;
	uint8_t *base = mem + p->offset0;

	volatile uint8_t *addr[2];

	addr[0] = buf;

	printf("Step %d, len %zu\n", step, len);
	printf("@Offset from base\toffset0\ttime (us)\n");
	for (uint64_t i = 0; i < len; i += step) {
		uint8_t *d = base + i;

		addr[1] = d;
		uint64_t t = hammer(p, addr, 2, p->measure_loops/5, 5);

		printf("@%08zx\t%zu\t%zu\n", d-buf, d-base, t/1000);
	}
}

int main(int argc, char *argv[])
{
	struct params p;

	if (argc != 4)
		die("Usage: %s config step length.\n", argv[0]);

	char *cfg = argv[1];
	int step = to_uint64_t(argv[2]);
	size_t len = to_uint64_t(argv[3]);

	init(&p, cfg);

	void *mem = alloc(&p);

	mem = linearize_alloc(&p, mem, p.size);
	printf("Allocated %zu bytes @%p\n", p.size, mem);

	measure(&p, mem, step, len);

	return 0;
}
