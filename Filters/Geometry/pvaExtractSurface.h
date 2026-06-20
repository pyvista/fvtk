// SPDX-License-Identifier: MIT
//
// Vendored from pyvista-algorithms (https://github.com/banesullivan/pyvista-algorithms)
//   src/cpp/extract_surface.hpp  — MIT licensed (Bane Sullivan and contributors).
//
// Fast parallel boundary-surface extraction for vtkUnstructuredGrid: a
// re-implementation of vtkDataSetSurfaceFilter's chained-hash face dedup with
// per-cell-type inner loops, 32-bit ids, a bump arena, and an OpenMP parallel
// path. fvtk calls fse::extract_surface() directly (no nanobind) from the
// vtkDataSetSurfaceFilter fast path when the opt-in fast mode is enabled
// (fvtk.EnableFast() / FVTK_FAST). Output is order-relaxed (cells and surface
// points are emitted in a thread-/hash-dependent order). Unmodified upstream
// source below this banner.
//
// Fast boundary surface extraction from VTK-style unstructured grids.
//
// ---------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------
//
// This is a re-implementation of vtkDataSetSurfaceFilter's
// chained-hash face dedup, with several wins:
//
//   * Per-cell-type specialised inner loops (no virtual dispatch, no
//     per-face table lookup). HexLoop knows a hex has 6 quad faces,
//     TetLoop knows 4 triangles, etc. This eliminates a few branches
//     per face and keeps instruction cache hot.
//
//   * 24-byte chain entries vs. VTK's variable-size (32 B header +
//     N*8 B for ptArray). Two of ours fit in 64 B of pool so chain
//     traversal is cache-friendly.
//
//   * 32-bit point IDs throughout (vtkIdType is 64-bit on most builds
//     — half the cache footprint, doubles useful slots per cache
//     line).
//
//   * Bump-allocated arena (one big posix_memalign + lazy mmap zero
//     pages for the bucket-head array). No per-entry malloc. No
//     pre-count pass — we just bump until done.
//
// Algorithm (per face):
//   1. Find min-vertex (anchor). Rotate so anchor sits at position 0;
//      preserve the cell-local cyclic ordering of the remaining
//      vertices so that two cells sharing a face produce REVERSED
//      "other" tuples.
//   2. Walk bucket_head[anchor]'s chain. For each entry:
//        - tri  : match if (other[0],other[1]) equals (b,c) or (c,b)
//        - quad : prefilter on opposite vertex (other[1] == c) — c is
//                 invariant under rotation/reflection — then check
//                 (other[0],other[2]) against (b,d) or (d,b).
//        - if match: mark sibling INTERIOR, skip insert.
//   3. No match: bump-allocate a new entry, prepend to bucket chain.
//
// Emit pass: walk all chains, emit UNIQUE entries in cell-local
// orientation (winding preserved via the original face template).
//
// ---------------------------------------------------------------------
// Parallel path
// ---------------------------------------------------------------------
//
// The parallel path keeps the per-vertex anchor-bucket layout from the
// previous version (contiguous bucket via Pass A pre-count + Pass B
// prefix sum). Atomics on per-bucket cursors and a tri-state status
// byte handle within-bucket races. This avoids the chain-walk cost
// of repeated cache-misses across multiple threads racing for the
// same anchor head.
//
// ---------------------------------------------------------------------
// Cell types: TETRA, HEXAHEDRON, WEDGE, PYRAMID; 2D TRIANGLE / QUAD
// pass through verbatim.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <omp.h>

// Portable always-inline. MSVC uses __forceinline; clang/gcc support
// __attribute__((always_inline)).
#if defined(_MSC_VER)
#define FSE_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define FSE_FORCE_INLINE inline __attribute__((always_inline))
#else
#define FSE_FORCE_INLINE inline
#endif

// Portable count-trailing-zeros on a non-zero 64-bit word.
#if defined(_MSC_VER)
#include <intrin.h>
static FSE_FORCE_INLINE int fse_ctzll(unsigned long long w) {
    unsigned long idx;
    _BitScanForward64(&idx, w);
    return (int)idx;
}
#else
static FSE_FORCE_INLINE int fse_ctzll(unsigned long long w) {
    return __builtin_ctzll(w);
}
#endif

namespace fse {

// ------------------------------------------------------------------ //
// Cell type constants (VTK-compatible).
// ------------------------------------------------------------------ //
enum CellType : int {
    VTK_VERTEX = 1,
    VTK_LINE = 3,
    VTK_TRIANGLE = 5,
    VTK_QUAD = 9,
    VTK_TETRA = 10,
    VTK_VOXEL = 11,
    VTK_HEXAHEDRON = 12,
    VTK_WEDGE = 13,
    VTK_PYRAMID = 14,
    // Quadratic (mid-edge node) variants. Connectivity is
    //   [N corner verts ... M mid-edge verts]
    // Boundary-face dedup uses ONLY the corner verts (mid-edge
    // nodes are determined by the corner pair in a conforming
    // mesh). Emitted faces include all corner + mid-edge nodes.
    VTK_QUADRATIC_TETRA = 24,      // 10 nodes
    VTK_QUADRATIC_HEXAHEDRON = 25, // 20 nodes
    VTK_QUADRATIC_WEDGE = 26,      // 15 nodes
    VTK_QUADRATIC_PYRAMID = 27,    // 13 nodes
};

// ------------------------------------------------------------------ //
// Per-cell-type face tables (used by emit pass).
// ------------------------------------------------------------------ //
struct FaceTpl {
    int n_verts;
    int offsets[4];
};

constexpr FaceTpl TET_FACES[4] = {
    {3, {0, 2, 1, 0}},
    {3, {0, 1, 3, 0}},
    {3, {1, 2, 3, 0}},
    {3, {2, 0, 3, 0}},
};

constexpr FaceTpl HEX_FACES[6] = {
    {4, {0, 3, 2, 1}}, {4, {4, 5, 6, 7}}, {4, {0, 1, 5, 4}},
    {4, {1, 2, 6, 5}}, {4, {2, 3, 7, 6}}, {4, {3, 0, 4, 7}},
};

// Voxel layout (axis-aligned): 0=(0,0,0), 1=(1,0,0), 2=(0,1,0),
// 3=(1,1,0), 4=(0,0,1), 5=(1,0,1), 6=(0,1,1), 7=(1,1,1).
// Faces in CCW outward winding.
constexpr FaceTpl VOXEL_FACES[6] = {
    {4, {0, 2, 3, 1}}, // -k
    {4, {4, 5, 7, 6}}, // +k
    {4, {0, 1, 5, 4}}, // -j
    {4, {2, 6, 7, 3}}, // +j
    {4, {0, 4, 6, 2}}, // -i
    {4, {1, 3, 7, 5}}, // +i
};

constexpr FaceTpl WEDGE_FACES[5] = {
    {3, {0, 1, 2, 0}}, {3, {3, 5, 4, 0}}, {4, {0, 3, 4, 1}}, {4, {1, 4, 5, 2}}, {4, {2, 5, 3, 0}},
};

constexpr FaceTpl PYRAMID_FACES[5] = {
    {4, {0, 3, 2, 1}}, {3, {0, 1, 4, 0}}, {3, {1, 2, 4, 0}}, {3, {2, 3, 4, 0}}, {3, {3, 0, 4, 0}},
};

// ------------------------------------------------------------------ //
// Quadratic (mid-edge) node offsets per face. The CORNER offsets are
// shared with the linear analog (a quadratic tet's corner topology
// matches a linear tet's), so we only enumerate the mid-edge node
// connectivity-offsets for each face. n_mid == n_verts of the linear
// face (3 for tri, 4 for quad).
//
// The full emitted face is: corners[0..n-1] then mids[0..n-1].
// ------------------------------------------------------------------ //
struct FaceMidTpl {
    int n_mid;
    int offsets[4];
};

// VTK_QUADRATIC_TETRA: 10 nodes. Corners 0..3, mids 4..9 along edges:
//   4: 0-1, 5: 1-2, 6: 2-0, 7: 0-3, 8: 1-3, 9: 2-3
constexpr FaceMidTpl QUAD_TET_MID[4] = {
    {3, {6, 5, 4, 0}}, // face 0 corners (0,2,1) -> edges (2-0,2-1,0-1) -> mids 6,5,4
    {3, {4, 8, 7, 0}}, // face 1 corners (0,1,3) -> edges (0-1,1-3,0-3) -> mids 4,8,7
    {3, {5, 9, 8, 0}}, // face 2 corners (1,2,3) -> edges (1-2,2-3,1-3) -> mids 5,9,8
    {3, {6, 7, 9, 0}}, // face 3 corners (2,0,3) -> edges (2-0,0-3,2-3) -> mids 6,7,9
};

// VTK_QUADRATIC_HEXAHEDRON: 20 nodes. Corners 0..7, mids 8..19 along edges:
//   bottom ring  8:0-1  9:1-2 10:2-3 11:3-0
//   top    ring 12:4-5 13:5-6 14:6-7 15:7-4
//   verticals   16:0-4 17:1-5 18:2-6 19:3-7
constexpr FaceMidTpl QUAD_HEX_MID[6] = {
    {4, {11, 10, 9, 8}},   // face 0 (0,3,2,1)
    {4, {12, 13, 14, 15}}, // face 1 (4,5,6,7)
    {4, {8, 17, 12, 16}},  // face 2 (0,1,5,4)
    {4, {9, 18, 13, 17}},  // face 3 (1,2,6,5)
    {4, {10, 19, 14, 18}}, // face 4 (2,3,7,6)
    {4, {11, 16, 15, 19}}, // face 5 (3,0,4,7)
};

// VTK_QUADRATIC_WEDGE: 15 nodes. Corners 0..5, mids 6..14:
//   bottom tri  6:0-1  7:1-2  8:2-0
//   top    tri  9:3-4 10:4-5 11:5-3
//   verticals  12:0-3 13:1-4 14:2-5
constexpr FaceMidTpl QUAD_WEDGE_MID[5] = {
    {3, {6, 7, 8, 0}},    // face 0 tri (0,1,2)
    {3, {11, 10, 9, 0}},  // face 1 tri (3,5,4)
    {4, {12, 9, 13, 6}},  // face 2 quad (0,3,4,1)
    {4, {13, 10, 14, 7}}, // face 3 quad (1,4,5,2)
    {4, {14, 11, 12, 8}}, // face 4 quad (2,5,3,0)
};

// VTK_QUADRATIC_PYRAMID: 13 nodes. Corners 0..4 (apex=4), mids 5..12:
//   base ring  5:0-1  6:1-2  7:2-3  8:3-0
//   to apex    9:0-4 10:1-4 11:2-4 12:3-4
constexpr FaceMidTpl QUAD_PYR_MID[5] = {
    {4, {8, 7, 6, 5}},   // face 0 quad (0,3,2,1)
    {3, {5, 10, 9, 0}},  // face 1 tri (0,1,4)
    {3, {6, 11, 10, 0}}, // face 2 tri (1,2,4)
    {3, {7, 12, 11, 0}}, // face 3 tri (2,3,4)
    {3, {8, 9, 12, 0}},  // face 4 tri (3,0,4)
};

struct CellTypeTables {
    std::uint8_t faces_per_cell[256] = {};
    const FaceTpl *face_tpl[256] = {};
    // Mid-edge offsets per face for QUADRATIC cell types. nullptr
    // means "linear cell — emit corners only". Non-null means
    // emit corners + mids (the full quadratic face).
    const FaceMidTpl *face_mid_tpl[256] = {};

