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

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "lib.h"
#include "mm.h"

struct row {
	int idx;
	uint8_t *start;
	uint32_t len;
};


static unsigned int physaddr_to_row(uintptr_t addr, int sort_rows_shift)
{
	uintptr_t row = (addr >> sort_rows_shift) & 0xffff;
	uintptr_t bit3 = (row & (1 << 3)) >> 3;

	/* "Defeating Software Mitigations against Rowhammer: a Surgical
	 *  Precision Hammer".  Section 3.1, Sub-heading : Remapping.
	 */
	row = row ^ (bit3 << 2);
	row = row ^ (bit3 << 1);
	return row;
}

static int row_compar(const void *r1, const void *r2, void *arg)
{
	const struct row *row1 = r1;
	const struct row *row2 = r2;
	unsigned int phys_row1;
	unsigned int phys_row2;
	struct params *p = (struct params *) arg;

	phys_row1 = physaddr_to_row(physical_address(row1->start),
				    p->sort_rows_shift);
	phys_row2 = physaddr_to_row(physical_address(row2->start),
				    p->sort_rows_shift);

	if (phys_row1 < phys_row2)
		return -1;
	else
		return 1;
}

/*
 * Alternative find_row implementation:  the idea is as follows:
 * p->atomic_unit is the largest number of bytes that when naturally
 * aligned can only reside in
 * a single row.  This will probably be cache size.
 * An atomic_unit is either in our bank or in another
 * bank.  If it is in our bank, the measurement will be high and
 * we should take that atomic_unit and make it part of the current
 * row.  We also know p->row_size in atomic units which tells us
 * the point at which the current row is complete.  A caveat is
 * that there is no guarantee that we started at the beginning of
 * a row.  But this is okay, because if p->row_size is accurate,
 * then we will also find the same offset in the next row.  For
 * purposes of hammering, only the 'starting' address of the row
 * matters.
 */
#define MAX_BANKS		16
static int find_rows_alt(struct params *p, uint8_t *base,
		uint8_t *mem, size_t len, struct row *rows, int rows_len)
{
	volatile uint8_t *addr[2];
	int nr_rows = 0;
	int nr_atomic_units = 0;
	uint8_t *row_start;
	uint8_t *bank[MAX_BANKS];
	int cur_bank = 0;
	int set_row = 0;
	int found_banks = 1;
	int phase = 0;

	bank[0] = mem;
	for (uintptr_t off = p->bank_find_step; off < len &&
	     found_banks < p->nr_banks;
	     off += p->atomic_unit) {
		int i;
		uint8_t *target = mem + off;

		addr[1] = target;
		/* Start by finding a row in the previous bank in the
		 * current region.  This allows us to keep the bank
		 * order natural.
		 */
		if (phase == 0) {
			addr[0] = bank[found_banks - 1];
			uint64_t t = hammer(p, addr, 2, p->measure_loops/5, 5);

			if (t/1000 > p->conflict_th_us)
				phase = 1;

			continue;
		}

		for (i = 0; i < found_banks; i++) {
			addr[0] = bank[i];
			uint64_t t = hammer(p, addr, 2, p->measure_loops/5, 5);

			if (t/1000 > p->conflict_th_us)
				break;

		}

		if (i == found_banks) {
			bank[found_banks++] = target;
			off += p->bank_find_step;
			phase = 0;
		}
	}

	addr[0] = bank[0];
	for (uint64_t off = p->offset0; off < len ; off += p->atomic_unit) {
		uint8_t *target = mem + off;

		addr[1] = target;

		uint64_t t = hammer(p, addr, 2, p->measure_loops/5, 5);

		int conflict = (t/1000 > p->conflict_th_us);

		if (!conflict)
			continue;

		if (!nr_atomic_units)
			row_start = target;
		nr_atomic_units++;
		printf("%08zx (phys: %08zx): gathered nr_atomic_units: %d\n",
		    target - base, p->pagemap ? physical_address(target) : 0,
		    nr_atomic_units);
		if (nr_atomic_units == p->row_size) {
			nr_atomic_units = 0;
			if (nr_rows > 0)
				rows[nr_rows - 1].len = row_start -
					rows[nr_rows - 1].start;
			printf(" (row %d at phys %lx)\n", nr_rows,
				p->pagemap ? physical_address(row_start) : 0);

			rows[nr_rows].idx = nr_rows;
			rows[nr_rows++].start = row_start;
			set_row++;
			if (set_row == p->rows_per_set) {
				cur_bank++;
				set_row = 0;
			}
			if (cur_bank == p->nr_banks)
				cur_bank = 0;
			addr[0] = bank[cur_bank];
			if (nr_rows == rows_len)
				break;

		}
	}

	if (nr_rows > 0)
		rows[nr_rows - 1].len = rows[nr_rows - 2].len;

	return nr_rows;
}



