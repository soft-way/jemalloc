#define JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/util.h"

JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

/******************************************************************************/
/* Data. */

/*
 * Define names for both unininitialized and initialized phases, so that
 * options and mallctl processing are straightforward.
 */
const char *percpu_arena_mode_names[] = {
	"percpu",
	"phycpu",
	"disabled",
	"percpu",
	"phycpu"
};
percpu_arena_mode_t opt_percpu_arena = PERCPU_ARENA_DEFAULT;

ssize_t opt_dirty_decay_ms = DIRTY_DECAY_MS_DEFAULT;
ssize_t opt_muzzy_decay_ms = MUZZY_DECAY_MS_DEFAULT;

static atomic_zd_t dirty_decay_ms_default;
static atomic_zd_t muzzy_decay_ms_default;

const uint64_t h_steps[SMOOTHSTEP_NSTEPS] = {
#define STEP(step, h, x, y)			\
		h,
		SMOOTHSTEP
#undef STEP
};

static div_info_t arena_binind_div_info[SC_NBINS];

size_t opt_oversize_threshold = OVERSIZE_THRESHOLD_DEFAULT;
size_t oversize_threshold = OVERSIZE_THRESHOLD_DEFAULT;
static unsigned huge_arena_ind;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void arena_decay_to_limit(tsdn_t *tsdn, arena_t *arena,
    arena_decay_t *decay, eset_t *eset, bool all, size_t npages_limit,
    size_t npages_decay_max, bool is_background_thread);
static bool arena_decay_dirty(tsdn_t *tsdn, arena_t *arena,
    bool is_background_thread, bool all);
static void arena_dalloc_bin_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    bin_t *bin);
static void arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    bin_t *bin);

/******************************************************************************/

void
arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy) {
	*nthreads += arena_nthreads_get(arena, false);
	*dss = dss_prec_names[arena_dss_prec_get(arena)];
	*dirty_decay_ms = arena_dirty_decay_ms_get(arena);
	*muzzy_decay_ms = arena_muzzy_decay_ms_get(arena);
	*nactive += atomic_load_zu(&arena->nactive, ATOMIC_RELAXED);
	*ndirty += eset_npages_get(&arena->eset_dirty);
	*nmuzzy += eset_npages_get(&arena->eset_muzzy);
}

void
arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy, arena_stats_t *astats,
    bin_stats_t *bstats, arena_stats_large_t *lstats,
    arena_stats_extents_t *estats) {
	cassert(config_stats);

	arena_basic_stats_merge(tsdn, arena, nthreads, dss, dirty_decay_ms,
	    muzzy_decay_ms, nactive, ndirty, nmuzzy);

	size_t base_allocated, base_resident, base_mapped, metadata_thp;
	base_stats_get(tsdn, arena->base, &base_allocated, &base_resident,
	    &base_mapped, &metadata_thp);

	arena_stats_lock(tsdn, &arena->stats);

	arena_stats_accum_zu(&astats->mapped, base_mapped
	    + arena_stats_read_zu(tsdn, &arena->stats, &arena->stats.mapped));
	arena_stats_accum_zu(&astats->retained,
	    eset_npages_get(&arena->eset_retained) << LG_PAGE);

	atomic_store_zu(&astats->extent_avail,
	    atomic_load_zu(&arena->extent_avail_cnt, ATOMIC_RELAXED),
	    ATOMIC_RELAXED);

	arena_stats_accum_u64(&astats->decay_dirty.npurge,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_dirty.npurge));
	arena_stats_accum_u64(&astats->decay_dirty.nmadvise,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_dirty.nmadvise));
	arena_stats_accum_u64(&astats->decay_dirty.purged,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_dirty.purged));

	arena_stats_accum_u64(&astats->decay_muzzy.npurge,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_muzzy.npurge));
	arena_stats_accum_u64(&astats->decay_muzzy.nmadvise,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_muzzy.nmadvise));
	arena_stats_accum_u64(&astats->decay_muzzy.purged,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_muzzy.purged));

	arena_stats_accum_zu(&astats->base, base_allocated);
	arena_stats_accum_zu(&astats->internal, arena_internal_get(arena));
	arena_stats_accum_zu(&astats->metadata_thp, metadata_thp);
	arena_stats_accum_zu(&astats->resident, base_resident +
	    (((atomic_load_zu(&arena->nactive, ATOMIC_RELAXED) +
	    eset_npages_get(&arena->eset_dirty) +
	    eset_npages_get(&arena->eset_muzzy)) << LG_PAGE)));
	arena_stats_accum_zu(&astats->abandoned_vm, atomic_load_zu(
	    &arena->stats.abandoned_vm, ATOMIC_RELAXED));

	for (szind_t i = 0; i < SC_NSIZES - SC_NBINS; i++) {
		uint64_t nmalloc = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].nmalloc);
		arena_stats_accum_u64(&lstats[i].nmalloc, nmalloc);
		arena_stats_accum_u64(&astats->nmalloc_large, nmalloc);

		uint64_t ndalloc = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].ndalloc);
		arena_stats_accum_u64(&lstats[i].ndalloc, ndalloc);
		arena_stats_accum_u64(&astats->ndalloc_large, ndalloc);

		uint64_t nrequests = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].nrequests);
		arena_stats_accum_u64(&lstats[i].nrequests,
		    nmalloc + nrequests);
		arena_stats_accum_u64(&astats->nrequests_large,
		    nmalloc + nrequests);

		/* nfill == nmalloc for large currently. */
		arena_stats_accum_u64(&lstats[i].nfills, nmalloc);
		arena_stats_accum_u64(&astats->nfills_large, nmalloc);

		uint64_t nflush = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].nflushes);
		arena_stats_accum_u64(&lstats[i].nflushes, nflush);
		arena_stats_accum_u64(&astats->nflushes_large, nflush);

		assert(nmalloc >= ndalloc);
		assert(nmalloc - ndalloc <= SIZE_T_MAX);
		size_t curlextents = (size_t)(nmalloc - ndalloc);
		lstats[i].curlextents += curlextents;
		arena_stats_accum_zu(&astats->allocated_large,
		    curlextents * sz_index2size(SC_NBINS + i));
	}

	for (pszind_t i = 0; i < SC_NPSIZES; i++) {
		size_t dirty, muzzy, retained, dirty_bytes, muzzy_bytes,
		    retained_bytes;
		dirty = eset_nextents_get(&arena->eset_dirty, i);
		muzzy = eset_nextents_get(&arena->eset_muzzy, i);
		retained = eset_nextents_get(&arena->eset_retained, i);
		dirty_bytes = eset_nbytes_get(&arena->eset_dirty, i);
		muzzy_bytes = eset_nbytes_get(&arena->eset_muzzy, i);
		retained_bytes = eset_nbytes_get(&arena->eset_retained, i);

		atomic_store_zu(&estats[i].ndirty, dirty, ATOMIC_RELAXED);
		atomic_store_zu(&estats[i].nmuzzy, muzzy, ATOMIC_RELAXED);
		atomic_store_zu(&estats[i].nretained, retained, ATOMIC_RELAXED);
		atomic_store_zu(&estats[i].dirty_bytes, dirty_bytes,
		    ATOMIC_RELAXED);
		atomic_store_zu(&estats[i].muzzy_bytes, muzzy_bytes,
		    ATOMIC_RELAXED);
		atomic_store_zu(&estats[i].retained_bytes, retained_bytes,
		    ATOMIC_RELAXED);
	}

	arena_stats_unlock(tsdn, &arena->stats);

	/* tcache_bytes counts currently cached bytes. */
	atomic_store_zu(&astats->tcache_bytes, 0, ATOMIC_RELAXED);
	malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
	cache_bin_array_descriptor_t *descriptor;
	ql_foreach(descriptor, &arena->cache_bin_array_descriptor_ql, link) {
		for (szind_t i = 0; i < SC_NBINS; i++) {
			cache_bin_t *tbin = &descriptor->bins_small[i];
			arena_stats_accum_zu(&astats->tcache_bytes,
			    cache_bin_ncached_get(tbin, i) * sz_index2size(i));
		}
		for (szind_t i = 0; i < nhbins - SC_NBINS; i++) {
			cache_bin_t *tbin = &descriptor->bins_large[i];
			arena_stats_accum_zu(&astats->tcache_bytes,
			    cache_bin_ncached_get(tbin, i + SC_NBINS) *
			    sz_index2size(i));
		}
	}
	malloc_mutex_prof_read(tsdn,
	    &astats->mutex_prof_data[arena_prof_mutex_tcache_list],
	    &arena->tcache_ql_mtx);
	malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);

#define READ_ARENA_MUTEX_PROF_DATA(mtx, ind)				\
    malloc_mutex_lock(tsdn, &arena->mtx);				\
    malloc_mutex_prof_read(tsdn, &astats->mutex_prof_data[ind],		\
        &arena->mtx);							\
    malloc_mutex_unlock(tsdn, &arena->mtx);

	/* Gather per arena mutex profiling data. */
	READ_ARENA_MUTEX_PROF_DATA(large_mtx, arena_prof_mutex_large);
	READ_ARENA_MUTEX_PROF_DATA(extent_avail_mtx,
	    arena_prof_mutex_extent_avail)
	READ_ARENA_MUTEX_PROF_DATA(eset_dirty.mtx,
	    arena_prof_mutex_extents_dirty)
	READ_ARENA_MUTEX_PROF_DATA(eset_muzzy.mtx,
	    arena_prof_mutex_extents_muzzy)
	READ_ARENA_MUTEX_PROF_DATA(eset_retained.mtx,
	    arena_prof_mutex_extents_retained)
	READ_ARENA_MUTEX_PROF_DATA(decay_dirty.mtx,
	    arena_prof_mutex_decay_dirty)
	READ_ARENA_MUTEX_PROF_DATA(decay_muzzy.mtx,
	    arena_prof_mutex_decay_muzzy)
	READ_ARENA_MUTEX_PROF_DATA(base->mtx,
	    arena_prof_mutex_base)
