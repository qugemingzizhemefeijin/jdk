/*
 * Copyright (c) 2001, 2013, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#include "precompiled.hpp"
#include "gc_implementation/shared/collectorCounters.hpp"
#include "gc_implementation/shared/gcPolicyCounters.hpp"
#include "gc_implementation/shared/gcHeapSummary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/spaceDecorator.hpp"
#include "memory/defNewGeneration.inline.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/genCollectedHeap.hpp"
#include "memory/genOopClosures.inline.hpp"
#include "memory/genRemSet.hpp"
#include "memory/generationSpec.hpp"
#include "memory/iterator.hpp"
#include "memory/referencePolicy.hpp"
#include "memory/space.inline.hpp"
#include "oops/instanceRefKlass.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/copy.hpp"
#include "utilities/stack.inline.hpp"

//
// DefNewGeneration functions.

// Methods of protected closure types.

DefNewGeneration::IsAliveClosure::IsAliveClosure(Generation* g) : _g(g) {
  assert(g->level() == 0, "Optimized for youngest gen.");
}
bool DefNewGeneration::IsAliveClosure::do_object_b(oop p) {
  return (HeapWord*)p >= _g->reserved().end() || p->is_forwarded();
}

DefNewGeneration::KeepAliveClosure::
KeepAliveClosure(ScanWeakRefClosure* cl) : _cl(cl) {
  GenRemSet* rs = GenCollectedHeap::heap()->rem_set();
  assert(rs->rs_kind() == GenRemSet::CardTable, "Wrong rem set kind.");
  _rs = (CardTableRS*)rs;
}

void DefNewGeneration::KeepAliveClosure::do_oop(oop* p)       { DefNewGeneration::KeepAliveClosure::do_oop_work(p); }
void DefNewGeneration::KeepAliveClosure::do_oop(narrowOop* p) { DefNewGeneration::KeepAliveClosure::do_oop_work(p); }


DefNewGeneration::FastKeepAliveClosure::
FastKeepAliveClosure(DefNewGeneration* g, ScanWeakRefClosure* cl) :
  DefNewGeneration::KeepAliveClosure(cl) {
  _boundary = g->reserved().end();
}

void DefNewGeneration::FastKeepAliveClosure::do_oop(oop* p)       { DefNewGeneration::FastKeepAliveClosure::do_oop_work(p); }
void DefNewGeneration::FastKeepAliveClosure::do_oop(narrowOop* p) { DefNewGeneration::FastKeepAliveClosure::do_oop_work(p); }

DefNewGeneration::EvacuateFollowersClosure::
EvacuateFollowersClosure(GenCollectedHeap* gch, int level,
                         ScanClosure* cur, ScanClosure* older) :
  _gch(gch), _level(level),
  _scan_cur_or_nonheap(cur), _scan_older(older)
{}

void DefNewGeneration::EvacuateFollowersClosure::do_void() {
  do {
    _gch->oop_since_save_marks_iterate(_level, _scan_cur_or_nonheap,
                                       _scan_older);
  } while (!_gch->no_allocs_since_save_marks(_level));
}

DefNewGeneration::FastEvacuateFollowersClosure::
FastEvacuateFollowersClosure(GenCollectedHeap* gch, int level,
                             DefNewGeneration* gen,
                             FastScanClosure* cur, FastScanClosure* older) :
  _gch(gch), _level(level), _gen(gen),
  _scan_cur_or_nonheap(cur), _scan_older(older)
{}

void DefNewGeneration::FastEvacuateFollowersClosure::do_void() {
  do {
    _gch->oop_since_save_marks_iterate(_level, _scan_cur_or_nonheap,
                                       _scan_older);
  } while (!_gch->no_allocs_since_save_marks(_level));
  guarantee(_gen->promo_failure_scan_is_complete(), "Failed to finish scan");
}

ScanClosure::ScanClosure(DefNewGeneration* g, bool gc_barrier) :
    OopsInKlassOrGenClosure(g), _g(g), _gc_barrier(gc_barrier)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

void ScanClosure::do_oop(oop* p)       { ScanClosure::do_oop_work(p); }
void ScanClosure::do_oop(narrowOop* p) { ScanClosure::do_oop_work(p); }

FastScanClosure::FastScanClosure(DefNewGeneration* g, bool gc_barrier) :
    OopsInKlassOrGenClosure(g), _g(g), _gc_barrier(gc_barrier)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

// 跟扫描的时候的instanceOop实例的处理逻辑
// openjdk/hotspot/src/share/vm/memory/defNewGeneration.cpp
// void FastScanClosure::do_oop(oop* p) {
//  // hotspot/src/share/vm/memory/genOopClosures.inline.hpp
//  // template <class T> inline void FastScanClosure::do_oop_work(T* p) {...}
//  FastScanClosure::do_oop_work(p);
// }
void FastScanClosure::do_oop(oop* p)       { FastScanClosure::do_oop_work(p); }
void FastScanClosure::do_oop(narrowOop* p) { FastScanClosure::do_oop_work(p); }

void KlassScanClosure::do_klass(Klass* klass) {
#ifndef PRODUCT
  if (TraceScavenge) {
    ResourceMark rm;
    gclog_or_tty->print_cr("KlassScanClosure::do_klass %p, %s, dirty: %s",
                           klass,
                           klass->external_name(),
                           klass->has_modified_oops() ? "true" : "false");
  }
#endif

  // If the klass has not been dirtied we know that there's
  // no references into  the young gen and we can skip it.
  if (klass->has_modified_oops()) {
    if (_accumulate_modified_oops) {
      klass->accumulate_modified_oops();
    }

    // Clear this state since we're going to scavenge all the metadata.
    klass->clear_modified_oops();

    // Tell the closure which Klass is being scanned so that it can be dirtied
    // if oops are left pointing into the young gen.
    _scavenge_closure->set_scanned_klass(klass);

    klass->oops_do(_scavenge_closure);

    _scavenge_closure->set_scanned_klass(NULL);
  }
}

ScanWeakRefClosure::ScanWeakRefClosure(DefNewGeneration* g) :
  _g(g)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

void ScanWeakRefClosure::do_oop(oop* p)       { ScanWeakRefClosure::do_oop_work(p); }
void ScanWeakRefClosure::do_oop(narrowOop* p) { ScanWeakRefClosure::do_oop_work(p); }

void FilteringClosure::do_oop(oop* p)       { FilteringClosure::do_oop_work(p); }
void FilteringClosure::do_oop(narrowOop* p) { FilteringClosure::do_oop_work(p); }

KlassScanClosure::KlassScanClosure(OopsInKlassOrGenClosure* scavenge_closure,
                                   KlassRemSet* klass_rem_set)
    : _scavenge_closure(scavenge_closure),
      _accumulate_modified_oops(klass_rem_set->accumulate_modified_oops()) {}


DefNewGeneration::DefNewGeneration(ReservedSpace rs,
                                   size_t initial_size,
                                   int level,
                                   const char* policy)
  : Generation(rs, initial_size, level),
    _promo_failure_drain_in_progress(false),
    _should_allocate_from_space(false)
{
  MemRegion cmr((HeapWord*)_virtual_space.low(),
                (HeapWord*)_virtual_space.high());
  Universe::heap()->barrier_set()->resize_covered_region(cmr);

  if (GenCollectedHeap::heap()->collector_policy()->has_soft_ended_eden()) {
    _eden_space = new ConcEdenSpace(this);
  } else {
    _eden_space = new EdenSpace(this);
  }
  _from_space = new ContiguousSpace();
  _to_space   = new ContiguousSpace();

  if (_eden_space == NULL || _from_space == NULL || _to_space == NULL)
    vm_exit_during_initialization("Could not allocate a new gen space");

  // Compute the maximum eden and survivor space sizes. These sizes
  // are computed assuming the entire reserved space is committed.
  // These values are exported as performance counters.
  uintx alignment = GenCollectedHeap::heap()->collector_policy()->space_alignment();
  uintx size = _virtual_space.reserved_size();
  _max_survivor_size = compute_survivor_size(size, alignment);
  _max_eden_size = size - (2*_max_survivor_size);

  // allocate the performance counters

  // Generation counters -- generation 0, 3 subspaces
  _gen_counters = new GenerationCounters("new", 0, 3, &_virtual_space);
  _gc_counters = new CollectorCounters(policy, 0);

  _eden_counters = new CSpaceCounters("eden", 0, _max_eden_size, _eden_space,
                                      _gen_counters);
  _from_counters = new CSpaceCounters("s0", 1, _max_survivor_size, _from_space,
                                      _gen_counters);
  _to_counters = new CSpaceCounters("s1", 2, _max_survivor_size, _to_space,
                                    _gen_counters);

  compute_space_boundaries(0, SpaceDecorator::Clear, SpaceDecorator::Mangle);
  update_counters();
  // 指向下一个内存代，当前年轻代的下一个内存代为老年代
  _next_gen = NULL;
  // 控制新生代对象晋升到老年代中的最大阈值
  _tenuring_threshold = MaxTenuringThreshold;
  // 当新对象申请的内存空间大于这个参数值的时候，直接在老年代中分配内存
  _pretenure_size_threshold_words = PretenureSizeThreshold >> LogHeapWordSize;

  _gc_timer = new (ResourceObj::C_HEAP, mtGC) STWGCTimer();
}

// 计算Eden和两个Survivor空间的边界
void DefNewGeneration::compute_space_boundaries(uintx minimum_eden_size,
                                                bool clear_space,
                                                bool mangle_space) {
  uintx alignment =
    GenCollectedHeap::heap()->collector_policy()->space_alignment();

  // If the spaces are being cleared (only done at heap initialization
  // currently), the survivor spaces need not be empty.
  // Otherwise, no care is taken for used areas in the survivor spaces
  // so check.
  assert(clear_space || (to()->is_empty() && from()->is_empty()),
    "Initialization of the survivor spaces assumes these are empty");

  // Compute sizes
  // 计算Eden和两个个Survivor空间的值，注意在获取内存时调用的是committed_size()函数也就是实际上获取的是已经分配的物理内存
  uintx size = _virtual_space.committed_size();
  uintx survivor_size = compute_survivor_size(size, alignment);
  uintx eden_size = size - (2*survivor_size);
  assert(eden_size > 0 && survivor_size <= eden_size, "just checking");

  if (eden_size < minimum_eden_size) {
    // May happen due to 64Kb rounding, if so adjust eden size back up
    minimum_eden_size = align_size_up(minimum_eden_size, alignment);
    uintx maximum_survivor_size = (size - minimum_eden_size) / 2;
    uintx unaligned_survivor_size =
      align_size_down(maximum_survivor_size, alignment);
    survivor_size = MAX2(unaligned_survivor_size, alignment);
    eden_size = size - (2*survivor_size);
    assert(eden_size > 0 && survivor_size <= eden_size, "just checking");
    assert(eden_size >= minimum_eden_size, "just checking");
  }

  char *eden_start = _virtual_space.low();
  char *from_start = eden_start + eden_size;
  char *to_start   = from_start + survivor_size;
  char *to_end     = to_start   + survivor_size;

  assert(to_end == _virtual_space.high(), "just checking");
  assert(Space::is_aligned((HeapWord*)eden_start), "checking alignment");
  assert(Space::is_aligned((HeapWord*)from_start), "checking alignment");
  assert(Space::is_aligned((HeapWord*)to_start),   "checking alignment");

  MemRegion edenMR((HeapWord*)eden_start, (HeapWord*)from_start);
  MemRegion fromMR((HeapWord*)from_start, (HeapWord*)to_start);
  MemRegion toMR  ((HeapWord*)to_start, (HeapWord*)to_end);

  // A minimum eden size implies that there is a part of eden that
  // is being used and that affects the initialization of any
  // newly formed eden.
  bool live_in_eden = minimum_eden_size > 0;

  // If not clearing the spaces, do some checking to verify that
  // the space are already mangled.
  if (!clear_space) {
    // Must check mangling before the spaces are reshaped.  Otherwise,
    // the bottom or end of one space may have moved into another
    // a failure of the check may not correctly indicate which space
    // is not properly mangled.
    if (ZapUnusedHeapArea) {
      HeapWord* limit = (HeapWord*) _virtual_space.high();
      eden()->check_mangled_unused_area(limit);
      from()->check_mangled_unused_area(limit);
        to()->check_mangled_unused_area(limit);
    }
  }

  // Reset the spaces for their new regions.
  eden()->initialize(edenMR,
                     clear_space && !live_in_eden,
                     SpaceDecorator::Mangle);
  // If clear_space and live_in_eden, we will not have cleared any
  // portion of eden above its top. This can cause newly
  // expanded space not to be mangled if using ZapUnusedHeapArea.
  // We explicitly do such mangling here.
  if (ZapUnusedHeapArea && clear_space && live_in_eden && mangle_space) {
    eden()->mangle_unused_area();
  }
  from()->initialize(fromMR, clear_space, mangle_space);
  to()->initialize(toMR, clear_space, mangle_space);

  // Set next compaction spaces.
  eden()->set_next_compaction_space(from());
  // The to-space is normally empty before a compaction so need
  // not be considered.  The exception is during promotion
  // failure handling when to-space can contain live objects.
  from()->set_next_compaction_space(NULL);
}

void DefNewGeneration::swap_spaces() {
  // 简单交换From Survivor和To Survivor空间的首地址即可
  ContiguousSpace* s = from();
  _from_space        = to();
  _to_space          = s;
  // Eden空间的下一个压缩空间为From Survivor，FGC在压缩年轻代时通常会压缩这两个空间。
  // 如果YGC晋升失败，则From Survivor的下一个压缩空间是To Survivor，因此FGC会压缩整理这三个空间
  eden()->set_next_compaction_space(from());
  // The to-space is normally empty before a compaction so need
  // not be considered.  The exception is during promotion
  // failure handling when to-space can contain live objects.
  from()->set_next_compaction_space(NULL);

  if (UsePerfData) {
    CSpaceCounters* c = _from_counters;
    _from_counters = _to_counters;
    _to_counters = c;
  }
}

bool DefNewGeneration::expand(size_t bytes) {
  MutexLocker x(ExpandHeap_lock);
  HeapWord* prev_high = (HeapWord*) _virtual_space.high();
  bool success = _virtual_space.expand_by(bytes);
  if (success && ZapUnusedHeapArea) {
    // Mangle newly committed space immediately because it
    // can be done here more simply that after the new
    // spaces have been computed.
    HeapWord* new_high = (HeapWord*) _virtual_space.high();
    MemRegion mangle_region(prev_high, new_high);
    SpaceMangler::mangle_region(mangle_region);
  }

  // Do not attempt an expand-to-the reserve size.  The
  // request should properly observe the maximum size of
  // the generation so an expand-to-reserve should be
  // unnecessary.  Also a second call to expand-to-reserve
  // value potentially can cause an undue expansion.
  // For example if the first expand fail for unknown reasons,
  // but the second succeeds and expands the heap to its maximum
  // value.
  if (GC_locker::is_active()) {
    if (PrintGC && Verbose) {
      gclog_or_tty->print_cr("Garbage collection disabled, "
        "expanded heap instead");
    }
  }

  return success;
}


void DefNewGeneration::compute_new_size() {
  // This is called after a gc that includes the following generation
  // (which is required to exist.)  So from-space will normally be empty.
  // Note that we check both spaces, since if scavenge failed they revert roles.
  // If not we bail out (otherwise we would have to relocate the objects)
  if (!from()->is_empty() || !to()->is_empty()) {
    return;
  }

  int next_level = level() + 1;
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  assert(next_level < gch->_n_gens,
         "DefNewGeneration cannot be an oldest gen");

  Generation* next_gen = gch->_gens[next_level];
  size_t old_size = next_gen->capacity();
  size_t new_size_before = _virtual_space.committed_size();
  size_t min_new_size = spec()->init_size();
  size_t max_new_size = reserved().byte_size();
  assert(min_new_size <= new_size_before &&
         new_size_before <= max_new_size,
         "just checking");
  // All space sizes must be multiples of Generation::GenGrain.
  size_t alignment = Generation::GenGrain;

  // Compute desired new generation size based on NewRatio and
  // NewSizeThreadIncrease
  size_t desired_new_size = old_size/NewRatio;
  int threads_count = Threads::number_of_non_daemon_threads();
  size_t thread_increase_size = threads_count * NewSizeThreadIncrease;
  desired_new_size = align_size_up(desired_new_size + thread_increase_size, alignment);

  // Adjust new generation size
  desired_new_size = MAX2(MIN2(desired_new_size, max_new_size), min_new_size);
  assert(desired_new_size <= max_new_size, "just checking");

  bool changed = false;
  if (desired_new_size > new_size_before) {
    size_t change = desired_new_size - new_size_before;
    assert(change % alignment == 0, "just checking");
    if (expand(change)) {
       changed = true;
    }
    // If the heap failed to expand to the desired size,
    // "changed" will be false.  If the expansion failed
    // (and at this point it was expected to succeed),
    // ignore the failure (leaving "changed" as false).
  }
  if (desired_new_size < new_size_before && eden()->is_empty()) {
    // bail out of shrinking if objects in eden
    size_t change = new_size_before - desired_new_size;
    assert(change % alignment == 0, "just checking");
    _virtual_space.shrink_by(change);
    changed = true;
  }
  if (changed) {
    // The spaces have already been mangled at this point but
    // may not have been cleared (set top = bottom) and should be.
    // Mangling was done when the heap was being expanded.
    compute_space_boundaries(eden()->used(),
                             SpaceDecorator::Clear,
                             SpaceDecorator::DontMangle);
    MemRegion cmr((HeapWord*)_virtual_space.low(),
                  (HeapWord*)_virtual_space.high());
    Universe::heap()->barrier_set()->resize_covered_region(cmr);
    if (Verbose && PrintGC) {
      size_t new_size_after  = _virtual_space.committed_size();
      size_t eden_size_after = eden()->capacity();
      size_t survivor_size_after = from()->capacity();
      gclog_or_tty->print("New generation size " SIZE_FORMAT "K->"
        SIZE_FORMAT "K [eden="
        SIZE_FORMAT "K,survivor=" SIZE_FORMAT "K]",
        new_size_before/K, new_size_after/K,
        eden_size_after/K, survivor_size_after/K);
      if (WizardMode) {
        gclog_or_tty->print("[allowed " SIZE_FORMAT "K extra for %d threads]",
          thread_increase_size/K, threads_count);
      }
      gclog_or_tty->cr();
    }
  }
}

void DefNewGeneration::younger_refs_iterate(OopsInGenClosure* cl) {
  assert(false, "NYI -- are you sure you want to call this?");
}


size_t DefNewGeneration::capacity() const {
  return eden()->capacity()
       + from()->capacity();  // to() is only used during scavenge
}


size_t DefNewGeneration::used() const {
  // 将Eden区和From Survivor区已使用的内存空间加起来即为年轻代总的使用空间。
  return eden()->used()
       + from()->used();      // to() is only used during scavenge
}


size_t DefNewGeneration::free() const {
  return eden()->free()
       + from()->free();      // to() is only used during scavenge
}

size_t DefNewGeneration::max_capacity() const {
  const size_t alignment = GenCollectedHeap::heap()->collector_policy()->space_alignment();
  const size_t reserved_bytes = reserved().byte_size();
  return reserved_bytes - compute_survivor_size(reserved_bytes, alignment);
}

size_t DefNewGeneration::unsafe_max_alloc_nogc() const {
  return eden()->free();
}

size_t DefNewGeneration::capacity_before_gc() const {
  return eden()->capacity();
}

size_t DefNewGeneration::contiguous_available() const {
  return eden()->free();
}


HeapWord** DefNewGeneration::top_addr() const { return eden()->top_addr(); }
HeapWord** DefNewGeneration::end_addr() const { return eden()->end_addr(); }

void DefNewGeneration::object_iterate(ObjectClosure* blk) {
  eden()->object_iterate(blk);
  from()->object_iterate(blk);
}


void DefNewGeneration::space_iterate(SpaceClosure* blk,
                                     bool usedOnly) {
  blk->do_space(eden());
  blk->do_space(from());
  blk->do_space(to());
}

// The last collection bailed out, we are running out of heap space,
// so we try to allocate the from-space, too.
HeapWord* DefNewGeneration::allocate_from_space(size_t size) {
  HeapWord* result = NULL;
  if (Verbose && PrintGCDetails) {
    gclog_or_tty->print("DefNewGeneration::allocate_from_space(%u):"
                        "  will_fail: %s"
                        "  heap_lock: %s"
                        "  free: " SIZE_FORMAT,
                        size,
                        GenCollectedHeap::heap()->incremental_collection_will_fail(false /* don't consult_young */) ?
                          "true" : "false",
                        Heap_lock->is_locked() ? "locked" : "unlocked",
                        from()->free());
  }
  // 支持在From Survivor空间中分配内存，或当前正在进行GC操作时可能会从From Survivor空间中分配内存
  if (should_allocate_from_space() || GC_locker::is_active_and_needs_gc()) {
    if (Heap_lock->owned_by_self() ||                       // 当前线程拥有堆的全局锁
        // 执行GC的VMThread线程借助From Survivor空间完成一些安全点下的操作
        (SafepointSynchronize::is_at_safepoint() &&
         Thread::current()->is_VM_thread())) {
      // If the Heap_lock is not locked by this thread, this will be called
      // again later with the Heap_lock held.
      result = from()->allocate(size);
    } else if (PrintGC && Verbose) {
      gclog_or_tty->print_cr("  Heap_lock is not owned by self");
    }
  } else if (PrintGC && Verbose) {
    gclog_or_tty->print_cr("  should_allocate_from_space: NOT");
  }
  if (PrintGC && Verbose) {
    gclog_or_tty->print_cr("  returns %s", result == NULL ? "NULL" : "object");
  }
  return result;
}

