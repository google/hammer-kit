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
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lib.h"
#include "third_party/linux-uapi/ion.h"

#define ION_MAX_HEAPS 16

void *alloc(struct params *p)
{
	void *mem;
	long page_size = sysconf(_SC_PAGE_SIZE);
	uint64_t aligned_size = (p->size + page_size - 1) & ~(page_size - 1);
	/* Not contiguous? Make sure it's page aligned and mlocked */
	if (!p->contig && p->cached) {
		/*
		 * Memory from mmap is only virtually contiguos and may not
		 * be physically contiguous.
		 */
		mem = mmap(NULL, aligned_size,
			   PROT_READ | PROT_WRITE,
			   MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
			   -1, 0);
		if (!mem)
			die("unable to allocate\n");

		if (mlock(mem, aligned_size) < 0)
			die("unable to mlock\n");
		return mem;
	}
	/* Use ION to get contiguous memory from CMA, or uncached memory. */
	int fd = open("/dev/ion", O_RDONLY);

	if (fd < 0)
		pdie("Could not open /dev/ion.\n");

	struct ion_heap_data heaps[ION_MAX_HEAPS];
	struct ion_heap_query query = {
		.cnt = ION_MAX_HEAPS,
		.heaps = (uint64_t)&heaps[0],
	};
	if (ioctl(fd, ION_IOC_HEAP_QUERY, &query))
		pdie("Heap query");

	/*
	 * ION_HEAP_TYPE_SYSTEM_CONTIG is not able to give us large chunks
	 * of memory (>4MB), so use ION_HEAP_TYPE_DMA, which comes from
	 * the CMA allocator.
	 */
	const uint32_t type =
		p->contig ? ION_HEAP_TYPE_DMA : ION_HEAP_TYPE_SYSTEM;
	int heap_id = -1;

	for (int i = 0; i < query.cnt; i++) {
		printf("heap %s: type: %d id: %d\n",
			heaps[i].name, heaps[i].type, heaps[i].heap_id);
		if (heaps[i].type == type)
			heap_id = heaps[i].heap_id;
	}
	if (heap_id < 0)
		die("Can't find suitable heap (type: %d).\n", type);

	struct ion_allocation_data alloc = {
		.len = p->size,
		.heap_id_mask = 1 << heap_id,
		.flags = p->cached ? ION_FLAG_CACHED : 0,
	};
	if (ioctl(fd, ION_IOC_ALLOC, &alloc))
		pdie("ION alloc");

	if (alloc.fd < 0 || alloc.len < p->size)
		die("alloc error");

	mem = mmap(NULL, p->size, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_POPULATE, alloc.fd, 0);
	return mem;
}



struct page {
	uint8_t *virt;
	uint64_t phys;
	int consec_len;
};

struct range {
	int nr_pages;
	uint8_t **virt_addrs;
};

static int page_compar(const void *p0, const void *p1)
{
	struct page *page0 = (struct page *)p0;
	struct page *page1 = (struct page *)p1;

	if (page0->phys < page1->phys)
		return -1;
	else
		return 1;
}


static int range_compar(const void *p0, const void *p1)
{
	struct range *range0 = *(struct range **)p0;
	struct range *range1 = *(struct range **)p1;

	if (range0->nr_pages > range1->nr_pages)
		return -1;
	else
		return 1;
}


/*
 * Given a virtual address range, returns an array of contiguous physical
 * ranges expressed as list of virtual addresses.
 *
 * Assumes page alignment of the address and length.
 * Assumes that the provided memory is populated and mlocked.
 *
 * Returns the number of physical ranges.  Output parameter "ranges"
 * contains the actual ranges.
 */
static int get_contig_ranges(struct params *p,
		      uint8_t *virtual_address, uint64_t len,
		      struct range ***ret_ranges)
{
	int64_t page_idx;
	uint64_t offset;
	struct page *pages;
	const int page_size = sysconf(_SC_PAGE_SIZE);
	int range_count = 1;
	int next_page = 0;
	int range_idx = -1;
	const int page_count = len / page_size;
	struct range **ranges;