#undef READ_ARENA_MUTEX_PROF_DATA

	nstime_copy(&astats->uptime, &arena->create_time);
	nstime_update(&astats->uptime);
	nstime_subtract(&astats->uptime, &arena->create_time);

	for (szind_t i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_stats_merge(tsdn, &bstats[i],
			    &arena->bins[i].bin_shards[j]);
		}
	}
}

void
arena_extents_dirty_dalloc(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	extents_dalloc(tsdn, arena, r_extent_hooks, &arena->eset_dirty,
	    extent);
	if (arena_dirty_decay_ms_get(arena) == 0) {
		arena_decay_dirty(tsdn, arena, false, true);
	} else {
		arena_background_thread_inactivity_check(tsdn, arena, false);
	}
}

static void *
arena_slab_reg_alloc(extent_t *slab, const bin_info_t *bin_info) {
	void *ret;
	slab_data_t *slab_data = extent_slab_data_get(slab);
	size_t regind;

	assert(extent_nfree_get(slab) > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(slab_data->bitmap, &bin_info->bitmap_info);
	ret = (void *)((uintptr_t)extent_addr_get(slab) +
	    (uintptr_t)(bin_info->reg_size * regind));
	extent_nfree_dec(slab);
	return ret;
}

static void
arena_slab_reg_alloc_batch(extent_t *slab, const bin_info_t *bin_info,
			   unsigned cnt, void** ptrs) {
	slab_data_t *slab_data = extent_slab_data_get(slab);

	assert(extent_nfree_get(slab) >= cnt);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

#if (! defined JEMALLOC_INTERNAL_POPCOUNTL) || (defined BITMAP_USE_TREE)
	for (unsigned i = 0; i < cnt; i++) {
		size_t regind = bitmap_sfu(slab_data->bitmap,
					   &bin_info->bitmap_info);
		*(ptrs + i) = (void *)((uintptr_t)extent_addr_get(slab) +
		    (uintptr_t)(bin_info->reg_size * regind));
	}
#else
	unsigned group = 0;
	bitmap_t g = slab_data->bitmap[group];
	unsigned i = 0;
	while (i < cnt) {
		while (g == 0) {
			g = slab_data->bitmap[++group];
		}
		size_t shift = group << LG_BITMAP_GROUP_NBITS;
		size_t pop = popcount_lu(g);
		if (pop > (cnt - i)) {
			pop = cnt - i;
		}

		/*
		 * Load from memory locations only once, outside the
		 * hot loop below.
		 */
		uintptr_t base = (uintptr_t)extent_addr_get(slab);
		uintptr_t regsize = (uintptr_t)bin_info->reg_size;
		while (pop--) {
			size_t bit = cfs_lu(&g);
			size_t regind = shift + bit;
			*(ptrs + i) = (void *)(base + regsize * regind);

			i++;
		}
		slab_data->bitmap[group] = g;
	}
#endif
	extent_nfree_sub(slab, cnt);
}

#ifndef JEMALLOC_JET
static
#endif
size_t
arena_slab_regind(extent_t *slab, szind_t binind, const void *ptr) {
	size_t diff, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)extent_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)extent_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)extent_addr_get(slab)) %
	    (uintptr_t)bin_infos[binind].reg_size == 0);

	diff = (size_t)((uintptr_t)ptr - (uintptr_t)extent_addr_get(slab));

	/* Avoid doing division with a variable divisor. */
	regind = div_compute(&arena_binind_div_info[binind], diff);

	assert(regind < bin_infos[binind].nregs);

	return regind;
}

static void
arena_slab_reg_dalloc(extent_t *slab, slab_data_t *slab_data, void *ptr) {
	szind_t binind = extent_szind_get(slab);
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t regind = arena_slab_regind(slab, binind, ptr);

	assert(extent_nfree_get(slab) < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);
	extent_nfree_inc(slab);
}

static void
arena_nactive_add(arena_t *arena, size_t add_pages) {
	atomic_fetch_add_zu(&arena->nactive, add_pages, ATOMIC_RELAXED);
}

static void
arena_nactive_sub(arena_t *arena, size_t sub_pages) {
	assert(atomic_load_zu(&arena->nactive, ATOMIC_RELAXED) >= sub_pages);
	atomic_fetch_sub_zu(&arena->nactive, sub_pages, ATOMIC_RELAXED);
}

static void
arena_large_malloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < SC_LARGE_MINCLASS) {
		usize = SC_LARGE_MINCLASS;
	}
	index = sz_size2index(usize);
	hindex = (index >= SC_NBINS) ? index - SC_NBINS : 0;

	arena_stats_add_u64(tsdn, &arena->stats,
	    &arena->stats.lstats[hindex].nmalloc, 1);
}

static void
arena_large_dalloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < SC_LARGE_MINCLASS) {
		usize = SC_LARGE_MINCLASS;
	}
	index = sz_size2index(usize);
	hindex = (index >= SC_NBINS) ? index - SC_NBINS : 0;

	arena_stats_add_u64(tsdn, &arena->stats,
	    &arena->stats.lstats[hindex].ndalloc, 1);
}

static void
arena_large_ralloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t oldusize,
    size_t usize) {
	arena_large_dalloc_stats_update(tsdn, arena, oldusize);
	arena_large_malloc_stats_update(tsdn, arena, usize);
}

static bool
arena_may_have_muzzy(arena_t *arena) {
	return (pages_can_purge_lazy && (arena_muzzy_decay_ms_get(arena) != 0));
}

extent_t *
arena_extent_alloc_large(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool *zero) {
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	szind_t szind = sz_size2index(usize);
	size_t mapped_add;
	bool commit = true;
	extent_t *extent = extents_alloc(tsdn, arena, &extent_hooks,
	    &arena->eset_dirty, NULL, usize, sz_large_pad, alignment, false,
	    szind, zero, &commit);
	if (extent == NULL && arena_may_have_muzzy(arena)) {
		extent = extents_alloc(tsdn, arena, &extent_hooks,
		    &arena->eset_muzzy, NULL, usize, sz_large_pad, alignment,
		    false, szind, zero, &commit);
	}
	size_t size = usize + sz_large_pad;
	if (extent == NULL) {
		extent = extent_alloc_wrapper(tsdn, arena, &extent_hooks, NULL,
		    usize, sz_large_pad, alignment, false, szind, zero,
		    &commit);
		if (config_stats) {
			/*
			 * extent may be NULL on OOM, but in that case
			 * mapped_add isn't used below, so there's no need to
			 * conditionlly set it to 0 here.
			 */
			mapped_add = size;
		}
	} else if (config_stats) {
		mapped_add = 0;
	}

	if (extent != NULL) {
		if (config_stats) {
			arena_stats_lock(tsdn, &arena->stats);
			arena_large_malloc_stats_update(tsdn, arena, usize);
			if (mapped_add != 0) {
				arena_stats_add_zu(tsdn, &arena->stats,
				    &arena->stats.mapped, mapped_add);
			}
			arena_stats_unlock(tsdn, &arena->stats);
		}
		arena_nactive_add(arena, size >> LG_PAGE);
	}

	return extent;
}

void
arena_extent_dalloc_large_prep(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_large_dalloc_stats_update(tsdn, arena,
		    extent_usize_get(extent));
		arena_stats_unlock(tsdn, &arena->stats);
	}
	arena_nactive_sub(arena, extent_size_get(extent) >> LG_PAGE);
}

void
arena_extent_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldusize) {
	size_t usize = extent_usize_get(extent);
	size_t udiff = oldusize - usize;

	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		arena_stats_unlock(tsdn, &arena->stats);
	}
	arena_nactive_sub(arena, udiff >> LG_PAGE);
}

void
arena_extent_ralloc_large_expand(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldusize) {
	size_t usize = extent_usize_get(extent);
	size_t udiff = usize - oldusize;

	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		arena_stats_unlock(tsdn, &arena->stats);
	}
	arena_nactive_add(arena, udiff >> LG_PAGE);
}

static ssize_t
arena_decay_ms_read(arena_decay_t *decay) {
	return atomic_load_zd(&decay->time_ms, ATOMIC_RELAXED);
}

static void
arena_decay_ms_write(arena_decay_t *decay, ssize_t decay_ms) {
	atomic_store_zd(&decay->time_ms, decay_ms, ATOMIC_RELAXED);
}

static void
arena_decay_deadline_init(arena_decay_t *decay) {
	/*
	 * Generate a new deadline that is uniformly random within the next
	 * epoch after the current one.
	 */
	nstime_copy(&decay->deadline, &decay->epoch);
	nstime_add(&decay->deadline, &decay->interval);
	if (arena_decay_ms_read(decay) > 0) {
		nstime_t jitter;

		nstime_init(&jitter, prng_range_u64(&decay->jitter_state,
		    nstime_ns(&decay->interval)));
		nstime_add(&decay->deadline, &jitter);
	}
}