    constexpr CellTypeTables() {
        for (int i = 0; i < 256; ++i) {
            faces_per_cell[i] = 0;
            face_tpl[i] = nullptr;
            face_mid_tpl[i] = nullptr;
        }
        faces_per_cell[VTK_TETRA] = 4;
        face_tpl[VTK_TETRA] = TET_FACES;
        faces_per_cell[VTK_VOXEL] = 6;
        face_tpl[VTK_VOXEL] = VOXEL_FACES;
        faces_per_cell[VTK_HEXAHEDRON] = 6;
        face_tpl[VTK_HEXAHEDRON] = HEX_FACES;
        faces_per_cell[VTK_WEDGE] = 5;
        face_tpl[VTK_WEDGE] = WEDGE_FACES;
        faces_per_cell[VTK_PYRAMID] = 5;
        face_tpl[VTK_PYRAMID] = PYRAMID_FACES;

        // Quadratic types share corner topology with their linear
        // analog (the first N entries of the connectivity ARE the
        // corner verts). The mid-edge offsets are looked up at emit
        // time via face_mid_tpl.
        faces_per_cell[VTK_QUADRATIC_TETRA] = 4;
        face_tpl[VTK_QUADRATIC_TETRA] = TET_FACES;
        face_mid_tpl[VTK_QUADRATIC_TETRA] = QUAD_TET_MID;
        faces_per_cell[VTK_QUADRATIC_HEXAHEDRON] = 6;
        face_tpl[VTK_QUADRATIC_HEXAHEDRON] = HEX_FACES;
        face_mid_tpl[VTK_QUADRATIC_HEXAHEDRON] = QUAD_HEX_MID;
        faces_per_cell[VTK_QUADRATIC_WEDGE] = 5;
        face_tpl[VTK_QUADRATIC_WEDGE] = WEDGE_FACES;
        face_mid_tpl[VTK_QUADRATIC_WEDGE] = QUAD_WEDGE_MID;
        faces_per_cell[VTK_QUADRATIC_PYRAMID] = 5;
        face_tpl[VTK_QUADRATIC_PYRAMID] = PYRAMID_FACES;
        face_mid_tpl[VTK_QUADRATIC_PYRAMID] = QUAD_PYR_MID;
    }
};

inline const CellTypeTables &tables() {
    static const CellTypeTables T;
    return T;
}

inline bool is_2d_surface_cell(int ct) {
    return ct == VTK_TRIANGLE || ct == VTK_QUAD;
}

// ------------------------------------------------------------------ //
// Constants / dispatch.
// ------------------------------------------------------------------ //
constexpr std::int32_t SMALL_MESH_THRESHOLD = 16384;
constexpr int DEFAULT_MAX_THREADS = 8;

inline int resolved_threads(int requested) {
    int max_avail = omp_get_max_threads();
    if (requested <= 0)
        requested = DEFAULT_MAX_THREADS;
    return std::min(requested, max_avail);
}

// ------------------------------------------------------------------ //
// ChainEntry: one per UNIQUE face seen during the insert pass. Lives
// in a bump-allocated arena. Linked into bucket_head[anchor].
//
//   next      — 1-based arena index of next entry in bucket chain
//                 (0 = end). 1-based so 0 can encode "empty".
//   meta      — packed: bit 0 INTERIOR; bit 1 NV4; bits 2..4 face_idx;
//                 bits 5..31 cell_id (27 bits = 128 M cells).
//   v[3]      — rotated other-vertex IDs; v[2]==0 for triangle faces.
// ------------------------------------------------------------------ //
struct ChainEntry {
    std::uint32_t next;
    std::uint32_t meta;
    std::uint32_t v[3];
    std::uint32_t pad;
};
static_assert(sizeof(ChainEntry) == 24, "ChainEntry must be 24 B");

constexpr std::uint32_t META_INTERIOR_BIT = 0x1u;
constexpr std::uint32_t META_NV4_BIT = 0x2u;
constexpr int META_FACE_SHIFT = 2;
constexpr std::uint32_t META_FACE_MASK = 0x7u << META_FACE_SHIFT;
constexpr int META_CELL_SHIFT = 5;

inline std::uint32_t pack_meta(std::uint32_t cell_id, std::uint32_t face_idx, bool nv4) {
    return (cell_id << META_CELL_SHIFT) | ((face_idx & 0x7u) << META_FACE_SHIFT) |
           (nv4 ? META_NV4_BIT : 0u);
}
inline std::uint32_t unpack_cell(std::uint32_t meta) {
    return meta >> META_CELL_SHIFT;
}
inline std::uint32_t unpack_face(std::uint32_t meta) {
    return (meta & META_FACE_MASK) >> META_FACE_SHIFT;
}
inline bool is_nv4(std::uint32_t meta) {
    return meta & META_NV4_BIT;
}

// ------------------------------------------------------------------ //
// Result.
// ------------------------------------------------------------------ //
struct ExtractResult {
    std::vector<std::int32_t> tri_indices;
    std::vector<std::int32_t> quad_indices;
    std::vector<std::int32_t> tri_origin_cell;
    std::vector<std::int32_t> quad_origin_cell;
    // Quadratic faces (6-node tri / 8-node quad) — empty unless the
    // input grid contains VTK_QUADRATIC_* cell types.
    std::vector<std::int32_t> qtri_indices;  // 6 cols per face
    std::vector<std::int32_t> qquad_indices; // 8 cols per face
    std::vector<std::int32_t> qtri_origin_cell;
    std::vector<std::int32_t> qquad_origin_cell;
    // Exactly one of {points, points_f64} is populated by compact_points*,
    // matching the input dtype so float64 input round-trips losslessly.
    std::vector<float> points;
    std::vector<double> points_f64;
    std::vector<std::int32_t> point_map;
    std::int32_t n_points_used = 0;
};

// ------------------------------------------------------------------ //
// Helpers.
// ------------------------------------------------------------------ //
inline void *aligned_alloc_raw(std::size_t bytes, std::size_t align = 64) {
    void *p = nullptr;
#if defined(_WIN32)
    p = _aligned_malloc(bytes, align);
#else
    if (posix_memalign(&p, align, bytes) != 0)
        p = nullptr;
#endif
    if (!p)
        throw std::bad_alloc();
    return p;
}
inline void aligned_free_raw(void *p) {
    if (!p)
        return;
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

inline void append_2d(const std::int32_t *connectivity, const std::int32_t *offsets, std::int32_t c,
                      int ct, ExtractResult &out) {
    if (!is_2d_surface_cell(ct))
        return;
    std::int32_t s = offsets[c];
    std::int32_t nv = offsets[c + 1] - s;
    if (nv == 3) {
        out.tri_indices.push_back(connectivity[s]);
        out.tri_indices.push_back(connectivity[s + 1]);
        out.tri_indices.push_back(connectivity[s + 2]);
        out.tri_origin_cell.push_back(c);
    } else if (nv == 4) {
        out.quad_indices.push_back(connectivity[s]);
        out.quad_indices.push_back(connectivity[s + 1]);
        out.quad_indices.push_back(connectivity[s + 2]);
        out.quad_indices.push_back(connectivity[s + 3]);
        out.quad_origin_cell.push_back(c);
    }
}

// ------------------------------------------------------------------ //
// Insert primitives — these are the inner-loop hot path. Each takes
// the rotated (anchor + other) vertex tuple and inserts into the
// chained hash table.
// ------------------------------------------------------------------ //

// Insert a TRIANGLE face. v[0] is anchor (=bucket index), v[1], v[2]
// are the rotated other vertices.
//
// Two cells sharing a triangle face produce reversed (b,c) tuples
// after anchoring at min — so we check both orderings.
FSE_FORCE_INLINE
void insert_tri(std::uint32_t *bucket_head, ChainEntry *arena, std::uint32_t &cursor,
                std::uint32_t anchor, std::uint32_t b, std::uint32_t c, std::uint32_t cell_id,
                std::uint32_t face_idx) {
    std::uint32_t idx = bucket_head[anchor];
    while (idx) {
        ChainEntry &e = arena[idx - 1];
        if (!(e.meta & META_INTERIOR_BIT) && !(e.meta & META_NV4_BIT)) {
            if ((e.v[0] == b && e.v[1] == c) || (e.v[0] == c && e.v[1] == b)) {
                e.meta |= META_INTERIOR_BIT;
                return;
            }
        }
        idx = e.next;
    }
    // Append (prepend to chain so most-recent inserts hit first).
    std::uint32_t my = cursor++;
    ChainEntry &ne = arena[my];
    ne.next = bucket_head[anchor];
    ne.meta = pack_meta(cell_id, face_idx, /*nv4=*/false);
    ne.v[0] = b;
    ne.v[1] = c;
    ne.v[2] = 0;
    ne.pad = 0;
    bucket_head[anchor] = my + 1;
}

// Insert a QUAD face. v[0] anchor, v[1]=b, v[2]=c, v[3]=d (rotated).
//
// VTK's clever trick: after rotation by smallest, the OPPOSITE vertex
// (position 2, "c") is invariant under cyclic rotation/reflection of
// the quad. So checking c == stored_c is a single-uint32 prefilter
// that rejects most non-matches without further compares.
FSE_FORCE_INLINE
void insert_quad(std::uint32_t *bucket_head, ChainEntry *arena, std::uint32_t &cursor,
                 std::uint32_t anchor, std::uint32_t b, std::uint32_t c, std::uint32_t d,
                 std::uint32_t cell_id, std::uint32_t face_idx) {
    std::uint32_t idx = bucket_head[anchor];
    while (idx) {
        ChainEntry &e = arena[idx - 1];
        std::uint32_t m = e.meta;
        if (!(m & META_INTERIOR_BIT) && (m & META_NV4_BIT)) {
            if (e.v[1] == c) {
                if ((e.v[0] == b && e.v[2] == d) || (e.v[0] == d && e.v[2] == b)) {
                    e.meta = m | META_INTERIOR_BIT;
                    return;
                }
            }
        }
        idx = e.next;
    }
    std::uint32_t my = cursor++;
    ChainEntry &ne = arena[my];
    ne.next = bucket_head[anchor];
    ne.meta = pack_meta(cell_id, face_idx, /*nv4=*/true);
    ne.v[0] = b;
    ne.v[1] = c;
    ne.v[2] = d;
    ne.pad = 0;
    bucket_head[anchor] = my + 1;
}

// ------------------------------------------------------------------ //
// Per-cell-type specialised insert loops. Each computes anchor + the
// rotated other-vertex tuple for each face, then dispatches to the
// appropriate insert primitive. Inlined and branch-free w.r.t. cell
// type.
// ------------------------------------------------------------------ //

// Find min of 3 uint32 and return its index (0/1/2) along with the
// minimum value. Branchless via cmov.
FSE_FORCE_INLINE
int min3_idx(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t &mn) {
    int i = 0;
    mn = a;
    if (b < mn) {
        mn = b;
        i = 1;
    }
    if (c < mn) {
        mn = c;
        i = 2;
    }
    return i;
}

FSE_FORCE_INLINE
int min4_idx(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d,
             std::uint32_t &mn) {
    int i = 0;
    mn = a;
    if (b < mn) {
        mn = b;
        i = 1;
    }
    if (c < mn) {
        mn = c;
        i = 2;
    }
    if (d < mn) {
        mn = d;
        i = 3;
    }
    return i;
}

inline void process_tet(const std::int32_t *cell_pts, std::uint32_t c, std::uint32_t *bucket_head,
                        ChainEntry *arena, std::uint32_t &cursor) {
    // 4 triangle faces. Cell-local vertex ordering from TET_FACES.
    // Read all 4 cell vertices once to keep them in registers.
    const std::uint32_t p0 = (std::uint32_t)cell_pts[0];
    const std::uint32_t p1 = (std::uint32_t)cell_pts[1];
    const std::uint32_t p2 = (std::uint32_t)cell_pts[2];
    const std::uint32_t p3 = (std::uint32_t)cell_pts[3];

    // Face 0: {0, 2, 1}  -> verts (p0, p2, p1)
    // Face 1: {0, 1, 3}  -> (p0, p1, p3)
    // Face 2: {1, 2, 3}  -> (p1, p2, p3)
    // Face 3: {2, 0, 3}  -> (p2, p0, p3)

    auto do_tri = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc,
                      std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min3_idx(fa, fb, fc, mn);
        // Rotate so min is first; preserve cyclic order.
        std::uint32_t b, cc;
        if (m == 0) {
            b = fb;
            cc = fc;
        } else if (m == 1) {
            b = fc;
            cc = fa;
        } else {
            b = fa;
            cc = fb;
        }
        insert_tri(bucket_head, arena, cursor, mn, b, cc, c, face_idx);
    };

    do_tri(p0, p2, p1, 0);
    do_tri(p0, p1, p3, 1);
    do_tri(p1, p2, p3, 2);
    do_tri(p2, p0, p3, 3);
}

inline void process_hex(const std::int32_t *cell_pts, std::uint32_t c, std::uint32_t *bucket_head,
                        ChainEntry *arena, std::uint32_t &cursor) {
    const std::uint32_t p0 = (std::uint32_t)cell_pts[0];
    const std::uint32_t p1 = (std::uint32_t)cell_pts[1];
    const std::uint32_t p2 = (std::uint32_t)cell_pts[2];
    const std::uint32_t p3 = (std::uint32_t)cell_pts[3];
    const std::uint32_t p4 = (std::uint32_t)cell_pts[4];
    const std::uint32_t p5 = (std::uint32_t)cell_pts[5];
    const std::uint32_t p6 = (std::uint32_t)cell_pts[6];
    const std::uint32_t p7 = (std::uint32_t)cell_pts[7];

    auto do_quad = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc, std::uint32_t fd,
                       std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min4_idx(fa, fb, fc, fd, mn);
        std::uint32_t b, cc, dd;
        switch (m) {
        case 0:
            b = fb;
            cc = fc;
            dd = fd;
            break;
        case 1:
            b = fc;
            cc = fd;
            dd = fa;
            break;
        case 2:
            b = fd;
            cc = fa;
            dd = fb;
            break;
        default:
            b = fa;
            cc = fb;
            dd = fc;
            break; // 3
        }
        insert_quad(bucket_head, arena, cursor, mn, b, cc, dd, c, face_idx);
    };