/* Find rows interval [mem + p->offset0, mem + len) that are in the
 * same bank as mem.
 *
 * Threshold can be found from measure tool, we expect a higher timing
 * value for bank conflict.
 */
static int find_rows_in_same_bank(struct params *p, uint8_t *base,
		uint8_t *mem, int step, size_t len, struct row *rows,
		int rows_len)
{

	volatile uint8_t *addr[2];

	addr[0] = mem;

	int nr_rows = 0;

	printf("Finding rows. Step %d, offset0 %zu, len %zu\n",
		step, p->offset0, len);
	printf("Offset from base\toffset0\ttime (us)\n");
	for (uint64_t off = p->offset0; off < len; off += step) {
		uint8_t *target = mem + off;

		int is_bank_conflict = 0;
		// Note the <=.
		for (uint64_t fuzz = 0; fuzz <= p->max_fuzz;
		     fuzz += p->fuzz_step, target += p->fuzz_step) {
			addr[1] = target;
			uint64_t t = hammer(p, addr, 2, p->measure_loops/5, 5);

			is_bank_conflict = (t/1000 > p->conflict_th_us);

			printf("%08zx\t%zu\t%zu", target-base,
					off-p->offset0, t/1000);
			if (!is_bank_conflict) {
				printf(" (%d rows so far)\n", nr_rows);
				continue;
			}

			break;
		}

		if (!is_bank_conflict)
			continue;

		/* TODO: This code assumes that step is chosen large enough
		 * so that no 2 steps can end up in the same row, but this
		 * may not be feasible depending on memory mapping layout,
		 * e.g. if we use non-contig memory and step > 4K (page).
		 * (if so, we'd risk picking 2 addresses in the same row,
		 * which may or may not decrease attack efficiency).
		 */
		printf(" (conflict -- row %d at phys %lx)\n", nr_rows,
				p->pagemap ? physical_address(target) : 0);
		if (nr_rows > 0)
			rows[nr_rows - 1].len = target -
						rows[nr_rows - 1].start;
		rows[nr_rows].idx = nr_rows;
		rows[nr_rows++].start = target;
		if (nr_rows == rows_len)
			break;
	}

	rows[nr_rows - 1].len = rows[nr_rows - 2].len;

	/*
	 * Sort rows based on a known mapping.
	 */
	if (p->sort_rows) {
		int row;

		if (!p->pagemap)
			die("sort_rows requires pagemap");

		qsort_r(rows, nr_rows, sizeof(*rows), row_compar, p);
		for (row = 0; row < nr_rows; row++) {
			rows[row].idx = row;
			printf("Row %d is now at %08zx (phys: %08zx).  Len: %d\n",
			    row, rows[row].start - base,
			    p->pagemap ? physical_address(rows[row].start) : 0,
			    rows[row].len);
		}
	}


	return nr_rows;
}

static void fill_rows_mod_k(struct params *p, struct row *rows, int n_rows,
		int shift)
{
	int i;
	int j;

	if (shift < 0 || shift >= p->mod)
		die("%s: invalid shift value", __func__);

	for (i = 0; i < n_rows; i++) {
		int mod = (rows[i].idx + p->mod - shift) % p->mod;
		uint32_t pattern = (p->victim_mask &
				(1UL << mod)) ? p->victim_data_pattern :
				~(p->victim_data_pattern);
		for (j = 0; j < rows[i].len / 4; j++) {
			uint32_t *virt = (uint32_t *)rows[i].start + j;
			*virt = pattern;
			if (p->cached)
				flush(virt);
		}
	}

}

