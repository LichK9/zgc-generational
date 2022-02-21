/*
 * Copyright (c) 2015, 2020, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zCollectedHeap.hpp"
#include "gc/z/zForwarding.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "utilities/align.hpp"

//
// Reference count states:
//
// * If the reference count is zero, it will never change again.
//
// * If the reference count is positive, it can be both retained
//   (increased) and released (decreased).
//
// * If the reference count is negative, is can only be released
//   (increased). A negative reference count means that one or more
//   threads are waiting for one or more other threads to release
//   their references.
//
// The reference lock is used for waiting until the reference
// count has become zero (released) or negative one (claimed).
//

static const ZStatCriticalPhase ZCriticalPhaseRelocationStall("Relocation Stall");

bool ZForwarding::claim() {
  return Atomic::cmpxchg(&_claimed, false, true) == false;
}

void ZForwarding::in_place_relocation_start(zoffset relocated_watermark) {
  _page->log_msg(" In-place reloc start  - relocated to: " PTR_FORMAT, untype(relocated_watermark));

  _in_place = true;

  // Support for ZHeap::is_in checks of from-space objects
  // in a page that is in-place relocating
  Atomic::store(&_in_place_thread, Thread::current());
  _in_place_top_at_start = _page->top();
}

void ZForwarding::in_place_relocation_finish() {
  assert(_in_place, "Must be an in-place relocated page");

  _page->log_msg(" In-place reloc finish - top at start: " PTR_FORMAT, untype(_in_place_top_at_start));

  if (_from_age == ZPageAge::old || _to_age != ZPageAge::old) {
    // Only do this for non-promoted pages, that still need to reset live map.
    // Done with iterating over the "from-page" view, so can now drop the _livemap.
    _page->finalize_reset_for_in_place_relocation();
  }

  // Disable relaxed ZHeap::is_in checks
  Atomic::store(&_in_place_thread, (Thread*)nullptr);
}

bool ZForwarding::in_place_relocation_is_below_top_at_start(zoffset offset) const {
  // Only the relocating thread is allowed to know about the old relocation top.
  return Atomic::load(&_in_place_thread) == Thread::current() && offset < _in_place_top_at_start;
}

bool ZForwarding::retain_page() {
  for (;;) {
    const int32_t ref_count = Atomic::load_acquire(&_ref_count);

    if (ref_count == 0) {
      // Released
      return false;
    }

    if (ref_count < 0) {
      // Claimed
      const bool success = wait_page_released();
      assert(success, "Should always succeed");
      return false;
    }

    if (Atomic::cmpxchg(&_ref_count, ref_count, ref_count + 1) == ref_count) {
      // Retained
      return true;
    }
  }
}

void ZForwarding::in_place_relocation_claim_page() {
  for (;;) {
    const int32_t ref_count = Atomic::load(&_ref_count);
    assert(ref_count > 0, "Invalid state");

    // Invert reference count
    if (Atomic::cmpxchg(&_ref_count, ref_count, -ref_count) != ref_count) {
      continue;
    }

    // If the previous reference count was 1, then we just changed it to -1,
    // and we have now claimed the page. Otherwise we wait until it is claimed.
    if (ref_count != 1) {
      ZLocker<ZConditionLock> locker(&_ref_lock);
      while (Atomic::load_acquire(&_ref_count) != -1) {
        _ref_lock.wait();
      }
    }

    // Done
    break;
  }
}

void ZForwarding::release_page() {
  for (;;) {
    const int32_t ref_count = Atomic::load(&_ref_count);
    assert(ref_count != 0, "Invalid state");

    if (ref_count > 0) {
      // Decrement reference count
      if (Atomic::cmpxchg(&_ref_count, ref_count, ref_count - 1) != ref_count) {
        continue;
      }

      // If the previous reference count was 1, then we just decremented
      // it to 0 and we should signal that the page is now released.
      if (ref_count == 1) {
        // Notify released
        ZLocker<ZConditionLock> locker(&_ref_lock);
        _ref_lock.notify_all();
      }
    } else {
      // Increment reference count
      if (Atomic::cmpxchg(&_ref_count, ref_count, ref_count + 1) != ref_count) {
        continue;
      }

      // If the previous reference count was -2 or -1, then we just incremented it
      // to -1 or 0, and we should signal the that page is now claimed or released.
      if (ref_count == -2 || ref_count == -1) {
        // Notify claimed or released
        ZLocker<ZConditionLock> locker(&_ref_lock);
        _ref_lock.notify_all();
      }
    }

    return;
  }
}

bool ZForwarding::wait_page_released() const {
  if (Atomic::load_acquire(&_ref_count) != 0) {
    ZStatTimer timer(ZCriticalPhaseRelocationStall);
    ZLocker<ZConditionLock> locker(&_ref_lock);
    while (Atomic::load_acquire(&_ref_count) != 0) {
      if (_ref_abort) {
        return false;
      }

      _ref_lock.wait();
    }
  }

  return true;
}

ZPage* ZForwarding::detach_page() {
  // Wait until released
  if (Atomic::load_acquire(&_ref_count) != 0) {
    ZLocker<ZConditionLock> locker(&_ref_lock);
    while (Atomic::load_acquire(&_ref_count) != 0) {
      _ref_lock.wait();
    }
  }

  return _page;
}

ZPage* ZForwarding::page() {
  assert(Atomic::load(&_ref_count) != 0, "The page has been released/detached");
  return _page;
}

void ZForwarding::abort_page() {
  ZLocker<ZConditionLock> locker(&_ref_lock);
  assert(Atomic::load(&_ref_count) > 0, "Invalid state");
  assert(!_ref_abort, "Invalid state");
  _ref_abort = true;
  _ref_lock.notify_all();
}

//
// The relocated_remembered_fields are used when the old generation
// collection is relocating objects, concurrently with the young
// generation collection's remembered set scanning for the marking.
//
// When the OC is relocating objects, the old remembered set bits
// for the from-space objects need to be moved over to the to-space
// objects.
//
// The YC doesn't want to wait for the OC, so it eagerly helps relocating
// objects with remembered set bits, so that it can perform marking on the
// to-space copy of the object fields that are associated with the remembered
// set bits.
//
// This requires some synchronization between the OC and YC, and this is
// mainly done via the _relocated_remembered_fields_state in each ZForwarding.
// The values corresponds to:
//
// 0: Starting state - neither OC nor YC has stated their intentions
// 1: The OC has completed relocating all objects, and published an array
//    of all to-space fields that should have a remembered set entry.
// 2: The OC relocation of the page happened concurrentely with the YC
//    remset scanning. Two situations:
//    a) The page had not been released yet: The YC eagerly relocated and scanned
//    the to-space objects with remset entries.
//    b) The page had been released: The YC accepts the array published in (1).
// 3: The YC found that the forwarding/page had already been relocated when
//    the YC started.
//
// Central to this logic is the ZRemembered::scan_forwarding function, where
// the YC tries to "retain" the forwarding/page. If it succeeds it means that
// the OC has not finished (or maybe not even started) the relocation of all objects.
//
// When the YC manages to retaining the page it will bring the state from:
//  0 -> 2 - Started collecting remembered set info
//  1 -> 2 - Rejected the OC's remembered set info
//  2 -> 2 - An earlier YC had already handled the remembered set info
//  3 ->   - Invalid state - will not happen
//
// When the YC fails to retain the page the state transitions are:
// 0 -> x - The page was relocated before the YC started
// 1 -> x - The OC completed relocation before YC visited this forwarding.
//          The YC will use the remembered set info collected by the OC.
// 2 -> x - A previous YC has already handled the remembered set info
// 3 -> x - See above
//
// x is:
//  2 - if the relocation finished while the current YC was running
//  3 - if the relocation finished before the current YC started
//
// Note the subtlety that even though the relocation could released the page
// and made it non-retainable, the relocation code might not have gotten to
// the point where the page is removed from the page table. It could also be
// the case that the relocated page became in-place relocated, and we therefore
// shouldn't be scanning it this YC.
//
// The (2) state is the "dangerous" state, where both OC and YC work on
// the same forwarding/page somewhat concurrently. While (3) denotes that
// that the entire relocation of a page (including freeing/reusing it) was
// completed before the current YC started.
//
// After all remset entries of relocated objects have been scanned, the code
// proceeds to visit all pages in the page table, to scan all pages not part
// of the OC relocation set. Pages with virtual addresses that doesn't match
// any of the once in the OC relocation set will be visited. Pages with
// virtual address that *do* have a corresponding forwarding entry has two
// cases:
//
// a) The forwarding entry is marked with (2). This means that the
//    corresponding page is guaranteed to be one that has been relocated by the
//    current OC during the active YC. Any remset entry is guaranteed to have
//    already been scanned by the scan_forwarding code.
//
// b) The forwarding entry is marked with (3). This means that the page was
//    *not* created by the OC relocation during this YC, which means that the
//    page must be scanned.
//

void ZForwarding::relocated_remembered_fields_after_relocate() {
  assert(from_age() == ZPageAge::old, "Only old pages have remsets");

  _relocated_remembered_fields_publish_young_seqnum = ZGeneration::young()->seqnum();

  if (ZGeneration::young()->is_phase_mark()) {
    relocated_remembered_fields_publish();
  }
}

void ZForwarding::relocated_remembered_fields_publish() {


  // The OC has relocated all objects and collected all fields that
  // used to have remembered set entries. Now publish the fields to
  // the YC.

  const int res = Atomic::cmpxchg(&_relocated_remembered_fields_state, 0, 1);

  // 0: OK to publish
  // 1: Not possible - this operation makes this transition
  // 2: YC started scanning the "from" page concurrently and rejects the fields
  //    the OC collected.
  // 3: YC accepted the fields published by this function - not possible
  //    because they weren't published before the CAS above

  if (res == 0) {
    // fields were successfully published
    log_debug(gc, remset)("Forwarding remset published       : " PTR_FORMAT " " PTR_FORMAT, untype(start()), untype(end()));

    return;
  }

  log_debug(gc, remset)("Forwarding remset discarded       : " PTR_FORMAT " " PTR_FORMAT, untype(start()), untype(end()));

  // 2: YC scans the remset concurrently
  // 3: YC accepted published remset - not possible, we just atomically published it
  //    YC failed to retain page - not possible, since the current page is retainable
  assert(res == 2, "Unexpected value");

  // YC has rejected the stored values and will (or have already) find them them itself
  _relocated_remembered_fields_array.clear_and_deallocate();
}

void ZForwarding::relocated_remembered_fields_notify_concurrent_scan_of() {
  // Invariant: The page is being retained
  assert(ZGeneration::young()->is_phase_mark(), "Only called when");

  const int res = Atomic::cmpxchg(&_relocated_remembered_fields_state, 0, 2);

  // 0: OC has not completed relocation
  // 1: OC has completed and published all relocated remembered fields
  // 2: A previous YC has already handled the field
  // 3: A previous YC has determined that there's no concurrency between
  //    OC relocation and YC remembered fields scanning - not possible
  //    since the page has been retained (still being relocated) and
  //    we are in the process of scanning fields

  if (res == 0) {
    // Successfully notified and rejected any collected data from the OC
    log_debug(gc, remset)("Forwarding remset eager           : " PTR_FORMAT " " PTR_FORMAT, untype(start()), untype(end()));

    return;
  }

  if (res == 1) {
    // OC relocation already collected and published fields

    // TODO: Consider using this information instead of throwing it away?

    // Still notify concurrent scanning and reject the collected data from the OC
    const int res2 = Atomic::cmpxchg(&_relocated_remembered_fields_state, 1, 2);
    assert(res2 == 1, "Should not fail");

    log_debug(gc, remset)("Forwarding remset eager and reject: " PTR_FORMAT " " PTR_FORMAT, untype(start()), untype(end()));

    // The YC rejected the publish fields and is responsible for the array
    // Eagerly deallocate the memory
    _relocated_remembered_fields_array.clear_and_deallocate();
    return;
  }

  log_debug(gc, remset)("Forwarding remset redundant       : " PTR_FORMAT " " PTR_FORMAT, untype(start()), untype(end()));

  // Previous YC already handled the remembered fields
  assert(res == 2, "Unexpected value");
}

void ZForwarding::verify() const {
  guarantee(_ref_count != 0, "Invalid reference count");
  guarantee(_page != NULL, "Invalid page");

  uint32_t live_objects = 0;
  size_t live_bytes = 0;

  for (ZForwardingCursor i = 0; i < _entries.length(); i++) {
    const ZForwardingEntry entry = at(&i);
    if (!entry.populated()) {
      // Skip empty entries
      continue;
    }

    // Check from index
    guarantee(entry.from_index() < _page->object_max_count(), "Invalid from index");

    // Check for duplicates
    for (ZForwardingCursor j = i + 1; j < _entries.length(); j++) {
      const ZForwardingEntry other = at(&j);
      if (!other.populated()) {
        // Skip empty entries
        continue;
      }

      guarantee(entry.from_index() != other.from_index(), "Duplicate from");
      guarantee(entry.to_offset() != other.to_offset(), "Duplicate to");
    }

    const zaddress to_addr = ZOffset::address(to_zoffset(entry.to_offset()));
    const size_t size = ZUtils::object_size(to_addr);
    const size_t aligned_size = align_up(size, _page->object_alignment());
    live_bytes += aligned_size;
    live_objects++;
  }

  // Verify number of live objects and bytes
  _page->verify_live(live_objects, live_bytes, _in_place);
}
