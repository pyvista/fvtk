// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkSTLReader.h"

#include "vtkByteSwap.h"
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkErrorCode.h"
#include "vtkFileResourceStream.h"
#include "vtkFloatArray.h"
#include "vtkIdTypeArray.h"
#include "vtkIncrementalPointLocator.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMergePoints.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPolyData.h"
#include "vtkResourceParser.h"
#include "vtkResourceStream.h"
#include "vtkSmartPointer.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringScanner.h"
#include "vtkTypeInt32Array.h"
#include "vtkUnsignedCharArray.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <vtksys/SystemTools.hxx>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkSTLReader);

vtkCxxSetObjectMacro(vtkSTLReader, Locator, vtkIncrementalPointLocator);
vtkCxxSetObjectMacro(vtkSTLReader, BinaryHeader, vtkUnsignedCharArray);

namespace
{
// === cvista fast binary-STL hash-merge ========================================
// Ported from the fast STL reader in github.com/pyvista/stl-reader: the libstl
// 96-bit vertex hash by Aki Nyrhinen (MIT-licensed), with the VTK integration
// by Alex Kaszynski. The 96-bit Jenkins hash + EXACT 3-word (bitwise float)
// compare reproduces vtkMergePoints' exact-coincident merge as an order-free
// SET -- which is all an STL file's (index-less) mesh requires. Binary STL
// stores raw IEEE-754 float bits, so the merged points are byte-identical to
// VTK's; only their order may differ (acceptable: STL has no point index array).
//
// MIT License, Copyright (c) 2016 Aki Nyrhinen; modifications (c) A. Kaszynski.

inline uint32_t cvistaStlNextPow2(uint32_t v)
{
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return ++v;
}

inline uint32_t cvistaStlFinal96(uint32_t a, uint32_t b, uint32_t c)
{
  auto rot = [](uint32_t x, int k) { return (x << k) | (x >> (32 - k)); };
  c ^= b;
  c -= rot(b, 14);
  a ^= c;
  a -= rot(c, 11);
  b ^= a;
  b -= rot(a, 25);
  c ^= b;
  c -= rot(b, 16);
  a ^= c;
  a -= rot(c, 4);
  b ^= a;
  b -= rot(a, 14);
  c ^= b;
  c -= rot(b, 24);
  return c;
}

// Little-endian 32-bit load (endian-agnostic; matches vtkByteSwap::Swap4LE
// interpretation of the on-disk float bits on every host).
inline uint32_t cvistaStlGet32LE(const uint8_t* b)
{
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
    (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

inline bool cvistaStlWordFinite(uint32_t w)
{
  float f;
  std::memcpy(&f, &w, sizeof(f));
  return std::isfinite(f);
}

// vtkMergePoints merges by VALUE (double ==), so it treats -0.0 and +0.0 as the
// same point. Bit-equality is identical to value-equality for every finite
// float EXCEPT this one case, so canonicalize -0.0 (0x80000000) to +0.0. The
// stored coordinate is then +0.0, which is value-identical to either zero.
inline uint32_t cvistaStlCanonZero(uint32_t w)
{
  return w == 0x80000000u ? 0u : w;
}

// Open-addressing (linear-probe) vertex merge keyed on the exact 3x32-bit
// pattern (== bitwise float equality). Vertex ids are assigned in first-seen
// order; Vertices() holds 3 bit-preserved floats per unique vertex.
class cvistaStlVertexMerger
{
public:
  explicit cvistaStlVertexMerger(size_t estVerts)
  {
    const size_t cap =
      cvistaStlNextPow2(static_cast<uint32_t>(std::max<size_t>(estVerts * 2, 16)));
    this->Table.assign(cap, 0);
    this->Mask = static_cast<uint32_t>(cap - 1);
    this->Verts.reserve(estVerts * 3);
  }

  // w = the three raw 32-bit words (little-endian float bit patterns).
  uint32_t Insert(const uint32_t w[3])
  {
    if (static_cast<size_t>(this->NVerts + 1) * 2 > this->Table.size())
    {
      this->Grow();
    }
    const uint32_t hash = cvistaStlFinal96(w[0], w[1], w[2]);
    for (uint32_t i = 0;; ++i)
    {
      uint32_t& slot = this->Table[(hash + i) & this->Mask];
      if (slot == 0)
      {
        slot = this->NVerts + 1;
        float f[3];
        std::memcpy(f, w, sizeof(f));
        this->Verts.push_back(f[0]);
        this->Verts.push_back(f[1]);
        this->Verts.push_back(f[2]);
        return this->NVerts++;
      }
      const uint32_t vi = slot - 1;
      uint32_t e[3];
      std::memcpy(e, &this->Verts[3 * vi], sizeof(e));
      if (e[0] == w[0] && e[1] == w[1] && e[2] == w[2])
      {
        return vi;
      }
    }
  }

  uint32_t NumberOfVertices() const { return this->NVerts; }
  std::vector<float>& Vertices() { return this->Verts; }

private:
  void Grow()
  {
    this->Table.assign(this->Table.size() * 2, 0);
    this->Mask = static_cast<uint32_t>(this->Table.size() - 1);
    for (uint32_t vi = 0; vi < this->NVerts; ++vi)
    {
      uint32_t w[3];
      std::memcpy(w, &this->Verts[3 * vi], sizeof(w));
      const uint32_t hash = cvistaStlFinal96(w[0], w[1], w[2]);
      for (uint32_t i = 0;; ++i)
      {
        uint32_t& slot = this->Table[(hash + i) & this->Mask];
        if (slot == 0)
        {
          slot = vi + 1;
          break;
        }
      }
    }
  }

  std::vector<float> Verts;
  std::vector<uint32_t> Table;
  uint32_t Mask = 0;
  uint32_t NVerts = 0;
};
} // anonymous namespace

//------------------------------------------------------------------------------
vtkSTLReader::vtkSTLReader() = default;

//------------------------------------------------------------------------------
vtkSTLReader::~vtkSTLReader()
{
  this->SetLocator(nullptr);
  this->SetHeader(nullptr);
  this->SetBinaryHeader(nullptr);
}

//------------------------------------------------------------------------------
// Overload standard modified time function. If locator is modified,
// then this object is modified as well.
vtkMTimeType vtkSTLReader::GetMTime()
{
  vtkMTimeType mTime1 = this->Superclass::GetMTime();

  if (this->Locator)
  {
    vtkMTimeType mTime2 = this->Locator->GetMTime();
    mTime1 = std::max(mTime1, mTime2);
  }

  return mTime1;
}

//------------------------------------------------------------------------------
int vtkSTLReader::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  vtkPolyData* output = vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  // All of the data in the first piece.
  if (outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER()) > 0)
  {
    return 0;
  }