HeapWord* DefNewGeneration::expand_and_allocate(size_t size,
                                                bool   is_tlab,
                                                bool   parallel) {
  // We don't attempt to expand the young generation (but perhaps we should.)
  return allocate(size, is_tlab);
}

// -XX:TargetSurvivorRatio选项表示To Survivor空间占用百分比。调用adjust_desired_tenuring_threshold()函数是在YGC执行成功后，
// 所以此次年轻代垃圾回收后所有的存活对象都被移动到了To Survivor空间内。如果To Survivor空间内的活跃对象的占比较高，
// 会使下一次YGC时To Survivor空间轻易地被活跃对象占满，导致各种年龄代的对象晋升到老年代。
// 为了解决这个问题，每次成功执行YGC后需要动态调整年龄阈值，这个年龄阈值既可以保证To Survivor空间占比不过高，
// 也能保证晋升到老年代的对象都是达到了这个年龄阈值的对象。

// 如果在Survivor区中存活的对象比较多，那么晋升阈值可能会变小，当下一次回收时，大于晋升阈值的对象都会晋升到老年代。
void DefNewGeneration::adjust_desired_tenuring_threshold() {
  // Set the desired survivor size to half the real survivor space
  _tenuring_threshold =
    age_table()->compute_tenuring_threshold(to()->capacity()/HeapWordSize);
}

