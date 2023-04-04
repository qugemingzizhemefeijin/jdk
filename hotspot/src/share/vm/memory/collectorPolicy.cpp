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
#include "gc_implementation/shared/adaptiveSizePolicy.hpp"
#include "gc_implementation/shared/gcPolicyCounters.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "memory/cardTableRS.hpp"
#include "memory/collectorPolicy.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/genCollectedHeap.hpp"
#include "memory/generationSpec.hpp"
#include "memory/space.hpp"
#include "memory/universe.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_ALL_GCS
#include "gc_implementation/concurrentMarkSweep/cmsAdaptiveSizePolicy.hpp"
#include "gc_implementation/concurrentMarkSweep/cmsGCAdaptivePolicyCounters.hpp"
#endif // INCLUDE_ALL_GCS

// CollectorPolicy methods.

CollectorPolicy::CollectorPolicy() :
    _space_alignment(0),
    _heap_alignment(0),
    _initial_heap_byte_size(InitialHeapSize),           // InitialHeapSize表示初始堆内存，单位字节
    _max_heap_byte_size(MaxHeapSize),                   // MaxHeapSize表示最大堆内存，单位字节
    _min_heap_byte_size(Arguments::min_heap_size()),
    _max_heap_size_cmdline(false),
    _size_policy(NULL),
    _should_clear_all_soft_refs(false),
    _all_soft_refs_clear(false)
{}

#ifdef ASSERT
void CollectorPolicy::assert_flags() {
  assert(InitialHeapSize <= MaxHeapSize, "Ergonomics decided on incompatible initial and maximum heap sizes");
  assert(InitialHeapSize % _heap_alignment == 0, "InitialHeapSize alignment");
  assert(MaxHeapSize % _heap_alignment == 0, "MaxHeapSize alignment");
}

void CollectorPolicy::assert_size_info() {
  assert(InitialHeapSize == _initial_heap_byte_size, "Discrepancy between InitialHeapSize flag and local storage");
  assert(MaxHeapSize == _max_heap_byte_size, "Discrepancy between MaxHeapSize flag and local storage");
  assert(_max_heap_byte_size >= _min_heap_byte_size, "Ergonomics decided on incompatible minimum and maximum heap sizes");
  assert(_initial_heap_byte_size >= _min_heap_byte_size, "Ergonomics decided on incompatible initial and minimum heap sizes");
  assert(_max_heap_byte_size >= _initial_heap_byte_size, "Ergonomics decided on incompatible initial and maximum heap sizes");
  assert(_min_heap_byte_size % _heap_alignment == 0, "min_heap_byte_size alignment");
  assert(_initial_heap_byte_size % _heap_alignment == 0, "initial_heap_byte_size alignment");
  assert(_max_heap_byte_size % _heap_alignment == 0, "max_heap_byte_size alignment");
}
#endif // ASSERT

// 主要用于校验参数的合法性，并设置相关参数
void CollectorPolicy::initialize_flags() {
  // 校验_space_alignment和_heap_alignment的合法性，_heap_alignment必须大于_space_alignment，且是_space_alignment的整数倍
  assert(_space_alignment != 0, "Space alignment not set up properly");
  assert(_heap_alignment != 0, "Heap alignment not set up properly");
  assert(_heap_alignment >= _space_alignment,
         err_msg("heap_alignment: " SIZE_FORMAT " less than space_alignment: " SIZE_FORMAT,
                 _heap_alignment, _space_alignment));
  assert(_heap_alignment % _space_alignment == 0,
         err_msg("heap_alignment: " SIZE_FORMAT " not aligned by space_alignment: " SIZE_FORMAT,
                 _heap_alignment, _space_alignment));
  // 如果MaxHeapSize是通过命令行参数配置的
  if (FLAG_IS_CMDLINE(MaxHeapSize)) {
    // 校验MaxHeapSize和InitialHeapSize的合法性
    if (FLAG_IS_CMDLINE(InitialHeapSize) && InitialHeapSize > MaxHeapSize) {
      vm_exit_during_initialization("Initial heap size set to a larger value than the maximum heap size");
    }
    if (_min_heap_byte_size != 0 && MaxHeapSize < _min_heap_byte_size) {
      vm_exit_during_initialization("Incompatible minimum and maximum heap sizes specified");
    }
    _max_heap_size_cmdline = true;
  }

  // Check heap parameter properties
  // M是一个常量，表示1M
  if (InitialHeapSize < M) {
    vm_exit_during_initialization("Too small initial heap");
  }
  if (_min_heap_byte_size < M) {
    vm_exit_during_initialization("Too small minimum heap");
  }

  // User inputs from -Xmx and -Xms must be aligned
  // 将内存对齐
  _min_heap_byte_size = align_size_up(_min_heap_byte_size, _heap_alignment);
  uintx aligned_initial_heap_size = align_size_up(InitialHeapSize, _heap_alignment);
  uintx aligned_max_heap_size = align_size_up(MaxHeapSize, _heap_alignment);

  // Write back to flags if the values changed
  // 如果原来的设置值没有对齐，则修改原来设置的值
  if (aligned_initial_heap_size != InitialHeapSize) {
    FLAG_SET_ERGO(uintx, InitialHeapSize, aligned_initial_heap_size);
  }
  if (aligned_max_heap_size != MaxHeapSize) {
    FLAG_SET_ERGO(uintx, MaxHeapSize, aligned_max_heap_size);
  }
  // 校验参数合法性，必要时修改
  if (FLAG_IS_CMDLINE(InitialHeapSize) && _min_heap_byte_size != 0 &&
      InitialHeapSize < _min_heap_byte_size) {
    vm_exit_during_initialization("Incompatible minimum and initial heap sizes specified");
  }
  if (!FLAG_IS_DEFAULT(InitialHeapSize) && InitialHeapSize > MaxHeapSize) {
    FLAG_SET_ERGO(uintx, MaxHeapSize, InitialHeapSize);
  } else if (!FLAG_IS_DEFAULT(MaxHeapSize) && InitialHeapSize > MaxHeapSize) {
    FLAG_SET_ERGO(uintx, InitialHeapSize, MaxHeapSize);
    if (InitialHeapSize < _min_heap_byte_size) {
      _min_heap_byte_size = InitialHeapSize;
    }
  }

  _initial_heap_byte_size = InitialHeapSize;
  _max_heap_byte_size = MaxHeapSize;

  // MinHeapDeltaBytes参数表示堆内存空间因为GC而发生变动的最小值
  FLAG_SET_ERGO(uintx, MinHeapDeltaBytes, align_size_up(MinHeapDeltaBytes, _space_alignment));

  DEBUG_ONLY(CollectorPolicy::assert_flags();)
}