  if (!this->Stream && (!this->FileName || *this->FileName == 0))
  {
    vtkErrorMacro(<< "A FileName or stream must be specified.");
    this->SetErrorCode(vtkErrorCode::NoFileNameError);
    return 0;
  }

  vtkResourceStream* stream = this->Stream;
  vtkNew<vtkFileResourceStream> fileStream;
  if (stream)
  {
    stream->Seek(0, vtkResourceStream::SeekDirection::Begin);
  }
  else
  {
    if (!fileStream->Open(this->FileName))
    {
      vtkErrorMacro("Unable to open " << this->FileName << " . Aborting.");
      this->SetErrorCode(vtkErrorCode::CannotOpenFileError);
      return 0;
    }
    stream = fileStream;
  }

  // cvista fast path: single-pass hash-merge reader for the DEFAULT configuration
  // (Merging on, default locator, ScalarTags off). It produces the same
  // fundamental mesh as the locator-merge path below -- same exact-coincident
  // merged point SET and the same triangles (degenerate triangles dropped) --
  // but in one pass; point order is left unconstrained (STL carries no index
  // array, so order is not part of the mesh). For any non-default option, or an
  // input the fast path declines, it returns -1 and we fall through to the
  // unchanged legacy locator path.
  if (this->Merging && this->Locator == nullptr && !this->ScalarTags)
  {
    const int fastResult = this->ReadSTLFast(stream, output);
    if (fastResult >= 0)
    {
      return fastResult;
    }
    stream->Seek(0, vtkResourceStream::SeekDirection::Begin);
  }

