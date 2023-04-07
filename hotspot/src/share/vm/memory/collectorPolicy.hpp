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

#ifndef SHARE_VM_MEMORY_COLLECTORPOLICY_HPP
#define SHARE_VM_MEMORY_COLLECTORPOLICY_HPP

#include "memory/allocation.hpp"
#include "memory/barrierSet.hpp"
#include "memory/generationSpec.hpp"
#include "memory/genRemSet.hpp"
#include "utilities/macros.hpp"

// This class (or more correctly, subtypes of this class)
// are used to define global garbage collector attributes.
// This includes initialization of generations and any other
// shared resources they may need.
//
// In general, all flag adjustment and validation should be
// done in initialize_flags(), which is called prior to
// initialize_size_info().
//
// This class is not fully developed yet. As more collector(s)
// are added, it is expected that we will come across further
// behavior that requires global attention. The correct place
// to deal with those issues is this class.

// Forward declarations.
class GenCollectorPolicy;
class TwoGenerationCollectorPolicy;
class AdaptiveSizePolicy;
#if INCLUDE_ALL_GCS
class ConcurrentMarkSweepPolicy;
class G1CollectorPolicy;
#endif // INCLUDE_ALL_GCS

class GCPolicyCounters;
class MarkSweepPolicy;

// 该类及其子类用于定义垃圾回收器使用的全局属性，并初始化分代内存及其他共享资源。
// 用于根据虚拟机启动的参数分配heap堆的大小，以及将heap堆分成大小不同的区（比如年轻代和老年代），并且对不同的区定义不同的Generation的规范。
class CollectorPolicy : public CHeapObj<mtGC> {
 protected:
  GCPolicyCounters* _gc_policy_counters;

  virtual void initialize_alignments() = 0;
  // 参数校验等
  virtual void initialize_flags();
  virtual void initialize_size_info();

  DEBUG_ONLY(virtual void assert_flags();)
  DEBUG_ONLY(virtual void assert_size_info();)

  size_t _initial_heap_byte_size;                                           // 初始堆内存
  size_t _max_heap_byte_size;                                               // 最大堆内存
  size_t _min_heap_byte_size;                                               // 最小堆内存

  size_t _space_alignment;                                                  // space分配粒度
  size_t _heap_alignment;                                                   // heap分配粒度，_heap_alignment必须大于_space_alignment，且是_space_alignment的整数倍

  // Needed to keep information if MaxHeapSize was set on the command line
  // when the flag value is aligned etc by ergonomics
  bool _max_heap_size_cmdline;                                              // 是否通过命令行参数设置了最大堆内存

  // The sizing of the heap are controlled by a sizing policy.
  AdaptiveSizePolicy* _size_policy;                                         // 用来自适应调整堆内存大小的策略实现

  // Set to true when policy wants soft refs cleared.
  // Reset to false by gc after it clears all soft refs.
  bool _should_clear_all_soft_refs;                                         // 是否需要清除所有的软引用，当软引用清除结束，垃圾回收器会将其置为false

  // Set to true by the GC if the just-completed gc cleared all
  // softrefs.  This is set to true whenever a gc clears all softrefs, and
  // set to false each time gc returns to the mutator.  For example, in the
  // ParallelScavengeHeap case the latter would be done toward the end of
  // mem_allocate() where it returns op.result()
  bool _all_soft_refs_clear;                                                // 当GC刚清除完所有的软引用时会设置该属性为true，当返回mutator时被设置成false

  // 构造函数
  CollectorPolicy();

 public:
  virtual void initialize_all() {
    // 下面的三个方法均是虚函数，所以需要从实际的策略对象来看。
    // 如果使用了MarkSweepPolicy，我们就需要去这个类中查看这三个方法的实现。

    // 用来初始化分代内存及内存分配相关属性的，没有默认实现
    initialize_alignments();
    // 主要用于校验参数的合法性，并设置相关参数
    initialize_flags();
    initialize_size_info();
  }

  // Return maximum heap alignment that may be imposed by the policy
  static size_t compute_heap_alignment();

  size_t space_alignment()        { return _space_alignment; }
  size_t heap_alignment()         { return _heap_alignment; }

  size_t initial_heap_byte_size() { return _initial_heap_byte_size; }
  size_t max_heap_byte_size()     { return _max_heap_byte_size; }
  size_t min_heap_byte_size()     { return _min_heap_byte_size; }

  enum Name {
    CollectorPolicyKind,
    TwoGenerationCollectorPolicyKind,
    ConcurrentMarkSweepPolicyKind,
    ASConcurrentMarkSweepPolicyKind,
    G1CollectorPolicyKind
  };

  AdaptiveSizePolicy* size_policy() { return _size_policy; }
  bool should_clear_all_soft_refs() { return _should_clear_all_soft_refs; }
  void set_should_clear_all_soft_refs(bool v) { _should_clear_all_soft_refs = v; }
  // Returns the current value of _should_clear_all_soft_refs.
  // _should_clear_all_soft_refs is set to false as a side effect.
  bool use_should_clear_all_soft_refs(bool v);
  bool all_soft_refs_clear() { return _all_soft_refs_clear; }
  void set_all_soft_refs_clear(bool v) { _all_soft_refs_clear = v; }