static bool
arena_decay_deadline_reached(const arena_decay_t *decay, const nstime_t *time) {
	return (nstime_compare(&decay->deadline, time) <= 0);
}

static size_t
arena_decay_backlog_npages_limit(const arena_decay_t *decay) {
	uint64_t sum;
	size_t npages_limit_backlog;
	unsigned i;

	/*
	 * For each element of decay_backlog, multiply by the corresponding
	 * fixed-point smoothstep decay factor.  Sum the products, then divide
	 * to round down to the nearest whole number of pages.
	 */
	sum = 0;
	for (i = 0; i < SMOOTHSTEP_NSTEPS; i++) {
		sum += decay->backlog[i] * h_steps[i];
	}
	npages_limit_backlog = (size_t)(sum >> SMOOTHSTEP_BFP);

	return npages_limit_backlog;
}

static void
arena_decay_backlog_update_last(arena_decay_t *decay, size_t current_npages) {
	size_t npages_delta = (current_npages > decay->nunpurged) ?
	    current_npages - decay->nunpurged : 0;
	decay->backlog[SMOOTHSTEP_NSTEPS-1] = npages_delta;

	if (config_debug) {
		if (current_npages > decay->ceil_npages) {
			decay->ceil_npages = current_npages;
		}
		size_t npages_limit = arena_decay_backlog_npages_limit(decay);
		assert(decay->ceil_npages >= npages_limit);
		if (decay->ceil_npages > npages_limit) {
			decay->ceil_npages = npages_limit;
		}
	}
}

static void
arena_decay_backlog_update(arena_decay_t *decay, uint64_t nadvance_u64,
    size_t current_npages) {
	if (nadvance_u64 >= SMOOTHSTEP_NSTEPS) {
		memset(decay->backlog, 0, (SMOOTHSTEP_NSTEPS-1) *
		    sizeof(size_t));
	} else {
		size_t nadvance_z = (size_t)nadvance_u64;

		assert((uint64_t)nadvance_z == nadvance_u64);

		memmove(decay->backlog, &decay->backlog[nadvance_z],
		    (SMOOTHSTEP_NSTEPS - nadvance_z) * sizeof(size_t));
		if (nadvance_z > 1) {
			memset(&decay->backlog[SMOOTHSTEP_NSTEPS -
			    nadvance_z], 0, (nadvance_z-1) * sizeof(size_t));
		}
	}

	arena_decay_backlog_update_last(decay, current_npages);
}

static void
arena_decay_try_purge(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    eset_t *eset, size_t current_npages, size_t npages_limit,
    bool is_background_thread) {
	if (current_npages > npages_limit) {
		arena_decay_to_limit(tsdn, arena, decay, eset, false,
		    npages_limit, current_npages - npages_limit,
		    is_background_thread);
	}
}

static void
arena_decay_epoch_advance_helper(arena_decay_t *decay, const nstime_t *time,
    size_t current_npages) {
	assert(arena_decay_deadline_reached(decay, time));

	nstime_t delta;
	nstime_copy(&delta, time);
	nstime_subtract(&delta, &decay->epoch);

	uint64_t nadvance_u64 = nstime_divide(&delta, &decay->interval);
	assert(nadvance_u64 > 0);

	/* Add nadvance_u64 decay intervals to epoch. */
	nstime_copy(&delta, &decay->interval);
	nstime_imultiply(&delta, nadvance_u64);
	nstime_add(&decay->epoch, &delta);

	/* Set a new deadline. */
	arena_decay_deadline_init(decay);

	/* Update the backlog. */
	arena_decay_backlog_update(decay, nadvance_u64, current_npages);
}

static void
arena_decay_epoch_advance(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    eset_t *eset, const nstime_t *time, bool is_background_thread) {
	size_t current_npages = eset_npages_get(eset);
	arena_decay_epoch_advance_helper(decay, time, current_npages);

	size_t npages_limit = arena_decay_backlog_npages_limit(decay);
	/* We may unlock decay->mtx when try_purge(). Finish logging first. */
	decay->nunpurged = (npages_limit > current_npages) ? npages_limit :
	    current_npages;

	if (!background_thread_enabled() || is_background_thread) {
		arena_decay_try_purge(tsdn, arena, decay, eset,
		    current_npages, npages_limit, is_background_thread);
	}
}

static void
arena_decay_reinit(arena_decay_t *decay, ssize_t decay_ms) {
	arena_decay_ms_write(decay, decay_ms);
	if (decay_ms > 0) {
		nstime_init(&decay->interval, (uint64_t)decay_ms *
		    KQU(1000000));
		nstime_idivide(&decay->interval, SMOOTHSTEP_NSTEPS);
	}

	nstime_init(&decay->epoch, 0);
	nstime_update(&decay->epoch);
	decay->jitter_state = (uint64_t)(uintptr_t)decay;
	arena_decay_deadline_init(decay);
	decay->nunpurged = 0;
	memset(decay->backlog, 0, SMOOTHSTEP_NSTEPS * sizeof(size_t));
}

static bool
arena_decay_init(arena_decay_t *decay, ssize_t decay_ms,
    arena_stats_decay_t *stats) {
	if (config_debug) {
		for (size_t i = 0; i < sizeof(arena_decay_t); i++) {
			assert(((char *)decay)[i] == 0);
		}
		decay->ceil_npages = 0;
	}
	if (malloc_mutex_init(&decay->mtx, "decay", WITNESS_RANK_DECAY,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	decay->purging = false;
	arena_decay_reinit(decay, decay_ms);
	/* Memory is zeroed, so there is no need to clear stats. */
	if (config_stats) {
		decay->stats = stats;
	}
	return false;
}

static bool
arena_decay_ms_valid(ssize_t decay_ms) {
	if (decay_ms < -1) {
		return false;
	}
	if (decay_ms == -1 || (uint64_t)decay_ms <= NSTIME_SEC_MAX *
	    KQU(1000)) {
		return true;
	}
	return false;
}

static bool
arena_maybe_decay(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    eset_t *eset, bool is_background_thread) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	/* Purge all or nothing if the option is disabled. */
	ssize_t decay_ms = arena_decay_ms_read(decay);
	if (decay_ms <= 0) {
		if (decay_ms == 0) {
			arena_decay_to_limit(tsdn, arena, decay, eset, false,
			    0, eset_npages_get(eset),
			    is_background_thread);
		}
		return false;
	}

	nstime_t time;
	nstime_init(&time, 0);
	nstime_update(&time);
	if (unlikely(!nstime_monotonic() && nstime_compare(&decay->epoch, &time)
	    > 0)) {
		/*
		 * Time went backwards.  Move the epoch back in time and
		 * generate a new deadline, with the expectation that time
		 * typically flows forward for long enough periods of time that
		 * epochs complete.  Unfortunately, this strategy is susceptible
		 * to clock jitter triggering premature epoch advances, but
		 * clock jitter estimation and compensation isn't feasible here
		 * because calls into this code are event-driven.
		 */
		nstime_copy(&decay->epoch, &time);
		arena_decay_deadline_init(decay);
	} else {
		/* Verify that time does not go backwards. */
		assert(nstime_compare(&decay->epoch, &time) <= 0);
	}

	/*
	 * If the deadline has been reached, advance to the current epoch and
	 * purge to the new limit if necessary.  Note that dirty pages created
	 * during the current epoch are not subject to purge until a future
	 * epoch, so as a result purging only happens during epoch advances, or
	 * being triggered by background threads (scheduled event).
	 */
	bool advance_epoch = arena_decay_deadline_reached(decay, &time);
	if (advance_epoch) {
		arena_decay_epoch_advance(tsdn, arena, decay, eset, &time,
		    is_background_thread);
	} else if (is_background_thread) {
		arena_decay_try_purge(tsdn, arena, decay, eset,
		    eset_npages_get(eset),
		    arena_decay_backlog_npages_limit(decay),
		    is_background_thread);
	}

	return advance_epoch;
}

static ssize_t
arena_decay_ms_get(arena_decay_t *decay) {
	return arena_decay_ms_read(decay);
}

ssize_t
arena_dirty_decay_ms_get(arena_t *arena) {
	return arena_decay_ms_get(&arena->decay_dirty);
}

ssize_t
arena_muzzy_decay_ms_get(arena_t *arena) {
	return arena_decay_ms_get(&arena->decay_muzzy);
}

static bool
arena_decay_ms_set(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    eset_t *eset, ssize_t decay_ms) {
	if (!arena_decay_ms_valid(decay_ms)) {
		return true;
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	/*
	 * Restart decay backlog from scratch, which may cause many dirty pages
	 * to be immediately purged.  It would conceptually be possible to map
	 * the old backlog onto the new backlog, but there is no justification
	 * for such complexity since decay_ms changes are intended to be
	 * infrequent, either between the {-1, 0, >0} states, or a one-time
	 * arbitrary change during initial arena configuration.
	 */
	arena_decay_reinit(decay, decay_ms);
	arena_maybe_decay(tsdn, arena, decay, eset, false);
	malloc_mutex_unlock(tsdn, &decay->mtx);

	return false;
}

bool
arena_dirty_decay_ms_set(tsdn_t *tsdn, arena_t *arena,
    ssize_t decay_ms) {
	return arena_decay_ms_set(tsdn, arena, &arena->decay_dirty,
	    &arena->eset_dirty, decay_ms);
}

bool
arena_muzzy_decay_ms_set(tsdn_t *tsdn, arena_t *arena,
    ssize_t decay_ms) {
	return arena_decay_ms_set(tsdn, arena, &arena->decay_muzzy,
	    &arena->eset_muzzy, decay_ms);
}

static size_t
arena_stash_decayed(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, eset_t *eset, size_t npages_limit,
	size_t npages_decay_max, extent_list_t *decay_extents) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	extent_t *extent;
	while (nstashed < npages_decay_max &&
	    (extent = extents_evict(tsdn, arena, r_extent_hooks, eset,
	    npages_limit)) != NULL) {
		extent_list_append(decay_extents, extent);
		nstashed += extent_size_get(extent) >> LG_PAGE;
	}
	return nstashed;
}

static size_t
arena_decay_stashed(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, arena_decay_t *decay, eset_t *eset,
    bool all, extent_list_t *decay_extents, bool is_background_thread) {
	size_t nmadvise, nunmapped;
	size_t npurged;

	if (config_stats) {
		nmadvise = 0;
		nunmapped = 0;
	}
	npurged = 0;

	ssize_t muzzy_decay_ms = arena_muzzy_decay_ms_get(arena);
	for (extent_t *extent = extent_list_first(decay_extents); extent !=
	    NULL; extent = extent_list_first(decay_extents)) {
		if (config_stats) {
			nmadvise++;
		}
		size_t npages = extent_size_get(extent) >> LG_PAGE;
		npurged += npages;
		extent_list_remove(decay_extents, extent);
		switch (eset_state_get(eset)) {
		case extent_state_active:
			not_reached();
		case extent_state_dirty:
			if (!all && muzzy_decay_ms != 0 &&
			    !extent_purge_lazy_wrapper(tsdn, arena,
			    r_extent_hooks, extent, 0,
			    extent_size_get(extent))) {
				extents_dalloc(tsdn, arena, r_extent_hooks,
				    &arena->eset_muzzy, extent);
				arena_background_thread_inactivity_check(tsdn,
				    arena, is_background_thread);
				break;
			}
			JEMALLOC_FALLTHROUGH;
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, arena, r_extent_hooks,
			    extent);
			if (config_stats) {
				nunmapped += npages;
			}
			break;
		case extent_state_retained:
		default:
			not_reached();
		}
	}

	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_stats_add_u64(tsdn, &arena->stats, &decay->stats->npurge,
		    1);
		arena_stats_add_u64(tsdn, &arena->stats,
		    &decay->stats->nmadvise, nmadvise);
		arena_stats_add_u64(tsdn, &arena->stats, &decay->stats->purged,
		    npurged);
		arena_stats_sub_zu(tsdn, &arena->stats, &arena->stats.mapped,
		    nunmapped << LG_PAGE);
		arena_stats_unlock(tsdn, &arena->stats);
	}

	return npurged;
}