void CollectorPolicy::initialize_size_info() {
  // 打印日志
  if (PrintGCDetails && Verbose) {
    gclog_or_tty->print_cr("Minimum heap " SIZE_FORMAT "  Initial heap "
      SIZE_FORMAT "  Maximum heap " SIZE_FORMAT,
      _min_heap_byte_size, _initial_heap_byte_size, _max_heap_byte_size);
  }

  DEBUG_ONLY(CollectorPolicy::assert_size_info();)
}

bool CollectorPolicy::use_should_clear_all_soft_refs(bool v) {
  bool result = _should_clear_all_soft_refs;
  set_should_clear_all_soft_refs(false);
  return result;
}

GenRemSet* CollectorPolicy::create_rem_set(MemRegion whole_heap,
                                           int max_covered_regions) {
  return new CardTableRS(whole_heap, max_covered_regions);
}

void CollectorPolicy::cleared_all_soft_refs() {
  // If near gc overhear limit, continue to clear SoftRefs.  SoftRefs may
  // have been cleared in the last collection but if the gc overhear
  // limit continues to be near, SoftRefs should still be cleared.
  if (size_policy() != NULL) {
    _should_clear_all_soft_refs = size_policy()->gc_overhead_limit_near();
  }
  _all_soft_refs_clear = true;
}

size_t CollectorPolicy::compute_heap_alignment() {
  // The card marking array and the offset arrays for old generations are
  // committed in os pages as well. Make sure they are entirely full (to
  // avoid partial page problems), e.g. if 512 bytes heap corresponds to 1
  // byte entry and the os page size is 4096, the maximum heap size should
  // be 512*4096 = 2MB aligned.

  // There is only the GenRemSet in Hotspot and only the GenRemSet::CardTable
  // is supported.
  // Requirements of any new remembered set implementations must be added here.
  size_t alignment = GenRemSet::max_alignment_constraint(GenRemSet::CardTable);

  // Parallel GC does its own alignment of the generations to avoid requiring a
  // large page (256M on some platforms) for the permanent generation.  The
  // other collectors should also be updated to do their own alignment and then
  // this use of lcm() should be removed.
  if (UseLargePages && !UseParallelGC) {
      // in presence of large pages we have to make sure that our
      // alignment is large page aware
      alignment = lcm(os::large_page_size(), alignment);
  }

  return alignment;
}

// GenCollectorPolicy methods.

GenCollectorPolicy::GenCollectorPolicy() :
    _min_gen0_size(0),
    _initial_gen0_size(0),
    _max_gen0_size(0),
    _gen_alignment(0),
    _generations(NULL)
{}

size_t GenCollectorPolicy::scale_by_NewRatio_aligned(size_t base_size) {
  return align_size_down_bounded(base_size / (NewRatio + 1), _gen_alignment);
}

size_t GenCollectorPolicy::bound_minus_alignment(size_t desired_size,
                                                 size_t maximum_size) {
  size_t max_minus = maximum_size - _gen_alignment;
  return desired_size < max_minus ? desired_size : max_minus;
}


void GenCollectorPolicy::initialize_size_policy(size_t init_eden_size,
                                                size_t init_promo_size,
                                                size_t init_survivor_size) {
  const double max_gc_pause_sec = ((double) MaxGCPauseMillis) / 1000.0;
  _size_policy = new AdaptiveSizePolicy(init_eden_size,
                                        init_promo_size,
                                        init_survivor_size,
                                        max_gc_pause_sec,
                                        GCTimeRatio);
}

size_t GenCollectorPolicy::young_gen_size_lower_bound() {
  // The young generation must be aligned and have room for eden + two survivors
  // 年轻代包含三个区，eden+two survivors，所以这里是3
  return align_size_up(3 * _space_alignment, _gen_alignment);
}

#ifdef ASSERT
void GenCollectorPolicy::assert_flags() {
  CollectorPolicy::assert_flags();
  assert(NewSize >= _min_gen0_size, "Ergonomics decided on a too small young gen size");
  assert(NewSize <= MaxNewSize, "Ergonomics decided on incompatible initial and maximum young gen sizes");
  assert(FLAG_IS_DEFAULT(MaxNewSize) || MaxNewSize < MaxHeapSize, "Ergonomics decided on incompatible maximum young gen and heap sizes");
  assert(NewSize % _gen_alignment == 0, "NewSize alignment");
  assert(FLAG_IS_DEFAULT(MaxNewSize) || MaxNewSize % _gen_alignment == 0, "MaxNewSize alignment");
}

void TwoGenerationCollectorPolicy::assert_flags() {
  GenCollectorPolicy::assert_flags();
  assert(OldSize + NewSize <= MaxHeapSize, "Ergonomics decided on incompatible generation and heap sizes");
  assert(OldSize % _gen_alignment == 0, "OldSize alignment");
}

void GenCollectorPolicy::assert_size_info() {
  CollectorPolicy::assert_size_info();
  // GenCollectorPolicy::initialize_size_info may update the MaxNewSize
  assert(MaxNewSize < MaxHeapSize, "Ergonomics decided on incompatible maximum young and heap sizes");
  assert(NewSize == _initial_gen0_size, "Discrepancy between NewSize flag and local storage");
  assert(MaxNewSize == _max_gen0_size, "Discrepancy between MaxNewSize flag and local storage");
  assert(_min_gen0_size <= _initial_gen0_size, "Ergonomics decided on incompatible minimum and initial young gen sizes");
  assert(_initial_gen0_size <= _max_gen0_size, "Ergonomics decided on incompatible initial and maximum young gen sizes");
  assert(_min_gen0_size % _gen_alignment == 0, "_min_gen0_size alignment");
  assert(_initial_gen0_size % _gen_alignment == 0, "_initial_gen0_size alignment");
  assert(_max_gen0_size % _gen_alignment == 0, "_max_gen0_size alignment");
}