void DefNewGeneration::collect(bool   full,
                               bool   clear_all_soft_refs,
                               size_t size,
                               bool   is_tlab) {
  // 确保当前是一次FGC，或者需要分配的内存size大于0，否则不需要执行一次GC操作
  assert(full || size > 0, "otherwise we don't want to collect");

  GenCollectedHeap* gch = GenCollectedHeap::heap();

  _gc_timer->register_gc_start();
  DefNewTracer gc_tracer;
  gc_tracer.report_gc_start(gch->gc_cause(), _gc_timer->gc_start());

  // 使用-XX:+UseSerialGC命令后， DefNewGeneration::_next_gen为TenuredGeneration
  _next_gen = gch->next_gen(this);

  // If the next generation is too full to accommodate promotion
  // from this generation, pass on collection; let the next generation
  // do it.
  // 返回true: to()空间空闲 && 某个代的剩余空间能够容下本次垃圾回收需要的空间
  // 如果新生代全是需要晋升的存活对象，老年代可能容不下这些对象，此时设置增量垃圾回收失败，直接返回，后续会执行FGC。
  if (!collection_attempt_is_safe()) {
    if (Verbose && PrintGCDetails) {
      gclog_or_tty->print(" :: Collection attempt not safe :: ");
    }
    // 告诉内存堆管理器不要再考虑增量式GC(Minor Gc),因为一定会失败。
    // 设置_incremental_collection_failed为true，即放弃当前YGC
    gch->set_incremental_collection_failed(); // Slight lie: we did not even attempt one
    return;
  }
  // 在执行YGC时使用的是复制算法，因此要保存To Survivor区为空
  assert(to()->is_empty(), "Else not collection_attempt_is_safe");
  // 主要设置DefNewGeneration::_promotion_failed变量的值为false
  init_assuming_no_promotion_failure();

  GCTraceTime t1(GCCauseString("GC", gch->gc_cause()), PrintGC && !PrintGCDetails, true, NULL);
  // Capture heap used before collection (for printing).
  size_t gch_prev_used = gch->used();

  gch->trace_heap_before_gc(&gc_tracer);

  SpecializationStats::clear();

  // These can be shared for all code paths
  IsAliveClosure is_alive(this);            // 该闭包封装了判断对象是否存活的逻辑
  ScanWeakRefClosure scan_weak_ref(this);   // 该闭包封装了扫描弱引用的逻辑

  // 清空ageTable数据和To Survivor空间，ageTable会辅助判断对象晋升的条件，而保证To Survivor空间为空是执行复制算法的必备条件
  age_table()->clear();
  to()->clear(SpaceDecorator::Mangle);

  gch->rem_set()->prepare_for_younger_refs_iterate(false);

  assert(gch->no_allocs_since_save_marks(0),
         "save marks have not been newly set.");

  // Not very pretty.
  CollectorPolicy* cp = gch->collector_policy();

  FastScanClosure fsc_with_no_gc_barrier(this, false);  // 此闭包封装了存活对象的标识和复制逻辑
  FastScanClosure fsc_with_gc_barrier(this, true);

  KlassScanClosure klass_scan_closure(&fsc_with_no_gc_barrier,
                                      gch->rem_set()->klass_rem_set());

  set_promo_failure_scan_stack_closure(&fsc_with_no_gc_barrier);
  FastEvacuateFollowersClosure evacuate_followers(gch, _level, this,
                                                  &fsc_with_no_gc_barrier,
                                                  &fsc_with_gc_barrier);

  assert(gch->no_allocs_since_save_marks(0),
         "save marks have not been newly set.");

  int so = SharedHeap::SO_AllClasses | SharedHeap::SO_Strings | SharedHeap::SO_CodeCache;

  /*
   * 1.调用GenCollectedHeap类的gen_process_strong_roots()函数找出所有的根引用标记并复制，
   * 2.接着调用DefNewGeneration::FastEvacuateFollowersClosure::do_void()函数从已经标记的直接由根引用的对象出发，
   *   使用广度遍历标记所有对象并复制，这样就完成了YGC任务。
  */
  // 标记根集对象并复制到To Survivor空间中
  gch->gen_process_strong_roots(_level,                                     // 执行YGC和FGC时，此值都为1
                                true,  // Process younger gens, if any,
                                       // as strong roots.
                                true,  // activate StrongRootsScope
                                true,  // is scavenging                     // 执行对象复制操作
                                SharedHeap::ScanningOption(so),             //
                                &fsc_with_no_gc_barrier,                    // 类型为FastScanClosure
                                true,   // walk *all* scavengable nmethods
                                &fsc_with_gc_barrier,                       // 类型为FastScanClosure
                                &klass_scan_closure);                       // 类型为KlassScanClosure

  // "evacuate followers". 递归处理根集对象的引用对象，然后复制活跃对象到新的存储空间
  evacuate_followers.do_void();

  FastKeepAliveClosure keep_alive(this, &scan_weak_ref); // 处理发现的引用类型对象
  ReferenceProcessor* rp = ref_processor();
  rp->setup_policy(clear_all_soft_refs);
  // 调用此函数时，年轻代的所有对象都完成了标记阶段，这时候能够准确判断出对象是否可达，不同的引用类型的判断逻辑不一样。
  const ReferenceProcessorStats& stats =
  rp->process_discovered_references(&is_alive, &keep_alive, &evacuate_followers,
                                    NULL, _gc_timer);
  gc_tracer.report_gc_reference_stats(stats);

  // 在YGC回收的过程中，最终执行成功与否通过_promotion_failed变量的值来判断。
  // 如果成功，则清空Eden和From Survivor区，然后交换From Survivor和To Survivor的角色即可；
  // 如果失败，则需要触发FGC，如果不触发FGC将Eden、 From Survivor和To Survivor的活跃对象压缩在Eden和From Survivor空间，
  // 那么后续就不能发生YGC，因为复制算法找不到一个可用的空闲空间。

  // 当晋升成功
  if (!_promotion_failed) {
    // Swap the survivor spaces.
    // 清空Eden和From Survivor空间，因为这两个空间中剩余的没有被移动的对象都是死亡对象
    eden()->clear(SpaceDecorator::Mangle);
    from()->clear(SpaceDecorator::Mangle);
    if (ZapUnusedHeapArea) {
      // This is now done here because of the piece-meal mangling which
      // can check for valid mangling at intermediate points in the
      // collection(s).  When a minor collection fails to collect
      // sufficient space resizing of the young generation can occur
      // an redistribute the spaces in the young generation.  Mangle
      // here so that unzapped regions don't get distributed to
      // other spaces.
      to()->mangle_unused_area();
    }
    // 交换From Survivor和To Survivor的角色，这样已经清空的From Survivor空间会变为下一次回收的To Survivor空间
    swap_spaces();

    assert(to()->is_empty(), "to space should be empty now");

    // 动态计算晋升阈值
    adjust_desired_tenuring_threshold();

    // A successful scavenge should restart the GC time limit count which is
    // for full GC's.
    // 当YGC成功后，重新计算GC超时的时间计数
    AdaptiveSizePolicy* size_policy = gch->gen_policy()->size_policy();
    size_policy->reset_gc_overhead_limit_count();
    if (PrintGC && !PrintGCDetails) {
      gch->print_heap_change(gch_prev_used);
    }
    assert(!gch->incremental_collection_failed(), "Should be clear");
  } else {
    // 若发生了晋升失败，即老年代没有足够的内存空间用以存放新生代所晋升的所有对象
    assert(_promo_failure_scan_stack.is_empty(), "post condition");
    _promo_failure_scan_stack.clear(true); // Clear cached segments.

    // 恢复晋升失败对象的markOop，因为晋升失败的对象的转发地址已经指向自己
    remove_forwarding_pointers();
    if (PrintGCDetails) {
      gclog_or_tty->print(" (promotion failed) ");
    }
    // Add to-space to the list of space to compact
    // when a promotion failure has occurred.  In that
    // case there can be live objects in to-space
    // as a result of a partial evacuation of eden
    // and from-space.
    // 当晋升失败时，虽然会交换From Survivor和To Survivor的角色，但是并不会清空Eden和From Survivor空间，
    // 而会恢复晋升失败部分的对象头（加上To Survivor空间中的对象就是全部活跃对象了），这样在随后触发的FGC中能够对From Survivor
    // 和To Survivor空间进行压缩处理
    swap_spaces();   // For uniformity wrt ParNewGeneration.
    // 设置From Survivor的下一个压缩空间为To Survivor，由于晋升失败会触发FGC。
    // 所以FGC会将Eden、From Survivor和To Survivor空间的活跃对象压缩在Eden和From Survivor空间
    from()->set_next_compaction_space(to());
    // 设置堆的YGC失败标记，并通知老年代晋升失败，DefNewGeneration::collect()函数当collection_attempt_is_safe()函数返回true时
    // 也有可能晋升失败，就是因为此函数的判断条件中含有可用空间是否大于等于平均的晋升空间在实际执行YGC时，
    // 晋升量大于平均晋升量并且可用空间小于这个晋升空间时，会导致YGC失败。
    gch->set_incremental_collection_failed();

    // Inform the next generation that a promotion failure occurred.
    _next_gen->promotion_failure_occurred();
    gc_tracer.report_promotion_failed(_promotion_failed_info);

    // Reset the PromotionFailureALot counters.
    NOT_PRODUCT(Universe::heap()->reset_promotion_should_fail();)
  }
  // set new iteration safe limit for the survivor spaces
  from()->set_concurrent_iteration_safe_limit(from()->top());
  to()->set_concurrent_iteration_safe_limit(to()->top());
  SpecializationStats::print();

  // We need to use a monotonically non-decreasing time in ms
  // or we will see time-warp warnings and os::javaTimeMillis()
  // does not guarantee monotonicity.
  jlong now = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;
  update_time_of_last_gc(now);

  gch->trace_heap_after_gc(&gc_tracer);
  gc_tracer.report_tenuring_threshold(tenuring_threshold());

  _gc_timer->register_gc_end();

  gc_tracer.report_gc_end(_gc_timer->gc_end(), _gc_timer->time_partitions());
}