  std::string solid;
  vtkNew<vtkResourceParser> asciiTester;
  asciiTester->SetStream(stream);
  asciiTester->ReadLine(solid, 5);
  stream->Seek(0, vtkResourceStream::SeekDirection::Begin);

  vtkNew<vtkPoints> newPts;
  vtkNew<vtkCellArray> newPolys;
  vtkSmartPointer<vtkFloatArray> newScalars;

  if (solid == "solid")
  {
    // First word is "solid", which means the data should be ASCII.
    newPts->Allocate(5000);
    newPolys->AllocateEstimate(10000, 1);
    if (this->ScalarTags)
    {
      newScalars = vtkSmartPointer<vtkFloatArray>::New();
      newScalars->Allocate(5000);
    }

    vtkNew<vtkResourceParser> parser;
    parser->SetStream(stream);
    if (!this->ReadASCIISTL(parser, newPts.Get(), newPolys.Get(), newScalars))
    {
      // In relaxed mode, fallback to try reading as binary (because we have seen malformed STL
      // files in the wild that have the 80 byte header but start with `solid`).
      if (this->GetRelaxedConformance())
      {
        stream->Seek(0, vtkResourceStream::SeekDirection::Begin);
        if (!this->ReadBinarySTL(stream, newPts.Get(), newPolys.Get()))
        {
          vtkErrorMacro("Fallback reading as binary STL failed too. Aborting.");
          return 0;
        }
      }
    }
  }
  else
  {
    if (!this->ReadBinarySTL(stream, newPts.Get(), newPolys.Get()))
    {
      vtkErrorMacro("Error reading a binary STL. Aborting.");
      return 0;
    }
  }

  vtkDebugMacro(<< "Read: " << newPts->GetNumberOfPoints() << " points, "
                << newPolys->GetNumberOfCells() << " triangles");

  // If merging is on, create hash table and merge points/triangles.
  vtkSmartPointer<vtkPoints> mergedPts = newPts;
  vtkSmartPointer<vtkCellArray> mergedPolys = newPolys;
  vtkSmartPointer<vtkFloatArray> mergedScalars = newScalars;
  if (this->Merging)
  {
    mergedPts = vtkSmartPointer<vtkPoints>::New();
    mergedPts->Allocate(newPts->GetNumberOfPoints() / 2);
    mergedPolys = vtkSmartPointer<vtkCellArray>::New();
    mergedPolys->AllocateCopy(newPolys);
    if (newScalars)
    {
      mergedScalars = vtkSmartPointer<vtkFloatArray>::New();
      mergedScalars->Allocate(newPolys->GetNumberOfCells());
    }

    vtkSmartPointer<vtkIncrementalPointLocator> locator = this->Locator;
    if (this->Locator == nullptr)
    {
      locator.TakeReference(this->NewDefaultLocator());
    }
    locator->InitPointInsertion(mergedPts, newPts->GetBounds());

    vtkIdType nextCell = 0;
    const vtkIdType* pts = nullptr;
    vtkIdType npts;
    for (newPolys->InitTraversal(); newPolys->GetNextCell(npts, pts);)
    {
      vtkIdType nodes[3];
      for (int i = 0; i < 3; i++)
      {
        double x[3];
        newPts->GetPoint(pts[i], x);
        locator->InsertUniquePoint(x, nodes[i]);
      }

      if (nodes[0] != nodes[1] && nodes[0] != nodes[2] && nodes[1] != nodes[2])
      {
        mergedPolys->InsertNextCell(3, nodes);
        if (newScalars)
        {
          mergedScalars->InsertNextValue(newScalars->GetValue(nextCell));
        }
      }
      nextCell++;
    }

    vtkDebugMacro(<< "Merged to: " << mergedPts->GetNumberOfPoints() << " points, "
                  << mergedPolys->GetNumberOfCells() << " triangles");
  }

  output->SetPoints(mergedPts);
  output->SetPolys(mergedPolys);

  if (mergedScalars)
  {
    mergedScalars->SetName("STLSolidLabeling");
    output->GetCellData()->SetScalars(mergedScalars);
  }