void TwoGenerationCollectorPolicy::assert_size_info() {
  GenCollectorPolicy::assert_size_info();
  assert(OldSize == _initial_gen1_size, "Discrepancy between OldSize flag and local storage");
  assert(_min_gen1_size <= _initial_gen1_size, "Ergonomics decided on incompatible minimum and initial old gen sizes");
  assert(_initial_gen1_size <= _max_gen1_size, "Ergonomics decided on incompatible initial and maximum old gen sizes");
  assert(_max_gen1_size % _gen_alignment == 0, "_max_gen1_size alignment");
  assert(_initial_gen1_size % _gen_alignment == 0, "_initial_gen1_size alignment");
  assert(_max_heap_byte_size <= (_max_gen0_size + _max_gen1_size), "Total maximum heap sizes must be sum of generation maximum sizes");
}
#endif // ASSERT

void GenCollectorPolicy::initialize_flags() {
  // 调用父类方法初始化父类属性
  CollectorPolicy::initialize_flags();
  // 校验_gen_alignment参数的合法性，_gen_alignment必须被_space_alignment整除，_heap_alignment被_gen_alignment整除
  assert(_gen_alignment != 0, "Generation alignment not set up properly");
  assert(_heap_alignment >= _gen_alignment,
         err_msg("heap_alignment: " SIZE_FORMAT " less than gen_alignment: " SIZE_FORMAT,
                 _heap_alignment, _gen_alignment));
  assert(_gen_alignment % _space_alignment == 0,
         err_msg("gen_alignment: " SIZE_FORMAT " not aligned by space_alignment: " SIZE_FORMAT,
                 _gen_alignment, _space_alignment));
  assert(_heap_alignment % _gen_alignment == 0,
         err_msg("heap_alignment: " SIZE_FORMAT " not aligned by gen_alignment: " SIZE_FORMAT,
                 _heap_alignment, _gen_alignment));

  // All generational heaps have a youngest gen; handle those flags here

  // Make sure the heap is large enough for two generations
  // young_gen_size_lower_bound方法返回年轻代的内存最小值
  uintx smallest_new_size = young_gen_size_lower_bound();
  // 老年代只有一个区，将老年代的最小值加上年轻代的最小值，按照_heap_alignment向上取整就是堆内存的最小值
  uintx smallest_heap_size = align_size_up(smallest_new_size + align_size_up(_space_alignment, _gen_alignment),
                                           _heap_alignment);
  // 重置MaxHeapSize
  if (MaxHeapSize < smallest_heap_size) {
    FLAG_SET_ERGO(uintx, MaxHeapSize, smallest_heap_size);
    _max_heap_byte_size = MaxHeapSize;
  }
  // If needed, synchronize _min_heap_byte size and _initial_heap_byte_size
  if (_min_heap_byte_size < smallest_heap_size) {
    _min_heap_byte_size = smallest_heap_size;
    // 重置InitialHeapSize
    if (InitialHeapSize < _min_heap_byte_size) {
      FLAG_SET_ERGO(uintx, InitialHeapSize, smallest_heap_size);
      _initial_heap_byte_size = smallest_heap_size;
    }
  }

  // Now take the actual NewSize into account. We will silently increase NewSize
  // if the user specified a smaller value.
  // NewSize表示一个新的generation(分代内存区)的初始内存值
  smallest_new_size = MAX2(smallest_new_size, (uintx)align_size_down(NewSize, _gen_alignment));
  if (smallest_new_size != NewSize) {
    // 重置MaxNewSize
    FLAG_SET_ERGO(uintx, NewSize, smallest_new_size);
  }
  _initial_gen0_size = NewSize;

  // MaxNewSize表示表示一个新的generation(分代内存区)的最大内存值
  if (!FLAG_IS_DEFAULT(MaxNewSize)) { // 如果MaxNewSize不是默认值
    uintx min_new_size = MAX2(_gen_alignment, _min_gen0_size);

    if (MaxNewSize >= MaxHeapSize) {
      // Make sure there is room for an old generation
      uintx smaller_max_new_size = MaxHeapSize - _gen_alignment;
      if (FLAG_IS_CMDLINE(MaxNewSize)) {
        warning("MaxNewSize (" SIZE_FORMAT "k) is equal to or greater than the entire "
                "heap (" SIZE_FORMAT "k).  A new max generation size of " SIZE_FORMAT "k will be used.",
                MaxNewSize/K, MaxHeapSize/K, smaller_max_new_size/K);
      }
      // 重置MaxNewSize
      FLAG_SET_ERGO(uintx, MaxNewSize, smaller_max_new_size);
      if (NewSize > MaxNewSize) {
        // 重置NewSize
        FLAG_SET_ERGO(uintx, NewSize, MaxNewSize);
        _initial_gen0_size = NewSize;
      }
    } else if (MaxNewSize < min_new_size) {
      FLAG_SET_ERGO(uintx, MaxNewSize, min_new_size);
    } else if (!is_size_aligned(MaxNewSize, _gen_alignment)) {
      FLAG_SET_ERGO(uintx, MaxNewSize, align_size_down(MaxNewSize, _gen_alignment));
    }
    _max_gen0_size = MaxNewSize;
  }

  if (NewSize > MaxNewSize) {
    // At this point this should only happen if the user specifies a large NewSize and/or
    // a small (but not too small) MaxNewSize.
    if (FLAG_IS_CMDLINE(MaxNewSize)) {
      warning("NewSize (" SIZE_FORMAT "k) is greater than the MaxNewSize (" SIZE_FORMAT "k). "
              "A new max generation size of " SIZE_FORMAT "k will be used.",
              NewSize/K, MaxNewSize/K, NewSize/K);
    }
    // 重置MaxNewSize
    FLAG_SET_ERGO(uintx, MaxNewSize, NewSize);
    _max_gen0_size = MaxNewSize;
  }

  // SurvivorRatio表示eden/survivor的倍数，默认是8，NewRatio表示老年代与年轻代的内存空间比例，默认是2。
  if (SurvivorRatio < 1 || NewRatio < 1) {
    vm_exit_during_initialization("Invalid young gen ratio specified");
  }

  DEBUG_ONLY(GenCollectorPolicy::assert_flags();)
}

