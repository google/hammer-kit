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

#ifndef PARAMS_H
#define PARAMS_H

#include <stdint.h>

enum pattern {
	RANDOM = 0, /* Randomly pick rows */
	EVEN, /* Hammer even rows */
	TRRESPASS_ASSISTED_DOUBLE, /* Even rows, plus one distant aggressor */
};

enum fill_type {
	FILL_RANDOM = 0, /* pseudo random data */
	FILL_MOD, /* Generic modulus rule for filling.
		   * Configured using 'mod' and 'victim_mask'
		   */
};


/* More than we'll ever need */
#define MAX_CPUS		64

struct addr_loop {
	int count;  /* if 0, address is incremented until we exceed the max. */
	int step;
};

struct params {
	int cpus[MAX_CPUS];
	int nr_cpus;

	int alt_row_find; /* should we use the alternative row finder? */
	int atomic_unit; /* number of bytes in an atomic unit for
			  * alt row search
			  */
	int row_size; /* number of atomic units in a row */

	int rows_per_set; /* number of contiguous rows in a bank */
	int bank_find_step; /* a large enough step to avoid testing a
			     * row against itself
			     */
	int nr_banks;	/* number of banks to target */

	int delay_iters; /* NOP loop that in some case might be able to trick
			  * a memory controller into not combining accesses
			  */

	/* Memory allocation parameters ********************* */
	size_t size;
	int cached; /* cached/uncached memory */
	int contig; /* physically contiguous memory */
	int pagemap; /* do we want physical addresses? */

	int sort_rows; /* sort rows according to a scheme seen on some parts */

	int sort_rows_shift; /* shift to apply to extract row number */

	int sched_fifo; /* Use FIFO realtime scheduler, with the given prio */

	enum fill_type fill_type; /* what data do we write? */

	int check_rest; /* should we bother checking rest of the memory */

	int mod; /* mod used by FILL_MOD */

	int mod_stride; /* should we shift the modular pattern? */

	/* for mod_k fill_types, it helps to specify which rows mod k are
	 * the victims.  These rows will be set to victim_data_pattern.
	 * Other rows will be set to the inverse.
	 */
	uint64_t victim_mask;

	/* for mod_k fill_type: the pattern to write to the victims */
	uint32_t victim_data_pattern;

	/* Even if a bit flip is not detected, refill.  This is useful
	 * for patterns that specify where to look for victims, if
	 * we wish to ignore any incidental victims.  This is useful
	 * for characterization rather than finding bit flips.
	 */
	int always_refill;

	/* max_fuzz and fuzz_step parameters can be used to introduce a
	 * fudge factor into default row finding method.  For each find_step,
	 * it will try increments of fuzz_step until max_fuzz.  As soon as it
	 * finds a row, it will simply go to the next find_step increment.
	 * This fudging feature is considered deprecated.  The preferred way
	 * is to use the alternative row finder, which is more flexible
	 * (but slower).
	 */
	uint64_t max_fuzz;
	uint64_t fuzz_step;

	/***** Measurement parameters ******/

	/* How many hammering loops to execute to detect bank conflict. */
	int measure_loops;
	/* For bank detection, look at offset0 from start of the buffer.
	 * A few megabytes works well.
	 */
	size_t offset0;
	/* base for different bank so that multiple banks can be run in
	 * parallel
	 */
	size_t src_offset;

	/***** Hammering parameters ******/

	/* Value above which indicates a conflict, microsecond. */
	int conflict_th_us;
	/* Step to use in the row detection algo */
	int find_step;
	/* Number of rows to find at each iteration */
	int n_rows;
	/* min/max agressors */
	int min_aggr, max_aggr;

	/* Number of memory accesses to do in a loop (this is divided by the
	 * number of aggressors.
	 */
	int hammer_loops;

	/*
	 * How many times should a trial be repeated if we see bit flips?
	 */
	int repeat_flips;

	/* Number of row picks at each base offset. */
	int n_tries;

	/* Distance to the assistant in the TRRESPASS_ASSISTED_DOUBLE pattern */
	int assisted_double_dist;

	/* This is useful when the geometry is known:
	 *
	 * For example, on MT8183:
	 * step=0:512K
	 * step=8:4K
	 * step=2:256
	 *
	 * Takes steps of 512KB (until we run out of memory to scan: count=0).
	 * For each of these 512KB steps, take 8 4KB steps (8 banks).
	 * For each of these 4KB steps, take 2 256B steps (2 channels).
	 *
	 * This translates to the following loop:
	 *
	 * uintptr_t addr = 0;
	 * for (int i = 0; addr = i * 512K, addr < MAX; ++i) {
	 * 	for (int j = 0; j < 8; addr += 4K)  // Bank
	 * 		for (int k = 0; k < 2; addr += 256)  // Channel
	 */
#define MAX_ADDR_LOOPS 8
	struct addr_loop addr_loops[MAX_ADDR_LOOPS];

	enum pattern pattern;
};

uint64_t to_uint64_t(const char *value);

void read_config(struct params *p, const char *file);
void print_config(struct params *p);

#endif