  if (this->Locator)
  {
    this->Locator->Initialize(); // free storage
  }

  output->Squeeze();

  return 1;
}

//------------------------------------------------------------------------------
// cvista fast path. Returns 1 (handled), 0 (hard error, ErrorCode set), or
// -1 (decline -> caller uses the legacy locator path). Currently handles BINARY
// STL only: it stores raw IEEE-754 float bits, so merged points are byte-exact
// vs the legacy path (positions are sacred). ASCII is declined (-1) because its
// float parse must round identically to VTK's parser to keep positions exact;
// that is a follow-up. The merge is exact-coincident (== vtkMergePoints) and
// degenerate triangles are dropped, matching the legacy result up to point and
// triangle ORDER -- which an index-less STL mesh does not define.
int vtkSTLReader::ReadSTLFast(vtkResourceStream* stream, vtkPolyData* output)
{
  // Slurp the whole resource into memory (single sequential read).
  stream->Seek(0, vtkResourceStream::SeekDirection::Begin);
  const vtkTypeInt64 size = stream->Seek(0, vtkResourceStream::SeekDirection::End);
  stream->Seek(0, vtkResourceStream::SeekDirection::Begin);
  if (size < 84)
  {
    return -1; // too short / empty -> let the legacy path emit the right error
  }
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  size_t got = 0;
  while (got < buf.size())
  {
    const std::size_t n = stream->Read(buf.data() + got, buf.size() - got);
    if (n == 0)
    {
      return -1; // short read -> defer to legacy
    }
    got += n;
  }

  // "solid"-prefixed files are (usually) ASCII; decline so the legacy path runs
  // -- which also handles the malformed-binary-starting-with-"solid" fallback.
  if (std::memcmp(buf.data(), "solid", 5) == 0)
  {
    return -1;
  }

  const vtkTypeInt64 bodyLen = size - 84;
  if (bodyLen % 50 != 0)
  {
    return -1; // legacy reproduces the exact "Remaining file length bad" error
  }
  const vtkTypeInt64 numFile = bodyLen / 50;
  const uint32_t numField = cvistaStlGet32LE(buf.data() + 80);
  if (static_cast<vtkTypeInt64>(numField) != numFile && !this->RelaxedConformance)
  {
    return -1; // legacy reproduces the strict count-mismatch error
  }

  // Header (80 bytes), mirroring the legacy ReadBinarySTL bookkeeping.
  if (!this->BinaryHeader)
  {
    vtkNew<vtkUnsignedCharArray> binaryHeader;
    this->SetBinaryHeader(binaryHeader);
  }
  this->BinaryHeader->SetNumberOfValues(80 + 1);
  this->BinaryHeader->FillValue(0);
  std::memcpy(this->BinaryHeader->GetPointer(0), buf.data(), 80);
  this->SetHeader(reinterpret_cast<char*>(this->BinaryHeader->GetPointer(0)));
  this->BinaryHeader->Resize(80);

  cvistaStlVertexMerger merger(static_cast<size_t>(numFile) * 3);

  // Build the triangle connectivity directly into a contiguous buffer (upper
  // bound numFile*3) rather than calling InsertNextCell per triangle; offsets
  // are uniform (stride 3) and filled once at the end.
  vtkNew<vtkIdTypeArray> connArr;
  connArr->SetNumberOfValues(numFile * 3);
  vtkIdType* conn = connArr->GetPointer(0);
  vtkIdType nKept = 0;

  for (vtkTypeInt64 f = 0; f < numFile; ++f)
  {
    const uint8_t* rec = buf.data() + 84 + 50 * f;
    // Extract the 12 floats (normal + 3 verts) once, validating finiteness as
    // we go -- matching the legacy reader, which errors on a non-finite value.
    uint32_t w12[12];
    for (int k = 0; k < 12; ++k)
    {
      const uint32_t x = cvistaStlGet32LE(rec + 4 * k);
      if (!cvistaStlWordFinite(x))
      {
        return -1; // non-finite -> legacy emits the precise error + ErrorCode
      }
      w12[k] = x;
    }
    vtkIdType nodes[3];
    for (int v = 0; v < 3; ++v)
    {
      const uint32_t w[3] = { cvistaStlCanonZero(w12[3 + 3 * v]),
        cvistaStlCanonZero(w12[4 + 3 * v]), cvistaStlCanonZero(w12[5 + 3 * v]) };
      nodes[v] = static_cast<vtkIdType>(merger.Insert(w));
    }
    // Drop degenerate triangles (>=2 merged vertices coincide), as the legacy
    // path does after locator merge.
    if (nodes[0] != nodes[1] && nodes[0] != nodes[2] && nodes[1] != nodes[2])
    {
      conn[3 * nKept + 0] = nodes[0];
      conn[3 * nKept + 1] = nodes[1];
      conn[3 * nKept + 2] = nodes[2];
      ++nKept;
    }
  }

  // Finalize the cell array: shrink connectivity to the kept triangles and
  // synthesize the uniform offset array (0, 3, 6, ...). Per the cvista int32-
  // default rule ([[cvista-int32-default-width-relaxed]]), store offsets and
  // connectivity as int32 when every value fits -- numFile*3 bounds both the
  // largest offset (nKept*3) and the largest vertex index (numPts <= nKept*3) --
  // widening to int64 only for >2^31-element meshes. int32 halves the cell-array
  // footprint; the values are identical.
  connArr->SetNumberOfValues(nKept * 3);
  vtkNew<vtkCellArray> newPolys;
  if (numFile <= static_cast<vtkTypeInt64>(0x7FFFFFFF) / 3)
  {
    vtkNew<vtkTypeInt32Array> conn32;
    conn32->SetNumberOfValues(nKept * 3);
    vtkTypeInt32* c32 = conn32->GetPointer(0);
    for (vtkIdType i = 0; i < nKept * 3; ++i)
    {
      c32[i] = static_cast<vtkTypeInt32>(conn[i]);
    }
    vtkNew<vtkTypeInt32Array> off32;
    off32->SetNumberOfValues(nKept + 1);
    vtkTypeInt32* o32 = off32->GetPointer(0);
    for (vtkIdType i = 0; i <= nKept; ++i)
    {
      o32[i] = static_cast<vtkTypeInt32>(3 * i);
    }
    newPolys->SetData(off32, conn32);
  }
  else
  {
    vtkNew<vtkIdTypeArray> offArr;
    offArr->SetNumberOfValues(nKept + 1);
    vtkIdType* off = offArr->GetPointer(0);
    for (vtkIdType i = 0; i <= nKept; ++i)
    {
      off[i] = 3 * i;
    }
    newPolys->SetData(offArr, connArr);
  }

  const vtkIdType numPts = static_cast<vtkIdType>(merger.NumberOfVertices());
  vtkNew<vtkFloatArray> coords;
  coords->SetNumberOfComponents(3);
  coords->SetNumberOfTuples(numPts);
  if (numPts > 0)
  {
    std::memcpy(coords->GetPointer(0), merger.Vertices().data(),
      static_cast<size_t>(numPts) * 3 * sizeof(float));
  }
  vtkNew<vtkPoints> newPts;
  newPts->SetData(coords);

  output->SetPoints(newPts);
  output->SetPolys(newPolys);
  output->Squeeze();
  return 1;
}