void TwoGenerationCollectorPolicy::initialize_flags() {
  GenCollectorPolicy::initialize_flags();

  if (!is_size_aligned(OldSize, _gen_alignment)) {
    FLAG_SET_ERGO(uintx, OldSize, align_size_down(OldSize, _gen_alignment));
  }

  if (FLAG_IS_CMDLINE(OldSize) && FLAG_IS_DEFAULT(MaxHeapSize)) {
    // NewRatio will be used later to set the young generation size so we use
    // it to calculate how big the heap should be based on the requested OldSize
    // and NewRatio.
    assert(NewRatio > 0, "NewRatio should have been set up earlier");
    size_t calculated_heapsize = (OldSize / NewRatio) * (NewRatio + 1);

    calculated_heapsize = align_size_up(calculated_heapsize, _heap_alignment);
    FLAG_SET_ERGO(uintx, MaxHeapSize, calculated_heapsize);
    _max_heap_byte_size = MaxHeapSize;
    FLAG_SET_ERGO(uintx, InitialHeapSize, calculated_heapsize);
    _initial_heap_byte_size = InitialHeapSize;
  }

  // adjust max heap size if necessary
  if (NewSize + OldSize > MaxHeapSize) {
    if (_max_heap_size_cmdline) {
      // somebody set a maximum heap size with the intention that we should not
      // exceed it. Adjust New/OldSize as necessary.
      uintx calculated_size = NewSize + OldSize;
      double shrink_factor = (double) MaxHeapSize / calculated_size;
      uintx smaller_new_size = align_size_down((uintx)(NewSize * shrink_factor), _gen_alignment);
      FLAG_SET_ERGO(uintx, NewSize, MAX2(young_gen_size_lower_bound(), smaller_new_size));
      _initial_gen0_size = NewSize;

      // OldSize is already aligned because above we aligned MaxHeapSize to
      // _heap_alignment, and we just made sure that NewSize is aligned to
      // _gen_alignment. In initialize_flags() we verified that _heap_alignment
      // is a multiple of _gen_alignment.
      FLAG_SET_ERGO(uintx, OldSize, MaxHeapSize - NewSize);
    } else {
      FLAG_SET_ERGO(uintx, MaxHeapSize, align_size_up(NewSize + OldSize, _heap_alignment));
      _max_heap_byte_size = MaxHeapSize;
    }
  }

  always_do_update_barrier = UseConcMarkSweepGC;

  DEBUG_ONLY(TwoGenerationCollectorPolicy::assert_flags();)
}

// Values set on the command line win over any ergonomically
// set command line parameters.
// Ergonomic choice of parameters are done before this
// method is called.  Values for command line parameters such as NewSize
// and MaxNewSize feed those ergonomic choices into this method.
// This method makes the final generation sizings consistent with
// themselves and with overall heap sizings.
// In the absence of explicitly set command line flags, policies
// such as the use of NewRatio are used to size the generation.
void GenCollectorPolicy::initialize_size_info() {
  // 调用父类方法
  CollectorPolicy::initialize_size_info();

  // _space_alignment is used for alignment within a generation.
  // There is additional alignment done down stream for some
  // collectors that sometimes causes unwanted rounding up of
  // generations sizes.

  // Determine maximum size of gen0

  size_t max_new_size = 0;
  if (!FLAG_IS_DEFAULT(MaxNewSize)) {
    max_new_size = MaxNewSize;
  } else {
    // 按照NewRatio计算允许的新的generation的内存最大值
    max_new_size = scale_by_NewRatio_aligned(_max_heap_byte_size);
    // Bound the maximum size by NewSize below (since it historically
    // would have been NewSize and because the NewRatio calculation could
    // yield a size that is too small) and bound it by MaxNewSize above.
    // Ergonomics plays here by previously calculating the desired
    // NewSize and MaxNewSize.
    max_new_size = MIN2(MAX2(max_new_size, NewSize), MaxNewSize);
  }
  assert(max_new_size > 0, "All paths should set max_new_size");

  // Given the maximum gen0 size, determine the initial and
  // minimum gen0 sizes.

  if (_max_heap_byte_size == _min_heap_byte_size) {
    // The maximum and minimum heap sizes are the same so
    // the generations minimum and initial must be the
    // same as its maximum.
    _min_gen0_size = max_new_size;
    _initial_gen0_size = max_new_size;
    _max_gen0_size = max_new_size;
  } else {
    size_t desired_new_size = 0;
    if (!FLAG_IS_DEFAULT(NewSize)) {
      // If NewSize is set ergonomically (for example by cms), it
      // would make sense to use it.  If it is used, also use it
      // to set the initial size.  Although there is no reason
      // the minimum size and the initial size have to be the same,
      // the current implementation gets into trouble during the calculation
      // of the tenured generation sizes if they are different.
      // Note that this makes the initial size and the minimum size
      // generally small compared to the NewRatio calculation.
      _min_gen0_size = NewSize;
      desired_new_size = NewSize;
      max_new_size = MAX2(max_new_size, NewSize);
    } else {
      // For the case where NewSize is the default, use NewRatio
      // to size the minimum and initial generation sizes.
      // Use the default NewSize as the floor for these values.  If
      // NewRatio is overly large, the resulting sizes can be too
      // small.
      _min_gen0_size = MAX2(scale_by_NewRatio_aligned(_min_heap_byte_size), NewSize);
      desired_new_size =
        MAX2(scale_by_NewRatio_aligned(_initial_heap_byte_size), NewSize);
    }

    assert(_min_gen0_size > 0, "Sanity check");
    _initial_gen0_size = desired_new_size;
    _max_gen0_size = max_new_size;

    // At this point the desirable initial and minimum sizes have been
    // determined without regard to the maximum sizes.

    // Bound the sizes by the corresponding overall heap sizes.
    _min_gen0_size = bound_minus_alignment(_min_gen0_size, _min_heap_byte_size);
    _initial_gen0_size = bound_minus_alignment(_initial_gen0_size, _initial_heap_byte_size);
    _max_gen0_size = bound_minus_alignment(_max_gen0_size, _max_heap_byte_size);

    // At this point all three sizes have been checked against the
    // maximum sizes but have not been checked for consistency
    // among the three.

    // Final check min <= initial <= max
    _min_gen0_size = MIN2(_min_gen0_size, _max_gen0_size);
    _initial_gen0_size = MAX2(MIN2(_initial_gen0_size, _max_gen0_size), _min_gen0_size);
    _min_gen0_size = MIN2(_min_gen0_size, _initial_gen0_size);
  }

  // Write back to flags if necessary
  // 重置NewSize
  if (NewSize != _initial_gen0_size) {
    FLAG_SET_ERGO(uintx, NewSize, _initial_gen0_size);
  }

  // 重置MaxNewSize
  if (MaxNewSize != _max_gen0_size) {
    FLAG_SET_ERGO(uintx, MaxNewSize, _max_gen0_size);
  }

  if (PrintGCDetails && Verbose) {
    gclog_or_tty->print_cr("1: Minimum gen0 " SIZE_FORMAT "  Initial gen0 "
      SIZE_FORMAT "  Maximum gen0 " SIZE_FORMAT,
      _min_gen0_size, _initial_gen0_size, _max_gen0_size);
  }

  DEBUG_ONLY(GenCollectorPolicy::assert_size_info();)
}