class RemoveForwardPointerClosure: public ObjectClosure {
public:
  void do_object(oop obj) {
    obj->init_mark();
  }
};

void DefNewGeneration::init_assuming_no_promotion_failure() {
  _promotion_failed = false; // 如果在YGC过程中失败，那么这个变量的值会设置为true
  _promotion_failed_info.reset();
  // 将CompactibleSpace::_next_compaction_space属性的值设置为NULL
  from()->set_next_compaction_space(NULL);
}

void DefNewGeneration::remove_forwarding_pointers() {
  RemoveForwardPointerClosure rspc;
  eden()->object_iterate(&rspc);
  from()->object_iterate(&rspc);

  // Now restore saved marks, if any.
  assert(_objs_with_preserved_marks.size() == _preserved_marks_of_objs.size(),
         "should be the same");
  while (!_objs_with_preserved_marks.is_empty()) {
    oop obj   = _objs_with_preserved_marks.pop();
    markOop m = _preserved_marks_of_objs.pop();
    obj->set_mark(m);
  }
  _objs_with_preserved_marks.clear(true);
  _preserved_marks_of_objs.clear(true);
}

void DefNewGeneration::preserve_mark(oop obj, markOop m) {
  assert(_promotion_failed && m->must_be_preserved_for_promotion_failure(obj),
         "Oversaving!");
  _objs_with_preserved_marks.push(obj);
  _preserved_marks_of_objs.push(m);
}