//------------------------------------------------------------------------------
bool vtkSTLReader::ReadBinarySTL(
  vtkResourceStream* stream, vtkPoints* newPts, vtkCellArray* newPolys)
{
  struct facet_t_t
  {
    float n[3], v1[3], v2[3], v3[3];
    uint16_t attrByteCount;
  };
  using facet_t = struct facet_t_t;

  vtkDebugMacro(<< "Reading BINARY STL file");

  //  File is read to obtain raw information as well as bounding box
  //
  if (!this->BinaryHeader)
  {
    vtkNew<vtkUnsignedCharArray> binaryHeader;
    this->SetBinaryHeader(binaryHeader);
  }
  constexpr int headerSize = 80;                         // fixed in STL file format
  this->BinaryHeader->SetNumberOfValues(headerSize + 1); // allocate +1 byte for zero termination
  this->BinaryHeader->FillValue(0);
  if (stream->Read(this->BinaryHeader->GetPointer(0), headerSize) != headerSize)
  {
    vtkErrorMacro("STLReader error reading file. Premature EOF while reading header.");
    return false;
  }
  this->SetHeader(reinterpret_cast<char*>(this->BinaryHeader->GetPointer(0)));
  // Remove extra zero termination from binary header
  this->BinaryHeader->Resize(headerSize);

  uint32_t numTrisField;
  if (stream->Read(&numTrisField, sizeof(numTrisField)) != sizeof(numTrisField))
  {
    vtkErrorMacro("STLReader error reading file. Premature EOF while reading triangle count.");
    return false;
  }
  vtkByteSwap::Swap4LE(&numTrisField);

  // twelve 32-bit-floating point numbers + 2 byte for attribute byte count = 50 bytes.
  vtkTypeInt64 triSize = 12 * sizeof(float) + sizeof(uint16_t);

  // How many bytes are remaining in the file?
  vtkTypeInt64 current = stream->Tell();
  vtkTypeInt64 ulFileLength = stream->Seek(0, vtkResourceStream::SeekDirection::End);
  stream->Seek(current, vtkResourceStream::SeekDirection::Begin);
  ulFileLength -= headerSize + sizeof(uint32_t); // 80 byte - header, 4 byte - triangle count
  if (ulFileLength < 0 || ulFileLength % triSize != 0)
  {
    vtkErrorMacro("STLReader error reading file. Remaining file length bad.");
    return false;
  }
  vtkTypeInt64 numTrisFile = ulFileLength / triSize;

  // Many .stl files contain bogus triangle count. Let's compare to the remaining file size. If
  // we're being strict, they should match.
  if (numTrisFile != numTrisField && !this->GetRelaxedConformance())
  {
    vtkErrorMacro("STLReader error reading file. Triangle count / file size mismatch.");
    return false;
  }

  // now allocate the memory we need for the triangles.
  // note we ignore the triangle count field and read until end of file.
  newPts->Allocate(numTrisFile * 3);
  newPolys->AllocateEstimate(numTrisFile, 3);

  facet_t facet;
  for (size_t i = 0; stream->Read(&facet, triSize) > 0; ++i)
  {
    vtkByteSwap::Swap4LE(facet.n);
    vtkByteSwap::Swap4LE(facet.n + 1);
    vtkByteSwap::Swap4LE(facet.n + 2);
    if (!std::isfinite(facet.n[0]) || !std::isfinite(facet.n[1]) || !std::isfinite(facet.n[2]))
    {
      vtkErrorMacro("Normal vector non-finite.");
      return false;
    }

    vtkByteSwap::Swap4LE(facet.v1);
    vtkByteSwap::Swap4LE(facet.v1 + 1);
    vtkByteSwap::Swap4LE(facet.v1 + 2);
    if (!std::isfinite(facet.v1[0]) || !std::isfinite(facet.v1[1]) || !std::isfinite(facet.v1[2]))
    {
      vtkErrorMacro("vertex 1 non-finite.");
      return false;
    }

    vtkByteSwap::Swap4LE(facet.v2);
    vtkByteSwap::Swap4LE(facet.v2 + 1);
    vtkByteSwap::Swap4LE(facet.v2 + 2);
    if (!std::isfinite(facet.v2[0]) || !std::isfinite(facet.v2[1]) || !std::isfinite(facet.v2[2]))
    {
      vtkErrorMacro("vertex 2 non-finite.");
      return false;
    }

    vtkByteSwap::Swap4LE(facet.v3);
    vtkByteSwap::Swap4LE(facet.v3 + 1);
    vtkByteSwap::Swap4LE(facet.v3 + 2);
    if (!std::isfinite(facet.v3[0]) || !std::isfinite(facet.v3[1]) || !std::isfinite(facet.v3[2]))
    {
      vtkErrorMacro("vertex 3 non-finite.");
      return false;
    }

    vtkIdType pts[3];
    pts[0] = newPts->InsertNextPoint(facet.v1);
    pts[1] = newPts->InsertNextPoint(facet.v2);
    pts[2] = newPts->InsertNextPoint(facet.v3);

    newPolys->InsertNextCell(3, pts);

    if ((i % 100000) == 0 && i != 0)
    {
      vtkDebugMacro(<< "triangle# " << i);
      this->UpdateProgress(static_cast<double>(i) / numTrisFile);
    }
  }

  return true;
}