    // Faces from HEX_FACES.
    do_quad(p0, p3, p2, p1, 0);
    do_quad(p4, p5, p6, p7, 1);
    do_quad(p0, p1, p5, p4, 2);
    do_quad(p1, p2, p6, p5, 3);
    do_quad(p2, p3, p7, p6, 4);
    do_quad(p3, p0, p4, p7, 5);
}

inline void process_voxel(const std::int32_t *cell_pts, std::uint32_t c, std::uint32_t *bucket_head,
                          ChainEntry *arena, std::uint32_t &cursor) {
    // Voxel layout differs from hex (axis-aligned).
    const std::uint32_t p0 = (std::uint32_t)cell_pts[0];
    const std::uint32_t p1 = (std::uint32_t)cell_pts[1];
    const std::uint32_t p2 = (std::uint32_t)cell_pts[2];
    const std::uint32_t p3 = (std::uint32_t)cell_pts[3];
    const std::uint32_t p4 = (std::uint32_t)cell_pts[4];
    const std::uint32_t p5 = (std::uint32_t)cell_pts[5];
    const std::uint32_t p6 = (std::uint32_t)cell_pts[6];
    const std::uint32_t p7 = (std::uint32_t)cell_pts[7];

    auto do_quad = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc, std::uint32_t fd,
                       std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min4_idx(fa, fb, fc, fd, mn);
        std::uint32_t b, cc, dd;
        switch (m) {
        case 0:
            b = fb;
            cc = fc;
            dd = fd;
            break;
        case 1:
            b = fc;
            cc = fd;
            dd = fa;
            break;
        case 2:
            b = fd;
            cc = fa;
            dd = fb;
            break;
        default:
            b = fa;
            cc = fb;
            dd = fc;
            break;
        }
        insert_quad(bucket_head, arena, cursor, mn, b, cc, dd, c, face_idx);
    };

    do_quad(p0, p2, p3, p1, 0);
    do_quad(p4, p5, p7, p6, 1);
    do_quad(p0, p1, p5, p4, 2);
    do_quad(p2, p6, p7, p3, 3);
    do_quad(p0, p4, p6, p2, 4);
    do_quad(p1, p3, p7, p5, 5);
}