// Call this method during the sizing of the gen1 to make
// adjustments to gen0 because of gen1 sizing policy.  gen0 initially has
// the most freedom in sizing because it is done before the
// policy for gen1 is applied.  Once gen1 policies have been applied,
// there may be conflicts in the shape of the heap and this method
// is used to make the needed adjustments.  The application of the
// policies could be more sophisticated (iterative for example) but
// keeping it simple also seems a worthwhile goal.
bool TwoGenerationCollectorPolicy::adjust_gen0_sizes(size_t* gen0_size_ptr,
                                                     size_t* gen1_size_ptr,
                                                     const size_t heap_size) {
  bool result = false;

  if ((*gen0_size_ptr + *gen1_size_ptr) > heap_size) {
    uintx smallest_new_size = young_gen_size_lower_bound();
    if ((heap_size < (*gen0_size_ptr + _min_gen1_size)) &&
        (heap_size >= _min_gen1_size + smallest_new_size)) {
      // Adjust gen0 down to accommodate _min_gen1_size
      *gen0_size_ptr = align_size_down_bounded(heap_size - _min_gen1_size, _gen_alignment);
      result = true;
    } else {
      *gen1_size_ptr = align_size_down_bounded(heap_size - *gen0_size_ptr, _gen_alignment);
    }
  }
  return result;
}

// Minimum sizes of the generations may be different than
// the initial sizes.  An inconsistently is permitted here
// in the total size that can be specified explicitly by
// command line specification of OldSize and NewSize and
// also a command line specification of -Xms.  Issue a warning
// but allow the values to pass.

void TwoGenerationCollectorPolicy::initialize_size_info() {
  GenCollectorPolicy::initialize_size_info();

  // At this point the minimum, initial and maximum sizes
  // of the overall heap and of gen0 have been determined.
  // The maximum gen1 size can be determined from the maximum gen0
  // and maximum heap size since no explicit flags exits
  // for setting the gen1 maximum.
  _max_gen1_size = MAX2(_max_heap_byte_size - _max_gen0_size, _gen_alignment);

  // If no explicit command line flag has been set for the
  // gen1 size, use what is left for gen1.
  if (!FLAG_IS_CMDLINE(OldSize)) {
    // The user has not specified any value but the ergonomics
    // may have chosen a value (which may or may not be consistent
    // with the overall heap size).  In either case make
    // the minimum, maximum and initial sizes consistent
    // with the gen0 sizes and the overall heap sizes.
    _min_gen1_size = MAX2(_min_heap_byte_size - _min_gen0_size, _gen_alignment);
    _initial_gen1_size = MAX2(_initial_heap_byte_size - _initial_gen0_size, _gen_alignment);
    // _max_gen1_size has already been made consistent above
    FLAG_SET_ERGO(uintx, OldSize, _initial_gen1_size);
  } else {
    // It's been explicitly set on the command line.  Use the
    // OldSize and then determine the consequences.
    _min_gen1_size = MIN2(OldSize, _min_heap_byte_size - _min_gen0_size);
    _initial_gen1_size = OldSize;

    // If the user has explicitly set an OldSize that is inconsistent
    // with other command line flags, issue a warning.
    // The generation minimums and the overall heap mimimum should
    // be within one generation alignment.
    if ((_min_gen1_size + _min_gen0_size + _gen_alignment) < _min_heap_byte_size) {
      warning("Inconsistency between minimum heap size and minimum "
              "generation sizes: using minimum heap = " SIZE_FORMAT,
              _min_heap_byte_size);
    }
    if (OldSize > _max_gen1_size) {
      warning("Inconsistency between maximum heap size and maximum "
              "generation sizes: using maximum heap = " SIZE_FORMAT
              " -XX:OldSize flag is being ignored",
              _max_heap_byte_size);
    }
    // If there is an inconsistency between the OldSize and the minimum and/or
    // initial size of gen0, since OldSize was explicitly set, OldSize wins.
    if (adjust_gen0_sizes(&_min_gen0_size, &_min_gen1_size, _min_heap_byte_size)) {
      if (PrintGCDetails && Verbose) {
        gclog_or_tty->print_cr("2: Minimum gen0 " SIZE_FORMAT "  Initial gen0 "
              SIZE_FORMAT "  Maximum gen0 " SIZE_FORMAT,
              _min_gen0_size, _initial_gen0_size, _max_gen0_size);
      }
    }
    // Initial size
    if (adjust_gen0_sizes(&_initial_gen0_size, &_initial_gen1_size,
                          _initial_heap_byte_size)) {
      if (PrintGCDetails && Verbose) {
        gclog_or_tty->print_cr("3: Minimum gen0 " SIZE_FORMAT "  Initial gen0 "
          SIZE_FORMAT "  Maximum gen0 " SIZE_FORMAT,
          _min_gen0_size, _initial_gen0_size, _max_gen0_size);
      }
    }
  }
  // Enforce the maximum gen1 size.
  _min_gen1_size = MIN2(_min_gen1_size, _max_gen1_size);

  // Check that min gen1 <= initial gen1 <= max gen1
  _initial_gen1_size = MAX2(_initial_gen1_size, _min_gen1_size);
  _initial_gen1_size = MIN2(_initial_gen1_size, _max_gen1_size);

  // Write back to flags if necessary
  if (NewSize != _initial_gen0_size) {
    FLAG_SET_ERGO(uintx, NewSize, _initial_gen0_size);
  }

  if (MaxNewSize != _max_gen0_size) {
    FLAG_SET_ERGO(uintx, MaxNewSize, _max_gen0_size);
  }

  if (OldSize != _initial_gen1_size) {
    FLAG_SET_ERGO(uintx, OldSize, _initial_gen1_size);
  }

  if (PrintGCDetails && Verbose) {
    gclog_or_tty->print_cr("Minimum gen1 " SIZE_FORMAT "  Initial gen1 "
      SIZE_FORMAT "  Maximum gen1 " SIZE_FORMAT,
      _min_gen1_size, _initial_gen1_size, _max_gen1_size);
  }

  DEBUG_ONLY(TwoGenerationCollectorPolicy::assert_size_info();)
}