//------------------------------------------------------------------------------

// Local Functions
namespace
{
inline std::string stlParseEof(const std::string& expected)
{
  return "Premature EOF while reading '" + expected + "'";
}

inline std::string stlParseExpected(const std::string& expected, const std::string& found)
{
  return "Parse error. Expecting '" + expected + "' found '" + found + "'";
}

// Get three space-delimited floats from string.
bool stlReadVertex(char* buf, float vertCoord[3])
{
  std::string_view buffer = buf;

  for (int i = 0; i < 3; ++i)
  {
    auto result = vtk::scan_value<float>(buffer);
    if (!result)
    {
      return false;
    }
    vertCoord[i] = result->value();
    buffer = result->range().data();
  }

  return true;
}

} // end of anonymous namespace

// https://en.wikipedia.org/wiki/STL_%28file_format%29#ASCII_STL
//
// Format
//
// solid [name]
//
// * where name is an optional string.
// * The file continues with any number of triangles,
//   each represented as follows:
//
// [color ...]
// facet normal ni nj nk
//     outer loop
//         vertex v1x v1y v1z
//         vertex v2x v2y v2z
//         vertex v3x v3y v3z
//     endloop
// endfacet
//
// * where each n or v is a floating-point number.
// * The file concludes with
//
// endsolid [name]