inline void process_wedge(const std::int32_t *cell_pts, std::uint32_t c, std::uint32_t *bucket_head,
                          ChainEntry *arena, std::uint32_t &cursor) {
    const std::uint32_t p0 = (std::uint32_t)cell_pts[0];
    const std::uint32_t p1 = (std::uint32_t)cell_pts[1];
    const std::uint32_t p2 = (std::uint32_t)cell_pts[2];
    const std::uint32_t p3 = (std::uint32_t)cell_pts[3];
    const std::uint32_t p4 = (std::uint32_t)cell_pts[4];
    const std::uint32_t p5 = (std::uint32_t)cell_pts[5];

    auto do_tri = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc,
                      std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min3_idx(fa, fb, fc, mn);
        std::uint32_t b, cc;
        if (m == 0) {
            b = fb;
            cc = fc;
        } else if (m == 1) {
            b = fc;
            cc = fa;
        } else {
            b = fa;
            cc = fb;
        }
        insert_tri(bucket_head, arena, cursor, mn, b, cc, c, face_idx);
    };
    auto do_quad = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc, std::uint32_t fd,
                       std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min4_idx(fa, fb, fc, fd, mn);
        std::uint32_t b, cc, dd;
        switch (m) {
        case 0:
            b = fb;
            cc = fc;
            dd = fd;
            break;
        case 1:
            b = fc;
            cc = fd;
            dd = fa;
            break;
        case 2:
            b = fd;
            cc = fa;
            dd = fb;
            break;
        default:
            b = fa;
            cc = fb;
            dd = fc;
            break;
        }
        insert_quad(bucket_head, arena, cursor, mn, b, cc, dd, c, face_idx);
    };

    do_tri(p0, p1, p2, 0);
    do_tri(p3, p5, p4, 1);
    do_quad(p0, p3, p4, p1, 2);
    do_quad(p1, p4, p5, p2, 3);
    do_quad(p2, p5, p3, p0, 4);
}

inline void process_pyramid(const std::int32_t *cell_pts, std::uint32_t c,
                            std::uint32_t *bucket_head, ChainEntry *arena, std::uint32_t &cursor) {
    const std::uint32_t p0 = (std::uint32_t)cell_pts[0];
    const std::uint32_t p1 = (std::uint32_t)cell_pts[1];
    const std::uint32_t p2 = (std::uint32_t)cell_pts[2];
    const std::uint32_t p3 = (std::uint32_t)cell_pts[3];
    const std::uint32_t p4 = (std::uint32_t)cell_pts[4];

    auto do_tri = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc,
                      std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min3_idx(fa, fb, fc, mn);
        std::uint32_t b, cc;
        if (m == 0) {
            b = fb;
            cc = fc;
        } else if (m == 1) {
            b = fc;
            cc = fa;
        } else {
            b = fa;
            cc = fb;
        }
        insert_tri(bucket_head, arena, cursor, mn, b, cc, c, face_idx);
    };
    auto do_quad = [&](std::uint32_t fa, std::uint32_t fb, std::uint32_t fc, std::uint32_t fd,
                       std::uint32_t face_idx) {
        std::uint32_t mn;
        int m = min4_idx(fa, fb, fc, fd, mn);
        std::uint32_t b, cc, dd;
        switch (m) {
        case 0:
            b = fb;
            cc = fc;
            dd = fd;
            break;
        case 1:
            b = fc;
            cc = fd;
            dd = fa;
            break;
        case 2:
            b = fd;
            cc = fa;
            dd = fb;
            break;
        default:
            b = fa;
            cc = fb;
            dd = fc;
            break;
        }
        insert_quad(bucket_head, arena, cursor, mn, b, cc, dd, c, face_idx);
    };

    do_quad(p0, p3, p2, p1, 0);
    do_tri(p0, p1, p4, 1);
    do_tri(p1, p2, p4, 2);
    do_tri(p2, p3, p4, 3);
    do_tri(p3, p0, p4, 4);
}

// ------------------------------------------------------------------ //
// Single-threaded core.
// ------------------------------------------------------------------ //
// Rotate face vertex order so the cell-globally-smallest vertex
// is first. ``vtkDataSetSurfaceFilter`` does this, and downstream
// PyVista / VTK consumers (``.triangulate()`` diagonal,
// feature-edge detection, normals orientation) implicitly rely on
// it — when we deviate, the triangulation diagonal disagrees with
// VTK and downstream FE morphing tests regress.
//
// Linear path: rotate corners only.
// Quadratic path: rotate corners AND apply the matching cyclic
// rotation to the mid-edge offsets so the corner / mid-edge
// pairing stays consistent (mid k connects corner k to corner
// k+1 mod n).

FSE_FORCE_INLINE void emit_rotated_tri(std::int32_t v0, std::int32_t v1, std::int32_t v2,
                                       std::int32_t *out) {
    if (v0 <= v1 && v0 <= v2) {
        out[0] = v0;
        out[1] = v1;
        out[2] = v2;
    } else if (v1 <= v2) {
        out[0] = v1;
        out[1] = v2;
        out[2] = v0;
    } else {
        out[0] = v2;
        out[1] = v0;
        out[2] = v1;
    }
}

FSE_FORCE_INLINE void emit_rotated_qtri(std::int32_t v0, std::int32_t v1, std::int32_t v2,
                                        std::int32_t m0, std::int32_t m1, std::int32_t m2,
                                        std::int32_t *out) {
    if (v0 <= v1 && v0 <= v2) {
        out[0] = v0;
        out[1] = v1;
        out[2] = v2;
        out[3] = m0;
        out[4] = m1;
        out[5] = m2;
    } else if (v1 <= v2) {
        out[0] = v1;
        out[1] = v2;
        out[2] = v0;
        out[3] = m1;
        out[4] = m2;
        out[5] = m0;
    } else {
        out[0] = v2;
        out[1] = v0;
        out[2] = v1;
        out[3] = m2;
        out[4] = m0;
        out[5] = m1;
    }
}