	if (len % page_size != 0)
		die("get_contig_range: len is not page aligned");

	pages = malloc(sizeof(*pages) * page_count);
	for (page_idx = 0; page_idx < page_count; page_idx++) {
		offset = page_idx * page_size;
		pages[page_idx].virt = virtual_address + offset;
		pages[page_idx].phys = physical_address(virtual_address +
				offset);
	}

	/* Lowest physical address first */
	qsort(pages, page_count, sizeof(*pages), page_compar);

	/*
	 * Let D[j] be the length of the largest consecutive range
	 * in pages[0..j] ending at j.  Then,
	 *
	 * D[j] =   1 if pages[j] is not consecutive with pages[j-1]
	 *          1 + D[j - 1] if pages[j] is consecutive with pages[j-1]
	 */
	pages[0].consec_len = 1;
	for (page_idx = 1; page_idx < page_count; page_idx++) {
		if (pages[page_idx].phys == pages[page_idx - 1].phys +
						page_size) {
			pages[page_idx].consec_len =
				pages[page_idx - 1].consec_len + 1;
			/*
			 * Remove redundant information from shorter
			 * subsequences
			 */
			pages[page_idx - 1].consec_len = 0;
		} else {
			pages[page_idx].consec_len = 1;
			range_count++;
		}
	}

	ranges = malloc(sizeof(*ranges) * range_count);
	for (page_idx = page_count - 1; page_idx >= 0; page_idx--) {
		const int consec_len = pages[page_idx].consec_len;

		if (consec_len > 0) {
			range_idx++;
			ranges[range_idx] = malloc(sizeof(struct range));
			ranges[range_idx]->virt_addrs = malloc(
			      sizeof(uint8_t *) * consec_len);
			next_page = ranges[range_idx]->nr_pages = consec_len;
		}
		ranges[range_idx]->virt_addrs[--next_page] =
						pages[page_idx].virt;
	}
	free(pages);

	/* Largest range comes first */
	qsort(ranges, range_count, sizeof(*ranges), range_compar);
	*ret_ranges = ranges;
	return range_count;
}

/*
 * Remaps ranges so that the new mapping has them in sequential order.
 */
static uint8_t *linearize_ranges(struct range **ranges, int nr_ranges,
			  uint64_t len)
{
	int i, j;
	uint64_t offset = 0;
	void *ret;
	long page_size = sysconf(_SC_PAGESIZE);

	if (len % page_size != 0)
		die("%s: len is not divisible by page_size", __func__);

	/* Deliberately not using map populate to avoid extra
	 * allocation.
	 */
	uint8_t *new_range = mmap(NULL, len, PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	printf("Number of linear ranges: %d\n", nr_ranges);
	printf("Largest linear range: %ld\n", ranges[0]->nr_pages * page_size);
	for (i = 0; i < nr_ranges; i++) {
		for (j = 0; j < ranges[i]->nr_pages; j++) {
			/* mremap replaces the mapping at new_range + offset */
			ret = mremap(ranges[i]->virt_addrs[j], page_size,
					page_size,
					MREMAP_FIXED | MREMAP_MAYMOVE,
					new_range + offset);
			if (ret != new_range + offset)
				die("failed to remap, errno: %d\n", errno);
			offset += page_size;
		}

	}

	return new_range;
}

void free_ranges(struct range **ranges, int nr_ranges)
{
	int i;

	for (i = 0; i < nr_ranges; i++) {
		free(ranges[i]->virt_addrs);
		free(ranges[i]);
	}

	free(ranges);
}

uint8_t *linearize_alloc(struct params *p, uint8_t *mem, uint64_t len)
{
	struct range **ranges;
	int nr_ranges;
	uint8_t *ret;

	if (!p->pagemap)
		return mem;

	nr_ranges = get_contig_ranges(p, mem, len, &ranges);
	if (nr_ranges == 1) {
		free_ranges(ranges, nr_ranges);
		return mem;
	}
	ret = linearize_ranges(ranges, nr_ranges, len);
	free_ranges(ranges, nr_ranges);

	return ret;
}