static int check_rows_mod_k(struct params *p,
			    uint8_t *base, struct row *rows, int n_rows,
			    int shift)
{
	int i;
	int j;
	int flips = 0;

	for (i = 0; i < n_rows; i++) {
		int mod = (rows[i].idx + p->mod - shift) % p->mod;
		uint32_t pattern = (p->victim_mask & (1UL << mod)) ?
				p->victim_data_pattern :
				~(p->victim_data_pattern);

		for (j = 0; j < rows[i].len / 4; j++) {
			if (((uint32_t *)rows[i].start)[j] != pattern) {
				uint32_t *virt = (uint32_t *)rows[i].start + j;

				printf("@FLIP %08zx (phys: %lx) row %d offset %x %08x->%08x\n",
					(uint64_t)virt - (uint64_t)base,
					p->pagemap ? physical_address(virt) : 0,
					rows[i].idx, j*4, pattern,
					((uint32_t *)rows[i].start)[j]);

				flips++;
			}
		}

	}
	return flips;

}

static void get_row_range(struct params *p,
			  struct row *rows, int nr_rows,
			  uint8_t **rowstart, uint8_t **rowend)
{
	/* Restricted range to check (faster). */
	*rowstart = (uint8_t *)UINTPTR_MAX;
	*rowend = (uint8_t *)0;

	for (int i = 0; i < nr_rows; i++) {
		if (rows[i].start < *rowstart)
			*rowstart = rows[i].start;
		if (rows[i].start + rows[i].len > *rowend)
			*rowend = rows[i].start + rows[i].len;
	}
}


static void fill_rows_random(struct params *p, uint8_t *base,
			     struct row *rows, int nr_rows)
{
	uint8_t *rowstart;
	uint8_t *rowend;

	get_row_range(p, rows, nr_rows, &rowstart, &rowend);
	fill(p, base, rowstart, rowend-rowstart);
}


static int check_rows_random(struct params *p, uint8_t *base,
			     struct row *rows, int nr_rows)
{
	uint8_t *rowstart;
	uint8_t *rowend;

	get_row_range(p, rows, nr_rows, &rowstart, &rowend);
	return check(p, base, rowstart, rowend-rowstart);
}

static void fill_rows(struct params *p, uint8_t *base, struct row *rows,
		      int nr_rows, int shift)
{
	switch (p->fill_type) {
	case FILL_RANDOM:
		fill_rows_random(p, base, rows, nr_rows);
		break;
	case FILL_MOD:
		fill_rows_mod_k(p, rows, nr_rows, shift);
		break;
	default:
		die("Unexpected fill type\n");
	}

}

static int check_rows(struct params *p, uint8_t *base,
		       struct row *rows, int nr_rows, int shift)
{
	switch (p->fill_type) {
	case FILL_RANDOM:
		return check_rows_random(p, base, rows, nr_rows);
	case FILL_MOD:
		return check_rows_mod_k(p, base, rows, nr_rows, shift);
	default:
		die("Unexpected fill type\n");
	}

}

/* These count 32-bit words with one or more bit flips in them. */
static int flips_from_repeats;
static int total_flips;
static int total_tries;

/* 'try' can be seen as a salt value for the current try.  Different
 * patterns will use 'try' in a different way.
 */
static uint64_t select_and_hammer_aggr(struct params *p, int try,
		struct row *rows, void *base, int seed)
{
	if (seed)
		srand(seed);

	volatile uint8_t *aggr[p->max_aggr];
	int n_aggr = p->min_aggr +
		try % (p->max_aggr - p->min_aggr + 1);
	int row = 0;

	for (int i = 0; i < n_aggr; i++) {
		switch (p->pattern) {
		case RANDOM:
			row = rand() % p->n_rows;
			break;
		case EVEN:
			row = (try + i*2) % p->n_rows;
			break;
		case TRRESPASS_ASSISTED_DOUBLE:
			if (i == n_aggr - 1)
				row = (try + (n_aggr - 2)*2 +
				       p->assisted_double_dist) % p->n_rows;
			else
				row = (try + i*2) % p->n_rows;
			break;
		}
		printf("@Picking %d 0x%08zx (phys: 0x%08zx)\n", row,
			(uint64_t)rows[row].start-(uint64_t)base,
			p->pagemap ? physical_address(rows[row].start) : 0);
		aggr[i] = rows[row].start;
	}

	uint64_t time;

	time = parallel_hammer(p, aggr, n_aggr,
		       p->hammer_loops/n_aggr, 1);