FSE_FORCE_INLINE int min4_pos(std::int32_t v0, std::int32_t v1, std::int32_t v2, std::int32_t v3) {
    int m = 0;
    std::int32_t mn = v0;
    if (v1 < mn) {
        mn = v1;
        m = 1;
    }
    if (v2 < mn) {
        mn = v2;
        m = 2;
    }
    if (v3 < mn) {
        m = 3;
    }
    return m;
}

FSE_FORCE_INLINE void emit_rotated_quad(std::int32_t v0, std::int32_t v1, std::int32_t v2,
                                        std::int32_t v3, std::int32_t *out) {
    int m = min4_pos(v0, v1, v2, v3);
    switch (m) {
    case 0:
        out[0] = v0;
        out[1] = v1;
        out[2] = v2;
        out[3] = v3;
        return;
    case 1:
        out[0] = v1;
        out[1] = v2;
        out[2] = v3;
        out[3] = v0;
        return;
    case 2:
        out[0] = v2;
        out[1] = v3;
        out[2] = v0;
        out[3] = v1;
        return;
    default:
        out[0] = v3;
        out[1] = v0;
        out[2] = v1;
        out[3] = v2;
        return;
    }
}

FSE_FORCE_INLINE void emit_rotated_qquad(std::int32_t v0, std::int32_t v1, std::int32_t v2,
                                         std::int32_t v3, std::int32_t m0, std::int32_t m1,
                                         std::int32_t m2, std::int32_t m3, std::int32_t *out) {
    int m = min4_pos(v0, v1, v2, v3);
    switch (m) {
    case 0:
        out[0] = v0;
        out[1] = v1;
        out[2] = v2;
        out[3] = v3;
        out[4] = m0;
        out[5] = m1;
        out[6] = m2;
        out[7] = m3;
        return;
    case 1:
        out[0] = v1;
        out[1] = v2;
        out[2] = v3;
        out[3] = v0;
        out[4] = m1;
        out[5] = m2;
        out[6] = m3;
        out[7] = m0;
        return;
    case 2:
        out[0] = v2;
        out[1] = v3;
        out[2] = v0;
        out[3] = v1;
        out[4] = m2;
        out[5] = m3;
        out[6] = m0;
        out[7] = m1;
        return;
    default:
        out[0] = v3;
        out[1] = v0;
        out[2] = v1;
        out[3] = v2;
        out[4] = m3;
        out[5] = m0;
        out[6] = m1;
        out[7] = m2;
        return;
    }
}

inline void extract_serial(const std::int32_t *connectivity, const std::int32_t *offsets,
                           const std::uint8_t *cell_types, std::int32_t n_cells,
                           std::int32_t n_points, ExtractResult &out) {
    const auto &T = tables();

    // Total face upper bound (exact). Serves as arena capacity.
    std::int64_t n_total_upper = 0;
    for (std::int32_t c = 0; c < n_cells; ++c)
        n_total_upper += T.faces_per_cell[cell_types[c]];

    if (n_total_upper == 0) {
        for (std::int32_t c = 0; c < n_cells; ++c)
            append_2d(connectivity, offsets, c, cell_types[c], out);
        return;
    }

    // bucket_head: zero-init, calloc-backed mmap pages = effectively
    // free for the typical big sizes.
    std::uint32_t *bucket_head =
        (std::uint32_t *)std::calloc((std::size_t)n_points, sizeof(std::uint32_t));
    if (!bucket_head)
        throw std::bad_alloc();

    // Arena: bump-allocated. No zero-init required; a fresh ChainEntry
    // is fully overwritten by insert_*.
    ChainEntry *arena =
        (ChainEntry *)aligned_alloc_raw(sizeof(ChainEntry) * (std::size_t)n_total_upper);

    std::uint32_t cursor = 0;

    // Insert pass — one big switch per cell, then a tight specialised
    // loop. Because cell types are usually homogeneous within blocks,
    // branch prediction handles the dispatch well.
    for (std::int32_t c = 0; c < n_cells; ++c) {
        int ct = cell_types[c];
        const std::int32_t *cell_pts = connectivity + offsets[c];
        switch (ct) {
        case VTK_TETRA:
        case VTK_QUADRATIC_TETRA:
            // Quadratic cells share corner topology with their
            // linear analog — the first N entries of connectivity
            // are the corner verts. Mid-edge nodes are emitted at
            // emit-time via face_mid_tpl.
            process_tet(cell_pts, (std::uint32_t)c, bucket_head, arena, cursor);
            break;
        case VTK_HEXAHEDRON:
        case VTK_QUADRATIC_HEXAHEDRON:
            process_hex(cell_pts, (std::uint32_t)c, bucket_head, arena, cursor);
            break;
        case VTK_VOXEL:
            process_voxel(cell_pts, (std::uint32_t)c, bucket_head, arena, cursor);
            break;
        case VTK_WEDGE:
        case VTK_QUADRATIC_WEDGE:
            process_wedge(cell_pts, (std::uint32_t)c, bucket_head, arena, cursor);
            break;
        case VTK_PYRAMID:
        case VTK_QUADRATIC_PYRAMID:
            process_pyramid(cell_pts, (std::uint32_t)c, bucket_head, arena, cursor);
            break;
        default:
            append_2d(connectivity, offsets, c, ct, out);
            break;
        }
    }

    // Emit pass — iterate buckets, collect UNIQUE entries.
    // Reserve generously based on cursor (upper bound on unique count).
    out.tri_indices.reserve(out.tri_indices.size() + 3 * (std::size_t)cursor);
    out.quad_indices.reserve(out.quad_indices.size() + 4 * (std::size_t)cursor);
    out.tri_origin_cell.reserve(out.tri_origin_cell.size() + (std::size_t)cursor);
    out.quad_origin_cell.reserve(out.quad_origin_cell.size() + (std::size_t)cursor);

    // Walk all cursor entries directly (skipping bucket_head) — they
    // sit in arena[0..cursor) contiguously. Cache-friendly sequential
    // scan. Each entry knows its own (cell_id, face_idx, n_verts).
    for (std::uint32_t i = 0; i < cursor; ++i) {
        const ChainEntry &e = arena[i];
        if (e.meta & META_INTERIOR_BIT)
            continue;
        std::uint32_t cid = unpack_cell(e.meta);
        std::uint32_t fid = unpack_face(e.meta);
        int ct = cell_types[cid];
        const std::int32_t *cp = connectivity + offsets[cid];
        const FaceTpl &tpl = T.face_tpl[ct][fid];
        const FaceMidTpl *mtpl = T.face_mid_tpl[ct];
        if (tpl.n_verts == 3) {
            std::int32_t v0 = cp[tpl.offsets[0]];
            std::int32_t v1 = cp[tpl.offsets[1]];
            std::int32_t v2 = cp[tpl.offsets[2]];
            if (mtpl) {
                const int *m = mtpl[fid].offsets;
                std::int32_t buf[6];
                emit_rotated_qtri(v0, v1, v2, cp[m[0]], cp[m[1]], cp[m[2]], buf);
                out.qtri_indices.insert(out.qtri_indices.end(), buf, buf + 6);
                out.qtri_origin_cell.push_back((std::int32_t)cid);
            } else {
                std::int32_t buf[3];
                emit_rotated_tri(v0, v1, v2, buf);
                out.tri_indices.insert(out.tri_indices.end(), buf, buf + 3);
                out.tri_origin_cell.push_back((std::int32_t)cid);
            }
        } else {
            std::int32_t v0 = cp[tpl.offsets[0]];
            std::int32_t v1 = cp[tpl.offsets[1]];
            std::int32_t v2 = cp[tpl.offsets[2]];
            std::int32_t v3 = cp[tpl.offsets[3]];
            if (mtpl) {
                const int *m = mtpl[fid].offsets;
                std::int32_t buf[8];
                emit_rotated_qquad(v0, v1, v2, v3, cp[m[0]], cp[m[1]], cp[m[2]], cp[m[3]], buf);
                out.qquad_indices.insert(out.qquad_indices.end(), buf, buf + 8);
                out.qquad_origin_cell.push_back((std::int32_t)cid);
            } else {
                std::int32_t buf[4];
                emit_rotated_quad(v0, v1, v2, v3, buf);
                out.quad_indices.insert(out.quad_indices.end(), buf, buf + 4);
                out.quad_origin_cell.push_back((std::int32_t)cid);
            }
        }
    }

    aligned_free_raw(arena);
    std::free(bucket_head);
}

