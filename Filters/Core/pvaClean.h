// SPDX-License-Identifier: MIT
//
// Vendored from pyvista-algorithms (https://github.com/banesullivan/pyvista-algorithms)
//   src/cpp/clean.hpp  — MIT licensed (Bane Sullivan and contributors).
//
// Fast parallel point-dedup / coincident-point merge: a re-implementation of vtkStaticCleanUnstructuredGrid's merge-points + connectivity-rewrite. cvista calls pvu::clean::run_clean() directly (no nanobind) from the vtkStaticCleanUnstructuredGrid fast path when opt-in fast mode is enabled (cvista.EnableFast() / CVISTA_FAST). Output is points-order-relaxed (merged points + cells are renumbered in a thread/hash-dependent order).
// Unmodified upstream source below this banner (local #includes renamed to the
// pva* vendored filenames).
//
// Mesh clean: deduplicate coincident points and drop degenerate cells.
//
// ---------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------
//
// Replaces vtkStaticCleanPolyData / vtkStaticCleanUnstructuredGrid with
// an OpenMP C++ kernel templated on point dtype. The algorithm follows
// the eight-phase pipeline described in docs/ARCHITECTURE.md Section 4
// with the locked revisions in Sections 11-12.
//
// Phases:
//   A. bbox sweep (parallel reduction) -> derive eps
//   B. per-point key construction (parallel for)
//   C. parallel stable sort by (key, original_id)
//   D. segmented unique with explicit min-original-id reduction
//   E. tolerance-mode union-find merge across 27-cell Morton stencil
//   F. cell rewrite with per-VTK-type degenerate rules
//   G. unused-point removal via uint32 touched-mask
//   H. attribute gather (done in Python wrapper)
//
// Determinism:
//   * Identical-merge (tolerance==0): bit-identical output across
//     thread counts. Stable sort + lex tie-break + full bit-pattern
//     re-verification inside hash segments (M3).
//   * Tolerance mode: deterministic up to canonical representative —
//     the min(original_id) in each equivalence class is computed by an
//     explicit reduction (NOT relying on radix stability).
//
// Parallelism:
//   * OpenMP parallel-for / parallel-reduce throughout.
//   * Cell rewrite uses schedule(dynamic, 1024) per S-PerfHawk-6.
//   * Tolerance-mode union-find is the textbook serial form inside a
//     parallel candidate-pair gather (S-Pragmatic-5).
//
// Dtype / id-size assumptions:
//   * Point dtype: float32 or float64 (templated T).
//   * Connectivity / point ids: int32 throughout.
//   * Cell types: uint8 (VTK ids).
//   * Total point count must fit in int32 (caller-enforced).
//
// Memory peak: <= 8 * input-point-bytes + 2 * connectivity-bytes (M8).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <omp.h>

#include "pvaHash96.h"
#include "pvaLinearProbeTable.h"
#include "pvaParallelRadixSort.h"

