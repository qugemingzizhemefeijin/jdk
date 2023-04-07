/*
 * Copyright (c) 1999, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_THREADLOCALALLOCBUFFER_HPP
#define SHARE_VM_MEMORY_THREADLOCALALLOCBUFFER_HPP

#include "gc_implementation/shared/gcUtil.hpp"
#include "oops/typeArrayOop.hpp"
#include "runtime/perfData.hpp"

class GlobalTLABStats;

// ThreadLocalAllocBuffer: a descriptor for thread-local storage used by
// the threads for allocation.
//            It is thread-private at any time, but maybe multiplexed over
//            time across multiple threads. The park()/unpark() pair is
//            used to make it available for such multiplexing.

// ThreadLocalAllocBuffer 是 HotSpot 虚拟机中的一个重要数据结构，用于在线程本地堆（Thread Local Heap）上快速分配对象，提高分配速度和效率。
//
// 线程本地堆是 HotSpot 虚拟机的一个特性，是指每个线程都有一个本地堆，用于分配较小的对象。
// 对于小对象的分配，线程本地堆能够更快地完成内存分配和回收操作，避免了全局堆的锁竞争和内存碎片化问题，从而提高了系统的性能。

// ThreadLocalAllocBuffer 是线程本地堆的内部实现，其中包含了一个指针 top，用于指向当前线程本地堆中可用的内存位置。
// 当线程需要分配对象时，会从 top 指针处开始分配一段连续的内存，同时将 top 指针向后移动，指向下一块可用的内存位置。
// 当 top 指针达到线程本地堆的边界时，ThreadLocalAllocBuffer 会申请一块新的内存区域，作为线程本地堆的扩展。

// 需要注意的是，线程本地堆只用于分配较小的对象。对于较大的对象，仍然需要使用全局堆进行分配。
// HotSpot 虚拟机根据对象的大小来选择在线程本地堆还是全局堆中进行内存分配，以充分利用两者的优点。

// ThreadLocalAllocBuffer类中并没有专门用于释放内存的函数。因为它管理的是线程本地分配缓存（TLAB），它的生命周期是随着线程的生命周期而自动管理的。
// 当线程被销毁时，其TLAB中的内存将会被自动释放，这是由JVM内部的垃圾回收机制完成的。

// 通常，TLAB的最大值不会大于int[Integer.MAX_VALUE]，因为在分配新的TLAB时，原TLAB中未分配给对象的剩余内存需要填充整数类型的数组，
// 这个被填充的数组叫作dummy object，这样内存空间就是可解析的，非常有利于提高GC的扫描效率。为了一定能有填充dummy object的空间，
// TLAB一般会预留一个dummy object的header的空间，也是一个int[]的header，所以TLAB的值不能超过int数组的最大值，
// 否则无法用dummy object填满未使用的空间。

// 在理想状态下，线程的所有对象分配都希望在TLAB中进行，当线程进行了refill次新TLAB分配行为后，正好占满Eden空间。
// 不过实际情况是，总会有内存被填充了dummy object而造成浪费，另外，当GC发生时，无论当前正在进行对象分配的TLAB剩余空闲有多大，
// 都会被填充，每次分配的比例如何确定呢？从概率上说，GC可能会在任何时候发生，即对于某一时刻而言，GC发生的概率为0.5。
// 如果按照TLABWasteTargetPercent的默认设定，说明浪费的内存不超过1%，那么在0.5的概率时浪费的情况下TLAB分配的比例是多少才让浪费的期望值小于1%呢？
// 答案是2%的比例。

// 在ThreadLocalAllocBuffer::initialize()函数中调用initial_refill_waste_limit()函数计算_refill_waste_limit的初始值，
// 计算出来后通过ThreadLocalAllocBuffer类中的_refill_waste_limit变量保存。

// 当请求对象大于_refill_waste_limit时，会选择在堆中分配，若小于该值，则会废弃当前TLAB，新建TLAB来分配对象。
// 这个阈值可以使用-XX:TLABRefillWasteFraction命令来调整，表示TLAB中允许产生这种浪费的比例。
// 默认值为64，即表示使用约为1/64的TLAB空间作为_refill_waste_limit。
// 默认情况下，TLAB和_refill_waste_limit都会在运行时不断调整，使系统的运行状态达到最优。

// TLAB本身占用eEden区空间，在开启TLAB的情况下，虚拟机会为每个Java线程分配一块TLAB空间。
// 参数-XX:+UseTLAB开启TLAB，默认是开启的。TLAB空间的内存非常小，缺省情况下仅占有整个Eden空间的1%，当然可以通过选项-XX:TLABWasteTargetPercent设置TLAB空间所占用Eden空间的百分比大小。

// 由于TLAB空间一般不会很大，因此大对象无法在TLAB上进行分配，总是会直接分配在堆上。
// TLAB空间由于比较小，因此很容易装满。比如，一个100K的空间，已经使用了80KB，当需要再分配一个30KB的对象时，肯定就无能为力了。
// 这时虚拟机会有两种选择，第一，废弃当前TLAB，这样就会浪费20KB空间；第二，将这30KB的对象直接分配在堆上，保留当前的TLAB，
// 这样可以希望将来有小于20KB的对象分配请求可以直接使用这块空间。实际上虚拟机内部会维护一个叫作refill_waste的值，
// 当请求对象大于refill_waste时，会选择在堆中分配，若小于该值，则会废弃当前TLAB，新建TLAB来分配对象。
// 这个阈值可以使用TLABRefillWasteFraction来调整，它表示TLAB中允许产生这种浪费的比例。默认值为64，即表示使用约为1/64的TLAB空间作为refill_waste。
// 默认情况下，TLAB和refill_waste都会在运行时不断调整的，使系统的运行状态达到最优。如果想要禁用自动调整TLAB的大小，
// 可以使用-XX:-ResizeTLAB禁用ResizeTLAB，并使用-XX:TLABSize手工指定一个TLAB的大小。

// -XX:+PrintTLAB可以跟踪TLAB的使用情况。一般不建议手工修改TLAB相关参数，推荐使用虚拟机默认行为。
class ThreadLocalAllocBuffer: public CHeapObj<mtThread> {
  friend class VMStructs;
private:
  // _start和_top属性用于跟踪已分配内存的位置，_desired_size和_refill_waste_limit属性用于控制TLAB的大小和重填行为，
  // _gc_waste和_slow_allocations属性用于评估GC性能，_allocation_fraction属性用于跟踪Eden中分配给TLAB的比例。

  HeapWord* _start;                              // address of TLAB                             // TLAB的起始地址
  HeapWord* _top;                                // address after last allocation               // 当前未分配空间的顶部地址
  HeapWord* _pf_top;                             // allocation prefetch watermark               // 预取分配的水印地址
  HeapWord* _end;                                // allocation end (excluding alignment_reserve)// 当前分配空间的结尾地址
  size_t    _desired_size;                       // desired size   (including alignment_reserve)// 所需的TLAB大小（包括对齐保留区域）
  size_t    _refill_waste_limit;                 // hold onto tlab if free() is larger than this// 如果free（）大于此值，则保留TLAB

  static unsigned _target_refills;               // expected number of refills between GCs      // 预期在GC之间重新填充的次数

  unsigned  _number_of_refills;                                                                 // 已经重新填充的次数
  unsigned  _fast_refill_waste;                                                                 // 快速重填的浪费字节数
  unsigned  _slow_refill_waste;                                                                 // 慢速重填的浪费字节数
  unsigned  _gc_waste;                                                                          // GC期间废弃的字节数
  unsigned  _slow_allocations;                                                                  // 慢速分配的次数

  AdaptiveWeightedAverage _allocation_fraction;  // fraction of eden allocated in tlabs         // Eden中分配给TLAB的比例

  void accumulate_statistics();
  void initialize_statistics();

  void set_start(HeapWord* start)                { _start = start; }
  void set_end(HeapWord* end)                    { _end = end; }
  void set_top(HeapWord* top)                    { _top = top; }
  void set_pf_top(HeapWord* pf_top)              { _pf_top = pf_top; }
  void set_desired_size(size_t desired_size)     { _desired_size = desired_size; }
  void set_refill_waste_limit(size_t waste)      { _refill_waste_limit = waste;  }

  size_t initial_refill_waste_limit()            { return desired_size() / TLABRefillWasteFraction; }

  static int    target_refills()                 { return _target_refills; }
  size_t initial_desired_size();

  size_t remaining() const                       { return end() == NULL ? 0 : pointer_delta(hard_end(), top()); }

  // Make parsable and release it.
  void reset();

  // Resize based on amount of allocation, etc.
  void resize();

  void invariants() const { assert(top() >= start() && top() <= end(), "invalid tlab"); }

  void initialize(HeapWord* start, HeapWord* top, HeapWord* end);

  void print_stats(const char* tag);

  Thread* myThread();

  // statistics

  int number_of_refills() const { return _number_of_refills; }
  int fast_refill_waste() const { return _fast_refill_waste; }
  int slow_refill_waste() const { return _slow_refill_waste; }
  int gc_waste() const          { return _gc_waste; }
  int slow_allocations() const  { return _slow_allocations; }

  static GlobalTLABStats* _global_stats;
  static GlobalTLABStats* global_stats() { return _global_stats; }

public:
  ThreadLocalAllocBuffer() : _allocation_fraction(TLABAllocationWeight) {
    // do nothing.  tlabs must be inited by initialize() calls
  }

  static const size_t min_size()                 { return align_object_size(MinTLABSize / HeapWordSize); }
  static const size_t max_size();

  HeapWord* start() const                        { return _start; }
  HeapWord* end() const                          { return _end; }
  HeapWord* hard_end() const                     { return _end + alignment_reserve(); }
  HeapWord* top() const                          { return _top; }
  HeapWord* pf_top() const                       { return _pf_top; }
  size_t desired_size() const                    { return _desired_size; }
  size_t used() const                            { return pointer_delta(top(), start()); }
  size_t used_bytes() const                      { return pointer_delta(top(), start(), 1); }
  size_t free() const                            { return pointer_delta(end(), top()); }
  // Don't discard tlab if remaining space is larger than this.
  size_t refill_waste_limit() const              { return _refill_waste_limit; }

  // Allocate size HeapWords. The memory is NOT initialized to zero.
  // 用于分配指定大小的内存。
  inline HeapWord* allocate(size_t size);

  // Reserve space at the end of TLAB
  static size_t end_reserve() {
    int reserve_size = typeArrayOopDesc::header_size(T_INT);
    return MAX2(reserve_size, VM_Version::reserve_for_allocation_prefetch());
  }
  static size_t alignment_reserve()              { return align_object_size(end_reserve()); }
  static size_t alignment_reserve_in_bytes()     { return alignment_reserve() * HeapWordSize; }

  // Return tlab size or remaining space in eden such that the
  // space is large enough to hold obj_size and necessary fill space.
  // Otherwise return 0;
  inline size_t compute_size(size_t obj_size);

  // Record slow allocation
  inline void record_slow_allocation(size_t obj_size);

  // Initialization at startup // TLAB初始化
  static void startup_initialization();

  // Make an in-use tlab parsable, optionally also retiring it.
  void make_parsable(bool retire);

  // Retire in-use tlab before allocation of a new tlab
  void clear_before_allocation();

  // Accumulate statistics across all tlabs before gc
  static void accumulate_statistics_before_gc();

  // Resize tlabs for all threads
  static void resize_all_tlabs();

  void fill(HeapWord* start, HeapWord* top, size_t new_size);
  void initialize();

  static size_t refill_waste_limit_increment()   { return TLABWasteIncrement; }

  // Code generation support
  static ByteSize start_offset()                 { return byte_offset_of(ThreadLocalAllocBuffer, _start); }
  static ByteSize end_offset()                   { return byte_offset_of(ThreadLocalAllocBuffer, _end  ); }
  static ByteSize top_offset()                   { return byte_offset_of(ThreadLocalAllocBuffer, _top  ); }
  static ByteSize pf_top_offset()                { return byte_offset_of(ThreadLocalAllocBuffer, _pf_top  ); }
  static ByteSize size_offset()                  { return byte_offset_of(ThreadLocalAllocBuffer, _desired_size ); }
  static ByteSize refill_waste_limit_offset()    { return byte_offset_of(ThreadLocalAllocBuffer, _refill_waste_limit ); }

  static ByteSize number_of_refills_offset()     { return byte_offset_of(ThreadLocalAllocBuffer, _number_of_refills ); }
  static ByteSize fast_refill_waste_offset()     { return byte_offset_of(ThreadLocalAllocBuffer, _fast_refill_waste ); }
  static ByteSize slow_allocations_offset()      { return byte_offset_of(ThreadLocalAllocBuffer, _slow_allocations ); }

  void verify();
};

class GlobalTLABStats: public CHeapObj<mtThread> {
private:

  // Accumulate perfdata in private variables because
  // PerfData should be write-only for security reasons
  // (see perfData.hpp)
  unsigned _allocating_threads;
  unsigned _total_refills;
  unsigned _max_refills;
  size_t   _total_allocation;
  size_t   _total_gc_waste;
  size_t   _max_gc_waste;
  size_t   _total_slow_refill_waste;
  size_t   _max_slow_refill_waste;
  size_t   _total_fast_refill_waste;
  size_t   _max_fast_refill_waste;
  unsigned _total_slow_allocations;
  unsigned _max_slow_allocations;

  PerfVariable* _perf_allocating_threads;
  PerfVariable* _perf_total_refills;
  PerfVariable* _perf_max_refills;
  PerfVariable* _perf_allocation;
  PerfVariable* _perf_gc_waste;
  PerfVariable* _perf_max_gc_waste;
  PerfVariable* _perf_slow_refill_waste;
  PerfVariable* _perf_max_slow_refill_waste;
  PerfVariable* _perf_fast_refill_waste;
  PerfVariable* _perf_max_fast_refill_waste;
  PerfVariable* _perf_slow_allocations;
  PerfVariable* _perf_max_slow_allocations;

  AdaptiveWeightedAverage _allocating_threads_avg;

public:
  GlobalTLABStats();

  // Initialize all counters
  void initialize();

  // Write all perf counters to the perf_counters
  void publish();

  void print();

  // Accessors
  unsigned allocating_threads_avg() {
    return MAX2((unsigned)(_allocating_threads_avg.average() + 0.5), 1U);
  }

  size_t allocation() {
    return _total_allocation;
  }

  // Update methods

  void update_allocating_threads() {
    _allocating_threads++;
  }
  void update_number_of_refills(unsigned value) {
    _total_refills += value;
    _max_refills    = MAX2(_max_refills, value);
  }
  void update_allocation(size_t value) {
    _total_allocation += value;
  }
  void update_gc_waste(size_t value) {
    _total_gc_waste += value;
    _max_gc_waste    = MAX2(_max_gc_waste, value);
  }
  void update_fast_refill_waste(size_t value) {
    _total_fast_refill_waste += value;
    _max_fast_refill_waste    = MAX2(_max_fast_refill_waste, value);
  }
  void update_slow_refill_waste(size_t value) {
    _total_slow_refill_waste += value;
    _max_slow_refill_waste    = MAX2(_max_slow_refill_waste, value);
  }
  void update_slow_allocations(unsigned value) {
    _total_slow_allocations += value;
    _max_slow_allocations    = MAX2(_max_slow_allocations, value);
  }
};

#endif // SHARE_VM_MEMORY_THREADLOCALALLOCBUFFER_HPP
