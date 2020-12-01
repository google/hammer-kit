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
#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "lib.h"

#define PTE_PRESENT		(1ULL << 63)
#define PTE_SWAP		(1ULL << 62)
#define PTE_PFN_MASK		((1ULL << 55) - 1)

/*
 * Only called this for memory that cannot be swapped
 * out.  This means either special driver allocations or
 * memory that is explicitly mlocked.
 */
uintptr_t physical_address(void *virtual_address)
{
	uintptr_t virtual_page_number;
	uint64_t pte;
	uint64_t pagemap_offset;
	uint64_t page_offset;
	int pid;
	int fd;
	char name[30];
	long page_size = sysconf(_SC_PAGE_SIZE);

	pid = getpid();
	if (sprintf(name, "/proc/%d/pagemap", pid) >= sizeof(name))
		die("pagemap filename unexpectedly long\n");
	fd = open(name, O_RDONLY);
	if (fd < 0)
		die("Could not open pagemap\n");
	virtual_page_number = (uintptr_t)virtual_address / page_size;
	page_offset = (uintptr_t)virtual_address % page_size;
	pagemap_offset = virtual_page_number * sizeof(pte);
	if (pread(fd, &pte, sizeof(pte), pagemap_offset) != sizeof(pte))
		die("pagemap read failed\n");
	if (!(pte & PTE_PRESENT))
		die("page not present.\n");
	if (pte & PTE_SWAP)
		die("page swapped out.\n");
	close(fd);

	return (pte & PTE_PFN_MASK) * page_size + page_offset;
}

static void setcpu(struct params *p)
{
	cpu_set_t set;
	struct sched_param sched_param = {};

	CPU_ZERO(&set);
	CPU_SET(p->cpus[0], &set);
	if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
		pdie("sched_setaffinity");
	if (p->sched_fifo) {
		sched_param.sched_priority = p->sched_fifo;
		if (sched_setscheduler(getpid(), SCHED_FIFO, &sched_param))
			pdie("sched_setscheduler");

	}

}

struct cpu_descriptor {
	int cpu;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t condvar;
	struct params *params;

	volatile uint8_t **rows;
	int row_count;
	int loops;
	int timing_loops;
	uint64_t retval;
	bool ready;
	struct cpu_descriptor *next;
};

static struct cpu_descriptor *first_cpu;
static sem_t threads_done;
static int thread_count;

static void *hammer_thread(void *arg)
{
	struct cpu_descriptor *desc = (struct cpu_descriptor *)arg;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(desc->cpu, &set);

	if (pthread_setaffinity_np(desc->thread, sizeof(set), &set))
		die("pthread_set_affinity failed");

	pthread_mutex_lock(&desc->mutex);
	while (1) {
		pthread_cond_wait(&desc->condvar, &desc->mutex);
		if (!desc->ready)
			continue;

		desc->retval = hammer(desc->params, desc->rows,
				desc->row_count, desc->loops,
				desc->timing_loops);
		desc->ready = false;
		sem_post(&threads_done);
	}
	pthread_mutex_unlock(&desc->mutex);

	return NULL;
}


static void setup_thread(struct params *params, int cpu)
{
	struct cpu_descriptor *desc;

	desc = malloc(sizeof(*desc));
	desc->cpu = cpu;
	pthread_cond_init(&desc->condvar, NULL);
	if (pthread_mutex_init(&desc->mutex, NULL))
		die("Mutex initialization failed\n");
	desc->rows = NULL;
	desc->ready = false;
	desc->row_count = 0;
	desc->params = params;
	if (pthread_create(&desc->thread, NULL, hammer_thread, desc))
		die("Pthread creation failure\n");
	desc->next = first_cpu;
	first_cpu = desc;
	thread_count++;
}

void init_threads(struct params *params)
{
	int i;

	for (i = 0; i < params->nr_cpus; i++)
		setup_thread(params, params->cpus[i]);

}

uint64_t parallel_hammer(struct params *params, volatile uint8_t **rows,
		int row_count, int loops, int timing_loops)
{
	int rows_allotted = 0;
	int idx;
	struct cpu_descriptor *cur;
	uint64_t start;
	int i;

	if (params->nr_cpus < 2) {
		start = ns();
		(void) hammer(params, rows, row_count,
				loops, timing_loops);
		return ns() - start;
	}
	if (sem_init(&threads_done, 0, 0))
		die("sem_init");

	for (cur = first_cpu, idx = 0; cur; cur = cur->next, idx++) {
		pthread_mutex_lock(&cur->mutex);
		cur->rows = &rows[rows_allotted];
		cur->row_count = row_count / thread_count;
		if (idx < row_count % thread_count)
			cur->row_count++;

		cur->ready = true;
		rows_allotted += cur->row_count;
		cur->loops = loops;
		cur->timing_loops = timing_loops;
		pthread_mutex_unlock(&cur->mutex);
		pthread_cond_signal(&cur->condvar);
	}

	start = ns();
	for (i = 0; i < thread_count; i++)
		sem_wait(&threads_done);

	return ns() - start;
}