  // Called by the GC after Soft Refs have been cleared to indicate
  // that the request in _should_clear_all_soft_refs has been fulfilled.
  void cleared_all_soft_refs();

  // Identification methods.
  virtual GenCollectorPolicy*           as_generation_policy()            { return NULL; }
  virtual TwoGenerationCollectorPolicy* as_two_generation_policy()        { return NULL; }
  virtual MarkSweepPolicy*              as_mark_sweep_policy()            { return NULL; }
#if INCLUDE_ALL_GCS
  virtual ConcurrentMarkSweepPolicy*    as_concurrent_mark_sweep_policy() { return NULL; }
  virtual G1CollectorPolicy*            as_g1_policy()                    { return NULL; }
#endif // INCLUDE_ALL_GCS
  // Note that these are not virtual.
  bool is_generation_policy()            { return as_generation_policy() != NULL; }
  bool is_two_generation_policy()        { return as_two_generation_policy() != NULL; }
  bool is_mark_sweep_policy()            { return as_mark_sweep_policy() != NULL; }
#if INCLUDE_ALL_GCS
  bool is_concurrent_mark_sweep_policy() { return as_concurrent_mark_sweep_policy() != NULL; }
  bool is_g1_policy()                    { return as_g1_policy() != NULL; }
#else  // INCLUDE_ALL_GCS
  bool is_concurrent_mark_sweep_policy() { return false; }
  bool is_g1_policy()                    { return false; }
#endif // INCLUDE_ALL_GCS


  virtual BarrierSet::Name barrier_set_name() = 0;

  // Create the remembered set (to cover the given reserved region,
  // allowing breaking up into at most "max_covered_regions").
  virtual GenRemSet* create_rem_set(MemRegion reserved,
                                    int max_covered_regions);

  // This method controls how a collector satisfies a request
  // for a block of memory.  "gc_time_limit_was_exceeded" will
  // be set to true if the adaptive size policy determine that
  // an excessive amount of time is being spent doing collections
  // and caused a NULL to be returned.  If a NULL is not returned,
  // "gc_time_limit_was_exceeded" has an undefined meaning.
  virtual HeapWord* mem_allocate_work(size_t size,
                                      bool is_tlab,
                                      bool* gc_overhead_limit_was_exceeded) = 0;

  // This method controls how a collector handles one or more
  // of its generations being fully allocated.
  virtual HeapWord *satisfy_failed_allocation(size_t size, bool is_tlab) = 0;
  // This method controls how a collector handles a metadata allocation
  // failure.
  // 当Metaspace分配内存失败后调用的，用来清理Metaspace空间并尝试重新分配的
  virtual MetaWord* satisfy_failed_metadata_allocation(ClassLoaderData* loader_data,
                                                       size_t size,
                                                       Metaspace::MetadataType mdtype);

  // Performace Counter support
  GCPolicyCounters* counters()     { return _gc_policy_counters; }

  // Create the jstat counters for the GC policy.  By default, policy's
  // don't have associated counters, and we complain if this is invoked.
  virtual void initialize_gc_policy_counters() {
    ShouldNotReachHere();
  }

  virtual CollectorPolicy::Name kind() {
    return CollectorPolicy::CollectorPolicyKind;
  }

  // Returns true if a collector has eden space with soft end.
  virtual bool has_soft_ended_eden() {
    return false;
  }

  // Do any updates required to global flags that are due to heap initialization
  // changes
  virtual void post_heap_initialize() = 0;
};

class ClearedAllSoftRefs : public StackObj {
  bool _clear_all_soft_refs;
  CollectorPolicy* _collector_policy;
 public:
  ClearedAllSoftRefs(bool clear_all_soft_refs,
                     CollectorPolicy* collector_policy) :
    _clear_all_soft_refs(clear_all_soft_refs),
    _collector_policy(collector_policy) {}

  ~ClearedAllSoftRefs() {
    if (_clear_all_soft_refs) {
      _collector_policy->cleared_all_soft_refs();
    }
  }
};

// GenCollectorPolicy继承自CollectorPolicy，表示分代内存使用的CollectorPolicy
class GenCollectorPolicy : public CollectorPolicy {
 protected:
  // 保存分代中第一个代的最小值、初始值和最大值，任何分代堆都至少会有一个代。如果有多个代，则当前表示的是最年轻的代。
  size_t _min_gen0_size;            // gen0的内存最小值
  size_t _initial_gen0_size;        // gen0的内存初始值
  size_t _max_gen0_size;            // gen0的内存最大值

  // _gen_alignment and _space_alignment will have the same value most of the
  // time. When using large pages they can differ.
  // _gen_alignment一般和_space_alignment相同，MarkSweepPolicy::initialize_alignments()函数也是将两个属性赋予了相同的值
  size_t _gen_alignment;            // 分代内存分配粒度，_gen_alignment必须被_space_alignment整除，- _heap_alignment被_gen_alignment整除

  GenerationSpec **_generations;    // 一种特殊的Generation实现