void DefNewGeneration::preserve_mark_if_necessary(oop obj, markOop m) {
  if (m->must_be_preserved_for_promotion_failure(obj)) {
    preserve_mark(obj, m);
  }
}

void DefNewGeneration::handle_promotion_failure(oop old) {
  if (PrintPromotionFailure && !_promotion_failed) {
    gclog_or_tty->print(" (promotion failure size = " SIZE_FORMAT ") ",
                        old->size());
  }
  _promotion_failed = true;
  _promotion_failed_info.register_copy_failure(old->size());
  // 保存原对象的对象头信息，然后在对象头中设置转发指针指向自己
  preserve_mark_if_necessary(old, old->mark());
  // forward to self
  old->forward_to(old);

  // 将晋升失败的对象存储到_promo_failure_scan_stack栈中。
  // 这样会继续调用drain_promo_failure_scan_stack()函数处理晋升失败对象引用的其他对象。
  _promo_failure_scan_stack.push(old);

  if (!_promo_failure_drain_in_progress) {
    // prevent recursion in copy_to_survivor_space()
    _promo_failure_drain_in_progress = true;
    // 当前的对象晋升失败时，当前对象所引用的对象仍然要进行标记扫描并进行复制操作
    drain_promo_failure_scan_stack();
    _promo_failure_drain_in_progress = false;
  }

  // 对于YGC来说，如果发生晋升失败的情况，这些对象肯定在Eden空间或From Survivor空间，并且这些对象的转发指针指向自己。
}