/*
 * npages_limit: Decay at most npages_decay_max pages without violating the
 * invariant: (eset_npages_get(extents) >= npages_limit).  We need an upper
 * bound on number of pages in order to prevent unbounded growth (namely in
 * stashed), otherwise unbounded new pages could be added to extents during the
 * current decay run, so that the purging thread never finishes.
 */
static void
arena_decay_to_limit(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    eset_t *eset, bool all, size_t npages_limit, size_t npages_decay_max,
    bool is_background_thread) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 1);
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	if (decay->purging || npages_decay_max == 0) {
		return;
	}
	decay->purging = true;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	extent_hooks_t *extent_hooks = extent_hooks_get(arena);

	extent_list_t decay_extents;
	extent_list_init(&decay_extents);

	size_t npurge = arena_stash_decayed(tsdn, arena, &extent_hooks, eset,
	    npages_limit, npages_decay_max, &decay_extents);
	if (npurge != 0) {
		size_t npurged = arena_decay_stashed(tsdn, arena,
		    &extent_hooks, decay, eset, all, &decay_extents,
		    is_background_thread);
		assert(npurged == npurge);
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	decay->purging = false;
}

static bool
arena_decay_impl(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    eset_t *eset, bool is_background_thread, bool all) {
	if (all) {
		malloc_mutex_lock(tsdn, &decay->mtx);
		arena_decay_to_limit(tsdn, arena, decay, eset, all, 0,
		    eset_npages_get(eset), is_background_thread);
		malloc_mutex_unlock(tsdn, &decay->mtx);

		return false;
	}

	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		/* No need to wait if another thread is in progress. */
		return true;
	}

	bool epoch_advanced = arena_maybe_decay(tsdn, arena, decay, eset,
	    is_background_thread);
	size_t npages_new;
	if (epoch_advanced) {
		/* Backlog is updated on epoch advance. */
		npages_new = decay->backlog[SMOOTHSTEP_NSTEPS-1];
	}
	malloc_mutex_unlock(tsdn, &decay->mtx);

	if (have_background_thread && background_thread_enabled() &&
	    epoch_advanced && !is_background_thread) {
		background_thread_interval_check(tsdn, arena, decay,
		    npages_new);
	}

	return false;
}

static bool
arena_decay_dirty(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all) {
	return arena_decay_impl(tsdn, arena, &arena->decay_dirty,
	    &arena->eset_dirty, is_background_thread, all);
}

static bool
arena_decay_muzzy(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all) {
	if (eset_npages_get(&arena->eset_muzzy) == 0 &&
	    arena_muzzy_decay_ms_get(arena) <= 0) {
		return false;
	}
	return arena_decay_impl(tsdn, arena, &arena->decay_muzzy,
	    &arena->eset_muzzy, is_background_thread, all);
}

void
arena_decay(tsdn_t *tsdn, arena_t *arena, bool is_background_thread, bool all) {
	if (arena_decay_dirty(tsdn, arena, is_background_thread, all)) {
		return;
	}
	arena_decay_muzzy(tsdn, arena, is_background_thread, all);
}

static void
arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *slab) {
	arena_nactive_sub(arena, extent_size_get(slab) >> LG_PAGE);

	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;
	arena_extents_dirty_dalloc(tsdn, arena, &extent_hooks, slab);
}

static void
arena_bin_slabs_nonfull_insert(bin_t *bin, extent_t *slab) {
	assert(extent_nfree_get(slab) > 0);
	extent_heap_insert(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs++;
	}
}

static void
arena_bin_slabs_nonfull_remove(bin_t *bin, extent_t *slab) {
	extent_heap_remove(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs--;
	}
}

static extent_t *
arena_bin_slabs_nonfull_tryget(bin_t *bin) {
	extent_t *slab = extent_heap_remove_first(&bin->slabs_nonfull);
	if (slab == NULL) {
		return NULL;
	}
	if (config_stats) {
		bin->stats.reslabs++;
		bin->stats.nonfull_slabs--;
	}
	return slab;
}

static void
arena_bin_slabs_full_insert(arena_t *arena, bin_t *bin, extent_t *slab) {
	assert(extent_nfree_get(slab) == 0);
	/*
	 *  Tracking extents is required by arena_reset, which is not allowed
	 *  for auto arenas.  Bypass this step to avoid touching the extent
	 *  linkage (often results in cache misses) for auto arenas.
	 */
	if (arena_is_auto(arena)) {
		return;
	}
	extent_list_append(&bin->slabs_full, slab);
}

static void
arena_bin_slabs_full_remove(arena_t *arena, bin_t *bin, extent_t *slab) {
	if (arena_is_auto(arena)) {
		return;
	}
	extent_list_remove(&bin->slabs_full, slab);
}