  // Return true if an allocation should be attempted in the older
  // generation if it fails in the younger generation.  Return
  // false, otherwise.
  // 如果在年轻一代中失败，则应在老一代中尝试分配，则返回 true。 否则返回 false。
  virtual bool should_try_older_generation_allocation(size_t word_size) const;

  // 参数合法性校验
  void initialize_flags();
  void initialize_size_info();

  DEBUG_ONLY(void assert_flags();)
  DEBUG_ONLY(void assert_size_info();)

  // Try to allocate space by expanding the heap.
  virtual HeapWord* expand_heap_and_allocate(size_t size, bool is_tlab);

  // Compute max heap alignment
  size_t compute_max_alignment();

 // Scale the base_size by NewRatio according to
 //     result = base_size / (NewRatio + 1)
 // and align by min_alignment()
 size_t scale_by_NewRatio_aligned(size_t base_size);

 // Bound the value by the given maximum minus the min_alignment
 size_t bound_minus_alignment(size_t desired_size, size_t maximum_size);

 public:
  GenCollectorPolicy();

  // Accessors
  size_t min_gen0_size()     { return _min_gen0_size; }
  size_t initial_gen0_size() { return _initial_gen0_size; }
  size_t max_gen0_size()     { return _max_gen0_size; }
  size_t gen_alignment()     { return _gen_alignment; }

  virtual int number_of_generations() = 0;

  virtual GenerationSpec **generations() {
    assert(_generations != NULL, "Sanity check");
    return _generations;
  }

  virtual GenCollectorPolicy* as_generation_policy() { return this; }

  virtual void initialize_generations() { };

  // 初始化堆，内存等信息
  virtual void initialize_all() {
    // 初始化一些属性，尤其是与内存大小相关的一些属性
    CollectorPolicy::initialize_all();
    // 初始化堆
    initialize_generations();
  }

  // 返回年轻代的内存最小值
  size_t young_gen_size_lower_bound();

  // 用于从Java堆中分配指定大小的内存块，并在必要时触发GC
  HeapWord* mem_allocate_work(size_t size,
                              bool is_tlab,
                              bool* gc_overhead_limit_was_exceeded);

  HeapWord *satisfy_failed_allocation(size_t size, bool is_tlab);

  // Adaptive size policy
  virtual void initialize_size_policy(size_t init_eden_size,
                                      size_t init_promo_size,
                                      size_t init_survivor_size);

  virtual void post_heap_initialize() {
    assert(_max_gen0_size == MaxNewSize, "Should be taken care of by initialize_size_info");
  }
};

// All of hotspot's current collectors are subtypes of this
// class. Currently, these collectors all use the same gen[0],
// but have different gen[1] types. If we add another subtype
// of CollectorPolicy, this class should be broken out into
// its own file.

// TwoGenerationCollectorPolicy中声明的变量主要是保存老年代的最小值、初始值和最大值。
class TwoGenerationCollectorPolicy : public GenCollectorPolicy {
 protected:
  size_t _min_gen1_size;
  size_t _initial_gen1_size;
  size_t _max_gen1_size;

  void initialize_flags();

  // 调用各个类的initialize_size_info()函数确定年轻代与老年代内存的最小值、初始值和最大值，在调用该函数之前，由于已经调用过initialize_flags()函数，
  // 整个堆的最小值、初始值和最大值已经确定，所以代的内存计算必须要限制在堆的当前内存空间之内。
  // TwoGenerationCollectorPolicy::initialize_size_info -> GenCollectorPolicy::initialize_size_info -> CollectorPolicy::initialize_size_info
  // CollectorPolicy::initialize_size_info 是空的，只有打印信息。所以主要的逻辑在其两个子类中。
  void initialize_size_info();

  DEBUG_ONLY(void assert_flags();)
  DEBUG_ONLY(void assert_size_info();)

 public:
  TwoGenerationCollectorPolicy() : GenCollectorPolicy(), _min_gen1_size(0),
    _initial_gen1_size(0), _max_gen1_size(0) {}

  // Accessors
  size_t min_gen1_size()     { return _min_gen1_size; }
  size_t initial_gen1_size() { return _initial_gen1_size; }
  size_t max_gen1_size()     { return _max_gen1_size; }

  // Inherited methods
  TwoGenerationCollectorPolicy* as_two_generation_policy() { return this; }

  int number_of_generations()          { return 2; }
  BarrierSet::Name barrier_set_name()  { return BarrierSet::CardTableModRef; }

  virtual CollectorPolicy::Name kind() {
    return CollectorPolicy::TwoGenerationCollectorPolicyKind;
  }

  // Returns true if gen0 sizes were adjusted
  bool adjust_gen0_sizes(size_t* gen0_size_ptr, size_t* gen1_size_ptr,
                         const size_t heap_size);
};

class MarkSweepPolicy : public TwoGenerationCollectorPolicy {
 protected:
  void initialize_alignments();
  void initialize_generations();

 public:
  MarkSweepPolicy() {}

  MarkSweepPolicy* as_mark_sweep_policy() { return this; }

  void initialize_gc_policy_counters();
};

#endif // SHARE_VM_MEMORY_COLLECTORPOLICY_HPP