bool vtkSTLReader::ReadASCIISTL(
  vtkResourceParser* parser, vtkPoints* newPts, vtkCellArray* newPolys, vtkFloatArray* scalars)
{
  vtkDebugMacro(<< "Reading ASCII STL file");

  this->SetHeader(nullptr);
  this->SetBinaryHeader(nullptr);
  std::string header;

  std::string line;   // line buffer
  float vertCoord[3]; // scratch space when parsing "vertex %f %f %f"
  vtkIdType pts[3];   // point ids for building triangles
  int vertOff = 0;

  int solidId = -1;
  size_t lineNum = 0;

  enum StlAsciiScanState
  {
    scanSolid = 0,
    scanFacet,
    scanLoop,
    scanVerts,
    scanEndLoop,
    scanEndFacet,
    scanEndSolid
  };

  std::string errorMessage;

  for (StlAsciiScanState state = scanSolid; errorMessage.empty(); /*nil*/)
  {
    vtkParseResult res = parser->ReadLine(line);
    char* cmd = line.data();
    if (res == vtkParseResult::EndOfStream)
    {
      // If scanning for the next "solid" this is a valid way to exit,
      // but is an error if scanning for the initial "solid" or any other token

      switch (state)
      {
        case scanSolid:
        {
          // Emit error if EOF encountered without having read anything
          if (solidId < 0)
            errorMessage = stlParseEof("solid");
          break;
        }
        case scanFacet:
        {
          errorMessage = stlParseEof("facet");
          break;
        }
        case scanLoop:
        {
          errorMessage = stlParseEof("outer loop");
          break;
        }
        case scanVerts:
        {
          errorMessage = stlParseEof("vertex");
          break;
        }
        case scanEndLoop:
        {
          errorMessage = stlParseEof("endloop");
          break;
        }
        case scanEndFacet:
        {
          errorMessage = stlParseEof("endfacet");
          break;
        }
        case scanEndSolid:
        {
          errorMessage = stlParseEof("endsolid");
          break;
        }
      }

      // Terminate the parsing loop
      break;
    }

    // Cue to the first non-space.
    while (isspace(*cmd))
    {
      ++cmd;
    }

    // An empty line - try again
    if (!*cmd)
    {
      // Increment line-number, but not while still in the header
      if (lineNum)
        ++lineNum;
      continue;
    }

    // Ensure consistent case on the first token and separate from
    // subsequent arguments

    char* arg = cmd;
    while (*arg && !isspace(*arg))
    {
      *arg = tolower(*arg);
      ++arg;
    }

    // Terminate first token (cmd)
    if (*arg)
    {
      *arg = '\0';
      ++arg;

      while (isspace(*arg))
      {
        ++arg;
      }
    }

    ++lineNum;

    // Handle all expected parsed elements
    switch (state)
    {
      case scanSolid:
      {
        if (!strcmp(cmd, "solid"))
        {
          ++solidId;
          state = scanFacet; // Next state
          if (!header.empty())
          {
            header += "\n";
          }
          if (*arg)
          {
            header += arg;
            // strip end-of-line character from the end
            while (!header.empty() && (header.back() == '\r' || header.back() == '\n'))
            {
              header.pop_back();
            }
          }
        }
        else
        {
          errorMessage = stlParseExpected("solid", cmd);
        }
        break;
      }
      case scanFacet:
      {
        if (!strcmp(cmd, "color"))
        {
          // Optional 'color' entry (after solid) - continue looking for 'facet'
          continue;
        }

        if (!strcmp(cmd, "facet"))
        {
          state = scanLoop; // Next state
        }
        else if (!strcmp(cmd, "endsolid"))
        {
          // Finished with 'endsolid' - find next solid
          state = scanSolid;
        }
        else
        {
          errorMessage = stlParseExpected("facet", cmd);
        }
        break;
      }
      case scanLoop:
      {
        if (!strcmp(cmd, "outer")) // More pedantic => && !strcmp(arg, "loop")
        {
          state = scanVerts; // Next state
        }
        else
        {
          errorMessage = stlParseExpected("outer loop", cmd);
        }
        break;
      }
      case scanVerts:
      {
        if (!strcmp(cmd, "vertex"))
        {
          if (stlReadVertex(arg, vertCoord))
          {
            pts[vertOff] = newPts->InsertNextPoint(vertCoord);
            ++vertOff; // Next vertex

            if (vertOff >= 3)
            {
              // Finished this triangle.
              vertOff = 0;
              state = scanEndLoop; // Next state

              // Save as cell
              newPolys->InsertNextCell(3, pts);
              if (scalars)
              {
                scalars->InsertNextValue(solidId);
              }

              if ((newPolys->GetNumberOfCells() % 5000) == 0)
              {
                this->UpdateProgress((newPolys->GetNumberOfCells() % 50000) / 50000.0);
              }
            }
          }
          else
          {
            errorMessage = "Parse error reading STL vertex";
          }
        }
        else
        {
          errorMessage = stlParseExpected("vertex", cmd);
        }
        break;
      }
      case scanEndLoop:
      {
        if (!strcmp(cmd, "endloop"))
        {
          state = scanEndFacet; // Next state
        }
        else
        {
          errorMessage = stlParseExpected("endloop", cmd);
        }
        break;
      }
      case scanEndFacet:
      {
        if (!strcmp(cmd, "endfacet"))
        {
          state = scanFacet; // Next facet, or endsolid
        }
        else
        {
          errorMessage = stlParseExpected("endfacet", cmd);
        }
        break;
      }
      case scanEndSolid:
      {
        if (!strcmp(cmd, "endsolid"))
        {
          state = scanSolid; // Start over again
        }
        else
        {
          errorMessage = stlParseExpected("endsolid", cmd);
        }
        break;
      }
    }
  }

  this->SetHeader(header.c_str());

  if (!errorMessage.empty())
  {
    vtkDebugMacro("STLReader: unable to read line " << lineNum << ": " << errorMessage);
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Specify a spatial locator for merging points. By
// default an instance of vtkMergePoints is used.
vtkIncrementalPointLocator* vtkSTLReader::NewDefaultLocator()
{
  return vtkMergePoints::New();
}

//------------------------------------------------------------------------------
void vtkSTLReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "RelaxedConformance: " << (this->RelaxedConformance ? "On\n" : "Off\n");
  os << indent << "Merging: " << (this->Merging ? "On\n" : "Off\n");
  os << indent << "ScalarTags: " << (this->ScalarTags ? "On\n" : "Off\n");
  os << indent << "Locator: ";
  if (this->Locator)
  {
    this->Locator->PrintSelf(os << endl, indent.GetNextIndent());
  }
  else
  {
    os << "(none)\n";
  }
}
VTK_ABI_NAMESPACE_END
