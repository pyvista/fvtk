// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkInformationInternals
 * @brief   internal structure for vtkInformation
 *
 * vtkInformationInternals is used in internal implementation of
 * vtkInformation. This should only be accessed by friends
 * and sub-classes of that class.
 */

#ifndef vtkInformationInternals_h
#define vtkInformationInternals_h

#include "vtkInformationKey.h"
#include "vtkObjectBase.h"

#include <algorithm> // std::lower_bound
#include <cstdint>
#include <utility> // std::pair
#include <vector>

//----------------------------------------------------------------------------
VTK_ABI_NAMESPACE_BEGIN

// fvtk: flat small-map for vtkInformation's key->value store.
//
// Stock VTK keys this on std::unordered_map<vtkInformationKey*, vtkObjectBase*>
// (VTK_INFORMATION_USE_HASH_MAP). But a vtkInformation typically holds only a
// HANDFUL of keys, and the access pattern is lookup-dominated and EXTREMELY hot:
// every pipeline Update re-walks ComputePipelineMTime, and every render calls
// vtkMapper::Has{Opaque,Translucent}Geometry / GetBounds per actor — thousands of
// GetAsObjectBase() lookups per frame in a many-actor scene (profiled at ~8% of a
// CPU-bound frame). For maps this small a hash bucket index + heap-node pointer
// chase (cache miss) + per-entry node malloc costs more than a linear/branchless
// scan over a single contiguous {key,value} block that stays in cache.
//
// fvtkInformationFlatMap is that block: a std::vector of (key,value) pairs kept
// sorted by key POINTER, so find() is a binary search and iteration is ordered by
// pointer — exactly the std::map ordering, and a deterministic re-ordering of the
// stock unordered_map. BIT-EXACT: the bitexact gate already compares stock-vtk and
// fvtk as separate processes whose pointer-hashed iteration orders differ (ASLR)
// yet match byte-for-byte, so no output depends on info-key iteration order; a
// pointer-sorted order is therefore equally safe. It exposes exactly the STL
// subset vtkInformation.cxx / vtkInformationIterator.cxx use (begin/end/find/
// insert/erase + forward iterators with ->first/->second), so it is a drop-in.
class fvtkInformationFlatMap
{
public:
  typedef vtkInformationKey* KeyType;
  typedef vtkObjectBase* DataType;
  typedef std::pair<KeyType, DataType> value_type;
  typedef std::vector<value_type> StoreType;
  typedef StoreType::iterator iterator;
  typedef StoreType::const_iterator const_iterator;
  typedef StoreType::size_type size_type;

  iterator begin() { return this->Store.begin(); }
  iterator end() { return this->Store.end(); }
  const_iterator begin() const { return this->Store.begin(); }
  const_iterator end() const { return this->Store.end(); }

  size_type size() const { return this->Store.size(); }

  static bool KeyLess(const value_type& e, KeyType key) { return e.first < key; }

  iterator find(KeyType key)
  {
    iterator i = std::lower_bound(this->Store.begin(), this->Store.end(), key, KeyLess);
    if (i != this->Store.end() && i->first == key)
    {
      return i;
    }
    return this->Store.end();
  }
  const_iterator find(KeyType key) const
  {
    const_iterator i = std::lower_bound(this->Store.begin(), this->Store.end(), key, KeyLess);
    if (i != this->Store.end() && i->first == key)
    {
      return i;
    }
    return this->Store.end();
  }

  // Insert keeping the store sorted by key pointer. Mirrors std::map::insert:
  // a no-op if the key is already present (callers guard with find() first, but
  // stay correct regardless).
  std::pair<iterator, bool> insert(const value_type& entry)
  {
    iterator i = std::lower_bound(this->Store.begin(), this->Store.end(), entry.first, KeyLess);
    if (i != this->Store.end() && i->first == entry.first)
    {
      return std::make_pair(i, false);
    }
    iterator ins = this->Store.insert(i, entry);
    return std::make_pair(ins, true);
  }

  iterator erase(iterator pos) { return this->Store.erase(pos); }

private:
  StoreType Store;
};
class vtkInformationInternals
{
public:
  typedef vtkInformationKey* KeyType;
  typedef vtkObjectBase* DataType;
  typedef fvtkInformationFlatMap MapType;
  MapType Map;

  vtkInformationInternals() = default;

  ~vtkInformationInternals()
  {
    for (MapType::iterator i = this->Map.begin(); i != this->Map.end(); ++i)
    {
      if (vtkObjectBase* value = i->second)
      {
        value->UnRegister(nullptr);
      }
    }
  }

private:
  vtkInformationInternals(vtkInformationInternals const&) = delete;
};

VTK_ABI_NAMESPACE_END
#endif
// VTK-HeaderTest-Exclude: vtkInformationInternals.h