static void
arena_bin_reset(tsd_t *tsd, arena_t *arena, bin_t *bin) {
	extent_t *slab;

	malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	if (bin->slabcur != NULL) {
		slab = bin->slabcur;
		bin->slabcur = NULL;
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	while ((slab = extent_heap_remove_first(&bin->slabs_nonfull)) != NULL) {
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	for (slab = extent_list_first(&bin->slabs_full); slab != NULL;
	     slab = extent_list_first(&bin->slabs_full)) {
		arena_bin_slabs_full_remove(arena, bin, slab);
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	if (config_stats) {
		bin->stats.curregs = 0;
		bin->stats.curslabs = 0;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
}

void
arena_reset(tsd_t *tsd, arena_t *arena) {
	/*
	 * Locking in this function is unintuitive.  The caller guarantees that
	 * no concurrent operations are happening in this arena, but there are
	 * still reasons that some locking is necessary:
	 *
	 * - Some of the functions in the transitive closure of calls assume
	 *   appropriate locks are held, and in some cases these locks are
	 *   temporarily dropped to avoid lock order reversal or deadlock due to
	 *   reentry.
	 * - mallctl("epoch", ...) may concurrently refresh stats.  While
	 *   strictly speaking this is a "concurrent operation", disallowing
	 *   stats refreshes would impose an inconvenient burden.
	 */

	/* Large allocations. */
	malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);

	for (extent_t *extent = extent_list_first(&arena->large); extent !=
	    NULL; extent = extent_list_first(&arena->large)) {
		void *ptr = extent_base_get(extent);
		size_t usize;

		malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);
		alloc_ctx_t alloc_ctx;
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
		assert(alloc_ctx.szind != SC_NSIZES);

		if (config_stats || (config_prof && opt_prof)) {
			usize = sz_index2size(alloc_ctx.szind);
			assert(usize == isalloc(tsd_tsdn(tsd), ptr));
		}
		/* Remove large allocation from prof sample set. */
		if (config_prof && opt_prof) {
			prof_free(tsd, ptr, usize, &alloc_ctx);
		}
		large_dalloc(tsd_tsdn(tsd), extent);
		malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);

	/* Bins. */
	for (unsigned i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			arena_bin_reset(tsd, arena,
			    &arena->bins[i].bin_shards[j]);
		}
	}

	atomic_store_zu(&arena->nactive, 0, ATOMIC_RELAXED);
}

static void
arena_destroy_retained(tsdn_t *tsdn, arena_t *arena) {
	/*
	 * Iterate over the retained extents and destroy them.  This gives the
	 * extent allocator underlying the extent hooks an opportunity to unmap
	 * all retained memory without having to keep its own metadata
	 * structures.  In practice, virtual memory for dss-allocated extents is
	 * leaked here, so best practice is to avoid dss for arenas to be
	 * destroyed, or provide custom extent hooks that track retained
	 * dss-based extents for later reuse.
	 */
	extent_hooks_t *extent_hooks = extent_hooks_get(arena);
	extent_t *extent;
	while ((extent = extents_evict(tsdn, arena, &extent_hooks,
	    &arena->eset_retained, 0)) != NULL) {
		extent_destroy_wrapper(tsdn, arena, &extent_hooks, extent);
	}
}

void
arena_destroy(tsd_t *tsd, arena_t *arena) {
	assert(base_ind_get(arena->base) >= narenas_auto);
	assert(arena_nthreads_get(arena, false) == 0);
	assert(arena_nthreads_get(arena, true) == 0);

	/*
	 * No allocations have occurred since arena_reset() was called.
	 * Furthermore, the caller (arena_i_destroy_ctl()) purged all cached
	 * extents, so only retained extents may remain.
	 */
	assert(eset_npages_get(&arena->eset_dirty) == 0);
	assert(eset_npages_get(&arena->eset_muzzy) == 0);

	/* Deallocate retained memory. */
	arena_destroy_retained(tsd_tsdn(tsd), arena);

	/*
	 * Remove the arena pointer from the arenas array.  We rely on the fact
	 * that there is no way for the application to get a dirty read from the
	 * arenas array unless there is an inherent race in the application
	 * involving access of an arena being concurrently destroyed.  The
	 * application must synchronize knowledge of the arena's validity, so as
	 * long as we use an atomic write to update the arenas array, the
	 * application will get a clean read any time after it synchronizes
	 * knowledge that the arena is no longer valid.
	 */
	arena_set(base_ind_get(arena->base), NULL);

	/*
	 * Destroy the base allocator, which manages all metadata ever mapped by
	 * this arena.
	 */
	base_delete(tsd_tsdn(tsd), arena->base);
}

static extent_t *
arena_slab_alloc_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, const bin_info_t *bin_info,
    szind_t szind) {
	extent_t *slab;
	bool zero, commit;

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	zero = false;
	commit = true;
	slab = extent_alloc_wrapper(tsdn, arena, r_extent_hooks, NULL,
	    bin_info->slab_size, 0, PAGE, true, szind, &zero, &commit);

	if (config_stats && slab != NULL) {
		arena_stats_mapped_add(tsdn, &arena->stats,
		    bin_info->slab_size);
	}

	return slab;
}

static extent_t *
arena_slab_alloc(tsdn_t *tsdn, arena_t *arena, szind_t binind, unsigned binshard,
    const bin_info_t *bin_info) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;
	szind_t szind = sz_size2index(bin_info->reg_size);
	bool zero = false;
	bool commit = true;
	extent_t *slab = extents_alloc(tsdn, arena, &extent_hooks,
	    &arena->eset_dirty, NULL, bin_info->slab_size, 0, PAGE, true,
	    binind, &zero, &commit);
	if (slab == NULL && arena_may_have_muzzy(arena)) {
		slab = extents_alloc(tsdn, arena, &extent_hooks,
		    &arena->eset_muzzy, NULL, bin_info->slab_size, 0, PAGE,
		    true, binind, &zero, &commit);
	}
	if (slab == NULL) {
		slab = arena_slab_alloc_hard(tsdn, arena, &extent_hooks,
		    bin_info, szind);
		if (slab == NULL) {
			return NULL;
		}
	}
	assert(extent_slab_get(slab));

	/* Initialize slab internals. */
	slab_data_t *slab_data = extent_slab_data_get(slab);
	extent_nfree_binshard_set(slab, bin_info->nregs, binshard);
	bitmap_init(slab_data->bitmap, &bin_info->bitmap_info, false);

	arena_nactive_add(arena, extent_size_get(slab) >> LG_PAGE);

	return slab;
}

static extent_t *
arena_bin_nonfull_slab_get(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, unsigned binshard) {
	extent_t *slab;
	const bin_info_t *bin_info;

	/* Look for a usable slab. */
	slab = arena_bin_slabs_nonfull_tryget(bin);
	if (slab != NULL) {
		return slab;
	}
	/* No existing slabs have any space available. */

	bin_info = &bin_infos[binind];

	/* Allocate a new slab. */
	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	slab = arena_slab_alloc(tsdn, arena, binind, binshard, bin_info);
	/********************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (slab != NULL) {
		if (config_stats) {
			bin->stats.nslabs++;
			bin->stats.curslabs++;
		}
		return slab;
	}

	/*
	 * arena_slab_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped bin->lock above,
	 * so search one more time.
	 */
	slab = arena_bin_slabs_nonfull_tryget(bin);
	if (slab != NULL) {
		return slab;
	}

	return NULL;
}

/* Re-fill bin->slabcur, then call arena_slab_reg_alloc(). */
static void *
arena_bin_malloc_hard(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, unsigned binshard) {
	const bin_info_t *bin_info;
	extent_t *slab;

	bin_info = &bin_infos[binind];
	if (!arena_is_auto(arena) && bin->slabcur != NULL) {
		arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
		bin->slabcur = NULL;
	}
	slab = arena_bin_nonfull_slab_get(tsdn, arena, bin, binind, binshard);
	if (bin->slabcur != NULL) {
		/*
		 * Another thread updated slabcur while this one ran without the
		 * bin lock in arena_bin_nonfull_slab_get().
		 */
		if (extent_nfree_get(bin->slabcur) > 0) {
			void *ret = arena_slab_reg_alloc(bin->slabcur,
			    bin_info);
			if (slab != NULL) {
				/*
				 * arena_slab_alloc() may have allocated slab,
				 * or it may have been pulled from
				 * slabs_nonfull.  Therefore it is unsafe to
				 * make any assumptions about how slab has
				 * previously been used, and
				 * arena_bin_lower_slab() must be called, as if
				 * a region were just deallocated from the slab.
				 */
				if (extent_nfree_get(slab) == bin_info->nregs) {
					arena_dalloc_bin_slab(tsdn, arena, slab,
					    bin);
				} else {
					arena_bin_lower_slab(tsdn, arena, slab,
					    bin);
				}
			}
			return ret;
		}

		arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
		bin->slabcur = NULL;
	}

	if (slab == NULL) {
		return NULL;
	}
	bin->slabcur = slab;

	assert(extent_nfree_get(bin->slabcur) > 0);

	return arena_slab_reg_alloc(slab, bin_info);
}

/* Choose a bin shard and return the locked bin. */
bin_t *
arena_bin_choose_lock(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    unsigned *binshard) {
	bin_t *bin;
	if (tsdn_null(tsdn) || tsd_arena_get(tsdn_tsd(tsdn)) == NULL) {
		*binshard = 0;
	} else {
		*binshard = tsd_binshardsp_get(tsdn_tsd(tsdn))->binshard[binind];
	}
	assert(*binshard < bin_infos[binind].n_shards);
	bin = &arena->bins[binind].bin_shards[*binshard];
	malloc_mutex_lock(tsdn, &bin->lock);

	return bin;
}