namespace pvu {
namespace clean {

// VTK cell-type constants used in the per-type dispatch.
enum CleanCellType : int {
    VTK_VERTEX = 1,
    VTK_POLY_VERTEX = 2,
    VTK_LINE = 3,
    VTK_POLY_LINE = 4,
    VTK_TRIANGLE = 5,
    VTK_TRIANGLE_STRIP = 6,
    VTK_POLYGON = 7,
    VTK_QUAD = 9,
    VTK_TETRA = 10,
    VTK_HEXAHEDRON = 12,
    VTK_WEDGE = 13,
    VTK_PYRAMID = 14,
    VTK_QUADRATIC_TETRA = 24,
    VTK_QUADRATIC_HEXAHEDRON = 25,
    VTK_QUADRATIC_WEDGE = 26,
    VTK_QUADRATIC_PYRAMID = 27,
    VTK_POLYHEDRON = 42,
};

struct CleanOptions {
    double tolerance = 0.0;
    bool absolute = false;
    bool remove_unused_points = true;
    bool remove_degenerate_cells = true;
    bool drop_nonfinite_points = false;
};

template <typename T, typename IndexT = std::int32_t>
struct CleanInput {
    const T *points;
    std::size_t n_pts;
    // Connectivity / offsets are read in their native dtype (int32 or
    // int64). Values are always point indices and fit in int32 (the
    // dedup pipeline caps n_pts at 2^32-2); the IndexT path simply
    // reads the wider type without an upfront cast scratch buffer.
    const IndexT *conn;
    const IndexT *offsets;
    const std::uint8_t *cell_types;
    std::int32_t n_cells;
};

template <typename T>
struct CleanOutput {
    // Points after dedup + unused removal. Length 3 * n_unique.
    std::vector<T> points;
    // old -> new mapping. Length n_in. SENTINEL (-1) if dropped.
    std::vector<std::int32_t> point_map;
    // new -> first-old (canonical original id). Length n_unique.
    std::vector<std::int32_t> source_map;
    // Cell arrays after rewrite + drop.
    std::vector<std::int32_t> conn;
    std::vector<std::int32_t> offsets;
    std::vector<std::uint8_t> cell_types;
    // For each kept output cell, the input cell id it came from.
    // Non-decreasing (per M7); triangle-strip explosion produces
    // duplicate entries with the same source id.
    std::vector<std::int32_t> original_cell_ids;
    // True if any VTK_POLYHEDRON cell was seen; wrapper should fall
    // back to VTK clean per M4.
    bool needs_polyhedron_fallback = false;
    // True if any input point had a non-finite coordinate (NaN/Inf).
    // Reported regardless of ``drop_nonfinite_points``; the wrapper
    // raises ``ValueError`` when the option is false. Saves a separate
    // ``np.isfinite`` pass on the Python side — detection is folded
    // into the existing dedup non-finite check.
    bool has_nonfinite_input = false;
};

namespace detail {

constexpr std::int32_t SENTINEL_ID = -1;

// Helper: resize a std::vector<T> for trivially-constructible T to ``n``
// elements WITHOUT incurring the value-init / memset cost. We allocate
// fresh storage (uninitialised) via operator new[], then steal it into
// the vector by assigning element-by-element in parallel (overlapping
// the first-touch page faults with useful work).
//
// Usage pattern: call ``resize_noinit(v, n)`` immediately before a
// parallel loop that writes every element. Any unwritten positions hold
// indeterminate values.
//
// Implementation: vector::resize() of trivial T value-initialises (= zero
// fill) — that's the 80+ ms cost for a 240 MB points_out on the 20 M-tri
// workload. We replace it with a single bulk-touch parallel loop that
// writes one byte per page (forces fault, no zero-fill) and then call
// resize() which sees the page already present. For correctness with
// strict vector semantics we just reserve+resize; the OS-zeroed pages
// are still zeroed but the resize doesn't need to memset them again
// because the OS already did. Concretely: ``reserve(n)`` allocates
// uninitialised storage; subsequent ``resize(n)`` does the value-init.
// The trick is to swap the order: parallel-touch the reserve'd memory
// (forcing zero pages to be COW-mapped in parallel) BEFORE the resize.
template <typename T>
inline void resize_noinit_parallel(std::vector<T> &v, std::size_t n) {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    v.clear();
    v.reserve(n);
    T *p = v.data();
    constexpr std::size_t PAGE = 4096;
    std::size_t nb = n * sizeof(T);
    // Parallel page-touch. The OS maps fresh pages on first write; doing
    // this in parallel saturates memory bandwidth on the zero-page COW.
    if (nb > 0) {
        volatile char *raw = reinterpret_cast<volatile char *>(p);
#pragma omp parallel for schedule(static)
        for (long long off = 0; off < (long long)nb; off += (long long)PAGE) {
            raw[off] = 0;
        }
    }
    // Now resize — value-init is still incurred but pages are present;
    // typically glibc memset on warm pages is much faster.
    //
    // Even faster: use placement-new-equivalent via the noinit-resize
    // GCC libstdc++ extension. We instead skip resize and bump the size
    // manually via assign-from-self. That's not portable.
    //
    // Pragmatic compromise: do the resize. The improvement we get is
    // from parallel page-fault, which is the dominant cost on first
    // allocation. The serial memset over already-resident pages is
    // bandwidth-bound but much faster than serial page-fault.
    v.resize(n);
}

// ----- lightweight tracing -----------------------------------------------
// Enable by setting PVU_CLEAN_TRACE=1 in the environment. Prints per-phase
// wall-clock to stderr. Zero overhead when disabled (single env-var lookup
// at the start of run_clean).
struct CleanTrace {
    bool enabled = false;
    std::chrono::steady_clock::time_point t0;
    void start() {
        const char *e = std::getenv("PVU_CLEAN_TRACE");
        enabled = (e && e[0] && e[0] != '0');
        if (enabled)
            t0 = std::chrono::steady_clock::now();
    }
    void mark(const char *label) {
        if (!enabled)
            return;
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr, "  [clean] %-22s %7.3f ms\n", label, ms);
        t0 = t1;
    }
};

// Resolve n_threads parameter. 0 -> library default (cap at 8 to match
// extract_surface). Cap at omp_get_max_threads() so we never request
// more than the runtime offers.
inline int resolve_threads(int n_threads) {
    int max_t = omp_get_max_threads();
    if (n_threads <= 0) {
        return std::min(max_t, 8);
    }
    return std::min(n_threads, max_t);
}

// --------- key packing ----------------------------------------------

// Canonicalise -0.0 to +0.0 so the bit pattern dedupes those points
// (M5 / Section 3 quantisation contract).
template <typename T>
inline T canonicalise_zero(T x) {
    return (x == T(0)) ? T(0) : x;
}

template <typename T>
inline bool is_finite(T x) {
    return std::isfinite(x);
}

// Pack three float32 bit-patterns into a 96-bit key (three u32 words).
inline void pack_f32_key(float x, float y, float z, std::uint32_t out[3]) {
    float cx = canonicalise_zero(x);
    float cy = canonicalise_zero(y);
    float cz = canonicalise_zero(z);
    std::memcpy(&out[0], &cx, sizeof(float));
    std::memcpy(&out[1], &cy, sizeof(float));
    std::memcpy(&out[2], &cz, sizeof(float));
}

// For float64 identical-merge: fold each double's hi32 ^ lo32 into a
// u32 word. We still keep the full doubles around in a side array for
// segmented-unique verification (M3 / S-Pragmatic-6).
inline void pack_f64_hash_key(double x, double y, double z, std::uint32_t out[3]) {
    double cx = canonicalise_zero(x);
    double cy = canonicalise_zero(y);
    double cz = canonicalise_zero(z);
    std::uint64_t bx, by, bz;
    std::memcpy(&bx, &cx, sizeof(double));
    std::memcpy(&by, &cy, sizeof(double));
    std::memcpy(&bz, &cz, sizeof(double));
    out[0] = (std::uint32_t)(bx ^ (bx >> 32));
    out[1] = (std::uint32_t)(by ^ (by >> 32));
    out[2] = (std::uint32_t)(bz ^ (bz >> 32));
}

// Quantise a double coord to int32 lattice cell index.
inline std::int32_t quantise_coord(double v, double origin, double inv_eps) {
    double q = std::floor((v - origin) * inv_eps);
    // Bbox-vs-tol check at Phase A keeps this in range for sane inputs.
    if (q > 2.0e9) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (q < -2.0e9) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return (std::int32_t)q;
}

// Lex compare of two 96-bit keys.
inline int cmp_key96(const std::uint32_t *a, const std::uint32_t *b) {
    if (a[0] != b[0])
        return a[0] < b[0] ? -1 : 1;
    if (a[1] != b[1])
        return a[1] < b[1] ? -1 : 1;
    if (a[2] != b[2])
        return a[2] < b[2] ? -1 : 1;
    return 0;
}

// --------- Phase A: bbox sweep --------------------------------------

template <typename T>
struct BBox {
    T bbmin[3];
    T bbmax[3];
};

template <typename T>
inline BBox<T> compute_bbox(const T *points, std::size_t n_pts) {
    BBox<T> bb;
    if (n_pts == 0) {
        for (int d = 0; d < 3; ++d) {
            bb.bbmin[d] = T(0);
            bb.bbmax[d] = T(0);
        }
        return bb;
    }
    T mn0 = points[0], mn1 = points[1], mn2 = points[2];
    T mx0 = mn0, mx1 = mn1, mx2 = mn2;
#pragma omp parallel
    {
        T lmn0 = mn0, lmn1 = mn1, lmn2 = mn2;
        T lmx0 = mx0, lmx1 = mx1, lmx2 = mx2;
#pragma omp for nowait
        for (long long i = 1; i < (long long)n_pts; ++i) {
            T x = points[i * 3 + 0];
            T y = points[i * 3 + 1];
            T z = points[i * 3 + 2];
            // Non-finite values are excluded from the bbox so they
            // don't blow up eps; they're handled by the policy in
            // Phase B.
            if (std::isfinite(x)) {
                if (x < lmn0)
                    lmn0 = x;
                if (x > lmx0)
                    lmx0 = x;
            }
            if (std::isfinite(y)) {
                if (y < lmn1)
                    lmn1 = y;
                if (y > lmx1)
                    lmx1 = y;
            }
            if (std::isfinite(z)) {
                if (z < lmn2)
                    lmn2 = z;
                if (z > lmx2)
                    lmx2 = z;
            }
        }
#pragma omp critical
        {
            if (lmn0 < mn0)
                mn0 = lmn0;
            if (lmn1 < mn1)
                mn1 = lmn1;
            if (lmn2 < mn2)
                mn2 = lmn2;
            if (lmx0 > mx0)
                mx0 = lmx0;
            if (lmx1 > mx1)
                mx1 = lmx1;
            if (lmx2 > mx2)
                mx2 = lmx2;
        }
    }
    bb.bbmin[0] = mn0;
    bb.bbmin[1] = mn1;
    bb.bbmin[2] = mn2;
    bb.bbmax[0] = mx0;
    bb.bbmax[1] = mx1;
    bb.bbmax[2] = mx2;
    return bb;
}

template <typename T>
inline double bbox_diagonal(const BBox<T> &bb) {
    double dx = (double)bb.bbmax[0] - (double)bb.bbmin[0];
    double dy = (double)bb.bbmax[1] - (double)bb.bbmin[1];
    double dz = (double)bb.bbmax[2] - (double)bb.bbmin[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// --------- Phase B/C/D: identical-merge dedup -----------------------
//
// Build (key, original_id) pairs, stable-sort, segment-unique. The
// std::stable_sort fallback is used here for v1 correctness; the
// parallel-radix-with-payload upgrade is a follow-up (TODO below).

struct KeyEntry {
    std::uint32_t k0, k1, k2;
    std::int32_t orig_id;
};

inline bool key_lt(const KeyEntry &a, const KeyEntry &b) {
    if (a.k0 != b.k0)
        return a.k0 < b.k0;
    if (a.k1 != b.k1)
        return a.k1 < b.k1;
    if (a.k2 != b.k2)
        return a.k2 < b.k2;
    return a.orig_id < b.orig_id;
}

// Build the key array. For float32 the 96-bit packed key IS the float
// triple (bit-exact). For float64 the key is a 96-bit hash; the
// segmented-unique pass must re-verify with full doubles before
// merging.
//
// TODO(perf): replace std::stable_sort with parallel_radix_sort over
// the packed u64 (key_lo) + u32 payload (orig_id). See
// docs/DEFERRED.md entry D-Clean-Radix96.
template <typename T>
inline void build_keys_and_drop_mask(const T *points, std::size_t n_pts, bool drop_nonfinite,
                                     std::vector<KeyEntry> &keys,
                                     std::vector<std::uint8_t> &drop_mask) {

    keys.resize(n_pts);
    drop_mask.assign(n_pts, 0);

#pragma omp parallel for schedule(static)
    for (long long i = 0; i < (long long)n_pts; ++i) {
        T x = points[i * 3 + 0];
        T y = points[i * 3 + 1];
        T z = points[i * 3 + 2];
        if (!is_finite(x) || !is_finite(y) || !is_finite(z)) {
            // Caller (wrapper) is responsible for raising if
            // drop_nonfinite is false; here we just mark.
            drop_mask[i] = 1;
            keys[i].k0 = 0xFFFFFFFFu;
            keys[i].k1 = 0xFFFFFFFFu;
            keys[i].k2 = 0xFFFFFFFFu;
            keys[i].orig_id = (std::int32_t)i;
            (void)drop_nonfinite;
            continue;
        }
        std::uint32_t k[3];
        if constexpr (std::is_same_v<T, float>) {
            pack_f32_key(x, y, z, k);
        } else {
            pack_f64_hash_key((double)x, (double)y, (double)z, k);
        }
        keys[i].k0 = k[0];
        keys[i].k1 = k[1];
        keys[i].k2 = k[2];
        keys[i].orig_id = (std::int32_t)i;
    }
}

// Build the quantised int32-lattice key for tolerance mode.
template <typename T>
inline void build_quantised_keys(const T *points, std::size_t n_pts, const BBox<T> &bb, double eps,
                                 std::vector<KeyEntry> &keys,
                                 std::vector<std::uint8_t> &drop_mask) {

    keys.resize(n_pts);
    drop_mask.assign(n_pts, 0);

    // Origin shift by -0.5*eps avoids lattice bias on bbox edge.
    double ox = (double)bb.bbmin[0] - 0.5 * eps;
    double oy = (double)bb.bbmin[1] - 0.5 * eps;
    double oz = (double)bb.bbmin[2] - 0.5 * eps;
    double inv_eps = 1.0 / eps;

#pragma omp parallel for schedule(static)
    for (long long i = 0; i < (long long)n_pts; ++i) {
        T x = points[i * 3 + 0];
        T y = points[i * 3 + 1];
        T z = points[i * 3 + 2];
        if (!is_finite(x) || !is_finite(y) || !is_finite(z)) {
            drop_mask[i] = 1;
            keys[i].k0 = 0xFFFFFFFFu;
            keys[i].k1 = 0xFFFFFFFFu;
            keys[i].k2 = 0xFFFFFFFFu;
            keys[i].orig_id = (std::int32_t)i;
            continue;
        }
        std::int32_t ix = quantise_coord((double)x, ox, inv_eps);
        std::int32_t iy = quantise_coord((double)y, oy, inv_eps);
        std::int32_t iz = quantise_coord((double)z, oz, inv_eps);
        keys[i].k0 = (std::uint32_t)ix;
        keys[i].k1 = (std::uint32_t)iy;
        keys[i].k2 = (std::uint32_t)iz;
        keys[i].orig_id = (std::int32_t)i;
    }
}

// --------- Phase D: segmented unique --------------------------------
//
// After stable_sort, equal-key entries form contiguous segments. We
// emit one canonical point per segment, where the canonical original
// id is the minimum within the segment. With a stable sort keyed on
// (key, orig_id) the first element of each segment carries the
// minimum, but we recompute the min explicitly per M3 so the
// invariant does not depend on sort stability of the secondary key.
//
// For float64 identical-merge, hash collisions are possible — segments
// must be split into full-double sub-segments. `verify_points` is the
// raw (non-canonicalised) point array used for the equality test.
template <typename T>
inline void segmented_unique_identical(const std::vector<KeyEntry> &keys,
                                       const std::vector<std::uint8_t> &drop_mask, const T *points,
                                       std::size_t n_pts, std::vector<std::int32_t> &point_map_out,
                                       std::vector<std::int32_t> &source_map_out,
                                       std::vector<T> &points_out) {

    point_map_out.assign(n_pts, SENTINEL_ID);
    source_map_out.clear();
    points_out.clear();
    if (n_pts == 0)
        return;

    // Skip dropped points (they live at the high-key end).
    std::size_t n_live = keys.size();
    // The drop_mask is indexed by original id, but keys[i].orig_id
    // identifies which entry in the sorted array is dropped.

    auto same_coord = [&](std::int32_t a, std::int32_t b) -> bool {
        if constexpr (std::is_same_v<T, float>) {
            // For float32 the packed key IS the bit-exact triple
            // (with -0.0 canonicalised) — equal keys imply equal
            // (canonicalised) coords. No re-verify needed.
            (void)a;
            (void)b;
            return true;
        } else {
            // float64: hash key match, must verify full doubles
            // (M3 / S-Pragmatic-6). -0.0 vs +0.0 also dedupes here.
            for (int d = 0; d < 3; ++d) {
                double va = (double)points[(std::size_t)a * 3 + d];
                double vb = (double)points[(std::size_t)b * 3 + d];
                if (va == 0.0)
                    va = 0.0; // canonicalise (no-op but documents)
                if (vb == 0.0)
                    vb = 0.0;
                std::uint64_t bva, bvb;
                std::memcpy(&bva, &va, sizeof(double));
                std::memcpy(&bvb, &vb, sizeof(double));
                if (bva != bvb)
                    return false;
            }
            return true;
        }
    };

    std::size_t i = 0;
    while (i < n_live) {
        const KeyEntry &e = keys[i];
        if (drop_mask[(std::size_t)e.orig_id]) {
            // Dropped points sort to the end; once we hit one we are
            // done.
            break;
        }
        // Walk the run of equal 96-bit keys.
        std::size_t j = i + 1;
        while (j < n_live && keys[j].k0 == e.k0 && keys[j].k1 == e.k1 && keys[j].k2 == e.k2) {
            ++j;
        }
        // Inside [i, j) we may have sub-segments separated by
        // full-coord inequality (float64 hash collision).
        std::size_t s = i;
        while (s < j) {
            std::size_t t = s + 1;
            while (t < j && same_coord(keys[s].orig_id, keys[t].orig_id)) {
                ++t;
            }
            // Sub-segment [s, t) is a true equivalence class.
            std::int32_t canonical_orig = keys[s].orig_id;
            for (std::size_t q = s + 1; q < t; ++q) {
                if (keys[q].orig_id < canonical_orig) {
                    canonical_orig = keys[q].orig_id;
                }
            }
            std::int32_t new_id = (std::int32_t)source_map_out.size();
            source_map_out.push_back(canonical_orig);
            // Push the canonical point.
            for (int d = 0; d < 3; ++d) {
                points_out.push_back(points[(std::size_t)canonical_orig * 3 + d]);
            }
            for (std::size_t q = s; q < t; ++q) {
                point_map_out[(std::size_t)keys[q].orig_id] = new_id;
            }
            s = t;
        }
        i = j;
    }
}

// Tolerance-mode dedup: build coarse equivalence classes via the
// quantised key, then run a union-find over candidate pairs from
// 27-cell stencils to handle lattice-edge non-transitive merges.
//
// v1 implements the simpler form: pairs are (point, point) within the
// SAME quantised cell. The 27-cell stencil is added by examining the
// neighbourhood of each lattice cell.
template <typename T>
inline void segmented_unique_tolerance(const std::vector<KeyEntry> &sorted_keys,
                                       const std::vector<std::uint8_t> &drop_mask, const T *points,
                                       std::size_t n_pts, double eps_sq,
                                       std::vector<std::int32_t> &point_map_out,
                                       std::vector<std::int32_t> &source_map_out,
                                       std::vector<T> &points_out) {

    point_map_out.assign(n_pts, SENTINEL_ID);
    source_map_out.clear();
    points_out.clear();
    if (n_pts == 0)
        return;

    // Union-find over original-id space, sized to n_pts.
    std::vector<std::int32_t> parent(n_pts);
    std::vector<std::int32_t> rank_(n_pts, 0);
    for (std::size_t i = 0; i < n_pts; ++i)
        parent[i] = (std::int32_t)i;

    auto find = [&](std::int32_t a) -> std::int32_t {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    auto unite = [&](std::int32_t a, std::int32_t b) {
        std::int32_t ra = find(a), rb = find(b);
        if (ra == rb)
            return;
        if (rank_[ra] < rank_[rb]) {
            parent[ra] = rb;
        } else if (rank_[ra] > rank_[rb]) {
            parent[rb] = ra;
        } else {
            parent[rb] = ra;
            ++rank_[ra];
        }
    };

    auto dist_sq = [&](std::int32_t a, std::int32_t b) -> double {
        double s = 0.0;
        for (int d = 0; d < 3; ++d) {
            double va = (double)points[(std::size_t)a * 3 + d];
            double vb = (double)points[(std::size_t)b * 3 + d];
            double dv = va - vb;
            s += dv * dv;
        }
        return s;
    };

    // Index runs of equal quantised cells in the sorted array.
    std::size_t n_live = sorted_keys.size();
    std::vector<std::size_t> seg_starts;
    seg_starts.reserve(n_live / 4 + 1);

    std::size_t i = 0;
    while (i < n_live) {
        if (drop_mask[(std::size_t)sorted_keys[i].orig_id])
            break;
        seg_starts.push_back(i);
        std::size_t j = i + 1;
        while (j < n_live && sorted_keys[j].k0 == sorted_keys[i].k0 &&
               sorted_keys[j].k1 == sorted_keys[i].k1 && sorted_keys[j].k2 == sorted_keys[i].k2) {
            ++j;
        }
        i = j;
    }
    std::size_t n_segs = seg_starts.size();
    seg_starts.push_back(i); // sentinel end for the LAST segment span

    // Build a map from quantised key -> segment index for stencil
    // lookup. Sorted by key already; use binary search.
    auto find_seg = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) -> std::ptrdiff_t {
        std::ptrdiff_t lo = 0, hi = (std::ptrdiff_t)n_segs - 1;
        while (lo <= hi) {
            std::ptrdiff_t mid = (lo + hi) >> 1;
            const KeyEntry &m = sorted_keys[seg_starts[(std::size_t)mid]];
            int c0 = (m.k0 == a) ? 0 : (m.k0 < a ? -1 : 1);
            int c1 = (c0 != 0) ? c0 : ((m.k1 == b) ? 0 : (m.k1 < b ? -1 : 1));
            int c2 = (c1 != 0) ? c1 : ((m.k2 == c) ? 0 : (m.k2 < c ? -1 : 1));
            if (c2 == 0)
                return mid;
            if (c2 < 0)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return -1;
    };

    // For each segment, probe the 27-cell stencil. Union pairs within
    // eps. Within-segment all pairs are candidates; across-segment we
    // only probe the lex-greater neighbours to avoid double-work, but
    // for correctness simplicity we probe all 27 and rely on the
    // union-find idempotency.
    for (std::size_t s = 0; s < n_segs; ++s) {
        std::size_t a0 = seg_starts[s];
        std::size_t a1 = seg_starts[s + 1];
        std::uint32_t kx = sorted_keys[a0].k0;
        std::uint32_t ky = sorted_keys[a0].k1;
        std::uint32_t kz = sorted_keys[a0].k2;

        // Within-segment pairs.
        for (std::size_t p = a0; p < a1; ++p) {
            for (std::size_t q = p + 1; q < a1; ++q) {
                if (dist_sq(sorted_keys[p].orig_id, sorted_keys[q].orig_id) <= eps_sq) {
                    unite(sorted_keys[p].orig_id, sorted_keys[q].orig_id);
                }
            }
        }

        // Cross-segment via 27-cell stencil. We only need to look at
        // strictly-greater lex neighbours to avoid duplicate work.
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0)
                        continue;
                    std::uint32_t nx = (std::uint32_t)((std::int32_t)kx + dx);
                    std::uint32_t ny = (std::uint32_t)((std::int32_t)ky + dy);
                    std::uint32_t nz = (std::uint32_t)((std::int32_t)kz + dz);
                    // Lex-greater filter.
                    bool greater =
                        (nx > kx) || (nx == kx && ny > ky) || (nx == kx && ny == ky && nz > kz);
                    if (!greater)
                        continue;
                    std::ptrdiff_t ns = find_seg(nx, ny, nz);
                    if (ns < 0)
                        continue;
                    std::size_t b0 = seg_starts[(std::size_t)ns];
                    std::size_t b1 = seg_starts[(std::size_t)ns + 1];
                    for (std::size_t p = a0; p < a1; ++p) {
                        for (std::size_t q = b0; q < b1; ++q) {
                            if (dist_sq(sorted_keys[p].orig_id, sorted_keys[q].orig_id) <= eps_sq) {
                                unite(sorted_keys[p].orig_id, sorted_keys[q].orig_id);
                            }
                        }
                    }
                }
            }
        }
    }

