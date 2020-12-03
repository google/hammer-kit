# HammerKit

## Background
HammerKit is an open-source library for inducing and characterizing rowhammer that provides out-of-the-box support for Chrome OS platforms.

The goal behind this project is to provide a platform to share our findings with the wider community, and provide another tool to the mix, so that others have starting points for their own investigations.  We feel that democratization of row hammer research is essential for creating an environment where the problem will be taken seriously by the various parties.

## What is Row Hammer?  Why should I care?

The [Wikipedia article](https://en.wikipedia.org/wiki/Row_hammer) for Row Hammer is a good starting point.  To summarize: DRAM is composed of banks, which are themselves composed of rows.  When a row is repeatedly accessed, bits located in its neighbouring rows may change their values. As you can imagine, the ability to flip bits that you don't have direct access to has potential security implications.  Row hammer exploitation has already been [demonstrated](https://github.com/IAIK/rowhammerjs) from Javascript.   Since the initial discovery of row hammer, the DRAM vendors have made some efforts to mitigate row hammer.  These efforts have proven unsuccessful, as demonstrated by the TRResspass work.  The result is that bits can still be flipped via Row Hammer, but more sophisticated access patterns are required.  As the DRAM moves to smaller and smaller process nodes, the underlying Row Hammer phenomenon becomes worse, and the mitigation trade offs become harder.  This is a very pressing issue for computer security, going forward.

In early 2020, the [TRRespass](https://www.vusec.net/projects/trrespass/) work was produced as a result of a large collaboration between VU, Qualcomm and ETH Zurich.  This once again highlighted that Row Hammer is not fixed.  This present work is greatly inspired by the TRRespass work.

## Compiling

The Makefile that is provided is intended for the Chrome OS environment.  A good
starting point for Chrome OS development is the [Developer's
Guide](https://chromium.googlesource.com/chromiumos/docs/+/master/developer_guide.md).
After checking out the Chrome OS source tree, make a directory under *src* and
place the contents of this package there.  After that, make sure that you enter
the Chrome OS build chroot and setup the appropriate board.  Then, in the
directory where this package is kept, run the following command:

```
  
  
           cros_sdk --working-dir . -- make CC=aarch64-cros-linux-gnu-clang
```

assuming you are compiling for an ARM64 platform.  If the chosen board is
x86_64, you can do this instead:

```
  
  

          cros_sdk --working-dir . -- make CC=x86_64-cros-linux-gnu-clang
```

## Measure Tool

Usage: **measure** *config* *step_size* *length*

*config* signifies a config file, which contains several entries, one per line.
Each entry has is a name/value pair separated by '='.

**measure** provides timing data for a range of memory.  The idea is to pick a reference
memory address, let's say *start* and establish the time it takes when a
loop, such as the following, is executed for each *candidate* address:  

```
  
  

        for (i = 0; i < count; i++) {
          access *start*
          access *candidate*
        }
```

The *candidate* addresses are chosen using *step_size*, *length* and *offset0*
specified in the *config* file.

*candidate* address is *start + offset0 + i * step_size*.  The timing data is based on the number of iterations equal to the value of *measure_loops* in the config, which defaults to 250000. The theory is that any *candidate* address that is in the same bank as *start* is going to take a longer time to access, due to bank conflict.  By analyzing the resulting data, one can arrive at the *conflict_th_us* value, which is used for row finding in the **hammer** tool.  *offset0* is needed to avoid starting in the same row as *start*.

## Hammer Tool

Usage: **hammer** *config*

The sections that follow will discuss the details of operation of the hammer tool.


### Basic Row Finder

If either *alt_row_find* is unspecified or 0 in hammer config, hammer will resort to the basic row finder.  It will sample every *find_step* bytes, and in a similar manner to the measure tool, measure how long it takes to alternatingly access that address and the *start* *measure_loops* times.  If the time exceed conflict threshold, then it has found a new row.

Optionally, *max_fuzz* and *fuzz_step* parameters can be used to introduce a fudge factor into this method.  For each *find_step*, it will try increments of *fuzz_step* until *max_fuzz*.  As soon as it finds a row, it will simply go to the next *find_step* increment.  This fudging feature is considered deprecated.  The preferred way is to use the alternative row finder, which is more flexible (but slower).

### Alternative Row Finder

Alternatively, *alt_row_find* can be set to 1.

Rather than expecting to find entire rows bounded by some step size, the alternative row finding method allows the use of a smaller unit called *atomic_unit* to compose the rows.  The definition of an *atomic_unit* is the largest naturally aligned region that must belong to exactly one row.  Additionally, *row_size* specifies the number of *atomic_unit* in a row.  The atomic row finder will find atomic units until it has enough of them to compose a row.  Then, it will assemble a row and then move onto finding atomic units for the next one.

Additionally, this method permits finding rows in multiple banks simultaneously.  The rows within the same bank will be contiguously numbered.  The parameters used for this are

* *nr_banks* : number of banks
* *bank_find_step* : step size used for bank-finding heuristic
* *rows_per_set* : number of rows in each bank before we round robin to the next bank.

The bank finder runs before the core code of the alternative row finder, and finds the specified number of banks.  The rows are then found in those banks.  The algorithm for bank-finding is by exclusion from known banks.  The first bank can be represented by an arbitrary address.  Subsequent banks are found by taking a distant address and proving that it is not found in any of the existing banks, by showing that the measurement with respect to addresses in those banks always fall below *conflict_th_us*.

What follows are the various other configuration options that can be placed in a
hammer config.

### contig={0|1} and cached={0|1}

The android ION driver allows allocation of contiguous uncached memory.  The contiguity is useful for locating rows and the uncached memory is useful for hammering without explicit flushes.  Depending on the platform, the flushes might have an associated cost.  If *contig=0* and *cached=1*, a normal system allocation will be made and then possibly linearized.  See *pagemap={0|1}*.

### pagemap={0|1}

Setting *pagemap=1* allows using /proc/<pid>/pagemap to convert virtual addresses to physical addresses.  This allows sorting rows (see *sort_rows* below).  Additionally, it is generally useful to be able to inspect the physical addresses of bit flips.  *pagemap* requires a small kernel patch to work with memory from the ION driver.  When using *contig=0*, *pagemap=1* allows the linearlization of a non-contiguous allocation.  This involves using *mremap* to reorganize the allocation so that it is clumped into physically contiguous regions, with the largest region first.

### size=sz[K|M|G]

Specifies the size of the allocation.

### offset0=off[K|M|G]

Specifies the distance between the refrence row and the first address to be tested.  This should be large enough to guarantee that the two addresses are not on the same row.

### cpu=cpu

Specifies the CPU to pin to.  Can be used multiple times for multithreaded mode, which isn't particularly well-tested or encouraged.

### sched_fifo=priority

Specified that sched_fifo should be used with the given priority.

### n_rows=rows
### n_tries=tries

For each set of *rows*, run the specified number of *tries*.

### min_aggrs=min and max_aggrs=max

Specifies the minimum and maximum number of aggressors generated by the pattern.  The individual pattern will decide the placement of those aggressors.


### hammer_loops=iterations

Total number of hammer iterations, distributed across all aggressors.  For example, if 10M is specified and there are
4 aggressors, each will get 2.5M.

### sort_rows={0|1}

Sorts the rows for certain known row ordering schemes in DRAM banks.  Without this, the order in which rows are numbered in hammer does not match the adjacency order in the DRAM chip.  *sort_rows_shift* parameter specifies the lowest order row bit.

### fill_mode={random|mod}

When *fill_mode* is random, the rows are filled with random data prior to hammering and verification.

When *fill_mode* is mod, a modular pattern is used for filling rows.  The parameter *mod* specifies the divisor for the modulus.  The parameter *victim_mask* specifies the mask of the victims in this modulus.  For every bit *k* set in *victim_mask*, all rows *k* mod *mod* are considered victim rows.  Victim rows are filled with *victim_data_pattern*.  All other rows are filled with its inverse.  By default, the modular pattern is tried with all *mod* alignments: with the outer loop being the alignment.  This behaviour can be disabled by setting *mod_stride* to 0.  If *mod_stride* is zero, the modular pattern will only be run naturally aligned.

### check_rest={0|1}

Enabling *check_rest* allows covering uncertainty about row finding.  It checks the entire memory area instead of just the rows in the current set.

### always_refill={0|1}

Normally, we only refill if we detect bit flips.  However, some patterns are intentionally designed (for performance or experimental reasons) to not detect all bit flips, but to focus on designated victim rows.  For the code to work properly with these patterns, *always_refill=1* must be specified.

### repeat_flips=count

When a random pattern creates a bit flip, it is often useful to know if the flip is repeatable.  repeat_flips allows using exactly the same random numbers to repeat the experiment *count* times, only if a flip happens in the original attempt.

### step=*step*:*count*

Introduces an outer loop to hammering.  The loop increments by *step* until it
reaches count.  The address is simultaneously incremented by *step*.  When
multiple step parameters are specified, they represent loops starting with the
outermost and proceeding inwards.

### pattern=even

Even numbered aggressors.

### pattern=random

A uniformly random row is selected from n_rows for each aggressor in each trial.

## pattern=trrespass_assisted_double

Assisted double as defined in the trrespass paper.  The parameter
*assisted_double_dist* determines the distance from the end of the block of rows
to the assistant row.