oop DefNewGeneration::copy_to_survivor_space(oop old) {
  assert(is_in_reserved(old) && !old->is_forwarded(),
         "shouldn't be scavenging this oop");
  size_t s = old->size();
  oop obj = NULL;

  // Try allocating obj in to-space (unless too old)
  // 当对象的年龄没有达到晋升阈值时，尝试将此对象移动到To Survivor空间这里先分配内存
  if (old->age() < tenuring_threshold()) {
    obj = (oop) to()->allocate(s);
  }

  // Otherwise try allocating obj tenured
  // 当obj为NULL时，表示在To Survivor空间分配内存不成功，或者可能是对象达到了晋升阈值，而没有在To Survivor空间分配内存，此时需要晋升对象到老年代
  if (obj == NULL) {
    obj = _next_gen->promote(old, s);
    if (obj == NULL) {
      // 对象晋升到老年代时失败，设置_promotion_failed标记为true，当此值为true时会触发FGC
      handle_promotion_failure(old);
      return old;
    }
  } else {
    // Prefetch beyond obj
    const intx interval = PrefetchCopyIntervalInBytes;
    Prefetch::write(obj, interval);

    // Copy obj
    // 将原对象的数据内容复制到To Survivor空间
    Copy::aligned_disjoint_words((HeapWord*)old, (HeapWord*)obj, s);

    // Increment age if obj still in new generation
    // 增加新对象的age并更新ageTable中sizes变量的值
    obj->incr_age();
    age_table()->add(obj, s);
  }

  // Done, insert forward pointer to obj in this header
  // 调用forward_to()设置原对象的对象头为转发指针，表示该对象已被复制，转发指针指向新对象位置。
  old->forward_to(obj);

  return obj;
}

