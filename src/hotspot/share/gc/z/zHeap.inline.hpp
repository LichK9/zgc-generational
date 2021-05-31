/*
 * Copyright (c) 2015, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZHEAP_INLINE_HPP
#define SHARE_GC_Z_ZHEAP_INLINE_HPP

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zForwardingTable.inline.hpp"
#include "gc/z/zCycleId.hpp"
#include "gc/z/zCycle.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHash.inline.hpp"
#include "gc/z/zHeap.hpp"
#include "gc/z/zMark.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageTable.inline.hpp"
#include "gc/z/zRemember.inline.hpp"
#include "utilities/debug.hpp"

inline ZHeap* ZHeap::heap() {
  assert(_heap != NULL, "Not initialized");
  return _heap;
}

inline ZGeneration* ZHeap::get_generation(ZCycleId id) {
  if (id == ZCycleId::_minor) {
    return &_young_generation;
  } else {
    return &_old_generation;
  }
}

inline ZGeneration* ZHeap::get_generation(ZGenerationId id) {
  if (id == ZGenerationId::young) {
    return &_young_generation;
  } else {
    return &_old_generation;
  }
}

inline ZYoungGeneration* ZHeap::young_generation() {
  return &_young_generation;
}

inline ZOldGeneration* ZHeap::old_generation() {
  return &_old_generation;
}

inline ZCycle* ZHeap::get_cycle(ZCycleId id) {
  if (id == ZCycleId::_minor) {
    return &_minor_cycle;
  } else {
    return &_major_cycle;
  }
}

inline ZCycle* ZHeap::get_cycle(ZGenerationId id) {
  if (id == ZGenerationId::young) {
    return &_minor_cycle;
  } else {
    return &_major_cycle;
  }
}

inline ZMinorCycle* ZHeap::minor_cycle() {
  return &_minor_cycle;
}

inline ZMajorCycle* ZHeap::major_cycle() {
  return &_major_cycle;
}

inline uint32_t ZHeap::hash_oop(zaddress addr) const {
  const zoffset offset = ZAddress::offset(addr);
  return ZHash::offset_to_uint32(offset);
}

inline bool ZHeap::is_young(zaddress addr) const {
  ZPage* const page = ZHeap::heap()->page(addr);
  return page->is_young();
}

inline bool ZHeap::is_old(zaddress addr) const {
  return !is_young(addr);
}

inline ZCycle* ZHeap::remap_cycle(zpointer ptr) {
  assert(!ZPointer::is_load_good(ptr), "no need to remap load-good pointer");

  if (ZPointer::is_major_load_good(ptr)) {
    return &_minor_cycle;
  }

  if (ZPointer::is_minor_load_good(ptr)) {
    return &_major_cycle;
  }

  // Double remap bad - the pointer is neither major load good nor
  // minor load good. First the code ...

  const uintptr_t remembered_bits = untype(ptr) & ZAddressRememberedMask;
  const bool old_to_old_ptr = remembered_bits == ZAddressRememberedMask;

  if (old_to_old_ptr) {
    return &_major_cycle;
  }

  const zaddress_unsafe addr = ZPointer::uncolor_unsafe(ptr);
  if (_minor_cycle.forwarding(addr) != NULL) {
    assert(_major_cycle.forwarding(addr) == NULL, "Mutually exclusive");
    return &_minor_cycle;
  } else {
    return &_major_cycle;
  }

  // ... then the explanation. Time to put your seat belt on.

  // In this context we only have access to the ptr (colored oop), but we
  // don't know if this refers to a stale young gen or old gen object.
  // However, by being careful with when we run minor and major cycles,
  // and by explicitly remapping roots we can figure this out by looking
  // at the metadata bits in the pointer.

  // *Roots (including remset)*:
  //
  //will never have double remap bit errors,
  // and will never enter this path. The reason is that there's always a
  // phase that remaps all roots between all relocation phases:
  //
  // 1) Minor marking remaps the roots, before the minor relocate runs
  //
  // 2) The major roots_remap phase blocks out minor cycles and runs just
  //    before major relocate starts

  // *Heap object fields*:
  //
  // could have double remap bit errors, and may enter this path. We are using
  // knowledge about the how *remember* bits are to narrow down the
  // possibilities.

  // Short summary:
  //
  // If both remember bits are set, when we have a double
  // remap bit error, then we know that we are dealing with
  // an old-to-old pointer.
  //
  // Otherwise, we are dealing with a young-to-any pointer,
  // and the address that contained the pointed-to object, is
  // guaranteed to have only been used by either the young gen
  // the old gen.

  // Longer explanation:

  // Double remap bad pointers in young gen:
  //
  // After minor relocate, the young gen objects where promoted to old gen,
  // and we keep track of those old-to-young pointers via the remset
  // (described above in the roots section).
  //
  // However, when minor mark started, the current set of young gen objects
  // are snapshotted, and subsequent allocations end up in the next minor
  // collection. Between minor mark start, and minor relocate start, stores
  // can happen to the "young allocating" objects. These pointers will become
  // minor remap bad after minor relocate start. We don't maintain a remset
  // for the young allocating objects, so we don't have the same guarantee as
  // we have for roots (including remset). Pointers in these objects are
  // therefore therefore susceptible to become double remap bad.
  //
  // The scenario that can happen is:
  //   - Store in young allocating happens between minor mark start and minor
  //     relocate start
  //   - Minor relocate start makes this pointer minor remap bad
  //   - It is NOT fixed in roots_remap (it is not part of the remset or roots)
  //   - Major relocate start makes this pointer also major remap bad

  // Double remap bad pointers in old gen:
  //
  // When an object is promoted, all oop*s are added to the remset. (Could
  // have either double or single remember bits at this point)
  //
  // As long as we have a remset entry for the oop*, we ensure that the pointer
  // is not double remap bad. See the roots section.
  //
  // However, at some point the GC notices that the pointer points to an old
  // object, and that there's no need for a remset entry. Because of that,
  // the minor collection will not visit the pointer, and the pointer can
  // become double remap bad.
  //
  // The scenario that can happen is:
  //   - Major mark visits the object
  //   - Major relocate starts and then minor relocate starts
  //      or
  //   - Minor relocate start and then major relocate starts

  // About double *remember* bits:
  //
  // Whenever we:
  // - perform a store barrier, we heal with one remember bit.
  // - perform a non-store barrier, we heal with double remember bits.
  // - "remset forget" a pointer in an old object, we heal with double
  //   remember bits.
  //
  // Double remember bits ensures that *every* store that encounters it takes
  // a slow path.
  //
  // If we encounter a pointer that is both double remap bad *and* has double
  // remember bits, we know that it is can't be young and it has to be old!
  //
  // Pointers in young objects:
  //
  // The only double remap bad young pointers are inside "young allocating"
  // objects, as described above. When such a pointer was written into the
  // young allocating memory, the pointer was remap good and the store
  // barrier healed with a single remember bit. No other barrier could
  // replace that bit, because store good is the greatest barrier, and all
  // other barriers will take the fast-path. This is true until the minor
  // relocate starts.
  //
  // After the minor relocate has started, the pointer becames minor remap
  // bad, and maybe we even started a major relocate, and the pointer became
  // double remap bad. When the next load barrier triggers, it will self heal
  // with double remember bits, but *importantly* it will at the same time
  // heal with good remap bits.
  //
  // So, if we have entered this "double remap bad" path, and the pointer was
  // located in young gen, then it was young allocating, and it must only have
  // one remember bit set!
  //
  // Pointers in old objects:
  //
  // When pointers become forgotten, they are tagged with double remembered
  // bits. Only way to convert the pointer into having only one remembered
  // bit, is to perform a store. When that happens, the pointer becomes both
  // remap good and remembered again, and will be handled as the roots
  // described above.

  // With the above information:
  //
  // Iff we find a double remap bad pointer with *double remember bits*,
  // then we know that it is an old-to-old pointer, and we should use the
  // forwarding table of the major cycle.
  //
  // Iff we find a double remap bad pointer with a *single remember bit*,
  // then we know that it is and young-to-any pointer. We still don't know
  // if the pointed-to object is young or old.

  // Figuring out if a double remap bad pointer in young allocating is
  // young or old:
  //
  // The scenario that created a double remap bad pointer in the young
  // allocating memory is that it was written during the last minor mark
  // before the major relocate started. At that point, the major cycle has
  // already taken its marking snapshot, and determined what pages will be
  // marked and therefore eligible to become part of the major relocation
  // set. If the minor cycle relocated/freed a page (address range), and that
  // address range was then reused for an old page, it won't be part of the
  // major snapshot and it therefore won't be selected for major relocation.
  //
  // Because of this, we know that the object written into the young
  // allocating page will at most belong to one of the two relocation sets,
  // and we can therefore simply check in which table we installed
  // ZForwarding.
}

inline ZPage* ZHeap::page(zaddress addr) const {
  return _page_table.get(addr);
}

inline bool ZHeap::is_object_live(zaddress addr) const {
  ZPage* page = _page_table.get(addr);
  return page->is_object_live(addr);
}

inline bool ZHeap::is_object_strongly_live(zaddress addr) const {
  ZPage* page = _page_table.get(addr);
  return page->is_object_strongly_live(addr);
}

template <bool follow, bool finalizable, bool publish>
inline void ZHeap::mark_object(zaddress addr) {
  assert(oopDesc::is_oop(to_oop(addr), false), "must be oop");

  if (is_old(addr)) {
    if (_major_cycle.phase() == ZPhase::Mark) {
      _major_cycle.mark_object<follow, finalizable, publish>(addr);
    }
  } else {
    if (_minor_cycle.phase() == ZPhase::Mark) {
      _minor_cycle.mark_object<follow, ZMark::Strong, publish>(addr);
    }
  }
}

template <bool follow, bool publish>
inline void ZHeap::mark_minor_object(zaddress addr) {
  assert(_minor_cycle.phase() == ZPhase::Mark, "Wrong phase");
  assert(oopDesc::is_oop(to_oop(addr), false), "must be oop");

  if (is_young(addr)) {
    _minor_cycle.mark_object<follow, ZMark::Strong, publish>(addr);
  }
}

inline void ZHeap::remember(volatile zpointer* p) {
  _young_generation.remember(p);
}

inline void ZHeap::remember_filtered(volatile zpointer* p) {
  if (is_old(to_zaddress(uintptr_t(p)))) {
    remember(p);
  }
}

inline void ZHeap::remember_fields(zaddress addr) {
  _young_generation.remember_fields(addr);
}

inline void ZHeap::remember_fields_filtered(zaddress addr) {
  if (is_old(addr)) {
    remember_fields(addr);
  }
}

inline bool ZHeap::is_remembered(volatile zpointer* p) {
  return _young_generation.is_remembered(p);
}

inline void ZHeap::mark_follow_invisible_root(zaddress addr, size_t size) {
  if (is_old(addr)) {
    assert(_major_cycle.phase() == ZPhase::Mark, "Mark not allowed");
    _major_cycle.mark_follow_invisible_root(addr, size);
  } else {
    assert(_minor_cycle.phase() == ZPhase::Mark, "Mark not allowed");
    _minor_cycle.mark_follow_invisible_root(addr, size);
  }
}

inline bool ZHeap::is_alloc_stalled() const {
  return _page_allocator.is_alloc_stalled();
}

inline void ZHeap::check_out_of_memory() {
  _page_allocator.check_out_of_memory();
}

inline bool ZHeap::is_oop(uintptr_t addr) const {
  return is_in(addr);
}

#endif // SHARE_GC_Z_ZHEAP_INLINE_HPP