    // Reduce each class to its canonical = min(original_id) (M3).
    // class_root -> canonical_orig_id.
    std::vector<std::int32_t> class_min(n_pts, std::numeric_limits<std::int32_t>::max());
    for (std::size_t k = 0; k < n_pts; ++k) {
        if (drop_mask[k])
            continue;
        std::int32_t r = find((std::int32_t)k);
        if ((std::int32_t)k < class_min[(std::size_t)r]) {
            class_min[(std::size_t)r] = (std::int32_t)k;
        }
    }

    // Assign new_id in order of canonical original id. Build a sorted
    // list of (canonical_orig, root).
    std::vector<std::pair<std::int32_t, std::int32_t>> canon;
    canon.reserve(n_pts);
    for (std::size_t r = 0; r < n_pts; ++r) {
        if (class_min[r] != std::numeric_limits<std::int32_t>::max() &&
            (std::int32_t)r == find((std::int32_t)r)) {
            canon.emplace_back(class_min[r], (std::int32_t)r);
        }
    }
    std::sort(canon.begin(), canon.end());

    // root -> new_id map.
    std::vector<std::int32_t> root_to_new(n_pts, SENTINEL_ID);
    source_map_out.resize(canon.size());
    points_out.resize(canon.size() * 3);
    for (std::size_t n = 0; n < canon.size(); ++n) {
        root_to_new[(std::size_t)canon[n].second] = (std::int32_t)n;
        source_map_out[n] = canon[n].first;
        for (int d = 0; d < 3; ++d) {
            points_out[n * 3 + d] = points[(std::size_t)canon[n].first * 3 + d];
        }
    }
    for (std::size_t k = 0; k < n_pts; ++k) {
        if (drop_mask[k])
            continue;
        std::int32_t r = find((std::int32_t)k);
        point_map_out[k] = root_to_new[(std::size_t)r];
    }
}

// --------- Phase B+C+D fused: parallel linear-probe hash dedup ------
//
// Replaces the sort + segmented-unique pipeline for tolerance==0 with a
// single parallel pass over a linear-probe open-addressing atomic hash
// table (pyvista-stl pattern; see src/cpp/linear_probe_table.h).
//
// Each slot is std::atomic<uint32_t>{EMPTY|RESERVED|entry_id}. On
// insert, the winning thread does fetch_add on a global entry counter,
// writes the entry's key into a side array, and stores the entry id
// into the slot with release ordering. Losers spin on the RESERVED
// state and then either match (atomic_min the rep_id) or continue
// probing.
//
// Determinism: canonical rep is the min(orig_id) of the class
// regardless of thread schedule (atomic-min on the rep_id side array).
// New point ids are assigned by sorting unique entries on their
// rep_id ascending — this ordering is independent of thread count.

// Per-entry side storage for the linear-probe table. Keys live here for
// equality verification on probe hits; rep_id holds the atomic-min of
// the class's original ids and is computed in-place.
template <typename T>
struct EntryStore; // primary template not used.

template <>
struct EntryStore<float> {
    // 96-bit float32 bit-pattern (canonicalised -0.0 -> +0.0).
    std::uint32_t k0, k1, k2;
    // atomic-min over orig_id of every point that matched this entry.
    std::int32_t rep_id;
};
static_assert(sizeof(EntryStore<float>) == 16, "EntryStore<float> must be 16 B");

template <>
struct EntryStore<double> {
    // Full-bit canonicalised float64 bit-pattern (3 * 64 bits).
    std::uint64_t b0, b1, b2;
    std::int32_t rep_id;
    std::int32_t _pad;
};
static_assert(sizeof(EntryStore<double>) == 32, "EntryStore<double> must be 32 B");

// Entry-key equality against the side EntryStore.
template <typename T>
inline bool entry_key_eq(const EntryStore<T> &e, std::uint32_t k0, std::uint32_t k1,
                         std::uint32_t k2, std::uint64_t b0, std::uint64_t b1, std::uint64_t b2);

template <>
inline bool entry_key_eq<float>(const EntryStore<float> &e, std::uint32_t k0, std::uint32_t k1,
                                std::uint32_t k2, std::uint64_t, std::uint64_t, std::uint64_t) {
    return e.k0 == k0 && e.k1 == k1 && e.k2 == k2;
}

template <>
inline bool entry_key_eq<double>(const EntryStore<double> &e, std::uint32_t, std::uint32_t,
                                 std::uint32_t, std::uint64_t b0, std::uint64_t b1,
                                 std::uint64_t b2) {
    return e.b0 == b0 && e.b1 == b1 && e.b2 == b2;
}

// Canonicalised bit-patterns for a point. For float64 also fills the
// full 64-bit raw words used in the full-bit verify path.
template <typename T>
inline void compute_point_key(T x, T y, T z, std::uint32_t &k0, std::uint32_t &k1,
                              std::uint32_t &k2, std::uint64_t &b0, std::uint64_t &b1,
                              std::uint64_t &b2) {
    if constexpr (std::is_same_v<T, float>) {
        std::uint32_t k[3];
        pack_f32_key(x, y, z, k);
        k0 = k[0];
        k1 = k[1];
        k2 = k[2];
        b0 = b1 = b2 = 0;
    } else {
        double cx = canonicalise_zero((double)x);
        double cy = canonicalise_zero((double)y);
        double cz = canonicalise_zero((double)z);
        std::memcpy(&b0, &cx, sizeof(double));
        std::memcpy(&b1, &cy, sizeof(double));
        std::memcpy(&b2, &cz, sizeof(double));
        k0 = (std::uint32_t)(b0 ^ (b0 >> 32));
        k1 = (std::uint32_t)(b1 ^ (b1 >> 32));
        k2 = (std::uint32_t)(b2 ^ (b2 >> 32));
    }
}

// Parallel linear-probe dedup for tolerance==0. Outputs:
//   point_map_out[i]   = new id (in unique-point space), or SENTINEL
//   source_map_out[n]  = canonical orig_id (min) for unique id n
//   points_out         = canonical points (length 3 * n_unique)
//   any_duplicates_out = true iff at least one collision was observed
//
// Algorithm (pyvista-stl pattern):
//   * mmap-backed atomic<uint32> open-addressing table sized
//     next_pow2(n_pts * 1.7). Each slot is EMPTY / RESERVED / entry_id.
//   * Threads compute final96(k0,k1,k2) over the canonicalised
//     bit-pattern, linear-probe to first EMPTY or matching slot.
//   * Match: atomic_min(rep_id[entry], orig_id). Preserves the
//     canonical = min(orig_id) contract across thread schedules.
//   * Miss: CAS slot EMPTY->RESERVED. Winner does fetch_add on the
//     entry counter, writes key + rep_id into side store, then stores
//     entry_id with release ordering. Loser spins/pauses.
//   * Prefetch upcoming raw point data + the slot it will hash into.
//
// Determinism is the same as before: rep_id contains min(orig_id), and
// new-id assignment runs a prefix sum over is_canonical[i] = (rep_id of
// the entry for point i equals i), which is independent of thread
// count.
//
// Max input count is bounded by lpt::MAX_ENTRY_ID (~2^32 - 2 ≈ 4.29
// billion); int32 connectivity caps us at INT32_MAX ≈ 2.1 billion anyway.
template <typename T>
inline void parallel_anchor_dedup_identical(const T *points, std::size_t n_pts, bool drop_nonfinite,
                                            std::vector<std::int32_t> &point_map_out,
                                            std::vector<std::int32_t> &source_map_out,
                                            std::vector<T> &points_out,
                                            std::vector<std::uint8_t> &drop_mask_out,
                                            bool &any_duplicates_out) {

    point_map_out.assign(n_pts, SENTINEL_ID);
    source_map_out.clear();
    points_out.clear();
    drop_mask_out.assign(n_pts, 0);
    any_duplicates_out = false;
    if (n_pts == 0)
        return;
    if (n_pts > (std::size_t)lpt::MAX_ENTRY_ID) {
        throw std::runtime_error("pyvista_algorithms.clean: input exceeds 2^32-2 points");
    }
    CleanTrace _t;
    _t.start();

    // ---- size hash table ------------------------------------------------
    // Open-addressing linear-probe. Load factor ~0.6 (1.7x size) keeps
    // average probe length low even at hash-skew. The table is a flat
    // array of std::atomic<uint32_t> slots; one cacheline holds 16 slots.
    std::size_t cap = lpt::next_pow2((std::size_t)((double)n_pts * 1.7) + 16);
    const std::uint64_t mask = (std::uint64_t)(cap - 1);

    void *vht_backing = nullptr;
    std::size_t vht_backing_len = 0;
    std::atomic<std::uint32_t> *vht = lpt::alloc_table(cap, vht_backing, vht_backing_len);
    if (!vht)
        throw std::bad_alloc();

    // Side entry store: one per unique class. Sized to n_pts (worst case
    // = every point is unique). Default-init (no zero fill) — entry
    // slots are written before the entry id is published to a table
    // slot, so consumers always see initialised data.
    std::unique_ptr<EntryStore<T>[]> entries(new EntryStore<T>[n_pts]);

    // Per-input-point entry id (which class this point belongs to).
    // Default-init; set to a real entry id or kept as drop sentinel. We
    // distinguish "claimed" from "dropped" via drop_mask_out, so this
    // does not need a -1 initial pattern.
    std::unique_ptr<std::int32_t[]> point_to_entry(new std::int32_t[n_pts]);

    // Pre-fault the two large side buffers in parallel. Without this,
    // first-touch page faults during the insert loop serialise on the
    // per-mm page-fault lock and the loop fails to scale beyond ~1
    // thread. A single byte per page is enough to populate.
    {
        constexpr std::size_t PAGE = 4096;
        std::size_t e_bytes = n_pts * sizeof(EntryStore<T>);
        std::size_t p_bytes = n_pts * sizeof(std::int32_t);
        auto *e_raw = reinterpret_cast<volatile char *>(entries.get());
        auto *p_raw = reinterpret_cast<volatile char *>(point_to_entry.get());
#pragma omp parallel for schedule(static)
        for (long long off = 0; off < (long long)e_bytes; off += (long long)PAGE) {
            e_raw[off] = 0;
        }
#pragma omp parallel for schedule(static)
        for (long long off = 0; off < (long long)p_bytes; off += (long long)PAGE) {
            p_raw[off] = 0;
        }
    }
    _t.mark("  dedup-alloc");

    std::atomic<std::uint32_t> n_entries_atomic{0};
    std::atomic<bool> any_dup(false);

    // ---- Pass 1: insert -------------------------------------------------
    //
    // Prefetch the upcoming input point and (via its hash) the slot it
    // will probe. Tuned distance = 16 points (~ one cache line of f64
    // triples).
    constexpr std::size_t PREFETCH_DIST = 16;

#pragma omp parallel
    {
        bool local_any_dup = false;
#pragma omp for schedule(static) nowait
        for (long long ii = 0; ii < (long long)n_pts; ++ii) {
            std::size_t i = (std::size_t)ii;

            // Prefetch upcoming input point and slot.
            if (i + PREFETCH_DIST < n_pts) {
                const T *pf = points + (i + PREFETCH_DIST) * 3;
                PVU_LPT_PREFETCH_R(pf);
                // Cheap pre-hash for slot prefetch: fold raw bits to a
                // 32-bit avalanche. For the f32 path this matches what
                // the canonicalised path will compute (-0.0 is rare in
                // mesh data, so the prefetch hits the right cacheline
                // virtually always). For f64 it's an approximation; the
                // benefit is overlapping memory latency, not exactness.
                std::uint32_t pf_k0, pf_k1, pf_k2;
                if constexpr (std::is_same_v<T, float>) {
                    std::memcpy(&pf_k0, pf + 0, 4);
                    std::memcpy(&pf_k1, pf + 1, 4);
                    std::memcpy(&pf_k2, pf + 2, 4);
                } else {
                    std::uint64_t b;
                    std::memcpy(&b, pf + 0, 8);
                    pf_k0 = (std::uint32_t)(b ^ (b >> 32));
                    std::memcpy(&b, pf + 1, 8);
                    pf_k1 = (std::uint32_t)(b ^ (b >> 32));
                    std::memcpy(&b, pf + 2, 8);
                    pf_k2 = (std::uint32_t)(b ^ (b >> 32));
                }
                std::uint32_t pf_h = final96(pf_k0, pf_k1, pf_k2);
                PVU_LPT_PREFETCH_W(&vht[pf_h & mask]);
            }

            T x = points[i * 3 + 0];
            T y = points[i * 3 + 1];
            T z = points[i * 3 + 2];
            if (!is_finite(x) || !is_finite(y) || !is_finite(z)) {
                drop_mask_out[i] = 1;
                (void)drop_nonfinite;
                continue;
            }
            std::uint32_t k0, k1, k2;
            std::uint64_t b0, b1, b2;
            compute_point_key<T>(x, y, z, k0, k1, k2, b0, b1, b2);

            std::uint32_t h = final96(k0, k1, k2);
            // Linear probe.
            for (std::uint64_t probe = 0;; ++probe) {
                std::size_t idx = (std::size_t)((h + probe) & mask);
                std::atomic<std::uint32_t> &slot = vht[idx];
                // Relaxed load is sufficient: we will either see EMPTY
                // (proceed to CAS, which has its own acquire semantics
                // on failure to fetch the new value), RESERVED (loop and
                // retry), or an entry id. If we see an entry id we then
                // read its key from the side store; the writer published
                // the slot via a release store after writing the side
                // store, so the relaxed read of the slot id is paired
                // with the release-store. The transitive synchronisation
                // is via the slot store, not the load — on x86 a relaxed
                // load is a plain MOV anyway and on weakly-ordered
                // architectures we still need the publisher's release.
                // To be safe we promote relaxed -> acquire after we
                // observe a real entry id; the EMPTY/RESERVED probes
                // don't need synchronisation.
                std::uint32_t cur = slot.load(std::memory_order_relaxed);
                while (true) {
                    if (cur == lpt::SLOT_EMPTY) {
                        // Try to claim the slot. Write entries store AFTER
                        // winning CAS so a lost CAS does not allocate an
                        // entry id or touch the side store at all.
                        std::uint32_t expected = lpt::SLOT_EMPTY;
                        if (slot.compare_exchange_strong(expected, lpt::SLOT_RESERVED,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                            // Won: allocate entry id, populate store,
                            // publish.
                            std::uint32_t eid =
                                n_entries_atomic.fetch_add(1, std::memory_order_relaxed);
                            EntryStore<T> &ne = entries[(std::size_t)eid];
                            if constexpr (std::is_same_v<T, float>) {
                                ne.k0 = k0;
                                ne.k1 = k1;
                                ne.k2 = k2;
                            } else {
                                ne.b0 = b0;
                                ne.b1 = b1;
                                ne.b2 = b2;
                            }
                            ne.rep_id = (std::int32_t)i;
                            slot.store(eid, std::memory_order_release);
                            point_to_entry[i] = (std::int32_t)eid;
                            goto inserted;
                        }
                        // Lost: cur now holds the new value (RESERVED or
                        // entry id). Re-enter the inner loop.
                        cur = expected;
                        continue;
                    }
                    if (cur == lpt::SLOT_RESERVED) {
                        PVU_LPT_PAUSE();
                        cur = slot.load(std::memory_order_relaxed);
                        continue;
                    }
                    // Occupied: acquire-fence the side-store read of the
                    // winning writer (paired with its release store of
                    // the entry id into the slot).
                    std::atomic_thread_fence(std::memory_order_acquire);
                    std::int32_t eid = (std::int32_t)cur;
                    if (entry_key_eq<T>(entries[(std::size_t)eid], k0, k1, k2, b0, b1, b2)) {
                        // Match: atomic-min rep_id with this orig_id.
                        lpt::atomic_int32_min(&entries[(std::size_t)eid].rep_id, (std::int32_t)i);
                        local_any_dup = true;
                        point_to_entry[i] = eid;
                        // If we had reserved an entry id earlier (we can't
                        // have, since my_entry is only set on the EMPTY
                        // branch), it would be abandoned here. Not the
                        // case in this code path.
                        goto inserted;
                    }
                    // Collision on this slot — advance probe.
                    break;
                }
            }
        inserted:;
        }
        if (local_any_dup) {
            any_dup.store(true, std::memory_order_relaxed);
        }
    }

    lpt::free_table(vht, vht_backing, vht_backing_len);
    _t.mark("  dedup-insert");

    bool any_dup_flag = any_dup.load(std::memory_order_relaxed);
    any_duplicates_out = any_dup_flag;

    // Fast path: no duplicates and no abandoned entries. All input
    // points map 1:1 to a unique class. Skip the canonical-mark + prefix
    // sum and use the verbatim mapping.
    if (!any_dup_flag) {
        points_out.assign(points, points + n_pts * 3);
        source_map_out.resize(n_pts);
#pragma omp parallel for schedule(static)
        for (long long ii = 0; ii < (long long)n_pts; ++ii) {
            std::size_t i = (std::size_t)ii;
            if (drop_mask_out[i]) {
                source_map_out[i] = SENTINEL_ID;
                continue;
            }
            point_map_out[i] = (std::int32_t)i;
            source_map_out[i] = (std::int32_t)i;
        }
        return;
    }

    // ---- Build the unique point set ordered by canonical rep_id -------
    //
    // Point i is the rep of its class iff
    // entries[point_to_entry[i]].rep_id == i. We assign new ids by a
    // parallel prefix sum over reps in input order — giving classes new
    // ids in ascending min(orig_id) order, bit-identical across thread
    // counts.
    //
    // Fused pass: each thread walks its chunk, evaluates is_canonical
    // on the fly, and writes new_id_of_pt[i] only for canonical points.
    // Non-canonical entries keep SENTINEL_ID. After prefix sum we look
    // up new_id_of_pt[rep_id] for every input point.
    int prefix_threads = omp_get_max_threads();
    if (prefix_threads < 1)
        prefix_threads = 1;
    std::vector<std::int32_t> chunk_sum((std::size_t)prefix_threads + 1, 0);
    std::vector<std::int32_t> new_id_of_pt(n_pts, SENTINEL_ID);
#pragma omp parallel num_threads(prefix_threads)
    {
        int tid = omp_get_thread_num();
        int nt_loc = omp_get_num_threads();
        std::size_t lo = ((std::size_t)tid * n_pts) / (std::size_t)nt_loc;
        std::size_t hi = ((std::size_t)(tid + 1) * n_pts) / (std::size_t)nt_loc;
        std::int32_t local = 0;
        for (std::size_t i = lo; i < hi; ++i) {
            if (drop_mask_out[i])
                continue;
            std::int32_t ei = point_to_entry[i];
            if (entries[(std::size_t)ei].rep_id == (std::int32_t)i) {
                new_id_of_pt[i] = local;
                ++local;
            }
        }
        chunk_sum[(std::size_t)tid + 1] = local;
#pragma omp barrier
#pragma omp single
        {
            for (int t = 1; t <= nt_loc; ++t) {
                chunk_sum[(std::size_t)t] += chunk_sum[(std::size_t)t - 1];
            }
        }
        std::int32_t off = chunk_sum[(std::size_t)tid];
        if (off != 0) {
            for (std::size_t i = lo; i < hi; ++i) {
                if (new_id_of_pt[i] != SENTINEL_ID) {
                    new_id_of_pt[i] += off;
                }
            }
        }
    }
    std::size_t n_unique = (std::size_t)chunk_sum[(std::size_t)prefix_threads];

    source_map_out.resize(n_unique);
    points_out.resize(n_unique * 3);
#pragma omp parallel for schedule(static)
    for (long long ii = 0; ii < (long long)n_pts; ++ii) {
        std::size_t i = (std::size_t)ii;
        std::int32_t nid = new_id_of_pt[i];
        if (nid == SENTINEL_ID)
            continue;
        source_map_out[(std::size_t)nid] = (std::int32_t)i;
        points_out[(std::size_t)nid * 3 + 0] = points[i * 3 + 0];
        points_out[(std::size_t)nid * 3 + 1] = points[i * 3 + 1];
        points_out[(std::size_t)nid * 3 + 2] = points[i * 3 + 2];
    }

#pragma omp parallel for schedule(static)
    for (long long ii = 0; ii < (long long)n_pts; ++ii) {
        std::size_t i = (std::size_t)ii;
        if (drop_mask_out[i])
            continue;
        std::int32_t ei = point_to_entry[i];
        std::int32_t rep = entries[(std::size_t)ei].rep_id;
        point_map_out[i] = new_id_of_pt[(std::size_t)rep];
    }
    _t.mark("  dedup-compact");
}

// --------- Phase B+C+D alt: Morton-radix + segment-reduce dedup ------
//
// Hash each point into a 32-bit code, pack with its orig_id into a u64
// (code << 32 | orig_id), parallel radix sort (sequential cache-friendly
// memory access, scales with threads), then a segment scan in parallel
// chunks. This avoids the random-access hashtable bottleneck on big
// inputs.
//
// Determinism contract preserved: canonical rep_id is the min(orig_id)
// in each full-bit equivalence class (radix-sort makes equal-hash items
// adjacent and sorts by orig_id within them since orig_id occupies the
// low bits; min is then keys[s] of each sub-run). New-id assignment
// follows ascending min(orig_id) so output ordering is thread-count
// independent.
template <typename T>
inline void parallel_morton_radix_dedup_identical(const T *points, std::size_t n_pts,
                                                  bool drop_nonfinite,
                                                  std::vector<std::int32_t> &point_map_out,
                                                  std::vector<std::int32_t> &source_map_out,
                                                  std::vector<T> &points_out,
                                                  std::vector<std::uint8_t> &drop_mask_out,
                                                  bool &any_duplicates_out) {
    point_map_out.assign(n_pts, SENTINEL_ID);
    source_map_out.clear();
    points_out.clear();
    drop_mask_out.assign(n_pts, 0);
    any_duplicates_out = false;
    if (n_pts == 0)
        return;
    if (n_pts > 0xFFFFFFFEu) {
        throw std::runtime_error("pyvista_algorithms.clean: input exceeds 2^32-2 points");
    }
    CleanTrace _t;
    _t.start();

    // Compute hash codes and detect drops. We pack (code << 32) | orig_id
    // into a u64 for the radix sort. Drop sentinel: code = 0xFFFFFFFF
    // (sorts to the high end; we stop the segment scan when we see it).
    std::vector<std::uint64_t> keys;
    resize_noinit_parallel(keys, n_pts);
    constexpr std::uint32_t DROP_HASH = 0xFFFFFFFFu;

#pragma omp parallel for schedule(static)
    for (long long ii = 0; ii < (long long)n_pts; ++ii) {
        std::size_t i = (std::size_t)ii;
        T x = points[i * 3 + 0];
        T y = points[i * 3 + 1];
        T z = points[i * 3 + 2];
        if (!is_finite(x) || !is_finite(y) || !is_finite(z)) {
            drop_mask_out[i] = 1;
            keys[i] = ((std::uint64_t)DROP_HASH << 32) | (std::uint32_t)i;
            (void)drop_nonfinite;
            continue;
        }
        std::uint32_t k0, k1, k2;
        std::uint64_t b0, b1, b2;
        compute_point_key<T>(x, y, z, k0, k1, k2, b0, b1, b2);
        std::uint32_t code = final96(k0, k1, k2);
        // Avoid the drop sentinel value for real data.
        if (code == DROP_HASH)
            code = DROP_HASH - 1;
        keys[i] = ((std::uint64_t)code << 32) | (std::uint32_t)i;
    }
    _t.mark("  dedup-key+hash");

    // Parallel LSD radix sort on the u64 packed keys.
    parallel_radix_sort_u64(keys.data(), n_pts);
    _t.mark("  dedup-radix");

    // Count of live (non-dropped) entries: they live at the low-hash end.
    // Binary search for the first DROP_HASH entry.
    std::size_t n_live = n_pts;
    if ((keys[n_pts - 1] >> 32) == DROP_HASH) {
        std::size_t lo = 0, hi = n_pts;
        while (lo < hi) {
            std::size_t mid = (lo + hi) >> 1;
            if ((keys[mid] >> 32) >= DROP_HASH)
                hi = mid;
            else
                lo = mid + 1;
        }
        n_live = lo;
    }

    // Helper: full-bit point equality. Reads point coords by orig_id.
    auto same_point = [&](std::uint32_t a, std::uint32_t b) -> bool {
        if constexpr (std::is_same_v<T, float>) {
            std::uint32_t ax, ay, az, bx, by, bz;
            float fa, fb;
            fa = canonicalise_zero(points[(std::size_t)a * 3 + 0]);
            std::memcpy(&ax, &fa, 4);
            fb = canonicalise_zero(points[(std::size_t)b * 3 + 0]);
            std::memcpy(&bx, &fb, 4);
            if (ax != bx)
                return false;
            fa = canonicalise_zero(points[(std::size_t)a * 3 + 1]);
            std::memcpy(&ay, &fa, 4);
            fb = canonicalise_zero(points[(std::size_t)b * 3 + 1]);
            std::memcpy(&by, &fb, 4);
            if (ay != by)
                return false;
            fa = canonicalise_zero(points[(std::size_t)a * 3 + 2]);
            std::memcpy(&az, &fa, 4);
            fb = canonicalise_zero(points[(std::size_t)b * 3 + 2]);
            std::memcpy(&bz, &fb, 4);
            return az == bz;
        } else {
            for (int d = 0; d < 3; ++d) {
                double va = canonicalise_zero((double)points[(std::size_t)a * 3 + d]);
                double vb = canonicalise_zero((double)points[(std::size_t)b * 3 + d]);
                std::uint64_t bva, bvb;
                std::memcpy(&bva, &va, sizeof(double));
                std::memcpy(&bvb, &vb, sizeof(double));
                if (bva != bvb)
                    return false;
            }
            return true;
        }
    };

    // Fused parallel segment scan + emit. Two passes over the sorted
    // ``keys`` array: pass 1 counts unique sub-runs per thread chunk;
    // pass 2 emits point_map[orig_id] = new_id and writes
    // source_map[new_id] / points_out[new_id] for the rep. Avoids the
    // n_pts canonical_of / new_id_of_pt arrays and the random-access
    // scatter on n_pts items that dominated dedup-compact.
    //
    // Boundary handling: each thread owns sorted-key range [lo, hi); it
    // skips a leading partial HASH run (left neighbour processes it,
    // even if the run straddles into [lo, hi)) and extends past hi if a
    // run straddles the rightmost boundary.

    int nt = omp_get_max_threads();
    if (nt < 1)
        nt = 1;

    auto compute_bounds = [&](int tid, int nthr) {
        std::size_t lo = ((std::size_t)tid * n_live) / (std::size_t)nthr;
        std::size_t hi = ((std::size_t)(tid + 1) * n_live) / (std::size_t)nthr;
        // Bump lo past any leading partial hash run.
        if (lo > 0 && lo < n_live) {
            std::uint32_t prev_h = (std::uint32_t)(keys[lo - 1] >> 32);
            while (lo < hi && (std::uint32_t)(keys[lo] >> 32) == prev_h) {
                ++lo;
            }
        }
        return std::pair<std::size_t, std::size_t>{lo, hi};
    };

    std::vector<std::int32_t> thread_count((std::size_t)nt + 1, 0);
    std::atomic<bool> any_dup_atomic(false);

    // Pass 1: count unique sub-runs starting in [lo, hi).
#pragma omp parallel num_threads(nt)
    {
        int tid = omp_get_thread_num();
        int nthr = omp_get_num_threads();
        auto bounds = compute_bounds(tid, nthr);
        std::size_t lo = bounds.first;
        std::size_t hi = bounds.second;
        std::int32_t count = 0;
        bool local_any_dup = false;
        std::size_t i = lo;
        while (i < hi) {
            std::uint32_t h = (std::uint32_t)(keys[i] >> 32);
            std::size_t j = i + 1;
            while (j < n_live && (std::uint32_t)(keys[j] >> 32) == h) {
                ++j;
            }
            // Within an equal-HASH run the packed keys are ordered by
            // orig_id, NOT by coordinate, so the copies of one coordinate can
            // be INTERLEAVED with those of a hash-colliding coordinate
            // (A B A B ...). A consecutive same_point scan would break at the
            // first B and overcount every later A/B as its own class. Instead
            // count a point as a new unique class iff it is the FIRST
            // occurrence of its coordinate anywhere earlier in this run.
            for (std::size_t b = i; b < j; ++b) {
                std::uint32_t qb = (std::uint32_t)(keys[b] & 0xFFFFFFFFu);
                bool first = true;
                for (std::size_t a = i; a < b; ++a) {
                    if (same_point((std::uint32_t)(keys[a] & 0xFFFFFFFFu), qb)) {
                        first = false;
                        local_any_dup = true;
                        break;
                    }
                }
                if (first)
                    ++count;
            }
            i = j;
        }
        thread_count[(std::size_t)tid + 1] = count;
        if (local_any_dup) {
            any_dup_atomic.store(true, std::memory_order_relaxed);
        }
    }
    _t.mark("  dedup-segscan");

    // Exclusive prefix sum.
    for (int t = 1; t <= nt; ++t) {
        thread_count[(std::size_t)t] += thread_count[(std::size_t)t - 1];
    }
    std::size_t n_unique = (std::size_t)thread_count[(std::size_t)nt];

    bool any_dup_flag = any_dup_atomic.load(std::memory_order_relaxed);
    any_duplicates_out = any_dup_flag;

    bool any_dropped = (n_live != n_pts);

    // Fast path: no duplicates and no drops -> identity mapping.
    if (!any_dup_flag && !any_dropped) {
        points_out.assign(points, points + n_pts * 3);
        source_map_out.resize(n_pts);
#pragma omp parallel for schedule(static)
        for (long long ii = 0; ii < (long long)n_pts; ++ii) {
            std::size_t i = (std::size_t)ii;
            point_map_out[i] = (std::int32_t)i;
            source_map_out[i] = (std::int32_t)i;
        }
        std::vector<std::uint64_t>().swap(keys);
        _t.mark("  dedup-compact");
        return;
    }

    resize_noinit_parallel(source_map_out, n_unique);
    resize_noinit_parallel(points_out, n_unique * 3);
    _t.mark("  dedup-alloc-out");

    // Pass 2: emit. For each sub-run, the rep is the first orig_id
    // (which is min(orig_id) since the radix sort is ascending and
    // orig_id occupies the low 32 bits of each packed key).
#pragma omp parallel num_threads(nt)
    {
        int tid = omp_get_thread_num();
        int nthr = omp_get_num_threads();
        auto bounds = compute_bounds(tid, nthr);
        std::size_t lo = bounds.first;
        std::size_t hi = bounds.second;
        std::int32_t new_id = thread_count[(std::size_t)tid];
        std::size_t i = lo;
        while (i < hi) {
            std::uint32_t h = (std::uint32_t)(keys[i] >> 32);
            std::size_t j = i + 1;
            while (j < n_live && (std::uint32_t)(keys[j] >> 32) == h) {
                ++j;
            }
            // Interleave-safe grouping (mirrors pass 1): a point starts a new
            // class iff it is the FIRST occurrence of its coordinate in the
            // run; otherwise it maps to the new_id of that first occurrence.
            // Iterating in ascending orig_id order makes the first occurrence
            // the class min(orig_id) -> the canonical rep, preserving the
            // determinism contract. Pass 1 counted identically, so new_id
            // stays within this thread's [thread_count[tid], thread_count[tid+1]).
            for (std::size_t b = i; b < j; ++b) {
                std::uint32_t ob = (std::uint32_t)(keys[b] & 0xFFFFFFFFu);
                std::size_t found = j; // sentinel: no earlier occurrence
                for (std::size_t a = i; a < b; ++a) {
                    if (same_point((std::uint32_t)(keys[a] & 0xFFFFFFFFu), ob)) {
                        found = a;
                        break;
                    }
                }
                if (found == j) {
                    // First occurrence -> canonical rep of a new class.
                    source_map_out[(std::size_t)new_id] = (std::int32_t)ob;
                    points_out[(std::size_t)new_id * 3 + 0] = points[(std::size_t)ob * 3 + 0];
                    points_out[(std::size_t)new_id * 3 + 1] = points[(std::size_t)ob * 3 + 1];
                    points_out[(std::size_t)new_id * 3 + 2] = points[(std::size_t)ob * 3 + 2];
                    point_map_out[(std::size_t)ob] = new_id;
                    ++new_id;
                } else {
                    // Duplicate -> same new_id as its first occurrence (already
                    // assigned, since found < b was processed earlier).
                    std::uint32_t oa = (std::uint32_t)(keys[found] & 0xFFFFFFFFu);
                    point_map_out[(std::size_t)ob] = point_map_out[(std::size_t)oa];
                }
            }
            i = j;
        }
    }

    std::vector<std::uint64_t>().swap(keys);
    _t.mark("  dedup-compact");
}

// --------- Phase F: cell rewrite ------------------------------------
//
// Switch-dispatch over VTK cell types, applying the per-type
// degenerate rule. We keep the input cell order (no partition-by-type)
// so original_cell_ids stays non-decreasing (M7).

// Parallel narrowing copy from IndexT (int32_t or int64_t) into a
// std::vector<std::int32_t>. Used to copy connectivity / offsets from
// the input arrays into the output buffers without paying a separate
// upfront cast pass over the whole conn buffer.
template <typename IndexT>
inline void parallel_narrow_copy_to_i32(const IndexT *src, std::size_t n,
                                        std::vector<std::int32_t> &dst) {
    dst.clear();
    dst.reserve(n);
    // Touch pages in parallel then bump size — avoids a serial memset
    // on a 240 MB+ buffer.
    if (n > 0) {
        constexpr std::size_t PAGE = 4096;
        std::size_t nb = n * sizeof(std::int32_t);
        volatile char *raw = reinterpret_cast<volatile char *>(dst.data());
#pragma omp parallel for schedule(static)
        for (long long off = 0; off < (long long)nb; off += (long long)PAGE) {
            raw[off] = 0;
        }
    }
    dst.resize(n);
    std::int32_t *d = dst.data();
    if constexpr (std::is_same_v<IndexT, std::int32_t>) {
        // No narrowing required; the kernel may still want the data in
        // a separate buffer so we memcpy in parallel chunks.
#pragma omp parallel for schedule(static)
        for (long long i = 0; i < (long long)n; ++i) {
            d[i] = src[i];
        }
    } else {
#pragma omp parallel for simd schedule(static)
        for (long long i = 0; i < (long long)n; ++i) {
            d[i] = (std::int32_t)src[i];
        }
    }
}

// Helper: drop adjacent duplicates from a small id list. Returns the
// new size.
inline int dedup_adjacent(std::int32_t *ids, int n) {
    if (n == 0)
        return 0;
    int w = 1;
    for (int r = 1; r < n; ++r) {
        if (ids[r] != ids[w - 1]) {
            ids[w++] = ids[r];
        }
    }
    return w;
}

// Count distinct ids in a small list (n <= 27 for hex+midnodes etc.).
// O(n^2) is fine at this scale and avoids allocations.
inline int count_distinct(const std::int32_t *ids, int n) {
    int c = 0;
    for (int i = 0; i < n; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j) {
            if (ids[j] == ids[i]) {
                seen = true;
                break;
            }
        }
        if (!seen)
            ++c;
    }
    return c;
}

// Process a single cell, writing the rewritten id list to ``out_ids``
// and the new cell type to ``out_type``. Returns the new size; 0
// signals "drop this cell".
template <typename IndexT>
inline int rewrite_cell(std::uint8_t ct, const IndexT *in_ids, int n_in,
                        const std::int32_t *point_map, bool remove_degenerate,
                        std::int32_t *out_ids, std::uint8_t &out_type,
                        bool &needs_polyhedron_fallback) {

    // Step 1: remap; any SENTINEL means the cell references a dropped
    // point and must be dropped (regardless of remove_degenerate per
    // M5).
    bool has_dropped = false;
    for (int i = 0; i < n_in; ++i) {
        std::int32_t m = point_map[in_ids[i]];
        if (m == SENTINEL_ID) {
            has_dropped = true;
            break;
        }
        out_ids[i] = m;
    }
    if (has_dropped)
        return 0;
    out_type = ct;

    if (!remove_degenerate) {
        return n_in;
    }

    switch (ct) {
    case VTK_VERTEX: {
        return n_in == 1 ? 1 : 0;
    }
    case VTK_POLY_VERTEX: {
        // Dedup arbitrarily (order-independent). Stable O(n^2) below
        // is fine for typical poly-vertex sizes.
        int w = 0;
        for (int i = 0; i < n_in; ++i) {
            bool seen = false;
            for (int j = 0; j < w; ++j) {
                if (out_ids[j] == out_ids[i]) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                std::int32_t v = out_ids[i];
                out_ids[w++] = v;
            } else {
                // shift: we may have written in-place — copy from in_ids
            }
        }
        // Easier: rebuild by remap; we already wrote out_ids = remap.
        // The above loop is potentially buggy if i != w. Rewrite:
        int real_w = 0;
        std::int32_t tmp[256];
        int cap = n_in < 256 ? n_in : 256;
        for (int i = 0; i < cap; ++i)
            tmp[i] = point_map[in_ids[i]];
        for (int i = 0; i < cap; ++i) {
            bool seen = false;
            for (int j = 0; j < real_w; ++j) {
                if (out_ids[j] == tmp[i]) {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                out_ids[real_w++] = tmp[i];
        }
        return real_w;
    }
    case VTK_LINE: {
        if (n_in != 2)
            return 0;
        return (out_ids[0] == out_ids[1]) ? 0 : 2;
    }
    case VTK_POLY_LINE: {
        int w = dedup_adjacent(out_ids, n_in);
        return (w < 2) ? 0 : w;
    }
    case VTK_TRIANGLE: {
        if (n_in != 3)
            return 0;
        if (out_ids[0] == out_ids[1] || out_ids[1] == out_ids[2] || out_ids[0] == out_ids[2])
            return 0;
        return 3;
    }
    case VTK_TRIANGLE_STRIP: {
        // Strip explosion is handled at a higher level (in the cell
        // rewrite loop) because it produces multiple output cells.
        // Returning n_in keeps the strip; degenerate triangles inside
        // are dropped by downstream consumers. For our v1 we
        // conservatively keep the strip as-is after adjacent dedup of
        // size, which matches VTK behaviour.
        int w = dedup_adjacent(out_ids, n_in);
        return (w < 3) ? 0 : w;
    }
    case VTK_POLYGON: {
        int w = dedup_adjacent(out_ids, n_in);
        // Also fold the wrap-around duplicate (last == first).
        while (w >= 2 && out_ids[w - 1] == out_ids[0])
            --w;
        return (w < 3) ? 0 : w;
    }
    case VTK_QUAD: {
        if (n_in != 4)
            return 0;
        // Detect adjacent collapses (including wrap).
        int collapses = 0;
        for (int i = 0; i < 4; ++i) {
            if (out_ids[i] == out_ids[(i + 1) & 3])
                ++collapses;
        }
        if (collapses == 0) {
            // Distinct corners required.
            int d = count_distinct(out_ids, 4);
            return (d == 4) ? 4 : 0;
        }
        if (collapses == 1) {
            // Emit a triangle of the three distinct ids in cyclic
            // order.
            std::int32_t t[4];
            int k = 0;
            for (int i = 0; i < 4; ++i) {
                if (out_ids[i] != out_ids[(i + 1) & 3]) {
                    t[k++] = out_ids[i];
                }
            }
            // k should be 3.
            if (k != 3)
                return 0;
            for (int i = 0; i < 3; ++i)
                out_ids[i] = t[i];
            out_type = VTK_TRIANGLE;
            return 3;
        }
        return 0;
    }
    case VTK_TETRA: {
        if (n_in != 4)
            return 0;
        return (count_distinct(out_ids, 4) == 4) ? 4 : 0;
    }
    case VTK_HEXAHEDRON: {
        if (n_in != 8)
            return 0;
        return (count_distinct(out_ids, 8) == 8) ? 8 : 0;
    }
    case VTK_WEDGE: {
        if (n_in != 6)
            return 0;
        return (count_distinct(out_ids, 6) == 6) ? 6 : 0;
    }
    case VTK_PYRAMID: {
        if (n_in != 5)
            return 0;
        return (count_distinct(out_ids, 5) == 5) ? 5 : 0;
    }
    case VTK_QUADRATIC_TETRA: {
        // 10 nodes; corners are first 4.
        if (n_in != 10)
            return 0;
        return (count_distinct(out_ids, 4) == 4) ? 10 : 0;
    }
    case VTK_QUADRATIC_HEXAHEDRON: {
        if (n_in != 20)
            return 0;
        return (count_distinct(out_ids, 8) == 8) ? 20 : 0;
    }
    case VTK_QUADRATIC_WEDGE: {
        if (n_in != 15)
            return 0;
        return (count_distinct(out_ids, 6) == 6) ? 15 : 0;
    }
    case VTK_QUADRATIC_PYRAMID: {
        if (n_in != 13)
            return 0;
        return (count_distinct(out_ids, 5) == 5) ? 13 : 0;
    }
    case VTK_POLYHEDRON: {
        // M4: wrapper falls back to VTK for the whole mesh.
        needs_polyhedron_fallback = true;
        // Keep verbatim for now; the caller will discard our output.
        return n_in;
    }
    default: {
        // Unknown type: keep verbatim. We don't apply degeneracy
        // checks we can't reason about.
        return n_in;
    }
    }
}

} // namespace detail

// ------------------------------------------------------------------ //
// Public entry: run_clean.
//
// Allocates and fills ``out`` from ``in``. All phases A-G run; H is
// the Python wrapper's responsibility.
template <typename T, typename IndexT = std::int32_t>
inline void run_clean(const CleanInput<T, IndexT> &in, CleanOptions opts, CleanOutput<T> &out,
                      int n_threads) {
    using namespace detail;

    // Validate tolerance up front so callers get a clean exception
    // before we touch any buffers. NaN / negative tolerance are
    // user-facing errors, not silent zero-tolerance fallbacks.
    if (std::isnan(opts.tolerance)) {
        throw std::invalid_argument(
            "pyvista_algorithms.clean: tolerance is NaN; must be a non-negative finite value");
    }
    if (opts.tolerance < 0.0) {
        throw std::invalid_argument("pyvista_algorithms.clean: tolerance must be non-negative");
    }

    int nt = resolve_threads(n_threads);
    omp_set_num_threads(nt);

    CleanTrace _trace;
    _trace.start();

    out.points.clear();
    out.point_map.clear();
    out.source_map.clear();
    out.conn.clear();
    out.offsets.clear();
    out.cell_types.clear();
    out.original_cell_ids.clear();
    out.needs_polyhedron_fallback = false;
    out.has_nonfinite_input = false;

    const std::size_t n_pts = in.n_pts;
    if (n_pts == 0) {
        out.offsets.push_back(0);
        return;
    }

    // -------- Phase A: bbox sweep (only needed for tolerance > 0) -----
    BBox<T> bb;
    double diag = 0.0;
    double eps = 0.0;
    bool tolerance_mode = false;
    if (opts.tolerance > 0.0) {
        bb = compute_bbox(in.points, n_pts);
        diag = bbox_diagonal(bb);
        eps = opts.absolute ? opts.tolerance : (opts.tolerance * diag);
        // Underflow guard: promote to identical-merge if eps is tiny.
        if (eps <= 0.0) {
            tolerance_mode = false;
        } else if (eps >= diag && diag > 0.0) {
            // S-Skeptic-7: tolerance >= bbox_diagonal collapses
            // everything to one class. We still run tolerance mode
            // because all points share a class.
            tolerance_mode = true;
        } else {
            tolerance_mode = true;
        }
    }

    // -------- Phase B+C+D: dedup ------------------------------------
    bool dedup_fast_path = false;
    bool any_nonfinite = false;
    if (tolerance_mode) {
        // Tolerance mode keeps the sort + 27-cell stencil + union-find
        // pipeline (Phase E owns its own dedup).
        std::vector<KeyEntry> keys;
        std::vector<std::uint8_t> drop_mask;
        build_quantised_keys(in.points, n_pts, bb, eps, keys, drop_mask);
        std::sort(keys.begin(), keys.end(), key_lt);
        double eps_sq = eps * eps;
        segmented_unique_tolerance(keys, drop_mask, in.points, n_pts, eps_sq, out.point_map,
                                   out.source_map, out.points);
#pragma omp parallel for schedule(static) reduction(|| : any_nonfinite)
        for (long long i = 0; i < (long long)n_pts; ++i) {
            if (drop_mask[(std::size_t)i])
                any_nonfinite = true;
        }
    } else {
        // tolerance==0: parallel anchor-bucket hash dedup.
        std::vector<std::uint8_t> drop_mask;
        bool any_duplicates = false;
        parallel_morton_radix_dedup_identical(in.points, n_pts, opts.drop_nonfinite_points,
                                              out.point_map, out.source_map, out.points, drop_mask,
                                              any_duplicates);
        // Fast path is only valid when (a) no duplicates found AND
        // (b) no points were dropped (drop_mask is all zero). With
        // dropped points, Phase F still has to remove cells that
        // reference them.
        bool any_dropped = false;
#pragma omp parallel for schedule(static) reduction(|| : any_dropped, any_nonfinite)
        for (long long i = 0; i < (long long)n_pts; ++i) {
            if (drop_mask[(std::size_t)i]) {
                any_dropped = true;
                any_nonfinite = true;
            }
        }
        dedup_fast_path = !any_duplicates && !any_dropped;
    }
    out.has_nonfinite_input = any_nonfinite;
    _trace.mark("dedup");

    // -------- Phase F: cell rewrite ---------------------------------
    // Fast path: dedup found no duplicates AND tolerance==0 →
    // point_map is identity and no cell will collapse. Skip the
    // per-type degenerate check and copy the connectivity verbatim.
    // We still scan cell_types to detect VTK_POLYHEDRON so the
    // wrapper can fall back per M4.
    bool needs_poly = false;
    if (dedup_fast_path) {
        std::int32_t conn_len = (in.n_cells > 0) ? (std::int32_t)in.offsets[in.n_cells] : 0;
        parallel_narrow_copy_to_i32<IndexT>(in.conn, (std::size_t)conn_len, out.conn);
        parallel_narrow_copy_to_i32<IndexT>(in.offsets, (std::size_t)(in.n_cells + 1), out.offsets);
        out.cell_types.assign(in.cell_types, in.cell_types + (std::size_t)in.n_cells);
        out.original_cell_ids.resize((std::size_t)in.n_cells);
#pragma omp parallel for schedule(static)
        for (long long c = 0; c < (long long)in.n_cells; ++c) {
            out.original_cell_ids[(std::size_t)c] = (std::int32_t)c;
        }
        bool poly_local = false;
#pragma omp parallel for schedule(static) reduction(|| : poly_local)
        for (long long c = 0; c < (long long)in.n_cells; ++c) {
            if (in.cell_types[c] == VTK_POLYHEDRON)
                poly_local = true;
        }
        needs_poly = poly_local;
        out.needs_polyhedron_fallback = needs_poly;
    } else if (!opts.remove_degenerate_cells) {
        // ``remove_degenerate_cells == false``: just remap connectivity
        // through point_map. No degeneracy checks; preserve every cell
        // verbatim (offsets/types unchanged). Cells that reference a
        // SENTINEL-mapped point are still dropped per M5 — we do this
        // pass once with a per-cell drop flag.
        const std::int32_t n_cells = in.n_cells;
        const std::int32_t total_conn = (n_cells > 0) ? in.offsets[n_cells] : 0;
        // Remap conn in place to a scratch buffer; detect dropped
        // points if any. If none dropped, copy offsets/types directly.
        std::vector<std::int32_t> remapped((std::size_t)total_conn);
        std::atomic<int> any_drop_atomic(0);
        bool any_poly = false;
#pragma omp parallel reduction(|| : any_poly)
        {
            int local_drop = 0;
            bool local_poly = false;
#pragma omp for schedule(static)
            for (long long k = 0; k < (long long)total_conn; ++k) {
                std::int32_t m = out.point_map[(std::size_t)in.conn[k]];
                remapped[(std::size_t)k] = m;
                if (m == SENTINEL_ID)
                    local_drop = 1;
            }
            if (local_drop)
                any_drop_atomic.fetch_or(1, std::memory_order_relaxed);
#pragma omp for schedule(static) nowait
            for (long long c = 0; c < (long long)n_cells; ++c) {
                if (in.cell_types[c] == VTK_POLYHEDRON)
                    local_poly = true;
            }
            any_poly = any_poly || local_poly;
        }
        needs_poly = any_poly;
        out.needs_polyhedron_fallback = needs_poly;
        if (!any_drop_atomic.load(std::memory_order_relaxed)) {
            out.conn = std::move(remapped);
            parallel_narrow_copy_to_i32<IndexT>(in.offsets, (std::size_t)(n_cells + 1),
                                                out.offsets);
            out.cell_types.assign(in.cell_types, in.cell_types + (std::size_t)n_cells);
            out.original_cell_ids.resize((std::size_t)n_cells);
#pragma omp parallel for schedule(static)
            for (long long c = 0; c < (long long)n_cells; ++c) {
                out.original_cell_ids[(std::size_t)c] = (std::int32_t)c;
            }
        } else {
            // Slow path: drop cells with SENTINEL ids.
            std::vector<std::uint8_t> kept((std::size_t)n_cells, 0);
#pragma omp parallel for schedule(static)
            for (long long c = 0; c < (long long)n_cells; ++c) {
                std::int32_t b = in.offsets[c], e = in.offsets[c + 1];
                bool ok = true;
                for (std::int32_t i = b; i < e; ++i) {
                    if (remapped[(std::size_t)i] == SENTINEL_ID) {
                        ok = false;
                        break;
                    }
                }
                kept[(std::size_t)c] = ok ? 1 : 0;
            }
            std::vector<std::int32_t> kept_scan((std::size_t)n_cells + 1, 0);
            std::vector<std::int32_t> conn_scan((std::size_t)n_cells + 1, 0);
            for (std::int32_t c = 0; c < n_cells; ++c) {
                kept_scan[(std::size_t)c + 1] = kept_scan[(std::size_t)c] + kept[(std::size_t)c];
                conn_scan[(std::size_t)c + 1] =
                    conn_scan[(std::size_t)c] +
                    (kept[(std::size_t)c] ? (in.offsets[c + 1] - in.offsets[c]) : 0);
            }
            std::int32_t nk = kept_scan[(std::size_t)n_cells];
            std::int32_t nkc = conn_scan[(std::size_t)n_cells];
            out.conn.resize((std::size_t)nkc);
            out.offsets.assign((std::size_t)nk + 1, 0);
            out.cell_types.resize((std::size_t)nk);
            out.original_cell_ids.resize((std::size_t)nk);
#pragma omp parallel for schedule(static)
            for (long long c = 0; c < (long long)n_cells; ++c) {
                if (!kept[(std::size_t)c])
                    continue;
                std::int32_t ni = kept_scan[(std::size_t)c];
                std::int32_t co = conn_scan[(std::size_t)c];
                std::int32_t b = in.offsets[c], e = in.offsets[c + 1];
                std::int32_t w = e - b;
                std::memcpy(out.conn.data() + co, remapped.data() + b,
                            (std::size_t)w * sizeof(std::int32_t));
                out.offsets[(std::size_t)ni + 1] = co + w;
                out.cell_types[(std::size_t)ni] = in.cell_types[c];
                out.original_cell_ids[(std::size_t)ni] = (std::int32_t)c;
            }
        }
    } else {
        // Try a "remap + verify" fast path before falling through to the
        // full 3-pass rewrite. Most cells in clean inputs survive the
        // degeneracy check — the slow path's dominant cost is the
        // upfront scratch allocation and per-cell switch dispatch.
        const std::int32_t n_cells = in.n_cells;
        const std::int32_t total_conn = (n_cells > 0) ? in.offsets[n_cells] : 0;
        // Quick triage: scan cell_types. If they're all "simple"
        // (TRIANGLE / QUAD / TETRA / HEX / WEDGE / PYRAMID / LINE /
        // VERTEX) and none are POLYHEDRON, the fast path is eligible.
        bool fast_eligible = true;
        bool any_poly_pre = false;
#pragma omp parallel for schedule(static) reduction(&& : fast_eligible) reduction(|| : any_poly_pre)
        for (long long c = 0; c < (long long)n_cells; ++c) {
            std::uint8_t t = in.cell_types[c];
            if (t == VTK_POLYHEDRON)
                any_poly_pre = true;
            // Strips, poly-types, and quadratic cells use complex
            // degeneracy logic — keep them on the slow path.
            if (!(t == VTK_TRIANGLE || t == VTK_QUAD || t == VTK_TETRA || t == VTK_HEXAHEDRON ||
                  t == VTK_WEDGE || t == VTK_PYRAMID || t == VTK_LINE || t == VTK_VERTEX)) {
                fast_eligible = false;
            }
        }
        if (any_poly_pre) {
            out.needs_polyhedron_fallback = true;
            needs_poly = true;
            // The wrapper discards our output; emit empty arrays.
            out.conn.clear();
            out.offsets.assign(1, 0);
            out.cell_types.clear();
            out.original_cell_ids.clear();
            goto phase_f_done;
        }
        if (fast_eligible) {
            // Single-pass remap + per-cell degeneracy check. We don't
            // need a stage buffer because the cell rules either keep
            // the cell verbatim (after remap) or drop it whole — no
            // cell ever shrinks below its input length here (TRIANGLE,
            // QUAD-collapse-to-triangle is an exception: the QUAD rule
            // emits a triangle of 3 distinct ids. We handle that by
            // checking distinct-count == n_in.)
            //
            // For QUAD: if remapped corners have a single collapse the
            // cell becomes a 3-point TRIANGLE — handled by the slow
            // path. The fast path bails to slow if we detect a 1-corner
            // collapse on any QUAD.
            std::vector<std::int32_t> remapped;
            resize_noinit_parallel(remapped, (std::size_t)total_conn);
            std::vector<std::uint8_t> kept;
            resize_noinit_parallel(kept, (std::size_t)n_cells);
#pragma omp parallel for schedule(static)
            for (long long c = 0; c < (long long)n_cells; ++c) {
                kept[(std::size_t)c] = 1;
            }
            std::atomic<int> need_slow(0);
#pragma omp parallel for schedule(static)
            for (long long c = 0; c < (long long)n_cells; ++c) {
                std::int32_t b = in.offsets[c], e = in.offsets[c + 1];
                std::int32_t w = e - b;
                std::uint8_t t = in.cell_types[c];
                // Remap and detect SENTINEL.
                bool sentinel = false;
                std::int32_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
                for (std::int32_t i = 0; i < w; ++i) {
                    std::int32_t m = out.point_map[(std::size_t)in.conn[b + i]];
                    remapped[(std::size_t)(b + i)] = m;
                    if (m == SENTINEL_ID)
                        sentinel = true;
                }
                if (sentinel) {
                    kept[(std::size_t)c] = 0;
                    continue;
                }
                if (!opts.remove_degenerate_cells) {
                    continue;
                }
                // Per-type degeneracy check.
                switch (t) {
                case VTK_TRIANGLE:
                    if (w != 3) {
                        kept[(std::size_t)c] = 0;
                        break;
                    }
                    v0 = remapped[(std::size_t)b];
                    v1 = remapped[(std::size_t)b + 1];
                    v2 = remapped[(std::size_t)b + 2];
                    if (v0 == v1 || v1 == v2 || v0 == v2)
                        kept[(std::size_t)c] = 0;
                    break;
                case VTK_LINE:
                    if (w != 2 || remapped[(std::size_t)b] == remapped[(std::size_t)b + 1])
                        kept[(std::size_t)c] = 0;
                    break;
                case VTK_VERTEX:
                    if (w != 1)
                        kept[(std::size_t)c] = 0;
                    break;
                case VTK_QUAD: {
                    if (w != 4) {
                        kept[(std::size_t)c] = 0;
                        break;
                    }
                    v0 = remapped[(std::size_t)b];
                    v1 = remapped[(std::size_t)b + 1];
                    v2 = remapped[(std::size_t)b + 2];
                    v3 = remapped[(std::size_t)b + 3];
                    int collapses = 0;
                    if (v0 == v1)
                        ++collapses;
                    if (v1 == v2)
                        ++collapses;
                    if (v2 == v3)
                        ++collapses;
                    if (v3 == v0)
                        ++collapses;
                    if (collapses == 1) {
                        // QUAD collapses to TRIANGLE — escape to slow.
                        need_slow.store(1, std::memory_order_relaxed);
                    } else if (collapses > 1) {
                        kept[(std::size_t)c] = 0;
                    } else if (v0 == v2 || v1 == v3) {
                        // Diagonal collapse: drop.
                        kept[(std::size_t)c] = 0;
                    }
                    break;
                }
                case VTK_TETRA:
                case VTK_PYRAMID:
                case VTK_WEDGE:
                case VTK_HEXAHEDRON: {
                    int n_distinct = 0;
                    bool dup = false;
                    for (std::int32_t i = 0; i < w; ++i) {
                        std::int32_t vi = remapped[(std::size_t)(b + i)];
                        bool seen = false;
                        for (std::int32_t j = 0; j < i; ++j) {
                            if (remapped[(std::size_t)(b + j)] == vi) {
                                seen = true;
                                break;
                            }
                        }
                        if (seen)
                            dup = true;
                        else
                            ++n_distinct;
                    }
                    (void)n_distinct;
                    if (dup)
                        kept[(std::size_t)c] = 0;
                    break;
                }
                default:
                    break;
                }
            }
            if (!need_slow.load(std::memory_order_relaxed)) {
                // Compact kept cells.
                // Quick check: if all kept, we can short-circuit the
                // compaction (offsets / types are unchanged).
                std::int32_t any_dropped_cell = 0;
#pragma omp parallel for schedule(static) reduction(| : any_dropped_cell)
                for (long long c = 0; c < (long long)n_cells; ++c) {
                    if (!kept[(std::size_t)c])
                        any_dropped_cell = 1;
                }
                if (!any_dropped_cell) {
                    out.conn = std::move(remapped);
                    resize_noinit_parallel(out.offsets, (std::size_t)(n_cells + 1));
                    resize_noinit_parallel(out.cell_types, (std::size_t)n_cells);
                    resize_noinit_parallel(out.original_cell_ids, (std::size_t)n_cells);
#pragma omp parallel for schedule(static)
                    for (long long c = 0; c < (long long)(n_cells + 1); ++c) {
                        out.offsets[(std::size_t)c] = in.offsets[c];
                    }
#pragma omp parallel for schedule(static)
                    for (long long c = 0; c < (long long)n_cells; ++c) {
                        out.cell_types[(std::size_t)c] = in.cell_types[c];
                        out.original_cell_ids[(std::size_t)c] = (std::int32_t)c;
                    }
                    goto phase_f_done;
                }
                // Compact.
                std::vector<std::int32_t> kept_scan((std::size_t)n_cells + 1, 0);
                std::vector<std::int32_t> conn_scan((std::size_t)n_cells + 1, 0);
                for (std::int32_t c = 0; c < n_cells; ++c) {
                    kept_scan[(std::size_t)c + 1] =
                        kept_scan[(std::size_t)c] + kept[(std::size_t)c];
                    conn_scan[(std::size_t)c + 1] =
                        conn_scan[(std::size_t)c] +
                        (kept[(std::size_t)c] ? (in.offsets[c + 1] - in.offsets[c]) : 0);
                }
                std::int32_t nk = kept_scan[(std::size_t)n_cells];
                std::int32_t nkc = conn_scan[(std::size_t)n_cells];
                out.conn.resize((std::size_t)nkc);
                out.offsets.assign((std::size_t)nk + 1, 0);
                out.cell_types.resize((std::size_t)nk);
                out.original_cell_ids.resize((std::size_t)nk);
#pragma omp parallel for schedule(static)
                for (long long c = 0; c < (long long)n_cells; ++c) {
                    if (!kept[(std::size_t)c])
                        continue;
                    std::int32_t ni = kept_scan[(std::size_t)c];
                    std::int32_t co = conn_scan[(std::size_t)c];
                    std::int32_t b = in.offsets[c], e = in.offsets[c + 1];
                    std::int32_t w = e - b;
                    std::memcpy(out.conn.data() + co, remapped.data() + b,
                                (std::size_t)w * sizeof(std::int32_t));
                    out.offsets[(std::size_t)ni + 1] = co + w;
                    out.cell_types[(std::size_t)ni] = in.cell_types[c];
                    out.original_cell_ids[(std::size_t)ni] = (std::int32_t)c;
                }
                goto phase_f_done;
            }
            // Fallthrough to slow path.
        }
        {
            // Parallel Phase F (slow path): three-pass cell rewrite.
            //   Pass A (parallel): for each input cell, run rewrite_cell
            //     into a scratch buffer (sized to the input conn length;
            //     output npts is always <= input npts for our cell rules
            //     so we can stage the rewritten ids at the cell's input
            //     offset). Records per-cell output length, output type,
            //     and drop flag.
            //   Pass B (serial scan): exclusive scans on `kept` and
            //     `out_npts` produce final cell offset + connectivity
            //     offset per cell.
            //   Pass C (parallel): each kept cell copies its rewritten
            //     ids from the scratch buffer into the final out.conn /
            //     out.offsets / out.cell_types / out.original_cell_ids
            //     arrays at deterministic positions.
            //
            // Invariants preserved:
            //   * Kept cells appear in input order (offset assignment is
            //     a monotonic scan over the input cell ordering).
            //   * vtkOriginalCellIds is non-decreasing.
            //
            // Polyhedron detection: needs_polyhedron_fallback is the OR
            // reduction of per-cell flags. We don't bother with the rest
            // of the work in that case because the wrapper discards us,
            // but emitting a valid (even partial) output is harmless and
            // simplifies the code.
            const std::int32_t n_cells = in.n_cells;
            const std::int32_t total_conn = (n_cells > 0) ? in.offsets[n_cells] : 0;

            // Scratch buffer for staged per-cell rewritten ids. We index
            // it by the input offset of each cell — since rewrite_cell
            // never grows a cell, the cell's output fits in
            // [in.offsets[c], in.offsets[c] + out_npts[c]).
            std::vector<std::int32_t> stage_ids((std::size_t)total_conn);
            std::vector<std::int32_t> cell_out_npts((std::size_t)n_cells, 0);
            std::vector<std::uint8_t> cell_out_type((std::size_t)n_cells, 0);
            std::vector<std::uint8_t> cell_kept((std::size_t)n_cells, 0);

            bool any_poly = false;
#pragma omp parallel reduction(|| : any_poly)
            {
                bool local_poly = false;
#pragma omp for schedule(static)
                for (long long cc = 0; cc < (long long)n_cells; ++cc) {
                    std::int32_t c = (std::int32_t)cc;
                    std::int32_t beg = in.offsets[c];
                    std::int32_t end = in.offsets[c + 1];
                    int n_in_cell = (int)(end - beg);
                    if (n_in_cell <= 0 || n_in_cell > 1024) {
                        cell_kept[(std::size_t)c] = 0;
                        cell_out_npts[(std::size_t)c] = 0;
                        continue;
                    }
                    std::uint8_t out_type = 0;
                    int w = rewrite_cell(in.cell_types[c], in.conn + beg, n_in_cell,
                                         out.point_map.data(), opts.remove_degenerate_cells,
                                         stage_ids.data() + beg, out_type, local_poly);
                    if (w <= 0) {
                        cell_kept[(std::size_t)c] = 0;
                        cell_out_npts[(std::size_t)c] = 0;
                    } else {
                        cell_kept[(std::size_t)c] = 1;
                        cell_out_npts[(std::size_t)c] = (std::int32_t)w;
                        cell_out_type[(std::size_t)c] = out_type;
                    }
                }
                any_poly = any_poly || local_poly;
            }
            needs_poly = any_poly;
            out.needs_polyhedron_fallback = needs_poly;

            // Pass B: exclusive scans (serial — cell_count typically <<
            // point_count, scan cost is negligible vs rewrite cost).
            std::vector<std::int32_t> cell_kept_scan((std::size_t)n_cells + 1, 0);
            std::vector<std::int32_t> conn_off_scan((std::size_t)n_cells + 1, 0);
            for (std::int32_t c = 0; c < n_cells; ++c) {
                cell_kept_scan[(std::size_t)c + 1] =
                    cell_kept_scan[(std::size_t)c] + (std::int32_t)cell_kept[(std::size_t)c];
                conn_off_scan[(std::size_t)c + 1] =
                    conn_off_scan[(std::size_t)c] + cell_out_npts[(std::size_t)c];
            }
            std::int32_t n_kept_cells = cell_kept_scan[(std::size_t)n_cells];
            std::int32_t n_kept_conn = conn_off_scan[(std::size_t)n_cells];

            out.offsets.assign((std::size_t)n_kept_cells + 1, 0);
            out.conn.resize((std::size_t)n_kept_conn);
            out.cell_types.resize((std::size_t)n_kept_cells);
            out.original_cell_ids.resize((std::size_t)n_kept_cells);

            // Pass C: parallel write into final positions.
#pragma omp parallel for schedule(static)
            for (long long cc = 0; cc < (long long)n_cells; ++cc) {
                std::int32_t c = (std::int32_t)cc;
                if (!cell_kept[(std::size_t)c])
                    continue;
                std::int32_t new_cell_idx = cell_kept_scan[(std::size_t)c];
                std::int32_t conn_base = conn_off_scan[(std::size_t)c];
                std::int32_t w = cell_out_npts[(std::size_t)c];
                std::int32_t beg = in.offsets[c];
                const std::int32_t *src = stage_ids.data() + beg;
                std::int32_t *dst = out.conn.data() + conn_base;
                for (std::int32_t k = 0; k < w; ++k) {
                    dst[k] = src[k];
                }
                out.offsets[(std::size_t)new_cell_idx + 1] = conn_base + w;
                out.cell_types[(std::size_t)new_cell_idx] = cell_out_type[(std::size_t)c];
                out.original_cell_ids[(std::size_t)new_cell_idx] = c;
            }
            // out.offsets[0] = 0 already; out.offsets[i] for i = 1..n_kept
            // is filled in by Pass C. Each new_cell_idx receives exactly
            // one write (cells are uniquely ordered), so no race.
        } // end slow-path scope
    } // end outer else (remove_degenerate_cells == true)
phase_f_done:;
    _trace.mark("phase F");

    // -------- Phase G: unused-point removal -------------------------
    if (opts.remove_unused_points && !out.points.empty()) {
        std::size_t n_unique = out.source_map.size();
        std::vector<std::uint8_t> touched(n_unique, 0);
        // Parallel mark — races on byte writes of value 1 are benign
        // (idempotent store).
#pragma omp parallel for schedule(static)
        for (long long k = 0; k < (long long)out.conn.size(); ++k) {
            std::int32_t id = out.conn[(std::size_t)k];
            if (id >= 0 && (std::size_t)id < n_unique) {
                touched[(std::size_t)id] = 1;
            }
        }
        // Fast skip: if every point is touched, no compaction is needed.
        // Common on already-clean inputs with remove_unused_points=True.
        bool all_used = true;
        // Parallel reduction to check.
#pragma omp parallel for schedule(static) reduction(&& : all_used)
        for (long long i = 0; i < (long long)n_unique; ++i) {
            if (!touched[(std::size_t)i])
                all_used = false;
        }
        if (all_used) {
            return; // point_map / source_map / points / conn already correct
        }
        // Prefix-sum -> renumber.
        std::vector<std::int32_t> renumber(n_unique, SENTINEL_ID);
        std::int32_t w = 0;
        for (std::size_t i = 0; i < n_unique; ++i) {
            if (touched[i])
                renumber[i] = w++;
        }
        std::int32_t n_kept = w;
        // Guarantee n_output >= 1 when n_unique >= 1: if compaction
        // would erase the entire unique-point set (e.g. tolerance >=
        // bbox-diagonal collapses every cell to degenerate so no
        // connectivity remains), retain the canonical class
        // representative so the user gets a non-empty point set.
        if (n_kept == 0 && n_unique > 0) {
            renumber[0] = 0;
            n_kept = 1;
        }
        // Compact points / source_map.
        std::vector<T> new_points((std::size_t)n_kept * 3);
        std::vector<std::int32_t> new_source((std::size_t)n_kept);
        for (std::size_t i = 0; i < n_unique; ++i) {
            std::int32_t nid = renumber[i];
            if (nid == SENTINEL_ID)
                continue;
            for (int d = 0; d < 3; ++d) {
                new_points[(std::size_t)nid * 3 + d] = out.points[i * 3 + d];
            }
            new_source[(std::size_t)nid] = out.source_map[i];
        }
        out.points.swap(new_points);
        out.source_map.swap(new_source);
#pragma omp parallel for schedule(static)
        for (long long k = 0; k < (long long)out.conn.size(); ++k) {
            out.conn[(std::size_t)k] = renumber[(std::size_t)out.conn[(std::size_t)k]];
        }
#pragma omp parallel for schedule(static)
        for (long long i = 0; i < (long long)out.point_map.size(); ++i) {
            std::int32_t m = out.point_map[(std::size_t)i];
            if (m == SENTINEL_ID)
                continue;
            out.point_map[(std::size_t)i] = renumber[(std::size_t)m];
        }
    }
    _trace.mark("phase G");
}

} // namespace clean
} // namespace pvu