	return time;
}

static void run_hammer_once(struct params *p, void *base, uint8_t *mem,
			    size_t len)
{
	struct row rows[p->n_rows];
	int stride;
	int n;

	switch (p->alt_row_find) {
	case 0:
		n = find_rows_in_same_bank(p, base, mem, p->find_step,
					   len, rows, p->n_rows);
		break;
	case 1:
		n = find_rows_alt(p, base, mem, len, rows, p->n_rows);
		break;
	default:
		die("unknown row finding method");
	}

	if (n != p->n_rows) {
		printf("Can't find enough rows!\n");
		return;
	}

	/* Restricted range to check (faster). */
	uint8_t *rowstart;
	uint8_t *rowend;

	get_row_range(p, rows, p->n_rows, &rowstart, &rowend);

	/* Fill the rest of memory, we'll check later. */
	if (p->check_rest) {
		fill(p, base, mem, rowstart-mem);
		fill(p, base, rowend, len-(rowend-mem));
	}

	stride = p->mod_stride ? p->mod : 1;

	/*
	 * To reduce the overhead of filling rows, we want to split the
	 * tries by mod.  As an example, say we have tries:
	 *
	 * 0, 1, 2, 3, 4, 5, 6 ....
	 *
	 * mod == 3 and victim set is {1} (mod 3).
	 *
	 * Then, we want to split it like so:
	 *
	 * // Fill rows so that 1 mod 3 are the victims
	 * 0, 3, 6, 9, ....
	 * // Fill rows so that 2 mod 3 are the victims
	 * 1, 4, 7, 10, ....
	 * // Ffill rows so that 0 mod 3 are the victims
	 * 2, 5, 8, 11, ....
	 *
	 */
	for (int mod = 0; mod < stride; mod++) {
		fill_rows(p, base, rows, p->n_rows, mod);

		for (int try = mod; try < p->n_tries;
		     try += stride, total_tries++) {
			uint64_t time_taken;
			int rep = 0;
			int flips;
			unsigned long seed = 0;
			bool first_try = true;

			if (p->repeat_flips)
				seed = time(NULL);

			do {
				if (!first_try && p->repeat_flips)
					printf("Repeating: %d\n", rep);

				time_taken = select_and_hammer_aggr(p,
					try, rows, base, seed);

				flips = check_rows(p, base, rows,
						p->n_rows, mod);
				if (first_try)
					total_flips += flips;
				else
					flips_from_repeats += flips;
				printf("(time: %lu)\n", time_taken);
				printf("%d tries, %d flips, %d flips from repeats\n",
					total_tries,
					total_flips,
					flips_from_repeats);
				rep++;
				/* Restore if we had a flip or if we are using
				 * always_refill setting.
				 */
				if (flips || p->always_refill)
					fill_rows(p, base, rows,
						p->n_rows, mod);
				if (first_try && !flips)
					break;
				first_try = false;
			} while (p->repeat_flips && rep < p->repeat_flips);

		}
	}

	/* Check the whole memory. */
	if (p->check_rest) {
		total_flips += check(p, base, mem, rowstart-mem);
		total_flips += check(p, base, rowend, len-(rowend-mem));
	}
}

void run_hammer(struct params *p, uint8_t *base, uint8_t *mem,
		uint8_t *max_addr, int depth)
{
	if (p->addr_loops[depth].step == 0) {
		printf("Running at %zx\n", mem-base);
		run_hammer_once(p, base, mem, p->size-(mem-base));
		return;
	}

	for (int count = 0; mem < max_addr && (!p->addr_loops[depth].count ||
			   count < p->addr_loops[depth].count);
			   count++, mem += p->addr_loops[depth].step) {
		run_hammer(p, base, mem, max_addr, depth+1);
	}
}

int main(int argc, char *argv[])
{
	struct params p;

	if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
		die("setvbuf failed\n");

	if (argc != 2)
		die("Usage: %s config_file\n", argv[0]);

	char *cfg = argv[1];

	init(&p, cfg);
	void *mem = alloc(&p);

	mem = linearize_alloc(&p, mem, p.size);

	printf("Allocated %zu bytes @%p\n", p.size, mem);

	run_hammer(&p, mem, mem + p.src_offset, mem + p.size, 0);

	return (total_flips > 0) ? 1 : 0;
}