void
arena_tcache_fill_small(tsdn_t *tsdn, arena_t *arena, tcache_t *tcache,
    cache_bin_t *tbin, szind_t binind) {
	unsigned i, nfill, cnt;

	assert(cache_bin_ncached_get(tbin, binind) == 0);
	tcache->bin_refilled[binind] = true;

	unsigned binshard;
	bin_t *bin = arena_bin_choose_lock(tsdn, arena, binind, &binshard);

	void **empty_position = cache_bin_empty_position_get(tbin, binind);
	for (i = 0, nfill = (cache_bin_ncached_max_get(binind) >>
	    tcache->lg_fill_div[binind]); i < nfill; i += cnt) {
		extent_t *slab;
		if ((slab = bin->slabcur) != NULL && extent_nfree_get(slab) >
		    0) {
			unsigned tofill = nfill - i;
			cnt = tofill < extent_nfree_get(slab) ?
				tofill : extent_nfree_get(slab);
			arena_slab_reg_alloc_batch(
			   slab, &bin_infos[binind], cnt,
			   empty_position - nfill + i);
		} else {
			cnt = 1;
			void *ptr = arena_bin_malloc_hard(tsdn, arena, bin,
			    binind, binshard);
			/*
			 * OOM.  tbin->avail isn't yet filled down to its first
			 * element, so the successful allocations (if any) must
			 * be moved just before tbin->avail before bailing out.
			 */
			if (ptr == NULL) {
				if (i > 0) {
					memmove(empty_position - i,
						empty_position - nfill,
						i * sizeof(void *));
				}
				break;
			}
			/* Insert such that low regions get used first. */
			*(empty_position - nfill + i) = ptr;
		}
		if (config_fill && unlikely(opt_junk_alloc)) {
			for (unsigned j = 0; j < cnt; j++) {
				void* ptr = *(empty_position - nfill + i + j);
				arena_alloc_junk_small(ptr, &bin_infos[binind],
							true);
			}
		}
	}
	if (config_stats) {
		bin->stats.nmalloc += i;
		bin->stats.nrequests += tbin->tstats.nrequests;
		bin->stats.curregs += i;
		bin->stats.nfills++;
		tbin->tstats.nrequests = 0;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);
	cache_bin_ncached_set(tbin, binind, i);
	arena_decay_tick(tsdn, arena);
}

void
arena_alloc_junk_small(void *ptr, const bin_info_t *bin_info, bool zero) {
	if (!zero) {
		memset(ptr, JEMALLOC_ALLOC_JUNK, bin_info->reg_size);
	}
}

static void
arena_dalloc_junk_small_impl(void *ptr, const bin_info_t *bin_info) {
	memset(ptr, JEMALLOC_FREE_JUNK, bin_info->reg_size);
}
arena_dalloc_junk_small_t *JET_MUTABLE arena_dalloc_junk_small =
    arena_dalloc_junk_small_impl;

static void *
arena_malloc_small(tsdn_t *tsdn, arena_t *arena, szind_t binind, bool zero) {
	void *ret;
	bin_t *bin;
	size_t usize;
	extent_t *slab;

	assert(binind < SC_NBINS);
	usize = sz_index2size(binind);
	unsigned binshard;
	bin = arena_bin_choose_lock(tsdn, arena, binind, &binshard);

	if ((slab = bin->slabcur) != NULL && extent_nfree_get(slab) > 0) {
		ret = arena_slab_reg_alloc(slab, &bin_infos[binind]);
	} else {
		ret = arena_bin_malloc_hard(tsdn, arena, bin, binind, binshard);
	}

	if (ret == NULL) {
		malloc_mutex_unlock(tsdn, &bin->lock);
		return NULL;
	}

	if (config_stats) {
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
		bin->stats.curregs++;
	}

	malloc_mutex_unlock(tsdn, &bin->lock);

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk_alloc)) {
				arena_alloc_junk_small(ret,
				    &bin_infos[binind], false);
			} else if (unlikely(opt_zero)) {
				memset(ret, 0, usize);
			}
		}
	} else {
		if (config_fill && unlikely(opt_junk_alloc)) {
			arena_alloc_junk_small(ret, &bin_infos[binind],
			    true);
		}
		memset(ret, 0, usize);
	}

	arena_decay_tick(tsdn, arena);
	return ret;
}

void *
arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero) {
	assert(!tsdn_null(tsdn) || arena != NULL);

	if (likely(!tsdn_null(tsdn))) {
		arena = arena_choose_maybe_huge(tsdn_tsd(tsdn), arena, size);
	}
	if (unlikely(arena == NULL)) {
		return NULL;
	}

	if (likely(size <= SC_SMALL_MAXCLASS)) {
		return arena_malloc_small(tsdn, arena, ind, zero);
	}
	return large_malloc(tsdn, arena, sz_index2size(ind), zero);
}

void *
arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache) {
	void *ret;

	if (usize <= SC_SMALL_MAXCLASS
	    && (alignment < PAGE
	    || (alignment == PAGE && (usize & PAGE_MASK) == 0))) {
		/* Small; alignment doesn't require special slab placement. */
		ret = arena_malloc(tsdn, arena, usize, sz_size2index(usize),
		    zero, tcache, true);
	} else {
		if (likely(alignment <= CACHELINE)) {
			ret = large_malloc(tsdn, arena, usize, zero);
		} else {
			ret = large_palloc(tsdn, arena, usize, alignment, zero);
		}
	}
	return ret;
}

void
arena_prof_promote(tsdn_t *tsdn, void *ptr, size_t usize) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(isalloc(tsdn, ptr) == SC_LARGE_MINCLASS);
	assert(usize <= SC_SMALL_MAXCLASS);

	if (config_opt_safety_checks) {
		safety_check_set_redzone(ptr, usize, SC_LARGE_MINCLASS);
	}

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	extent_t *extent = rtree_extent_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true);

	szind_t szind = sz_size2index(usize);
	extent_szind_set(extent, szind);
	rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx, (uintptr_t)ptr,
	    szind, false);

	prof_idump_rollback(tsdn, usize);

	assert(isalloc(tsdn, ptr) == usize);
}

static size_t
arena_prof_demote(tsdn_t *tsdn, extent_t *extent, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	extent_szind_set(extent, SC_NBINS);
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx, (uintptr_t)ptr,
	    SC_NBINS, false);

	assert(isalloc(tsdn, ptr) == SC_LARGE_MINCLASS);

	return SC_LARGE_MINCLASS;
}

void
arena_dalloc_promoted(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    bool slow_path) {
	cassert(config_prof);
	assert(opt_prof);

	extent_t *extent = iealloc(tsdn, ptr);
	size_t usize = extent_usize_get(extent);
	size_t bumped_usize = arena_prof_demote(tsdn, extent, ptr);
	if (config_opt_safety_checks && usize < SC_LARGE_MINCLASS) {
		/*
		 * Currently, we only do redzoning for small sampled
		 * allocations.
		 */
		assert(bumped_usize == SC_LARGE_MINCLASS);
		safety_check_verify_redzone(ptr, usize, bumped_usize);
	}
	if (bumped_usize <= tcache_maxclass && tcache != NULL) {
		tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr,
		    sz_size2index(bumped_usize), slow_path);
	} else {
		large_dalloc(tsdn, extent);
	}
}

static void
arena_dissociate_bin_slab(arena_t *arena, extent_t *slab, bin_t *bin) {
	/* Dissociate slab from bin. */
	if (slab == bin->slabcur) {
		bin->slabcur = NULL;
	} else {
		szind_t binind = extent_szind_get(slab);
		const bin_info_t *bin_info = &bin_infos[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1) {
			arena_bin_slabs_full_remove(arena, bin, slab);
		} else {
			arena_bin_slabs_nonfull_remove(bin, slab);
		}
	}
}

static void
arena_dalloc_bin_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    bin_t *bin) {
	assert(slab != bin->slabcur);

	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	arena_slab_dalloc(tsdn, arena, slab);
	/****************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats) {
		bin->stats.curslabs--;
	}
}

static void
arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    bin_t *bin) {
	assert(extent_nfree_get(slab) > 0);

	/*
	 * Make sure that if bin->slabcur is non-NULL, it refers to the
	 * oldest/lowest non-full slab.  It is okay to NULL slabcur out rather
	 * than proactively keeping it pointing at the oldest/lowest non-full
	 * slab.
	 */
	if (bin->slabcur != NULL && extent_snad_comp(bin->slabcur, slab) > 0) {
		/* Switch slabcur. */
		if (extent_nfree_get(bin->slabcur) > 0) {
			arena_bin_slabs_nonfull_insert(bin, bin->slabcur);
		} else {
			arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
		}
		bin->slabcur = slab;
		if (config_stats) {
			bin->stats.reslabs++;
		}
	} else {
		arena_bin_slabs_nonfull_insert(bin, slab);
	}
}

static void
arena_dalloc_bin_locked_impl(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, extent_t *slab, void *ptr, bool junked) {
	slab_data_t *slab_data = extent_slab_data_get(slab);
	const bin_info_t *bin_info = &bin_infos[binind];

	if (!junked && config_fill && unlikely(opt_junk_free)) {
		arena_dalloc_junk_small(ptr, bin_info);
	}

	arena_slab_reg_dalloc(slab, slab_data, ptr);
	unsigned nfree = extent_nfree_get(slab);
	if (nfree == bin_info->nregs) {
		arena_dissociate_bin_slab(arena, slab, bin);
		arena_dalloc_bin_slab(tsdn, arena, slab, bin);
	} else if (nfree == 1 && slab != bin->slabcur) {
		arena_bin_slabs_full_remove(arena, bin, slab);
		arena_bin_lower_slab(tsdn, arena, slab, bin);
	}

	if (config_stats) {
		bin->stats.ndalloc++;
		bin->stats.curregs--;
	}
}

void
arena_dalloc_bin_junked_locked(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, extent_t *extent, void *ptr) {
	arena_dalloc_bin_locked_impl(tsdn, arena, bin, binind, extent, ptr,
	    true);
}

static void
arena_dalloc_bin(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr) {
	szind_t binind = extent_szind_get(extent);
	unsigned binshard = extent_binshard_get(extent);
	bin_t *bin = &arena->bins[binind].bin_shards[binshard];

	malloc_mutex_lock(tsdn, &bin->lock);
	arena_dalloc_bin_locked_impl(tsdn, arena, bin, binind, extent, ptr,
	    false);
	malloc_mutex_unlock(tsdn, &bin->lock);
}