// ------------------------------------------------------------------ //
// Parallel path — per-thread serial chained-hash, then a sharded
// cross-thread merge to find faces that surface in two different
// threads' cell ranges.
//
// Each thread runs the same insert_tri/insert_quad inner loop on its
// disjoint cell range with private arena + bucket_head. Once all
// threads finish, the merge walks each anchor's per-thread chains and
// marks faces that appear in two threads as INTERIOR.
//
// Correctness: an interior face shared by cells C and C' will be
// inserted into both cells' threads. Same-thread case: insert_*
// dedup catches it. Cross-thread case: merge catches it.
//
// Two faces match iff they have the same anchor (so they live in the
// same anchor bucket on both threads) AND their (rotated) other
// tuples match either forward or reverse — same compare used inside
// insert_tri/insert_quad.
// ------------------------------------------------------------------ //

// Returns true iff the entries describe the same face.
FSE_FORCE_INLINE
bool entries_equal(const ChainEntry &a, const ChainEntry &b) {
    bool a_quad = a.meta & META_NV4_BIT;
    bool b_quad = b.meta & META_NV4_BIT;
    if (a_quad != b_quad)
        return false;
    if (a_quad) {
        // Quad: opposite vertex (other[1]) must match; then other[0]/other[2]
        // either same or swapped.
        if (a.v[1] != b.v[1])
            return false;
        return (a.v[0] == b.v[0] && a.v[2] == b.v[2]) || (a.v[0] == b.v[2] && a.v[2] == b.v[0]);
    } else {
        // Tri: (other[0], other[1]) same or swapped.
        return (a.v[0] == b.v[0] && a.v[1] == b.v[1]) || (a.v[0] == b.v[1] && a.v[1] == b.v[0]);
    }
}

// Set FSE_PROFILE_PARALLEL=1 to print per-phase timings to stderr.
// Used during parallel-path tuning; off in production builds.
inline bool fse_profile_enabled() {
    static const bool v = []() {
        const char *e = std::getenv("FSE_PROFILE_PARALLEL");
        return e && e[0] && e[0] != '0';
    }();
    return v;
}

inline double fse_now_ms() {
    return omp_get_wtime() * 1000.0;
}