// 用于从Java堆中分配指定大小的内存块，并在必要时触发GC
HeapWord* GenCollectorPolicy::mem_allocate_work(size_t size,
                                        bool is_tlab,
                                        bool* gc_overhead_limit_was_exceeded) {
  // 获取Java堆对象
  GenCollectedHeap *gch = GenCollectedHeap::heap();

  debug_only(gch->check_for_valid_allocation_state());

  // 校验当前没有GC
  assert(gch->no_gc_in_progress(), "Allocation during gc not allowed");

  // In general gc_overhead_limit_was_exceeded should be false so
  // set it so here and reset it to true only if the gc time
  // limit is being exceeded as checked below.
  // 默认情况下gc_overhead_limit_was_exceeded是false，只有当GC耗时超过限制了才会置为true
  *gc_overhead_limit_was_exceeded = false;

  HeapWord* result = NULL;

  // Loop until the allocation is satisified,
  // or unsatisfied after GC.
  // 循环直到分配成功，或在 GC 后不满足分配。
  for (int try_count = 1, gclocker_stalled_count = 0; /* return or throw */; try_count += 1) {
    // 通过析构函数自动丢弃掉在每次循环过程中分配的Handle
    HandleMark hm; // discard any handles allocated in each iteration

    // First allocation attempt is lock-free.  第一次分配尝试是无锁的。
    // 获取年轻代
    Generation *gen0 = gch->get_gen(0);
    // 校验其是否支持线性分配方式分配一段地址连续的内存
    assert(gen0->supports_inline_contig_alloc(),
      "Otherwise, must do alloc within heap lock");
    // 是否应该从该Generation分配，如果size过大则应该在老年代分配
    if (gen0->should_allocate(size, is_tlab)) {
      // Generation内部加锁，分配指定大小的内存
      result = gen0->par_allocate(size, is_tlab);
      if (result != NULL) {
        // 分配成功，is_in_reserved方法检查某个指针是否在堆内存范围内
        assert(gch->is_in_reserved(result), "result not in heap");
        return result;
      }
    }

    // 如果不能在年轻代分配或者年轻代分配失败
    unsigned int gc_count_before;  // read inside the Heap_lock locked region  // 读取Heap_lock锁定区域内部
    {
      // 获取Heap_lock锁
      MutexLocker ml(Heap_lock);
      if (PrintGC && Verbose) {
        gclog_or_tty->print_cr("TwoGenerationCollectorPolicy::mem_allocate_work:"
                      " attempting locked slow path allocation");
      }
      // Note that only large objects get a shot at being
      // allocated in later generations.
      // first_only表示是否只尝试一次，should_try_older_generation_allocation方法返回是否尝试在老年代分配，只有大对象才会在老年代分配。
      bool first_only = ! should_try_older_generation_allocation(size);
      // 遍历所有的Generation尝试分配
      result = gch->attempt_allocation(size, is_tlab, first_only);
      if (result != NULL) {
        // 分配成功
        assert(gch->is_in_reserved(result), "result not in heap");
        return result;
      }

      // 在其他的Generation中同样分配失败，有线程处于JNI关键区且需要GC
      if (GC_locker::is_active_and_needs_gc()) {
        if (is_tlab) {
          // 如果TLAB，调用方会自动重试，其他线程会触发GC，所以这里返回NULL
          return NULL;  // Caller will retry allocating individual object   // 调用方将重试分配单个对象
        }
        // 如果某个Generation还能扩展
        if (!gch->is_maximal_no_gc()) {
          // Try and expand heap to satisfy request
          // 尝试扩展Java堆并分配
          result = expand_heap_and_allocate(size, is_tlab);
          // result could be null if we are out of space
          if (result != NULL) {
            // 分配成功
            return result;
          }
        }

        // 所有Generation都不能扩展了，只能GC了
        // GCLockerRetryAllocationCount表示当因为GC锁被阻塞了，尝试分配内存的次数，默认值是2
        if (gclocker_stalled_count > GCLockerRetryAllocationCount) {
          return NULL; // we didn't get to do a GC and we didn't get any memory  // 我们没有做GC，也没有获得任何内存
        }

        // If this thread is not in a jni critical section, we stall
        // the requestor until the critical section has cleared and
        // GC allowed. When the critical section clears, a GC is
        // initiated by the last thread exiting the critical section; so
        // we retry the allocation sequence from the beginning of the loop,
        // rather than causing more, now probably unnecessary, GC attempts.
        // 如果这个线程不在JNI关键区，则会阻塞当前线程，等待所有处于JNI关键区的线程从关键区中退出了，最后一个退出的线程会触发GC，避免触发更多的没必要的GC。
        JavaThread* jthr = JavaThread::current();
        // 判断当前线程是否处于JNI关键区内
        if (!jthr->in_critical()) {
          MutexUnlocker mul(Heap_lock);
          // Wait for JNI critical section to be exited
          // 阻塞当前线程直到处于JNI关键区的线程从关键区中退出了
          GC_locker::stall_until_clear();
          gclocker_stalled_count += 1;
          // GC结束，在下一次循环中尝试分配内存
          continue;
        } else {
          // 如果当前线程在JNI关键区内，CheckJNICalls表示是否校验JNI调用的参数，默认为false
          if (CheckJNICalls) {
            fatal("Possible deadlock due to allocating while"
                  " in jni critical section");
          }
          // 返回NULL，因为处于关键区中的线程没法GC，没法继续分配内存
          return NULL;
        }
      }

      // Read the gc count while the heap lock is held.
      // 在保持堆锁，读取GC的累计次数
      gc_count_before = Universe::heap()->total_collections();
    }

    // 处于关键区的线程退出了并执行GC了依然分配失败，没有线程处于关键区中，分配失败，触发GC。
    VM_GenCollectForAllocation op(size, is_tlab, gc_count_before);
    // 当前线程会被阻塞直到GC执行完成
    VMThread::execute(&op);
    // 如果执行了GC，返回false表示该次GC被跳过了，可能其他线程执行了GC
    if (op.prologue_succeeded()) {
      result = op.result();
      // gc_locked返回true表示GC后依然分配失败，需要再次GC
      if (op.gc_locked()) {
         assert(result == NULL, "must be NULL if gc_locked() is true");
         continue;  // retry and/or stall as necessary
      }

      // Allocation has failed and a collection
      // has been done.  If the gc time limit was exceeded the
      // this time, return NULL so that an out-of-memory
      // will be thrown.  Clear gc_overhead_limit_exceeded
      // so that the overhead exceeded does not persist.

      // GC完成且内存分配成功，如果GC耗时超过限制了则会返回NULL并抛出异常，然后重置gc_overhead_limit_exceeded
      // 这样gc_overhead_limit_exceeded不会持久化
      // gc_overhead_limit_exceeded返回是否GC耗时超过限制
      const bool limit_exceeded = size_policy()->gc_overhead_limit_exceeded();
      // all_soft_refs_clear返回是否清除了所有的软引用
      const bool softrefs_clear = all_soft_refs_clear();

      if (limit_exceeded && softrefs_clear) {
        *gc_overhead_limit_was_exceeded = true;
        // 重置gc_overhead_limit_exceeded属性
        size_policy()->set_gc_overhead_limit_exceeded(false);
        if (op.result() != NULL) {
          // 填充对象，因为GC耗时超了，为了避免后期GC耗时越来越长，所以认为内存分配失败，需要抛出异常
          CollectedHeap::fill_with_object(op.result(), size);
        }
        return NULL;
      }
      assert(result == NULL || gch->is_in_reserved(result),
             "result not in heap");
      return result;
    }

    // Give a warning if we seem to be looping forever.
    // 本次GC被跳过了，其他线程执行了GC，再次循环尝试分配内存
    // QueuedAllocationWarningCount表示尝试了一定次数后就打印warning日志，默认值是0
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
          warning("TwoGenerationCollectorPolicy::mem_allocate_work retries %d times \n\t"
                  " size=%d %s", try_count, size, is_tlab ? "(TLAB)" : "");
    }
  }
}