void
arena_dalloc_small(tsdn_t *tsdn, void *ptr) {
	extent_t *extent = iealloc(tsdn, ptr);
	arena_t *arena = arena_get_from_extent(extent);

	arena_dalloc_bin(tsdn, arena, extent, ptr);
	arena_decay_tick(tsdn, arena);
}

bool
arena_ralloc_no_move(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero, size_t *newsize) {
	bool ret;
	/* Calls with non-zero extra had to clamp extra. */
	assert(extra == 0 || size + extra <= SC_LARGE_MAXCLASS);

	extent_t *extent = iealloc(tsdn, ptr);
	if (unlikely(size > SC_LARGE_MAXCLASS)) {
		ret = true;
		goto done;
	}

	size_t usize_min = sz_s2u(size);
	size_t usize_max = sz_s2u(size + extra);
	if (likely(oldsize <= SC_SMALL_MAXCLASS && usize_min
	    <= SC_SMALL_MAXCLASS)) {
		/*
		 * Avoid moving the allocation if the size class can be left the
		 * same.
		 */
		assert(bin_infos[sz_size2index(oldsize)].reg_size ==
		    oldsize);
		if ((usize_max > SC_SMALL_MAXCLASS
		    || sz_size2index(usize_max) != sz_size2index(oldsize))
		    && (size > oldsize || usize_max < oldsize)) {
			ret = true;
			goto done;
		}

		arena_t *arena = arena_get_from_extent(extent);
		arena_decay_tick(tsdn, arena);
		ret = false;
	} else if (oldsize >= SC_LARGE_MINCLASS
	    && usize_max >= SC_LARGE_MINCLASS) {
		ret = large_ralloc_no_move(tsdn, extent, usize_min, usize_max,
		    zero);
	} else {
		ret = true;
	}
done:
	assert(extent == iealloc(tsdn, ptr));
	*newsize = extent_usize_get(extent);

	return ret;
}

static void *
arena_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache) {
	if (alignment == 0) {
		return arena_malloc(tsdn, arena, usize, sz_size2index(usize),
		    zero, tcache, true);
	}
	usize = sz_sa2u(usize, alignment);
	if (unlikely(usize == 0 || usize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}
	return ipalloct(tsdn, usize, alignment, zero, tcache, arena);
}

void *
arena_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args) {
	size_t usize = sz_s2u(size);
	if (unlikely(usize == 0 || size > SC_LARGE_MAXCLASS)) {
		return NULL;
	}

	if (likely(usize <= SC_SMALL_MAXCLASS)) {
		/* Try to avoid moving the allocation. */
		UNUSED size_t newsize;
		if (!arena_ralloc_no_move(tsdn, ptr, oldsize, usize, 0, zero,
		    &newsize)) {
			hook_invoke_expand(hook_args->is_realloc
			    ? hook_expand_realloc : hook_expand_rallocx,
			    ptr, oldsize, usize, (uintptr_t)ptr,
			    hook_args->args);
			return ptr;
		}
	}

	if (oldsize >= SC_LARGE_MINCLASS
	    && usize >= SC_LARGE_MINCLASS) {
		return large_ralloc(tsdn, arena, ptr, usize,
		    alignment, zero, tcache, hook_args);
	}

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and copying.
	 */
	void *ret = arena_ralloc_move_helper(tsdn, arena, usize, alignment,
	    zero, tcache);
	if (ret == NULL) {
		return NULL;
	}

	hook_invoke_alloc(hook_args->is_realloc
	    ? hook_alloc_realloc : hook_alloc_rallocx, ret, (uintptr_t)ret,
	    hook_args->args);
	hook_invoke_dalloc(hook_args->is_realloc
	    ? hook_dalloc_realloc : hook_dalloc_rallocx, ptr, hook_args->args);

	/*
	 * Junk/zero-filling were already done by
	 * ipalloc()/arena_malloc().
	 */
	size_t copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isdalloct(tsdn, ptr, oldsize, tcache, NULL, true);
	return ret;
}

dss_prec_t
arena_dss_prec_get(arena_t *arena) {
	return (dss_prec_t)atomic_load_u(&arena->dss_prec, ATOMIC_ACQUIRE);
}

bool
arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec) {
	if (!have_dss) {
		return (dss_prec != dss_prec_disabled);
	}
	atomic_store_u(&arena->dss_prec, (unsigned)dss_prec, ATOMIC_RELEASE);
	return false;
}

ssize_t
arena_dirty_decay_ms_default_get(void) {
	return atomic_load_zd(&dirty_decay_ms_default, ATOMIC_RELAXED);
}

bool
arena_dirty_decay_ms_default_set(ssize_t decay_ms) {
	if (!arena_decay_ms_valid(decay_ms)) {
		return true;
	}
	atomic_store_zd(&dirty_decay_ms_default, decay_ms, ATOMIC_RELAXED);
	return false;
}

ssize_t
arena_muzzy_decay_ms_default_get(void) {
	return atomic_load_zd(&muzzy_decay_ms_default, ATOMIC_RELAXED);
}

bool
arena_muzzy_decay_ms_default_set(ssize_t decay_ms) {
	if (!arena_decay_ms_valid(decay_ms)) {
		return true;
	}
	atomic_store_zd(&muzzy_decay_ms_default, decay_ms, ATOMIC_RELAXED);
	return false;
}

bool
arena_retain_grow_limit_get_set(tsd_t *tsd, arena_t *arena, size_t *old_limit,
    size_t *new_limit) {
	assert(opt_retain);

	pszind_t new_ind JEMALLOC_CC_SILENCE_INIT(0);
	if (new_limit != NULL) {
		size_t limit = *new_limit;
		/* Grow no more than the new limit. */
		if ((new_ind = sz_psz2ind(limit + 1) - 1) >= SC_NPSIZES) {
			return true;
		}
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &arena->extent_grow_mtx);
	if (old_limit != NULL) {
		*old_limit = sz_pind2sz(arena->retain_grow_limit);
	}
	if (new_limit != NULL) {
		arena->retain_grow_limit = new_ind;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->extent_grow_mtx);

	return false;
}

unsigned
arena_nthreads_get(arena_t *arena, bool internal) {
	return atomic_load_u(&arena->nthreads[internal], ATOMIC_RELAXED);
}

void
arena_nthreads_inc(arena_t *arena, bool internal) {
	atomic_fetch_add_u(&arena->nthreads[internal], 1, ATOMIC_RELAXED);
}

void
arena_nthreads_dec(arena_t *arena, bool internal) {
	atomic_fetch_sub_u(&arena->nthreads[internal], 1, ATOMIC_RELAXED);
}

size_t
arena_extent_sn_next(arena_t *arena) {
	return atomic_fetch_add_zu(&arena->extent_sn_next, 1, ATOMIC_RELAXED);
}