inline void extract_parallel(const std::int32_t *connectivity, const std::int32_t *offsets,
                             const std::uint8_t *cell_types, std::int32_t n_cells,
                             std::int32_t n_points, int n_threads, ExtractResult &out) {
    const auto &T = tables();
    const bool prof = fse_profile_enabled();
    double t0 = prof ? fse_now_ms() : 0.0;
    auto mark = [&](const char *label) {
        if (!prof)
            return;
        double t = fse_now_ms();
        std::fprintf(stderr, "[fse parallel] %-20s %7.2f ms\n", label, t - t0);
        t0 = t;
    };

    // 2D pass-through (rare).
    for (std::int32_t c = 0; c < n_cells; ++c) {
        if (T.faces_per_cell[cell_types[c]] == 0) {
            append_2d(connectivity, offsets, c, cell_types[c], out);
        }
    }

    // Compute per-thread cell ranges (static partition).
    std::vector<std::int32_t> range_lo(n_threads + 1, 0);
    {
        std::int64_t per = ((std::int64_t)n_cells + n_threads - 1) / n_threads;
        for (int t = 0; t <= n_threads; ++t)
            range_lo[t] = (std::int32_t)std::min<std::int64_t>(per * t, n_cells);
    }

    // Per-thread face count upper bounds (for arena sizing).
    std::vector<std::int64_t> per_thread_faces(n_threads, 0);
#pragma omp parallel num_threads(n_threads)
    {
        int tid = omp_get_thread_num();
        std::int64_t cnt = 0;
        for (std::int32_t c = range_lo[tid]; c < range_lo[tid + 1]; ++c)
            cnt += T.faces_per_cell[cell_types[c]];
        per_thread_faces[tid] = cnt;
    }

    // Per-thread state.
    std::vector<std::uint32_t *> bh_t(n_threads, nullptr);
    std::vector<ChainEntry *> arena_t(n_threads, nullptr);
    std::vector<std::uint32_t> cursor_t(n_threads, 0);

    for (int t = 0; t < n_threads; ++t) {
        bh_t[t] = (std::uint32_t *)std::calloc((std::size_t)n_points, sizeof(std::uint32_t));
        if (!bh_t[t])
            throw std::bad_alloc();
        if (per_thread_faces[t] > 0) {
            arena_t[t] = (ChainEntry *)aligned_alloc_raw(sizeof(ChainEntry) *
                                                         (std::size_t)per_thread_faces[t]);
        }
    }
    mark("alloc bh+arena");

// Per-thread serial chained-hash insert.
#pragma omp parallel num_threads(n_threads)
    {
        int tid = omp_get_thread_num();
        std::uint32_t *bh = bh_t[tid];
        ChainEntry *ar = arena_t[tid];
        std::uint32_t cur = 0;
        for (std::int32_t c = range_lo[tid]; c < range_lo[tid + 1]; ++c) {
            int ct = cell_types[c];
            const std::int32_t *cp = connectivity + offsets[c];
            switch (ct) {
            case VTK_TETRA:
            case VTK_QUADRATIC_TETRA:
                process_tet(cp, (std::uint32_t)c, bh, ar, cur);
                break;
            case VTK_HEXAHEDRON:
            case VTK_QUADRATIC_HEXAHEDRON:
                process_hex(cp, (std::uint32_t)c, bh, ar, cur);
                break;
            case VTK_VOXEL:
                process_voxel(cp, (std::uint32_t)c, bh, ar, cur);
                break;
            case VTK_WEDGE:
            case VTK_QUADRATIC_WEDGE:
                process_wedge(cp, (std::uint32_t)c, bh, ar, cur);
                break;
            case VTK_PYRAMID:
            case VTK_QUADRATIC_PYRAMID:
                process_pyramid(cp, (std::uint32_t)c, bh, ar, cur);
                break;
            default:
                break; // 2D handled serially above
            }
        }
        cursor_t[tid] = cur;
    }
    mark("per-thread insert");

    // ----- Cross-thread merge --------------------------------------
    // Build a UNION bitmap of "any thread touched this anchor", then
    // walk only the set bits. Most anchors on a typical irregular
    // mesh are touched by few faces, so most words have ~50 % bits
    // set; iterating set bits via fse_ctzll skips the empties
    // entirely. On the 4.7 M-cell rotor this drops the merge phase
    // from 53 ms to ~15 ms, ~13 % off the total parallel time.
    // Build TWO bitmaps in one pass: any-thread (was here already)
    // and multi-thread. multi[i] is set iff at least 2 threads
    // contributed an entry at anchor i — those are the only anchors
    // where cross-thread duplicates can possibly hide. On a typical
    // FE mesh ~5–15 % of anchors are multi-thread, so the merge
    // walks ~10× fewer chains than before.
    const std::int64_t n_words = (n_points + 63) / 64;
    std::vector<std::uint64_t> bitmap((std::size_t)n_words, 0);
    std::vector<std::uint64_t> bitmap_multi((std::size_t)n_words, 0);

    const std::int64_t words_per_thread = (n_words + n_threads - 1) / n_threads;
#pragma omp parallel num_threads(n_threads)
    {
        int wtid = omp_get_thread_num();
        std::int64_t lo = std::min<std::int64_t>(words_per_thread * wtid, n_words);
        std::int64_t hi = std::min<std::int64_t>(lo + words_per_thread, n_words);
        for (std::int64_t w = lo; w < hi; ++w) {
            std::uint64_t any_w = 0;
            std::uint64_t multi_w = 0;
            std::int64_t base = w * 64;
            std::int64_t end = std::min<std::int64_t>(base + 64, n_points);
            for (std::int64_t i = base; i < end; ++i) {
                bool seen_one = false;
                bool seen_two = false;
                for (int t = 0; t < n_threads; ++t) {
                    if (bh_t[t][i]) {
                        if (seen_one) {
                            seen_two = true;
                            break;
                        }
                        seen_one = true;
                    }
                }
                if (seen_one)
                    any_w |= 1ULL << (i - base);
                if (seen_two)
                    multi_w |= 1ULL << (i - base);
            }
            bitmap[w] = any_w;
            bitmap_multi[w] = multi_w;
        }
    }
    mark("build union bitmap");

    // Merge: iterate set bits of bitmap_multi (anchors with ≥2
    // contributing threads). Single-thread anchors cannot have
    // cross-thread duplicates and are skipped entirely.
#pragma omp parallel for schedule(dynamic, 64) num_threads(n_threads)
    for (std::int64_t w = 0; w < n_words; ++w) {
        std::uint64_t word = bitmap_multi[w];
        const std::int64_t base = w * 64;
        while (word) {
            int bit = fse_ctzll(word);
            std::int64_t i = base + bit;
            for (int t1 = 0; t1 < n_threads; ++t1) {
                std::uint32_t idx1 = bh_t[t1][i];
                while (idx1) {
                    ChainEntry &e1 = arena_t[t1][idx1 - 1];
                    if (!(e1.meta & META_INTERIOR_BIT)) {
                        for (int t2 = t1 + 1; t2 < n_threads; ++t2) {
                            std::uint32_t idx2 = bh_t[t2][i];
                            while (idx2) {
                                ChainEntry &e2 = arena_t[t2][idx2 - 1];
                                if (!(e2.meta & META_INTERIOR_BIT) && entries_equal(e1, e2)) {
                                    e1.meta |= META_INTERIOR_BIT;
                                    e2.meta |= META_INTERIOR_BIT;
                                    goto next_e1;
                                }
                                idx2 = e2.next;
                            }
                        }
                    }
                next_e1:
                    idx1 = e1.next;
                }
            }
            word &= word - 1;
        }
    }
    mark("cross-thread merge");

    // Per-thread tri/quad counts for output prefix sum. We split
    // into 4 buckets: linear-tri, linear-quad, quadratic-tri,
    // quadratic-quad. The "is this face quadratic?" check is a
    // single nullptr lookup on T.face_mid_tpl[ct].
    std::vector<std::int64_t> tri_counts(n_threads + 1, 0);
    std::vector<std::int64_t> quad_counts(n_threads + 1, 0);
    std::vector<std::int64_t> qtri_counts(n_threads + 1, 0);
    std::vector<std::int64_t> qquad_counts(n_threads + 1, 0);

#pragma omp parallel num_threads(n_threads)
    {
        int tid = omp_get_thread_num();
        std::int64_t lt = 0, lq = 0, qt = 0, qq = 0;
        for (std::uint32_t k = 0; k < cursor_t[tid]; ++k) {
            ChainEntry &e = arena_t[tid][k];
            if (e.meta & META_INTERIOR_BIT)
                continue;
            std::uint32_t cid = unpack_cell(e.meta);
            bool quadratic = T.face_mid_tpl[cell_types[cid]] != nullptr;
            if (is_nv4(e.meta)) {
                if (quadratic)
                    ++qq;
                else
                    ++lq;
            } else {
                if (quadratic)
                    ++qt;
                else
                    ++lt;
            }
        }
        tri_counts[tid + 1] = lt;
        quad_counts[tid + 1] = lq;
        qtri_counts[tid + 1] = qt;
        qquad_counts[tid + 1] = qq;
    }
    for (int t = 1; t <= n_threads; ++t) {
        tri_counts[t] += tri_counts[t - 1];
        quad_counts[t] += quad_counts[t - 1];
        qtri_counts[t] += qtri_counts[t - 1];
        qquad_counts[t] += qquad_counts[t - 1];
    }

    std::int64_t base_tri = (std::int64_t)out.tri_indices.size() / 3;
    std::int64_t base_quad = (std::int64_t)out.quad_indices.size() / 4;
    std::int64_t base_qtri = (std::int64_t)out.qtri_indices.size() / 6;
    std::int64_t base_qquad = (std::int64_t)out.qquad_indices.size() / 8;
    std::int64_t n_tri = tri_counts[n_threads];
    std::int64_t n_quad = quad_counts[n_threads];
    std::int64_t n_qtri = qtri_counts[n_threads];
    std::int64_t n_qquad = qquad_counts[n_threads];
    out.tri_indices.resize((base_tri + n_tri) * 3);
    out.quad_indices.resize((base_quad + n_quad) * 4);
    out.qtri_indices.resize((base_qtri + n_qtri) * 6);
    out.qquad_indices.resize((base_qquad + n_qquad) * 8);
    out.tri_origin_cell.resize(base_tri + n_tri);
    out.quad_origin_cell.resize(base_quad + n_quad);
    out.qtri_origin_cell.resize(base_qtri + n_qtri);
    out.qquad_origin_cell.resize(base_qquad + n_qquad);

#pragma omp parallel num_threads(n_threads)
    {
        int tid = omp_get_thread_num();
        std::int64_t ti = base_tri + tri_counts[tid];
        std::int64_t qi = base_quad + quad_counts[tid];
        std::int64_t qti = base_qtri + qtri_counts[tid];
        std::int64_t qqi = base_qquad + qquad_counts[tid];
        std::int32_t *tri_w = out.tri_indices.data();
        std::int32_t *quad_w = out.quad_indices.data();
        std::int32_t *qtri_w = out.qtri_indices.data();
        std::int32_t *qquad_w = out.qquad_indices.data();
        std::int32_t *tri_o = out.tri_origin_cell.data();
        std::int32_t *quad_o = out.quad_origin_cell.data();
        std::int32_t *qtri_o = out.qtri_origin_cell.data();
        std::int32_t *qquad_o = out.qquad_origin_cell.data();
        for (std::uint32_t k = 0; k < cursor_t[tid]; ++k) {
            ChainEntry &e = arena_t[tid][k];
            if (e.meta & META_INTERIOR_BIT)
                continue;
            std::uint32_t cid = unpack_cell(e.meta);
            std::uint32_t fid = unpack_face(e.meta);
            int ct = cell_types[cid];
            const std::int32_t *cp = connectivity + offsets[cid];
            const FaceTpl &tpl = T.face_tpl[ct][fid];
            const FaceMidTpl *mtpl = T.face_mid_tpl[ct];
            if (is_nv4(e.meta)) {
                std::int32_t v0 = cp[tpl.offsets[0]];
                std::int32_t v1 = cp[tpl.offsets[1]];
                std::int32_t v2 = cp[tpl.offsets[2]];
                std::int32_t v3 = cp[tpl.offsets[3]];
                if (mtpl) {
                    const int *m = mtpl[fid].offsets;
                    emit_rotated_qquad(v0, v1, v2, v3, cp[m[0]], cp[m[1]], cp[m[2]], cp[m[3]],
                                       qquad_w + 8 * qqi);
                    qquad_o[qqi] = (std::int32_t)cid;
                    ++qqi;
                } else {
                    emit_rotated_quad(v0, v1, v2, v3, quad_w + 4 * qi);
                    quad_o[qi] = (std::int32_t)cid;
                    ++qi;
                }
            } else {
                std::int32_t v0 = cp[tpl.offsets[0]];
                std::int32_t v1 = cp[tpl.offsets[1]];
                std::int32_t v2 = cp[tpl.offsets[2]];
                if (mtpl) {
                    const int *m = mtpl[fid].offsets;
                    emit_rotated_qtri(v0, v1, v2, cp[m[0]], cp[m[1]], cp[m[2]], qtri_w + 6 * qti);
                    qtri_o[qti] = (std::int32_t)cid;
                    ++qti;
                } else {
                    emit_rotated_tri(v0, v1, v2, tri_w + 3 * ti);
                    tri_o[ti] = (std::int32_t)cid;
                    ++ti;
                }
            }
        }
    }
    mark("emit");

    for (int t = 0; t < n_threads; ++t) {
        std::free(bh_t[t]);
        aligned_free_raw(arena_t[t]);
    }
}

// ------------------------------------------------------------------ //
// Top-level dispatch.
// ------------------------------------------------------------------ //
// ------------------------------------------------------------------ //
// Cell reordering helper.
//
// The parallel path's cross-thread merge cost is proportional to the
// number of faces shared across thread cell-range boundaries. For a
// regular hex grid in scanline order, this is tiny (cells are in
// spatial order, so neighbours fall in the same chunk). For an
// irregular mesh (e.g., a irregular tet mesh) cells incident to one
// vertex can span all threads, blowing up the merge.
//
// Sorting cells by their first vertex id puts spatially-related
// cells next to each other (assuming vertex IDs reflect mesh layout,
// which is typical). It also helps the SERIAL path via better
// bucket_head cache locality.
//
// Output arrays:
//   perm[i]  = original cell id of the new cell at position i.
//   inv[c]   = new cell id for original cell c (computed from perm).
//
// Cost: ~5 ms / 1 M cells in C++ (radix sort + parallel memcpy).
// ------------------------------------------------------------------ //
// O(n_cells + n_points) counting sort by first-vertex id. ~10x
// faster than std::sort with an indirect comparator on big meshes.
inline std::vector<std::int32_t>
compute_cell_reorder_by_first_vertex(const std::int32_t *connectivity, const std::int32_t *offsets,
                                     std::int32_t n_cells, std::int32_t n_points) {
    // Histogram of first-vertex ids.
    std::vector<std::int32_t> head((std::size_t)n_points + 1, 0);
    for (std::int32_t i = 0; i < n_cells; ++i)
        ++head[(std::size_t)connectivity[offsets[i]] + 1];
    // Exclusive prefix sum.
    for (std::int32_t i = 1; i <= n_points; ++i)
        head[i] += head[i - 1];
    std::vector<std::int32_t> perm((std::size_t)n_cells);
    // Place. Each first-vertex id's bucket gets cells in input order
    // (stable), which keeps original neighbour relationships.
    std::vector<std::int32_t> cursor((std::size_t)n_points, 0);
    for (std::int32_t i = 0; i < n_cells; ++i) {
        std::int32_t v = connectivity[offsets[i]];
        perm[head[v] + cursor[v]++] = i;
    }
    return perm;
}