HeapWord* GenCollectorPolicy::expand_heap_and_allocate(size_t size,
                                                       bool   is_tlab) {
  GenCollectedHeap *gch = GenCollectedHeap::heap();
  HeapWord* result = NULL;
  // 遍历所有的Generation
  for (int i = number_of_generations() - 1; i >= 0 && result == NULL; i--) {
    Generation *gen = gch->get_gen(i);
    if (gen->should_allocate(size, is_tlab)) {
      // 如果Generation可以分配，则尝试扩展并分配
      result = gen->expand_and_allocate(size, is_tlab);
    }
  }
  assert(result == NULL || gch->is_in_reserved(result), "result not in heap");
  return result;
}

HeapWord* GenCollectorPolicy::satisfy_failed_allocation(size_t size,
                                                        bool   is_tlab) {
  // 获取Java堆
  GenCollectedHeap *gch = GenCollectedHeap::heap();
  // 初始化GCCause
  GCCauseSetter x(gch, GCCause::_allocation_failure);
  HeapWord* result = NULL;

  assert(size != 0, "Precondition violated");
  if (GC_locker::is_active_and_needs_gc()) {
    // GC locker is active; instead of a collection we will attempt
    // to expand the heap, if there's room for expansion.
    // GC_locker是active说明还有线程处于JNI关键区未开始GC
    if (!gch->is_maximal_no_gc()) {
      result = expand_heap_and_allocate(size, is_tlab);
    }
    return result;   // could be null if we are out of space
  } else if (!gch->incremental_collection_will_fail(false /* don't consult_young */)) {
    // Do an incremental collection.
    // incremental_collection_will_fail表示如果执行增量收集是否会失败，参考上一次增量收集的结果，如果可以执行增量收集。
    gch->do_collection(false            /* full */,
                       false            /* clear_all_soft_refs */,
                       size             /* size */,
                       is_tlab          /* is_tlab */,
                       number_of_generations() - 1 /* max_level */);
  } else {
    if (Verbose && PrintGCDetails) {
      gclog_or_tty->print(" :: Trying full because partial may fail :: ");
    }
    // Try a full collection; see delta for bug id 6266275
    // for the original code and why this has been simplified
    // with from-space allocation criteria modified and
    // such allocation moved out of the safepoint path.
    // 执行全量的full GC
    gch->do_collection(true             /* full */,
                       false            /* clear_all_soft_refs */,
                       size             /* size */,
                       is_tlab          /* is_tlab */,
                       number_of_generations() - 1 /* max_level */);
  }

  // 尝试分配内存
  result = gch->attempt_allocation(size, is_tlab, false /*first_only*/);

  if (result != NULL) {
    // 分配成功
    assert(gch->is_in_reserved(result), "result not in heap");
    return result;
  }

  // OK, collection failed, try expansion.
  // 分配失败，尝试扩展Java堆并分配内存
  result = expand_heap_and_allocate(size, is_tlab);
  if (result != NULL) {
    return result;
  }

  // If we reach this point, we're really out of memory. Try every trick
  // we can to reclaim memory. Force collection of soft references. Force
  // a complete compaction of the heap. Any additional methods for finding
  // free memory should be here, especially if they are expensive. If this
  // attempt fails, an OOM exception will be thrown.

  // 分配失败，说明已经内存不足了，这时需要强制清理软引用，强制压实Java堆，任何可能的获取可用内存的方法都会执行，尽管代价昂贵。如果尝试失败，抛出OOM异常。
  {
    UIntFlagSetting flag_change(MarkSweepAlwaysCompactCount, 1); // Make sure the heap is fully compacted

    gch->do_collection(true             /* full */,
                       true             /* clear_all_soft_refs */,
                       size             /* size */,
                       is_tlab          /* is_tlab */,
                       number_of_generations() - 1 /* max_level */);
  }

  // 再次尝试分配内存
  result = gch->attempt_allocation(size, is_tlab, false /* first_only */);
  if (result != NULL) {
    assert(gch->is_in_reserved(result), "result not in heap");
    return result;
  }

  // 分配失败，软引用已经清理的，标志位被置为false
  assert(!should_clear_all_soft_refs(),
    "Flag should have been handled and cleared prior to this point");

  // What else?  We might try synchronous finalization later.  If the total
  // space available is large enough for the allocation, then a more
  // complete compaction phase than we've tried so far might be
  // appropriate.
  return NULL;
}