void DefNewGeneration::drain_promo_failure_scan_stack() {
  while (!_promo_failure_scan_stack.is_empty()) {
     oop obj = _promo_failure_scan_stack.pop();
     obj->oop_iterate(_promo_failure_scan_stack_closure);
  }
}

void DefNewGeneration::save_marks() {
  eden()->set_saved_mark();
  to()->set_saved_mark();
  from()->set_saved_mark();
}


void DefNewGeneration::reset_saved_marks() {
  eden()->reset_saved_mark();
  to()->reset_saved_mark();
  from()->reset_saved_mark();
}


bool DefNewGeneration::no_allocs_since_save_marks() {
  assert(eden()->saved_mark_at_top(), "Violated spec - alloc in eden");
  assert(from()->saved_mark_at_top(), "Violated spec - alloc in from");
  return to()->saved_mark_at_top();
}

#define DefNew_SINCE_SAVE_MARKS_DEFN(OopClosureType, nv_suffix) \
                                                                \
void DefNewGeneration::                                         \
oop_since_save_marks_iterate##nv_suffix(OopClosureType* cl) {   \
  cl->set_generation(this);                                     \
  eden()->oop_since_save_marks_iterate##nv_suffix(cl);          \
  to()->oop_since_save_marks_iterate##nv_suffix(cl);            \
  from()->oop_since_save_marks_iterate##nv_suffix(cl);          \
  cl->reset_generation();                                       \
  save_marks();                                                 \
}

ALL_SINCE_SAVE_MARKS_CLOSURES(DefNew_SINCE_SAVE_MARKS_DEFN)

#undef DefNew_SINCE_SAVE_MARKS_DEFN

void DefNewGeneration::contribute_scratch(ScratchBlock*& list, Generation* requestor,
                                         size_t max_alloc_words) {
  if (requestor == this || _promotion_failed) return;
  assert(requestor->level() > level(), "DefNewGeneration must be youngest");

  /* $$$ Assert this?  "trace" is a "MarkSweep" function so that's not appropriate.
  if (to_space->top() > to_space->bottom()) {
    trace("to_space not empty when contribute_scratch called");
  }
  */

  ContiguousSpace* to_space = to();
  assert(to_space->end() >= to_space->top(), "pointers out of order");
  size_t free_words = pointer_delta(to_space->end(), to_space->top());
  if (free_words >= MinFreeScratchWords) {
    ScratchBlock* sb = (ScratchBlock*)to_space->top();
    sb->num_words = free_words;
    sb->next = list;
    list = sb;
  }
}

void DefNewGeneration::reset_scratch() {
  // If contributing scratch in to_space, mangle all of
  // to_space if ZapUnusedHeapArea.  This is needed because
  // top is not maintained while using to-space as scratch.
  if (ZapUnusedHeapArea) {
    to()->mangle_unused_area_complete();
  }
}

// DefNewGeneration正式进行Gc前会先检测一下本次Minor Gc是否安全, 如果不安全则直接放弃本次Gc,检查策略是:
// 1. To区空闲
// 2. 下一个内存代的可用空间能够容纳当前内存代的所有对象(用于对象升级)
bool DefNewGeneration::collection_attempt_is_safe() {
  // To Survivor空间如果不为空，则无法采用复制算法，也就无法执行YGC
  if (!to()->is_empty()) { // To Survivor 区空闲
    if (Verbose && PrintGCDetails) {
      gclog_or_tty->print(" :: to is not empty :: ");
    }
    // 如果to区非空则返回false，正常都是空的
    return false;
  }
  // 设置年轻代的下一个代为老年代
  if (_next_gen == NULL) {
    // 初始化_next_gen，DefNewGeneration第一次准备垃圾回收前_next_gen一直为nul
    GenCollectedHeap* gch = GenCollectedHeap::heap();
    _next_gen = gch->next_gen(this);
  }
  // 判断next_gen是否有充足的空间，允许年轻代的对象复制到老年代中
  // 调用used()函数获取当前年轻代已经使用的所有内存
  return _next_gen->promotion_attempt_is_safe(used());
}

// gc_epilogue是在GC结束后调用的尾处理
void DefNewGeneration::gc_epilogue(bool full) {
  DEBUG_ONLY(static bool seen_incremental_collection_failed = false;)

  assert(!GC_locker::is_active(), "We should not be executing here");
  // Check if the heap is approaching full after a collection has
  // been done.  Generally the young generation is empty at
  // a minimum at the end of a collection.  If it is not, then
  // the heap is approaching full.
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  if (full) {
    DEBUG_ONLY(seen_incremental_collection_failed = false;)
    // 正常情况下GC结束后eden区是空的，如果非空说明堆内存满了，eden区中的存活对象未拷贝至老年代中
    if (!collection_attempt_is_safe() && !_eden_space->is_empty()) {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), full, not safe, set_failed, set_alloc_from, clear_seen",
                            GCCause::to_string(gch->gc_cause()));
      }
      // 通知GCH promote事变
      gch->set_incremental_collection_failed(); // Slight lie: a full gc left us in that state
      // 允许使用from区分配对象
      set_should_allocate_from_space(); // we seem to be running out of space
    } else {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), full, safe, clear_failed, clear_alloc_from, clear_seen",
                            GCCause::to_string(gch->gc_cause()));
      }
      gch->clear_incremental_collection_failed(); // We just did a full collection
      clear_should_allocate_from_space(); // if set
    }
  } else {
#ifdef ASSERT
    // It is possible that incremental_collection_failed() == true
    // here, because an attempted scavenge did not succeed. The policy
    // is normally expected to cause a full collection which should
    // clear that condition, so we should not be here twice in a row
    // with incremental_collection_failed() == true without having done
    // a full collection in between.
    if (!seen_incremental_collection_failed &&
        gch->incremental_collection_failed()) {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), not full, not_seen_failed, failed, set_seen_failed",
                            GCCause::to_string(gch->gc_cause()));
      }
      seen_incremental_collection_failed = true;
    } else if (seen_incremental_collection_failed) {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), not full, seen_failed, will_clear_seen_failed",
                            GCCause::to_string(gch->gc_cause()));
      }
      assert(gch->gc_cause() == GCCause::_scavenge_alot ||
             (gch->gc_cause() == GCCause::_java_lang_system_gc && UseConcMarkSweepGC && ExplicitGCInvokesConcurrent) ||
             !gch->incremental_collection_failed(),
             "Twice in a row");
      seen_incremental_collection_failed = false;
    }