void init(struct params *p, const char *file)
{
	srand(time(NULL));

	read_config(p, file);
	print_config(p);

	setcpu(p);
	if (p->nr_cpus >= 2)
		init_threads(p);
}

/* Other helpers. */

uint64_t ns(void)
{
	struct timespec tp;

	if (clock_gettime(CLOCK_MONOTONIC, &tp))
		pdie("Can't get time.");

	return (uint64_t)tp.tv_sec * 1000000000 + tp.tv_nsec;
}

/* Simple prng - parameters from rand48 man page. */
uint32_t myrand32_r(uint64_t *state)
{
	*state = (*state * 0x5DEECE66D + 0xB) & ((1LL << 48)-1);
	return *state >> 16;
}

/*
 * Hammering function
 *
 * Returns the number of nanoseconds it took to hammer.
 */
uint64_t hammer(struct params *p, volatile uint8_t **addr,
		int count, int loops, int timing_loops)
{
	uint64_t start, delta;
#ifdef __x86_64__
	int iters = p->delay_iters;
#endif
	uint64_t mintime = INT64_MAX;

	for (int t = 0; t < timing_loops; t++) {
		start = ns();

		for (int i = 0; i < loops; i++) {
#ifdef __x86_64__
			/*
			 * See: Drammer: Deterministic Rowhammer Attacks
			 * on Mobile Platforms, section 4.1.
			 */
			for (int k = 0; k < iters; k++)
				asm("nop");

			/*
			 * CPUID should serialize the instructions before it
			 * with respect to those after it.  We don't want the
			 * memory controller to mix two sets of hammers, because
			 * that may result in merger of accesses to the same
			 * address, resulting in a reduced hammering rate.
			 */
			asm("cpuid" : : : "rax", "rbx", "rcx", "rdx");
#endif
#ifdef __x86_64__
			for (int k = 0; k < count; k++) {
				*(addr[k]);
				if (p->cached)
					flush(addr[k]);
			}
#else

			/*
			 * The difference between the two-loop arrangement
			 * seen here and the single loop arrangement in x86
			 * is incidental.  It's not clear which one is better
			 * or if it matters at all on either platform.
			 */
			for (int k = 0; k < count; k++)
				*addr[k];
			ARM64_ISB();

			if (p->cached)
				for (int k = 0; k < count; k++)
					flush(addr[k]);
#endif
		}

		delta = ns()-start;
		if (delta < mintime)
			mintime = delta;
	}

	return mintime * timing_loops;
}

void fill(struct params *p, uint8_t *base, uint8_t *mem, size_t len)
{
	uint32_t *mem32 = (void *)mem;
	uint64_t state = (uint64_t)mem ^ ((uint64_t)mem >> 32);

	printf("Filling %zu bytes at %08zx.\n", len,
		(uint64_t)mem-(uint64_t)base);

	if (len % 4 != 0)
		die("len not divisible by 4 in %s", __func__);

	for (int i = 0; i < len/4; i++)
		mem32[i] = myrand32_r(&state);

	if (p->cached) {
		for (int i = 0; i < len/4; i++)
			flush(&mem32[i]);
	}

}

int check(struct params *p, uint8_t *base, uint8_t *mem, size_t len)
{
	uint32_t *mem32 = (void *)mem;
	uint64_t state = (uint64_t)mem ^ ((uint64_t)mem >> 32);

	printf("Checking %zu bytes at %08zx.\n", len,
		(uint64_t)mem-(uint64_t)base);

	int dcount = 0;

	if (len % 4 != 0)
		die("len not divisible by 4 in %s", __func__);

	if (p->cached) {
		for (int i = 0; i < len / 4; i++)
			flush(&mem32[i]);
	}


	for (int i = 0; i < len/4; i++) {
		uint32_t expect = myrand32_r(&state);

		uint32_t diff = mem32[i] ^ expect;

		if (diff != 0) {
			printf("@FLIP 0x%08zx (phys: 0x%08zx) 0x%08x->0x%08x\n",
				(uint64_t)&mem32[i]-(uint64_t)base,
				p->pagemap ? physical_address(&mem32[i]) : 0,
				mem32[i], expect);

			/* TODO: Count actual bit flips? */
			dcount++;
		}
	}

	return dcount;
}
