// SPDX-License-Identifier: MIT
//
// Vendored from pyvista-algorithms (https://github.com/banesullivan/pyvista-algorithms)
//   src/cpp/linear_probe_table.h  — MIT licensed (Bane Sullivan and contributors).
//
// Support header (open-addressing probe table) for the vendored point-dedup kernel pvaClean.h.
// Unmodified upstream source below this banner (local #includes renamed to the
// pva* vendored filenames).
//
// Linear-probe open-addressing atomic hash table for parallel vertex
// deduplication, adapted from pyvista-stl's mt_vertex / alloc_atomic_table
// pattern (see pyvista-stl/src/stlfile.cpp and hash96.h).
//
// Each slot is a single std::atomic<uint32_t> holding one of:
//   * EMPTY    (0xFFFFFFFF) — slot unused.
//   * RESERVED (0xFFFFFFFE) — slot claimed by a thread mid-insert.
//   * else                  — entry id (0..2^32-3) in side arrays.
//
// Side arrays are caller-owned: keys (96 bits per entry), rep_id (i32),
// etc. The table only stores the entry id; equality and atomic-min are
// performed by the caller against the side arrays.
//
// Allocation: mmap-backed (anonymous, MAP_POPULATE on Linux,
// MADV_HUGEPAGE hint) when the byte size is large enough to benefit;
// falls back to operator new[] on small tables and on non-mmap
// platforms.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#define PVU_LPT_HAVE_MMAP 1
#else
#define PVU_LPT_HAVE_MMAP 0
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define PVU_LPT_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define PVU_LPT_PAUSE() ((void)0)
#else
#define PVU_LPT_PAUSE() ((void)0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PVU_LPT_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 0)
#define PVU_LPT_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 0)
#else
#define PVU_LPT_PREFETCH_R(addr) ((void)0)
#define PVU_LPT_PREFETCH_W(addr) ((void)0)
#endif

// cvista portability edit: the upstream source uses GCC/Clang __atomic_* builtins
// for the int32 atomic-min below; MSVC lacks them, so route through the
// _Interlocked* intrinsic on MSVC (declared in <intrin.h>).
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace pvu {
namespace lpt {

constexpr std::uint32_t SLOT_EMPTY = 0xFFFFFFFFu;
constexpr std::uint32_t SLOT_RESERVED = 0xFFFFFFFEu;
constexpr std::uint32_t MAX_ENTRY_ID = 0xFFFFFFFDu; // exclusive of RESERVED/EMPTY

// Round v up to the next power of two (>= v, >= 16).
inline std::size_t next_pow2(std::size_t v) {
    if (v < 16)
        return 16;
    std::size_t p = 16;
    while (p < v)
        p <<= 1;
    return p;
}

// Allocate `n` zero-initialised std::atomic<uint32_t> slots set to
// SLOT_EMPTY. Returns the table pointer and fills `*backing` /
// `*backing_len` so free_table can release the mapping.
//
// IMPORTANT: SLOT_EMPTY == 0xFFFFFFFF, so we want every byte == 0xFF,
// not 0x00. mmap returns zeroed memory; for the mmap path we follow
// up with a parallel memset(0xFF). For the heap fallback we use
// std::memset directly on the raw byte buffer before constructing
// atomics in-place; std::atomic<uint32_t> is guaranteed to be
// trivially constructible from any uint32 byte pattern on the
// platforms this targets.
inline std::atomic<std::uint32_t> *alloc_table(std::size_t n, void *&backing,
                                               std::size_t &backing_len) {
    backing = nullptr;
    backing_len = 0;
    const std::size_t bytes = n * sizeof(std::atomic<std::uint32_t>);
#if PVU_LPT_HAVE_MMAP
    // Use mmap for tables >= 64 KiB so we get THP / huge pages and
    // avoid touching pages via the C++ allocator's heap bookkeeping.
    if (bytes >= (std::size_t)65536) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_POPULATE
        flags |= MAP_POPULATE;
#endif
        void *p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
            ::madvise(p, bytes, MADV_HUGEPAGE);
#endif
            backing = p;
            backing_len = bytes;
            // Fill with 0xFF (SLOT_EMPTY). memset is well-optimised
            // and the OS already populated the pages.
            std::memset(p, 0xFF, bytes);
            return reinterpret_cast<std::atomic<std::uint32_t> *>(p);
        }
    }
#endif
    // Heap fallback.
    auto *p = new (std::nothrow) std::atomic<std::uint32_t>[n];
    if (!p)
        return nullptr;
    // Initialise each slot to SLOT_EMPTY. Relaxed stores are fine —
    // the table is not yet visible to other threads.
    for (std::size_t i = 0; i < n; ++i) {
        p[i].store(SLOT_EMPTY, std::memory_order_relaxed);
    }
    return p;
}

inline void free_table(std::atomic<std::uint32_t> *p, void *backing, std::size_t backing_len) {
#if PVU_LPT_HAVE_MMAP
    if (backing) {
        ::munmap(backing, backing_len);
        return;
    }
#else
    (void)backing;
    (void)backing_len;
#endif
    delete[] p;
}

// Atomic min on int32 via CAS loop. Sets *p = min(*p, v).
inline void atomic_int32_min(std::int32_t *p, std::int32_t v) {
#if defined(_MSC_VER)
    // MSVC: int32 and long are both 32-bit; _InterlockedCompareExchange returns
    // the prior value, giving a lock-free CAS loop equivalent to the builtin.
    volatile long *lp = reinterpret_cast<volatile long *>(p);
    long old = *lp;
    while (v < old) {
        long prev = _InterlockedCompareExchange(lp, static_cast<long>(v), old);
        if (prev == old) {
            return;
        }
        old = prev;
    }
#else
    std::int32_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v < old) {
        if (__atomic_compare_exchange_n(p, &old, v, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return;
        }
    }
#endif
}

} // namespace lpt
} // namespace pvu