#endif // ASSERT
  }

  if (ZapUnusedHeapArea) {
    // check_mangled_unused_area_complete 对起止地址之间的整块内存区域都执行了mangle，
    // 因为需要遍历一遍整块内存区域，所以比较费时，只在调试mangle(DEBUG_MANGLING标志)下开启，否则该方法是空实现。
    eden()->check_mangled_unused_area_complete();
    from()->check_mangled_unused_area_complete();
    to()->check_mangled_unused_area_complete();
  }

  if (!CleanChunkPoolAsync) {
    // 清空ChunkPool
    Chunk::clean_chunk_pool();
  }

  // update the generation and space performance counters
  update_counters();
  gch->collector_policy()->counters()->update_counters();
}

void DefNewGeneration::record_spaces_top() {
  assert(ZapUnusedHeapArea, "Not mangling unused space");
  eden()->set_top_for_allocations();
  to()->set_top_for_allocations();
  from()->set_top_for_allocations();
}

void DefNewGeneration::ref_processor_init() {
  Generation::ref_processor_init();
}


void DefNewGeneration::update_counters() {
  if (UsePerfData) {
    _eden_counters->update_all();
    _from_counters->update_all();
    _to_counters->update_all();
    _gen_counters->update_all();
  }
}

void DefNewGeneration::verify() {
  eden()->verify();
  from()->verify();
    to()->verify();
}

void DefNewGeneration::print_on(outputStream* st) const {
  Generation::print_on(st);
  st->print("  eden");
  eden()->print_on(st);
  st->print("  from");
  from()->print_on(st);
  st->print("  to  ");
  to()->print_on(st);
}


const char* DefNewGeneration::name() const {
  return "def new generation";
}

// Moved from inline file as they are not called inline
CompactibleSpace* DefNewGeneration::first_compaction_space() const {
  return eden();
}

HeapWord* DefNewGeneration::allocate(size_t word_size,
                                     bool is_tlab) {
  // This is the slow-path allocation for the DefNewGeneration.
  // Most allocations are fast-path in compiled code.
  // We try to allocate from the eden.  If that works, we are happy.
  // Note that since DefNewGeneration supports lock-free allocation, we
  // have to use it here, as well.
  // 1.以并行的方式快速从Eden空间分配内存
  HeapWord* result = eden()->par_allocate(word_size);   // 快速分配
  if (result != NULL) {
    if (CMSEdenChunksRecordAlways && _next_gen != NULL) {
      _next_gen->sample_eden_chunk();
    }
    return result;
  }
  // 2.扩展Eden空间内存空间的方式分配内存
  do {
    HeapWord* old_limit = eden()->soft_end();
    if (old_limit < eden()->end()) {
      // Tell the next generation we reached a limit.
      // 通知下一个内存代管理器，Eden区的使用达到了逻辑(软)限制
      // 由下一个内存代管理器来决定Eden区新的(软)限制位置
      HeapWord* new_limit =
        next_gen()->allocation_limit_reached(eden(), eden()->top(), word_size);
      if (new_limit != NULL) {
        Atomic::cmpxchg_ptr(new_limit, eden()->soft_end_addr(), old_limit);
      } else {
        assert(eden()->soft_end() == eden()->end(),
               "invalid state after allocation_limit_reached returned null");
      }
    } else {
      // The allocation failed and the soft limit is equal to the hard limit,
      // there are no reasons to do an attempt to allocate
      assert(old_limit == eden()->end(), "sanity check");
      break;
    }
    // Try to allocate until succeeded or the soft limit can't be adjusted
    // 重试，直到内存分配成功或者软引用限制不能再调整
    result = eden()->par_allocate(word_size);
  } while (result == NULL);

  // If the eden is full and the last collection bailed out, we are running
  // out of heap space, and we try to allocate the from-space, too.
  // allocate_from_space can't be inlined because that would introduce a
  // circular dependency at compile time.
  // 如果Eden已满并且最后一个集合已获救助，则堆空间不足，我们也尝试分配from区的空间。allocate_from_space不能内联，因为这会在编译时引入循环依赖项。
  // 从From Survivor空间分配内存
  if (result == NULL) { // Eden区没有足够的空间,则从From区分配
    result = allocate_from_space(word_size);
  } else if (CMSEdenChunksRecordAlways && _next_gen != NULL) {
    _next_gen->sample_eden_chunk();
  }
  return result;
}

HeapWord* DefNewGeneration::par_allocate(size_t word_size,
                                         bool is_tlab) {
  HeapWord* res = eden()->par_allocate(word_size);
  if (CMSEdenChunksRecordAlways && _next_gen != NULL) {
    _next_gen->sample_eden_chunk();
  }
  return res;
}

// gc_prologue方法是在GC开始前调用的预处理
void DefNewGeneration::gc_prologue(bool full) {
  // Ensure that _end and _soft_end are the same in eden space.
  eden()->set_soft_end(eden()->end());
}

size_t DefNewGeneration::tlab_capacity() const {
  return eden()->capacity();
}

size_t DefNewGeneration::unsafe_max_tlab_alloc() const {
  return unsafe_max_alloc_nogc();
}