// Detect whether cells are roughly in first-vertex order already.
// Cheap O(N) scan that counts inversions on a stride-1k sample.
// Used to skip reorder when the mesh is already sorted (the common
// case for structured grids).
inline bool cells_already_first_vertex_sorted(const std::int32_t *connectivity,
                                              const std::int32_t *offsets, std::int32_t n_cells) {
    if (n_cells < 32)
        return true;
    const std::int32_t step = std::max<std::int32_t>(1, n_cells / 1024);
    std::int32_t prev = connectivity[offsets[0]];
    std::int32_t inversions = 0;
    std::int32_t samples = 0;
    for (std::int32_t i = step; i < n_cells; i += step) {
        std::int32_t cur = connectivity[offsets[i]];
        if (cur < prev)
            ++inversions;
        prev = cur;
        ++samples;
    }
    // Allow up to 5% inversions (tolerates minor reordering).
    return inversions * 20 < samples;
}

// Apply a cell permutation to (connectivity, offsets, cell_types) → new
// arrays. Caller owns the output vectors.
inline void apply_cell_reorder(const std::int32_t *connectivity, const std::int32_t *offsets,
                               const std::uint8_t *cell_types, const std::int32_t *perm,
                               std::int32_t n_cells, int n_threads,
                               std::vector<std::int32_t> &new_conn,
                               std::vector<std::int32_t> &new_off,
                               std::vector<std::uint8_t> &new_ct) {
    const int nt = resolved_threads(n_threads);
    new_off.resize((std::size_t)n_cells + 1);
    new_off[0] = 0;
    for (std::int32_t i = 0; i < n_cells; ++i) {
        std::int32_t oc = perm[i];
        new_off[i + 1] = new_off[i] + (offsets[oc + 1] - offsets[oc]);
    }
    new_conn.resize((std::size_t)new_off[n_cells]);
    new_ct.resize((std::size_t)n_cells);
#pragma omp parallel for schedule(static) num_threads(nt)
    for (std::int32_t i = 0; i < n_cells; ++i) {
        std::int32_t oc = perm[i];
        std::int32_t sz = offsets[oc + 1] - offsets[oc];
        std::memcpy(new_conn.data() + new_off[i], connectivity + offsets[oc],
                    sz * sizeof(std::int32_t));
        new_ct[i] = cell_types[oc];
    }
}
inline ExtractResult extract_surface(const std::int32_t *connectivity, const std::int32_t *offsets,
                                     const std::uint8_t *cell_types, std::int32_t n_cells,
                                     std::int32_t n_points, int n_threads = 0) {
    ExtractResult out;
    if (n_cells <= 0)
        return out;
    int nt = resolved_threads(n_threads);
    if (n_cells < SMALL_MESH_THRESHOLD || nt <= 1) {
        extract_serial(connectivity, offsets, cell_types, n_cells, n_points, out);
    } else {
        extract_parallel(connectivity, offsets, cell_types, n_cells, n_points, nt, out);
    }
    return out;
}

// ------------------------------------------------------------------ //
// Point compaction. Templated on the point dtype so we can preserve
// float32 OR float64 input verbatim — VTK / scientific data
// frequently uses float64 and silently downcasting would lose
// precision.
// ------------------------------------------------------------------ //
template <typename T>
inline void compact_points_into(ExtractResult &res, std::vector<T> &out_points, const T *points_xyz,
                                std::int32_t n_points, int n_threads) {
    const int nt = resolved_threads(n_threads);
    const std::size_t n_tri = res.tri_indices.size();
    const std::size_t n_quad = res.quad_indices.size();
    const std::size_t n_qtri = res.qtri_indices.size();
    const std::size_t n_qquad = res.qquad_indices.size();

    // calloc gives zero-filled pages on demand (lazy mmap on Linux).
    // n_points is often 8-16× larger than the actual surface point
    // count (interior points are unused), so most pages are NEVER
    // faulted in — saves a lot of cold-start memory traffic vs
    // std::vector(n, 0) which value-initialises every element.
    std::uint8_t *used = (std::uint8_t *)std::calloc((std::size_t)n_points, 1);
    if (!used)
        throw std::bad_alloc();

    auto mark = [&](const std::vector<std::int32_t> &idx) {
        if (idx.empty())
            return;
        const std::int32_t *p = idx.data();
        const std::int64_t n = (std::int64_t)idx.size();
#pragma omp parallel for schedule(static) num_threads(nt)
        for (std::int64_t i = 0; i < n; ++i)
            used[p[i]] = 1;
    };
    mark(res.tri_indices);
    mark(res.quad_indices);
    mark(res.qtri_indices);
    mark(res.qquad_indices);

    // Same trick for new_id: only the ~5–15 % "used" slots need a
    // real value, so calloc + lazy zero pages saves the
    // value-init traffic of std::vector(n_points). The unused slots
    // stay backed by the global zero page.
    std::int32_t *new_id = (std::int32_t *)std::calloc((std::size_t)n_points, sizeof(std::int32_t));
    if (!new_id) {
        std::free(used);
        throw std::bad_alloc();
    }

    std::vector<std::int64_t> chunk_sums(nt + 1, 0);
    const std::int64_t per_thread = ((std::int64_t)n_points + nt - 1) / nt;

#pragma omp parallel num_threads(nt)
    {
        int tid = omp_get_thread_num();
        std::int64_t lo = std::min<std::int64_t>(per_thread * tid, n_points);
        std::int64_t hi = std::min<std::int64_t>(lo + per_thread, n_points);
        std::int64_t s = 0;
        for (std::int64_t i = lo; i < hi; ++i)
            s += used[i];
        chunk_sums[tid + 1] = s;
#pragma omp barrier
#pragma omp single
        {
            for (int t = 0; t < nt; ++t)
                chunk_sums[t + 1] += chunk_sums[t];
        }
        std::int64_t off = chunk_sums[tid];
        for (std::int64_t i = lo; i < hi; ++i) {
            new_id[i] = (std::int32_t)off;
            off += used[i];
        }
    }
    std::int32_t n_used = (std::int32_t)chunk_sums[nt];

    auto rewrite = [&](std::vector<std::int32_t> &idx) {
        if (idx.empty())
            return;
        std::int32_t *p = idx.data();
        const std::int64_t n = (std::int64_t)idx.size();
#pragma omp parallel for schedule(static) num_threads(nt)
        for (std::int64_t i = 0; i < n; ++i)
            p[i] = new_id[p[i]];
    };
    rewrite(res.tri_indices);
    rewrite(res.quad_indices);
    rewrite(res.qtri_indices);
    rewrite(res.qquad_indices);

    out_points.resize(3 * (std::size_t)n_used);
    res.point_map.resize(n_used);

#pragma omp parallel for schedule(static) num_threads(nt)
    for (std::int64_t i = 0; i < (std::int64_t)n_points; ++i) {
        if (used[i]) {
            std::int32_t ni = new_id[i];
            out_points[3 * ni + 0] = points_xyz[3 * i + 0];
            out_points[3 * ni + 1] = points_xyz[3 * i + 1];
            out_points[3 * ni + 2] = points_xyz[3 * i + 2];
            res.point_map[ni] = (std::int32_t)i;
        }
    }

    std::free(new_id);
    std::free(used);
    res.n_points_used = n_used;
}

inline void compact_points(ExtractResult &res, const float *points_xyz, std::int32_t n_points,
                           int n_threads = 0) {
    compact_points_into<float>(res, res.points, points_xyz, n_points, n_threads);
}

inline void compact_points_f64(ExtractResult &res, const double *points_xyz, std::int32_t n_points,
                               int n_threads = 0) {
    compact_points_into<double>(res, res.points_f64, points_xyz, n_points, n_threads);
}

} // namespace fse
