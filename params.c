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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "params.h"
#include "util.h"

static enum pattern to_pattern(const char *value)
{
	if (!strcmp(value, "random"))
		return RANDOM;
	else if (!strcmp(value, "trrespass_assisted_double"))
		return TRRESPASS_ASSISTED_DOUBLE;
	else if (!strcmp(value, "even"))
		return EVEN;

	die("Bad pattern value ('%s').\n", value);
}

static enum fill_type to_fill_type(const char *value)
{
	if (!strcmp(value, "random"))
		return FILL_RANDOM;
	else if (!strcmp(value, "mod"))
		return FILL_MOD;

	die("Bad fill type value ('%s').\n", value);
}

uint64_t to_uint64_t(const char *value)
{
	char *endptr;
	uint64_t llvalue = strtoull(value, &endptr, 10);

	if (*value == '\0')
		die("Bad value ('%s').", value);

	switch (*endptr) {
	case 'G':
		llvalue *= 1024;
	case 'M':
		llvalue *= 1024;
	case 'K':
		llvalue *= 1024;
		endptr++;
		break;
	}

	if (*endptr != '\0')
		die("Bad value ('%s').\n", value);

	return llvalue;
}

static uint64_t parse_hex(const char *value)
{
	char *endptr;
	uint64_t llvalue = strtoull(value, &endptr, 16);

	if (*endptr != '\0')
		die("Bad value ('%s').\n", value);
	return llvalue;
}

static int to_bool(const char *value)
{
	char *endptr;
	uint64_t llvalue = strtoull(value, &endptr, 10);

	if (*value == '\0' || *endptr != '\0' ||
		(llvalue != 0 && llvalue != 1))
		die("Bad value ('%s').", value);

	return (int)llvalue;
}

static void parse_step(struct params *p, const char *value)
{
	int cur;
	char *colon = strchr(value, ':');

	if (!colon)
		die("Bad value: '%s'.\n", value);
	*colon = '\0';

	for (cur = 0; cur < MAX_ADDR_LOOPS; cur++)
		if (p->addr_loops[cur].step == 0)
			break;

	if (cur == MAX_ADDR_LOOPS)
		die("Too many steps.");

	p->addr_loops[cur].count = to_uint64_t(value);
	p->addr_loops[cur].step = to_uint64_t(colon+1);

	if (p->addr_loops[cur].step == 0)
		die("%s: invalid step", __func__);
}