arena_t *
arena_new(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;
	base_t *base;
	unsigned i;

	if (ind == 0) {
		base = b0get();
	} else {
		base = base_new(tsdn, ind, extent_hooks);
		if (base == NULL) {
			return NULL;
		}
	}

	unsigned nbins_total = 0;
	for (i = 0; i < SC_NBINS; i++) {
		nbins_total += bin_infos[i].n_shards;
	}
	size_t arena_size = sizeof(arena_t) + sizeof(bin_t) * nbins_total;
	arena = (arena_t *)base_alloc(tsdn, base, arena_size, CACHELINE);
	if (arena == NULL) {
		goto label_error;
	}

	atomic_store_u(&arena->nthreads[0], 0, ATOMIC_RELAXED);
	atomic_store_u(&arena->nthreads[1], 0, ATOMIC_RELAXED);
	arena->last_thd = NULL;

	if (config_stats) {
		if (arena_stats_init(tsdn, &arena->stats)) {
			goto label_error;
		}

		ql_new(&arena->tcache_ql);
		ql_new(&arena->cache_bin_array_descriptor_ql);
		if (malloc_mutex_init(&arena->tcache_ql_mtx, "tcache_ql",
		    WITNESS_RANK_TCACHE_QL, malloc_mutex_rank_exclusive)) {
			goto label_error;
		}
	}

	if (config_prof) {
		if (prof_accum_init(tsdn)) {
			goto label_error;
		}
	}

	atomic_store_zu(&arena->extent_sn_next, 0, ATOMIC_RELAXED);

	atomic_store_u(&arena->dss_prec, (unsigned)extent_dss_prec_get(),
	    ATOMIC_RELAXED);

	atomic_store_zu(&arena->nactive, 0, ATOMIC_RELAXED);

	extent_list_init(&arena->large);
	if (malloc_mutex_init(&arena->large_mtx, "arena_large",
	    WITNESS_RANK_ARENA_LARGE, malloc_mutex_rank_exclusive)) {
		goto label_error;
	}

	/*
	 * Delay coalescing for dirty extents despite the disruptive effect on
	 * memory layout for best-fit extent allocation, since cached extents
	 * are likely to be reused soon after deallocation, and the cost of
	 * merging/splitting extents is non-trivial.
	 */
	if (eset_init(tsdn, &arena->eset_dirty, extent_state_dirty, true)) {
		goto label_error;
	}
	/*
	 * Coalesce muzzy extents immediately, because operations on them are in
	 * the critical path much less often than for dirty extents.
	 */
	if (eset_init(tsdn, &arena->eset_muzzy, extent_state_muzzy, false)) {
		goto label_error;
	}
	/*
	 * Coalesce retained extents immediately, in part because they will
	 * never be evicted (and therefore there's no opportunity for delayed
	 * coalescing), but also because operations on retained extents are not
	 * in the critical path.
	 */
	if (eset_init(tsdn, &arena->eset_retained, extent_state_retained,
	    false)) {
		goto label_error;
	}

	if (arena_decay_init(&arena->decay_dirty,
	    arena_dirty_decay_ms_default_get(), &arena->stats.decay_dirty)) {
		goto label_error;
	}
	if (arena_decay_init(&arena->decay_muzzy,
	    arena_muzzy_decay_ms_default_get(), &arena->stats.decay_muzzy)) {
		goto label_error;
	}

	arena->extent_grow_next = sz_psz2ind(HUGEPAGE);
	arena->retain_grow_limit = sz_psz2ind(SC_LARGE_MAXCLASS);
	if (malloc_mutex_init(&arena->extent_grow_mtx, "extent_grow",
	    WITNESS_RANK_EXTENT_GROW, malloc_mutex_rank_exclusive)) {
		goto label_error;
	}

	extent_avail_new(&arena->extent_avail);
	if (malloc_mutex_init(&arena->extent_avail_mtx, "extent_avail",
	    WITNESS_RANK_EXTENT_AVAIL, malloc_mutex_rank_exclusive)) {
		goto label_error;
	}

	/* Initialize bins. */
	uintptr_t bin_addr = (uintptr_t)arena + sizeof(arena_t);
	atomic_store_u(&arena->binshard_next, 0, ATOMIC_RELEASE);
	for (i = 0; i < SC_NBINS; i++) {
		unsigned nshards = bin_infos[i].n_shards;
		arena->bins[i].bin_shards = (bin_t *)bin_addr;
		bin_addr += nshards * sizeof(bin_t);
		for (unsigned j = 0; j < nshards; j++) {
			bool err = bin_init(&arena->bins[i].bin_shards[j]);
			if (err) {
				goto label_error;
			}
		}
	}
	assert(bin_addr == (uintptr_t)arena + arena_size);

	arena->base = base;
	/* Set arena before creating background threads. */
	arena_set(ind, arena);

	nstime_init(&arena->create_time, 0);
	nstime_update(&arena->create_time);

	/* We don't support reentrancy for arena 0 bootstrapping. */
	if (ind != 0) {
		/*
		 * If we're here, then arena 0 already exists, so bootstrapping
		 * is done enough that we should have tsd.
		 */
		assert(!tsdn_null(tsdn));
		pre_reentrancy(tsdn_tsd(tsdn), arena);
		if (test_hooks_arena_new_hook) {
			test_hooks_arena_new_hook();
		}
		post_reentrancy(tsdn_tsd(tsdn));
	}

	return arena;
label_error:
	if (ind != 0) {
		base_delete(tsdn, base);
	}
	return NULL;
}

arena_t *
arena_choose_huge(tsd_t *tsd) {
	/* huge_arena_ind can be 0 during init (will use a0). */
	if (huge_arena_ind == 0) {
		assert(!malloc_initialized());
	}

	arena_t *huge_arena = arena_get(tsd_tsdn(tsd), huge_arena_ind, false);
	if (huge_arena == NULL) {
		/* Create the huge arena on demand. */
		assert(huge_arena_ind != 0);
		huge_arena = arena_get(tsd_tsdn(tsd), huge_arena_ind, true);
		if (huge_arena == NULL) {
			return NULL;
		}
		/*
		 * Purge eagerly for huge allocations, because: 1) number of
		 * huge allocations is usually small, which means ticker based
		 * decay is not reliable; and 2) less immediate reuse is
		 * expected for huge allocations.
		 */
		if (arena_dirty_decay_ms_default_get() > 0) {
			arena_dirty_decay_ms_set(tsd_tsdn(tsd), huge_arena, 0);
		}
		if (arena_muzzy_decay_ms_default_get() > 0) {
			arena_muzzy_decay_ms_set(tsd_tsdn(tsd), huge_arena, 0);
		}
	}

	return huge_arena;
}

bool
arena_init_huge(void) {
	bool huge_enabled;

	/* The threshold should be large size class. */
	if (opt_oversize_threshold > SC_LARGE_MAXCLASS ||
	    opt_oversize_threshold < SC_LARGE_MINCLASS) {
		opt_oversize_threshold = 0;
		oversize_threshold = SC_LARGE_MAXCLASS + PAGE;
		huge_enabled = false;
	} else {
		/* Reserve the index for the huge arena. */
		huge_arena_ind = narenas_total_get();
		oversize_threshold = opt_oversize_threshold;
		huge_enabled = true;
	}

	return huge_enabled;
}

bool
arena_is_huge(unsigned arena_ind) {
	if (huge_arena_ind == 0) {
		return false;
	}
	return (arena_ind == huge_arena_ind);
}

void
arena_boot(sc_data_t *sc_data) {
	arena_dirty_decay_ms_default_set(opt_dirty_decay_ms);
	arena_muzzy_decay_ms_default_set(opt_muzzy_decay_ms);
	for (unsigned i = 0; i < SC_NBINS; i++) {
		sc_t *sc = &sc_data->sc[i];
		div_init(&arena_binind_div_info[i],
		    (1U << sc->lg_base) + (sc->ndelta << sc->lg_delta));
	}
}

void
arena_prefork0(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_prefork(tsdn, &arena->decay_muzzy.mtx);
}

void
arena_prefork1(tsdn_t *tsdn, arena_t *arena) {
	if (config_stats) {
		malloc_mutex_prefork(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_prefork2(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->extent_grow_mtx);
}

void
arena_prefork3(tsdn_t *tsdn, arena_t *arena) {
	eset_prefork(tsdn, &arena->eset_dirty);
	eset_prefork(tsdn, &arena->eset_muzzy);
	eset_prefork(tsdn, &arena->eset_retained);
}

void
arena_prefork4(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->extent_avail_mtx);
}

void
arena_prefork5(tsdn_t *tsdn, arena_t *arena) {
	base_prefork(tsdn, arena->base);
}

void
arena_prefork6(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->large_mtx);
}

void
arena_prefork7(tsdn_t *tsdn, arena_t *arena) {
	for (unsigned i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_prefork(tsdn, &arena->bins[i].bin_shards[j]);
		}
	}
}

void
arena_postfork_parent(tsdn_t *tsdn, arena_t *arena) {
	unsigned i;

	for (i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_postfork_parent(tsdn,
			    &arena->bins[i].bin_shards[j]);
		}
	}
	malloc_mutex_postfork_parent(tsdn, &arena->large_mtx);
	base_postfork_parent(tsdn, arena->base);
	malloc_mutex_postfork_parent(tsdn, &arena->extent_avail_mtx);
	eset_postfork_parent(tsdn, &arena->eset_dirty);
	eset_postfork_parent(tsdn, &arena->eset_muzzy);
	eset_postfork_parent(tsdn, &arena->eset_retained);
	malloc_mutex_postfork_parent(tsdn, &arena->extent_grow_mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->decay_muzzy.mtx);
	if (config_stats) {
		malloc_mutex_postfork_parent(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_postfork_child(tsdn_t *tsdn, arena_t *arena) {
	unsigned i;

	atomic_store_u(&arena->nthreads[0], 0, ATOMIC_RELAXED);
	atomic_store_u(&arena->nthreads[1], 0, ATOMIC_RELAXED);
	if (tsd_arena_get(tsdn_tsd(tsdn)) == arena) {
		arena_nthreads_inc(arena, false);
	}
	if (tsd_iarena_get(tsdn_tsd(tsdn)) == arena) {
		arena_nthreads_inc(arena, true);
	}
	if (config_stats) {
		ql_new(&arena->tcache_ql);
		ql_new(&arena->cache_bin_array_descriptor_ql);
		tcache_t *tcache = tcache_get(tsdn_tsd(tsdn));
		if (tcache != NULL && tcache->arena == arena) {
			ql_elm_new(tcache, link);
			ql_tail_insert(&arena->tcache_ql, tcache, link);
			cache_bin_array_descriptor_init(
			    &tcache->cache_bin_array_descriptor,
			    tcache->bins_small, tcache->bins_large);
			ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
			    &tcache->cache_bin_array_descriptor, link);
		}
	}

	for (i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_postfork_child(tsdn, &arena->bins[i].bin_shards[j]);
		}
	}
	malloc_mutex_postfork_child(tsdn, &arena->large_mtx);
	base_postfork_child(tsdn, arena->base);
	malloc_mutex_postfork_child(tsdn, &arena->extent_avail_mtx);
	eset_postfork_child(tsdn, &arena->eset_dirty);
	eset_postfork_child(tsdn, &arena->eset_muzzy);
	eset_postfork_child(tsdn, &arena->eset_retained);
	malloc_mutex_postfork_child(tsdn, &arena->extent_grow_mtx);
	malloc_mutex_postfork_child(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_postfork_child(tsdn, &arena->decay_muzzy.mtx);
	if (config_stats) {
		malloc_mutex_postfork_child(tsdn, &arena->tcache_ql_mtx);
	}
}