MetaWord* CollectorPolicy::satisfy_failed_metadata_allocation(
                                                 ClassLoaderData* loader_data,
                                                 size_t word_size,
                                                 Metaspace::MetadataType mdtype) {
  uint loop_count = 0;
  uint gc_count = 0;
  uint full_gc_count = 0;

  // 当前线程没有拥有Heap_lock
  assert(!Heap_lock->owned_by_self(), "Should not be holding the Heap_lock");

  do {
    MetaWord* result = NULL;
    // is_active表示有线程处在JNI关键区中，不能执行GC，needs_gc表示需要GC
    if (GC_locker::is_active_and_needs_gc()) {
      // If the GC_locker is active, just expand and allocate.
      // If that does not succeed, wait if this thread is not
      // in a critical section itself.
      // 尝试去扩展Metaspace并分配
      result =
        loader_data->metaspace_non_null()->expand_and_allocate(word_size,
                                                               mdtype);
      if (result != NULL) {
        // 分配成功
        return result;
      }
      JavaThread* jthr = JavaThread::current();
      // 如果当前线程不在JNI关键区中
      if (!jthr->in_critical()) {
        // Wait for JNI critical section to be exited
        // 阻塞直到所有处在JNI关键区中的线程从JNI关键区中退出，最后一个从关键区退出的线程会在退出时触发GC，参考GC_Locker的实现
        GC_locker::stall_until_clear();
        // 离开关键部分的最后一个线程调用的 GC 将是一个年轻的集合，卸载类（当前）需要一个完整的集合，因此请继续下一次迭代以获得完整的 GC？
        // The GC invoked by the last thread leaving the critical
        // section will be a young collection and a full collection
        // is (currently) needed for unloading classes so continue
        // to the next iteration to get a full GC.
        // GC已经执行，重新循环尝试分配，GC执行完成后is_active_and_needs_gc会返回false
        continue;
      } else {
        if (CheckJNICalls) {
          fatal("Possible deadlock due to allocating while"
                " in jni critical section");
        }
        // 线程在关键区中不能GC，所以分配失败，返回NULL
        return NULL;
      }
    }

    // GC完成后下一次循环is_active_and_needs_gc返回false，就会进入此逻辑
    {  // Need lock to get self consistent gc_count's
      MutexLocker ml(Heap_lock); // 获取Heap_lock，读取GC的累计次数
      gc_count      = Universe::heap()->total_collections();
      full_gc_count = Universe::heap()->total_full_collections();
    }

    // 如果之前有线程处于关键区中，则通过VM_CollectForMetadataAllocation会执行内存分配
    // 如果之前没有线程处于关键区中，则通过VM_CollectForMetadataAllocation触发GC并在GC完成后分配内存
    // 在VM_CollectForMetadataAllocation 执行的时候会判断是否需要跳过此次GC，因为退出关键区的线程已经触发了GC，但是有可能短期内又没内存了需要再次GC
    // Generate a VM operation
    VM_CollectForMetadataAllocation op(loader_data,
                                       word_size,
                                       mdtype,
                                       gc_count,
                                       full_gc_count,
                                       GCCause::_metadata_GC_threshold);
    // 当前线程会被阻塞直到VM_CollectForMetadataAllocation被VMThread执行完成
    VMThread::execute(&op);

    // 如果GC被锁定，请重试。在检查成功之前进行检查，因为开场可能已经成功，并且GC仍然被锁定。
    // If GC was locked out, try again.  Check
    // before checking success because the prologue
    // could have succeeded and the GC still have
    // been locked out.
    // VM_CollectForMetadataAllocation已经执行完成，gc_locked为true说明此次GC后依然没有足够的内存，需要再次循环再次GC
    if (op.gc_locked()) {
      continue;
    }

    // gc_locked返回false，说明跳过了本次GC或者GC正常已完成内存分配
    // prologue_succeeded返回false表示会跳过此次GC，VM_CollectForMetadataAllocation中的doit方法不会被执行，即不会分配内存，返回true表示会执行内存分配，分配失败再执行GC
    if (op.prologue_succeeded()) {
      return op.result();
    }
    // prologue_succeeded返回false继续循环
    loop_count++;
    // QueuedAllocationWarningCount表示循环了多少次数后打印warning日志（-XX:QueuedAllocationWarningCount=N），默认值为0
    if ((QueuedAllocationWarningCount > 0) &&
        (loop_count % QueuedAllocationWarningCount == 0)) {
      warning("satisfy_failed_metadata_allocation() retries %d times \n\t"
              " size=%d", loop_count, word_size);
    }
  } while (true);  // Until a GC is done
}

// Return true if any of the following is true:
// . the allocation won't fit into the current young gen heap
// . gc locker is occupied (jni critical section)
// . heap memory is tight -- the most recent previous collection
//   was a full collection because a partial collection (would
//   have) failed and is likely to fail again
// 如果满足以下任一条件，则返回 true：
// - 不适合分配在当前的年轻代
// - GC锁被占用（JNI关键区）
// - 堆内存很紧张。（最近的上一个堆满了，因为一部分堆失败了并且会再次失败）？？
bool GenCollectorPolicy::should_try_older_generation_allocation(
        size_t word_size) const {
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  // 获取年轻代的当前可用空间
  size_t gen0_capacity = gch->get_gen(0)->capacity_before_gc();
  return    (word_size > heap_word_size(gen0_capacity))
         || GC_locker::is_active_and_needs_gc()
         || gch->incremental_collection_failed();
}


//
// MarkSweepPolicy methods
//

void MarkSweepPolicy::initialize_alignments() {
  _space_alignment = _gen_alignment = (uintx)Generation::GenGrain;
  _heap_alignment = compute_heap_alignment();
}

void MarkSweepPolicy::initialize_generations() {
  _generations = NEW_C_HEAP_ARRAY3(GenerationSpecPtr, number_of_generations(), mtGC, 0, AllocFailStrategy::RETURN_NULL);
  if (_generations == NULL) {
    vm_exit_during_initialization("Unable to allocate gen spec");
  }

  if (UseParNewGC) {
    _generations[0] = new GenerationSpec(Generation::ParNew, _initial_gen0_size, _max_gen0_size);
  } else {
    _generations[0] = new GenerationSpec(Generation::DefNew, _initial_gen0_size, _max_gen0_size);
  }
  _generations[1] = new GenerationSpec(Generation::MarkSweepCompact, _initial_gen1_size, _max_gen1_size);

  if (_generations[0] == NULL || _generations[1] == NULL) {
    vm_exit_during_initialization("Unable to allocate gen spec");
  }
}

void MarkSweepPolicy::initialize_gc_policy_counters() {
  // initialize the policy counters - 2 collectors, 3 generations
  if (UseParNewGC) {
    _gc_policy_counters = new GCPolicyCounters("ParNew:MSC", 2, 3);
  } else {
    _gc_policy_counters = new GCPolicyCounters("Copy:MSC", 2, 3);
  }
}