static void set(struct params *p, const char *name, const char *value)
{
	if (!strcmp(name, "cpu"))
		p->cpus[p->nr_cpus++] = to_uint64_t(value);
	else if (!strcmp(name, "alt_row_find"))
		p->alt_row_find = to_uint64_t(value);
	else if (!strcmp(name, "atomic_unit"))
		p->atomic_unit = to_uint64_t(value);
	else if (!strcmp(name, "rows_per_set"))
		p->rows_per_set = to_uint64_t(value);
	else if (!strcmp(name, "nr_banks"))
		p->nr_banks = to_uint64_t(value);
	else if (!strcmp(name, "row_size"))
		p->row_size = to_uint64_t(value);
	else if (!strcmp(name, "bank_find_step"))
		p->bank_find_step = to_uint64_t(value);
	else if (!strcmp(name, "delay_iters"))
		p->delay_iters = to_uint64_t(value);
	else if (!strcmp(name, "pagemap"))
		p->pagemap = to_uint64_t(value);
	else if (!strcmp(name, "sort_rows"))
		p->sort_rows = to_uint64_t(value);
	else if (!strcmp(name, "sort_rows_shift"))
		p->sort_rows_shift = to_uint64_t(value);
	else if (!strcmp(name, "check_rest"))
		p->check_rest = to_uint64_t(value);
	else if (!strcmp(name, "fill_type"))
		p->fill_type = to_fill_type(value);
	else if (!strcmp(name, "mod"))
		p->mod = to_uint64_t(value);
	else if (!strcmp(name, "mod_stride"))
		p->mod_stride = to_uint64_t(value);
	else if (!strcmp(name, "victim_mask"))
		p->victim_mask = parse_hex(value);
	else if (!strcmp(name, "victim_data_pattern"))
		p->victim_data_pattern = parse_hex(value);
	else if (!strcmp(name, "size"))
		p->size = to_uint64_t(value);
	else if (!strcmp(name, "contig"))
		p->contig = to_bool(value);
	else if (!strcmp(name, "cached"))
		p->cached = to_bool(value);
	else if (!strcmp(name, "measure_loops"))
		p->measure_loops = to_uint64_t(value);
	else if (!strcmp(name, "offset0"))
		p->offset0 = to_uint64_t(value);
	else if (!strcmp(name, "src_offset"))
		p->src_offset = to_uint64_t(value);
	else if (!strcmp(name, "conflict_th_us"))
		p->conflict_th_us = to_uint64_t(value);
	else if (!strcmp(name, "find_step"))
		p->find_step = to_uint64_t(value);
	else if (!strcmp(name, "fuzz_step"))
		p->fuzz_step = to_uint64_t(value);
	else if (!strcmp(name, "max_fuzz"))
		p->max_fuzz = to_uint64_t(value);
	else if (!strcmp(name, "n_rows"))
		p->n_rows = to_uint64_t(value);
	else if (!strcmp(name, "min_aggr"))
		p->min_aggr = to_uint64_t(value);
	else if (!strcmp(name, "max_aggr"))
		p->max_aggr = to_uint64_t(value);
	else if (!strcmp(name, "hammer_loops"))
		p->hammer_loops = to_uint64_t(value);
	else if (!strcmp(name, "repeat_flips"))
		p->repeat_flips = to_uint64_t(value);
	else if (!strcmp(name, "n_tries"))
		p->n_tries = to_uint64_t(value);
	else if (!strcmp(name, "assisted_double_dist"))
		p->assisted_double_dist = to_uint64_t(value);
	else if (!strcmp(name, "step"))
		parse_step(p, value);
	else if (!strcmp(name, "pattern"))
		p->pattern = to_pattern(value);
	else if (!strcmp(name, "always_refill"))
		p->always_refill = to_uint64_t(value);
	else if (!strcmp(name, "sched_fifo"))
		p->sched_fifo = to_uint64_t(value);
	else
		die("Bad name ('%s').\n", name);
}

static void set_defaults(struct params *p)
{
	memset(p, 0, sizeof(*p));

	p->nr_cpus = 0;
	p->cpus[0] = 0;
	p->size = 128*MB;
	p->contig = 1;
	p->cached = 0;
	p->check_rest = 1;
	p->pagemap = 0;
	p->mod = 1;
	p->mod_stride = 1;
	p->assisted_double_dist = 7;
	p->victim_data_pattern = 0xffffffff;
	p->delay_iters = 1000;
	p->alt_row_find = 0;
	p->nr_banks = 1;
	p->rows_per_set = 1;
	p->bank_find_step = 512*1024;
	p->always_refill = 0;
	p->sort_rows_shift = 15;

	/* No fuzz by default */
	p->max_fuzz = 0;
	p->fuzz_step = 64;

	p->offset0 = 16*MB;
	p->src_offset = 0;
	p->measure_loops = 250000;

	p->sched_fifo = 0;
}

/* Read configuration */
void read_config(struct params *p, const char *file)
{
	FILE *f;
	char *line = NULL;
	size_t len;

	set_defaults(p);

	f = fopen(file, "r");
	if (f == NULL)
		pdie("Can't open config.");

	while (getline(&line, &len, f) != -1) {
		int len = strlen(line);

		if (len == 0 || *line == '#')
			continue;
		if (line[len-1] == '\n')
			line[--len] = '\0';
		if (len == 0)
			continue;

		char *eq = strchr(line, '=');

		if (!eq)
			die("Bad line: '%s'.\n", line);
		*eq = '\0';
		set(p, line, eq+1);
	}

	free(line);
}

void print_config(struct params *p)
{
	printf("Configuration:\n");
	if (p->nr_cpus == 0)
		printf("cpu=%d\n", p->cpus[0]);
	else {
		int i;

		printf("cpu=");
		for (i = 0; i < p->nr_cpus; i++)
			printf("%d,", p->cpus[i]);
	}
	printf("\n");

	printf("size=%zu\n", p->size);
	printf("contig=%d\n", p->contig);
	printf("cached=%d\n", p->cached);
	printf("measure_loops=%d\n", p->measure_loops);
	printf("offset0=%zu\n", p->offset0);
}


