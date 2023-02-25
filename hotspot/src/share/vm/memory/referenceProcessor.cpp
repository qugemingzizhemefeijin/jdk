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
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "gc_interface/collectedHeap.inline.hpp"
#include "memory/referencePolicy.hpp"
#include "memory/referenceProcessor.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/jniHandles.hpp"

ReferencePolicy* ReferenceProcessor::_always_clear_soft_ref_policy = NULL;
ReferencePolicy* ReferenceProcessor::_default_soft_ref_policy      = NULL;
bool             ReferenceProcessor::_pending_list_uses_discovered_field = false;
jlong            ReferenceProcessor::_soft_ref_timestamp_clock = 0;

void referenceProcessor_init() {
  ReferenceProcessor::init_statics();
}

// 初始化引用类型处理的相关变量
void ReferenceProcessor::init_statics() {
  // We need a monotonically non-deccreasing time in ms but
  // os::javaTimeMillis() does not guarantee monotonicity.
  jlong now = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;

  // Initialize the soft ref timestamp clock.
  // 这个变量会参与决定每次GC操作时软引用是否需要被回收
  // 时间戳时钟，由垃圾收集器更新（此戳的功能就是用于更新软引用的timestamp，用于软引用对象的referent置空）
  _soft_ref_timestamp_clock = now;
  // Also update the soft ref clock in j.l.r.SoftReference
  // 更新时间戳（此戳单位是毫秒）
  java_lang_ref_SoftReference::set_clock(_soft_ref_timestamp_clock);
  // 影响软引用的回收策略未 JVM参数 SoftRefLRUPolicyMSPerMB ，默认值1000，字面意思是 每MB的软引用存活时间为1秒，
  // 当 SoftRefLRUPolicyMSPerMB = 0的时候，则软引用经历一次GC即被回收。

  // 默认初始化的引用回收策略为总是回收
  _always_clear_soft_ref_policy = new AlwaysClearPolicy();
  // 初始化软引用处理的策略，COMPILER2_PRESENT为Server端策略，NOT_COMPILER2为Client端策略
  // 默认初始化的软引用回收策略为最近最少使用的被回收 LRUMaxHeapPolicy
  _default_soft_ref_policy      = new COMPILER2_PRESENT(LRUMaxHeapPolicy())
                                      NOT_COMPILER2(LRUCurrentHeapPolicy());
  if (_always_clear_soft_ref_policy == NULL || _default_soft_ref_policy == NULL) {
    vm_exit_during_initialization("Could not allocate reference policy object");
  }
  guarantee(RefDiscoveryPolicy == ReferenceBasedDiscovery ||
            RefDiscoveryPolicy == ReferentBasedDiscovery,
            "Unrecongnized RefDiscoveryPolicy");
  _pending_list_uses_discovered_field = JDK_Version::current().pending_list_uses_discovered_field();
}

void ReferenceProcessor::enable_discovery(bool verify_disabled, bool check_no_refs) {
#ifdef ASSERT
  // Verify that we're not currently discovering refs
  assert(!verify_disabled || !_discovering_refs, "nested call?");

  if (check_no_refs) {
    // Verify that the discovered lists are empty
    verify_no_references_recorded();
  }
#endif // ASSERT

  // Someone could have modified the value of the static
  // field in the j.l.r.SoftReference class that holds the
  // soft reference timestamp clock using reflection or
  // Unsafe between GCs. Unconditionally update the static
  // field in ReferenceProcessor here so that we use the new
  // value during reference discovery.

  _soft_ref_timestamp_clock = java_lang_ref_SoftReference::clock();
  _discovering_refs = true;
}

// 初始化引用处理器ReferenceProcessor中的静态变量。 年轻代和老年代都有自己的引用处理器ReferenceProcessor实例， 在HotSpot VM初始化的过程中就会创建
// 如：hotspot/src/share/vm/memory/generation.cpp 的 Generation::ref_processor_init() 方法
// 以下代码中初始化了4个DiscoveredList列表，用来存放不同的引用类型。
// 查找到的引用类型最终都会通过discovered连接成单链表，然后让pending变量指向链表头后形成PendingList列表。
ReferenceProcessor::ReferenceProcessor(MemRegion span,
                                       bool      mt_processing,
                                       uint      mt_processing_degree,
                                       bool      mt_discovery,
                                       uint      mt_discovery_degree,
                                       bool      atomic_discovery,
                                       BoolObjectClosure* is_alive_non_header,
                                       bool      discovered_list_needs_barrier)  :
  _discovering_refs(false),
  _enqueuing_is_done(false),
  _is_alive_non_header(is_alive_non_header),
  _discovered_list_needs_barrier(discovered_list_needs_barrier),
  _bs(NULL),
  _processing_is_mt(mt_processing),
  _next_id(0)
{
  // 年轻代或老年代的整个地址使用范围
  _span = span;
  // 对于单线程收集器Serial/Serial来说， 以下变量的值为true
  _discovery_is_atomic = atomic_discovery;
  // 对于单线程收集器Serial/Serial来说， 以下变量的值为false， 表示不会以并行方式查找引用类型
  _discovery_is_mt     = mt_discovery;
  // 对于单线程收集器Serial/Serial来说， 以下两个变量的值都为1
  _num_q               = MAX2(1U, mt_processing_degree);
  _max_num_q           = MAX2(_num_q, mt_discovery_degree);
  // number_of_subclasses_of_ref()函数的值为4，表示初始化一个类型为DiscoveredList的数组，
  // 数组的大小为4，为了使用方便，数组下标0～3位置存储的DiscoveredList
  // 分别由 _discoveredSoftRefs 和 _discoveredWeakRefs等引用。
  _discovered_refs     = NEW_C_HEAP_ARRAY(DiscoveredList,
            _max_num_q * number_of_subclasses_of_ref(), mtGC);

  if (_discovered_refs == NULL) {
    vm_exit_during_initialization("Could not allocated RefProc Array");
  }
  _discoveredSoftRefs    = &_discovered_refs[0];
  _discoveredWeakRefs    = &_discoveredSoftRefs[_max_num_q];
  _discoveredFinalRefs   = &_discoveredWeakRefs[_max_num_q];
  _discoveredPhantomRefs = &_discoveredFinalRefs[_max_num_q];

  // Initialize all entries to NULL
  // 对数组中的每个列表初始化
  for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
    _discovered_refs[i].set_head(NULL);
    _discovered_refs[i].set_length(0);
  }

  // If we do barriers, cache a copy of the barrier set.
  if (discovered_list_needs_barrier) {
    _bs = Universe::heap()->barrier_set();
  }
  // 对软引用的回收策略进行设置
  setup_policy(false /* default soft ref policy */);
}

#ifndef PRODUCT
void ReferenceProcessor::verify_no_references_recorded() {
  guarantee(!_discovering_refs, "Discovering refs?");
  for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
    guarantee(_discovered_refs[i].is_empty(),
              "Found non-empty discovered list");
  }
}
#endif

void ReferenceProcessor::weak_oops_do(OopClosure* f) {
  for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
    if (UseCompressedOops) {
      f->do_oop((narrowOop*)_discovered_refs[i].adr_head());
    } else {
      f->do_oop((oop*)_discovered_refs[i].adr_head());
    }
  }
}

void ReferenceProcessor::update_soft_ref_master_clock() {
  // Update (advance) the soft ref master clock field. This must be done
  // after processing the soft ref list.

  // We need a monotonically non-deccreasing time in ms but
  // os::javaTimeMillis() does not guarantee monotonicity.
  jlong now = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;
  jlong soft_ref_clock = java_lang_ref_SoftReference::clock();
  assert(soft_ref_clock == _soft_ref_timestamp_clock, "soft ref clocks out of sync");

  NOT_PRODUCT(
  if (now < _soft_ref_timestamp_clock) {
    warning("time warp: "INT64_FORMAT" to "INT64_FORMAT,
            _soft_ref_timestamp_clock, now);
  }
  )
  // The values of now and _soft_ref_timestamp_clock are set using
  // javaTimeNanos(), which is guaranteed to be monotonically
  // non-decreasing provided the underlying platform provides such
  // a time source (and it is bug free).
  // In product mode, however, protect ourselves from non-monotonicty.
  if (now > _soft_ref_timestamp_clock) {
    // 更新_soft_ref_timestamp_clock属性的值为当前时间并将当前的时间保存到SoftReference.clock中
    _soft_ref_timestamp_clock = now;
    java_lang_ref_SoftReference::set_clock(now);
  }
  // Else leave clock stalled at its old value until time progresses
  // past clock value.
}

size_t ReferenceProcessor::total_count(DiscoveredList lists[]) {
  size_t total = 0;
  for (uint i = 0; i < _max_num_q; ++i) {
    total += lists[i].length();
  }
  return total;
}

// 处理引用对象（包括软引用、 弱引用、 虚引用和最终引用）
ReferenceProcessorStats ReferenceProcessor::process_discovered_references(
  BoolObjectClosure*           is_alive,
  OopClosure*                  keep_alive,
  VoidClosure*                 complete_gc,
  AbstractRefProcTaskExecutor* task_executor,
  GCTimer*                     gc_timer) {
  NOT_PRODUCT(verify_ok_to_handle_reflists());

  assert(!enqueuing_is_done(), "If here enqueuing should not be complete");
  // Stop treating discovered references specially.
  disable_discovery();

  // If discovery was concurrent, someone could have modified
  // the value of the static field in the j.l.r.SoftReference
  // class that holds the soft reference timestamp clock using
  // reflection or Unsafe between when discovery was enabled and
  // now. Unconditionally update the static field in ReferenceProcessor
  // here so that we use the new value during processing of the
  // discovered soft refs.

  // 获取SoftReference类中的静态变量clock的值
  _soft_ref_timestamp_clock = java_lang_ref_SoftReference::clock();

  bool trace_time = PrintGCDetails && PrintReferenceGC;

  // Soft references
  size_t soft_count = 0;
  {
    GCTraceTime tt("SoftReference", trace_time, false, gc_timer);
    soft_count =
      process_discovered_reflist(_discoveredSoftRefs, _current_soft_ref_policy, true,
                                 is_alive, keep_alive, complete_gc, task_executor);
  }

  // 更新SoftReference类中的静态变量clock的值
  update_soft_ref_master_clock();

  // Weak references
  size_t weak_count = 0;
  {
    GCTraceTime tt("WeakReference", trace_time, false, gc_timer);
    weak_count =
      process_discovered_reflist(_discoveredWeakRefs, NULL, true,
                                 is_alive, keep_alive, complete_gc, task_executor);
  }

  // Final references
  size_t final_count = 0;
  {
    GCTraceTime tt("FinalReference", trace_time, false, gc_timer);
    final_count =
      process_discovered_reflist(_discoveredFinalRefs, NULL, false,
                                 is_alive, keep_alive, complete_gc, task_executor);
  }

  // Phantom references
  size_t phantom_count = 0;
  {
    GCTraceTime tt("PhantomReference", trace_time, false, gc_timer);
    phantom_count =
      process_discovered_reflist(_discoveredPhantomRefs, NULL, false,
                                 is_alive, keep_alive, complete_gc, task_executor);
  }

  // Weak global JNI references. It would make more sense (semantically) to
  // traverse these simultaneously with the regular weak references above, but
  // that is not how the JDK1.2 specification is. See #4126360. Native code can
  // thus use JNI weak references to circumvent the phantom references and
  // resurrect a "post-mortem" object.
  {
    GCTraceTime tt("JNI Weak Reference", trace_time, false, gc_timer);
    if (task_executor != NULL) {
      task_executor->set_single_threaded_mode();
    }
    process_phaseJNI(is_alive, keep_alive, complete_gc);
  }

  return ReferenceProcessorStats(soft_count, weak_count, final_count, phantom_count);
}

#ifndef PRODUCT
// Calculate the number of jni handles.
uint ReferenceProcessor::count_jni_refs() {
  class AlwaysAliveClosure: public BoolObjectClosure {
  public:
    virtual bool do_object_b(oop obj) { return true; }
  };

  class CountHandleClosure: public OopClosure {
  private:
    int _count;
  public:
    CountHandleClosure(): _count(0) {}
    void do_oop(oop* unused)       { _count++; }
    void do_oop(narrowOop* unused) { ShouldNotReachHere(); }
    int count() { return _count; }
  };
  CountHandleClosure global_handle_count;
  AlwaysAliveClosure always_alive;
  JNIHandles::weak_oops_do(&always_alive, &global_handle_count);
  return global_handle_count.count();
}
#endif

void ReferenceProcessor::process_phaseJNI(BoolObjectClosure* is_alive,
                                          OopClosure*        keep_alive,
                                          VoidClosure*       complete_gc) {
#ifndef PRODUCT
  if (PrintGCDetails && PrintReferenceGC) {
    unsigned int count = count_jni_refs();
    gclog_or_tty->print(", %u refs", count);
  }
#endif
  JNIHandles::weak_oops_do(is_alive, keep_alive);
  complete_gc->do_void();
}


// 将多个DiscoveredList中的引用类型都添加到PendingList中
template <class T>
bool enqueue_discovered_ref_helper(ReferenceProcessor* ref,
                                   AbstractRefProcTaskExecutor* task_executor) {

  // Remember old value of pending references list
  // pending_list_addr是Reference的私有静态变量pending的地址
  T* pending_list_addr = (T*)java_lang_ref_Reference::pending_list_addr();
  T old_pending_list_value = *pending_list_addr;

  // Enqueue references that are not made active again, and
  // clear the decks for the next collection (cycle).
  // 将Reference对象添加到PendingList列表中， 这些对象不会再变为Active状态
  // 将DiscoveredList中的引用对象添加到PendingList中，其实就是将多个DiscoveredList中的引用对象用Reference类中定义的discovered变量连接起来，
  // 然后让Reference中的pending变量指向链表的首元素。
  ref->enqueue_discovered_reflists((HeapWord*)pending_list_addr, task_executor);
  // Do the oop-check on pending_list_addr missed in
  // enqueue_discovered_reflist. We should probably
  // do a raw oop_check so that future such idempotent
  // oop_stores relying on the oop-check side-effect
  // may be elided automatically and safely without
  // affecting correctness.
  oop_store(pending_list_addr, oopDesc::load_decode_heap_oop(pending_list_addr));

  // Stop treating discovered references specially.
  // 将_discovering_refs的值设置为false
  ref->disable_discovery();

  // Return true if new pending references were added
  // 如果有新的Reference对象加入PendingList， 则函数返回true
  return old_pending_list_value != *pending_list_addr;
}

// 将多个DiscoveredList中的引用类型都添加到PendingList中
bool ReferenceProcessor::enqueue_discovered_references(AbstractRefProcTaskExecutor* task_executor) {
  NOT_PRODUCT(verify_ok_to_handle_reflists());
  // 是否开启了指针压缩
  if (UseCompressedOops) {
    return enqueue_discovered_ref_helper<narrowOop>(this, task_executor);
  } else {
    return enqueue_discovered_ref_helper<oop>(this, task_executor);
  }
}

void ReferenceProcessor::enqueue_discovered_reflist(DiscoveredList& refs_list,
                                                    HeapWord* pending_list_addr) {
  // Given a list of refs linked through the "discovered" field
  // (java.lang.ref.Reference.discovered), self-loop their "next" field
  // thus distinguishing them from active References, then
  // prepend them to the pending list.
  // BKWRD COMPATIBILITY NOTE: For older JDKs (prior to the fix for 4956777),
  // the "next" field is used to chain the pending list, not the discovered
  // field.

  if (TraceReferenceGC && PrintGCDetails) {
    gclog_or_tty->print_cr("ReferenceProcessor::enqueue_discovered_reflist list "
                           INTPTR_FORMAT, (address)refs_list.head());
  }

  oop obj = NULL;
  oop next_d = refs_list.head();
  // 在OpenJDK 8中使用discovered变量实现PendingList
  if (pending_list_uses_discovered_field()) { // New behaviour
    // Walk down the list, self-looping the next field
    // so that the References are not considered active.
    // 将DiscoveredList中的所有引用对象添加到PendingList中，添加到PendingList中的对象的next属性指向自己，这样这些引用对象就不再是Active状态了
    while (obj != next_d) {
      obj = next_d;
      assert(obj->is_instanceRef(), "should be reference object");
      next_d = java_lang_ref_Reference::discovered(obj);
      if (TraceReferenceGC && PrintGCDetails) {
        gclog_or_tty->print_cr("        obj " INTPTR_FORMAT "/next_d " INTPTR_FORMAT,
                               (void *)obj, (void *)next_d);
      }
      assert(java_lang_ref_Reference::next(obj) == NULL,
             "Reference not active; should not be discovered");
      // Self-loop next, so as to make Ref not active.
      // 执行的操作为obj._next = obj，这样就变成了Pending状态
      java_lang_ref_Reference::set_next(obj, obj);
      // 当前处理的Reference对象是DiscoveredList中的最后一个对象
      if (next_d == obj) {  // obj is last
        // Swap refs_list into pendling_list_addr and
        // set obj's discovered to what we read from pending_list_addr.
        oop old = oopDesc::atomic_exchange_oop(refs_list.head(), pending_list_addr);
        // Need oop_check on pending_list_addr above;
        // see special oop-check code at the end of
        // enqueue_discovered_reflists() further below.
        // 执行的操作为obj._discovered=old
        java_lang_ref_Reference::set_discovered(obj, old); // old may be NULL
      }
    }
  } else { // Old behaviour
    // Walk down the list, copying the discovered field into
    // the next field and clearing the discovered field.
    while (obj != next_d) {
      obj = next_d;
      assert(obj->is_instanceRef(), "should be reference object");
      next_d = java_lang_ref_Reference::discovered(obj);
      if (TraceReferenceGC && PrintGCDetails) {
        gclog_or_tty->print_cr("        obj " INTPTR_FORMAT "/next_d " INTPTR_FORMAT,
                               (void *)obj, (void *)next_d);
      }
      assert(java_lang_ref_Reference::next(obj) == NULL,
             "The reference should not be enqueued");
      if (next_d == obj) {  // obj is last
        // Swap refs_list into pendling_list_addr and
        // set obj's next to what we read from pending_list_addr.
        oop old = oopDesc::atomic_exchange_oop(refs_list.head(), pending_list_addr);
        // Need oop_check on pending_list_addr above;
        // see special oop-check code at the end of
        // enqueue_discovered_reflists() further below.
        if (old == NULL) {
          // obj should be made to point to itself, since
          // pending list was empty.
          java_lang_ref_Reference::set_next(obj, obj);
        } else {
          java_lang_ref_Reference::set_next(obj, old);
        }
      } else {
        java_lang_ref_Reference::set_next(obj, next_d);
      }
      java_lang_ref_Reference::set_discovered(obj, (oop) NULL);
    }
  }
}

// Parallel enqueue task
class RefProcEnqueueTask: public AbstractRefProcTaskExecutor::EnqueueTask {
public:
  RefProcEnqueueTask(ReferenceProcessor& ref_processor,
                     DiscoveredList      discovered_refs[],
                     HeapWord*           pending_list_addr,
                     int                 n_queues)
    : EnqueueTask(ref_processor, discovered_refs,
                  pending_list_addr, n_queues)
  { }

  virtual void work(unsigned int work_id) {
    assert(work_id < (unsigned int)_ref_processor.max_num_q(), "Index out-of-bounds");
    // Simplest first cut: static partitioning.
    int index = work_id;
    // The increment on "index" must correspond to the maximum number of queues
    // (n_queues) with which that ReferenceProcessor was created.  That
    // is because of the "clever" way the discovered references lists were
    // allocated and are indexed into.
    assert(_n_queues == (int) _ref_processor.max_num_q(), "Different number not expected");
    for (int j = 0;
         j < ReferenceProcessor::number_of_subclasses_of_ref();
         j++, index += _n_queues) {
      _ref_processor.enqueue_discovered_reflist(
        _refs_lists[index], _pending_list_addr);
      _refs_lists[index].set_head(NULL);
      _refs_lists[index].set_length(0);
    }
  }
};

// Enqueue references that are not made active again
// 必须要对保存软引用和弱引用等引用对象的DiscoveredList进行处理， 完成后清空DiscoveredList， 然后等待下一次继续重复使用这些列表。
void ReferenceProcessor::enqueue_discovered_reflists(HeapWord* pending_list_addr,
  AbstractRefProcTaskExecutor* task_executor) {
  if (_processing_is_mt && task_executor != NULL) {
    // Parallel code
    RefProcEnqueueTask tsk(*this, _discovered_refs,
                           pending_list_addr, _max_num_q);
    task_executor->execute(tsk);
  } else {
    // Serial code: call the parent class's implementation
    for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
      enqueue_discovered_reflist(_discovered_refs[i], pending_list_addr);
      _discovered_refs[i].set_head(NULL);
      _discovered_refs[i].set_length(0);
    }
  }
}

void DiscoveredListIterator::load_ptrs(DEBUG_ONLY(bool allow_null_referent)) {
  _discovered_addr = java_lang_ref_Reference::discovered_addr(_ref);
  oop discovered = java_lang_ref_Reference::discovered(_ref);
  assert(_discovered_addr && discovered->is_oop_or_null(),
         "discovered field is bad");
  _next = discovered;
  _referent_addr = java_lang_ref_Reference::referent_addr(_ref);
  _referent = java_lang_ref_Reference::referent(_ref);
  assert(Universe::heap()->is_in_reserved_or_null(_referent),
         "Wrong oop found in java.lang.Reference object");
  assert(allow_null_referent ?
             _referent->is_oop_or_null()
           : _referent->is_oop(),
         "bad referent");
}

void DiscoveredListIterator::remove() {
  assert(_ref->is_oop(), "Dropping a bad reference");
  oop_store_raw(_discovered_addr, NULL);

  // First _prev_next ref actually points into DiscoveredList (gross).
  oop new_next;
  if (_next == _ref) {
    // At the end of the list, we should make _prev point to itself.
    // If _ref is the first ref, then _prev_next will be in the DiscoveredList,
    // and _prev will be NULL.
    new_next = _prev;
  } else {
    new_next = _next;
  }

  if (UseCompressedOops) {
    // Remove Reference object from list.
    oopDesc::encode_store_heap_oop((narrowOop*)_prev_next, new_next);
  } else {
    // Remove Reference object from list.
    oopDesc::store_heap_oop((oop*)_prev_next, new_next);
  }
  NOT_PRODUCT(_removed++);
  _refs_list.dec_length(1);
}

// Make the Reference object active again.
void DiscoveredListIterator::make_active() {
  // For G1 we don't want to use set_next - it
  // will dirty the card for the next field of
  // the reference object and will fail
  // CT verification.
  if (UseG1GC) {
    BarrierSet* bs = oopDesc::bs();
    HeapWord* next_addr = java_lang_ref_Reference::next_addr(_ref);

    if (UseCompressedOops) {
      bs->write_ref_field_pre((narrowOop*)next_addr, NULL);
    } else {
      bs->write_ref_field_pre((oop*)next_addr, NULL);
    }
    java_lang_ref_Reference::set_next_raw(_ref, NULL);
  } else {
    java_lang_ref_Reference::set_next(_ref, NULL);
  }
}

void DiscoveredListIterator::clear_referent() {
  oop_store_raw(_referent_addr, NULL);
}

// NOTE: process_phase*() are largely similar, and at a high level
// merely iterate over the extant list applying a predicate to
// each of its elements and possibly removing that element from the
// list and applying some further closures to that element.
// We should consider the possibility of replacing these
// process_phase*() methods by abstracting them into
// a single general iterator invocation that receives appropriate
// closures that accomplish this work.

// (SoftReferences only) Traverse the list and remove any SoftReferences whose
// referents are not alive, but that should be kept alive for policy reasons.
// Keep alive the transitive closure of all such referents.
// 在内存足够的情况下，将对应的SoftReference对象从refs_list中移除。
void
ReferenceProcessor::process_phase1(DiscoveredList&    refs_list,
                                   ReferencePolicy*   policy,
                                   BoolObjectClosure* is_alive,
                                   OopClosure*        keep_alive,
                                   VoidClosure*       complete_gc) {
  assert(policy != NULL, "Must have a non-NULL policy");
  DiscoveredListIterator iter(refs_list, keep_alive, is_alive);
  // Decide which softly reachable refs should be kept alive.
  while (iter.has_next()) {
    iter.load_ptrs(DEBUG_ONLY(!discovery_is_atomic() /* allow_null_referent */));
    bool referent_is_dead = (iter.referent() != NULL) && !iter.is_referent_alive();
    if (referent_is_dead && // 被引用对象referent已经不存活
        // 根据相关策略判断， 这个不存活的对象还不应该被回收
        !policy->should_clear_reference(iter.obj(), _soft_ref_timestamp_clock)) {
      if (TraceReferenceGC) {
        gclog_or_tty->print_cr("Dropping reference (" INTPTR_FORMAT ": %s"  ") by policy",
                               (void *)iter.obj(), iter.obj()->klass()->internal_name());
      }
      // Remove Reference object from list
      // 将引用对象从refs_list中移除
      iter.remove();
      // Make the Reference object active again
      // 让引用对象存活
      iter.make_active();
      // keep the referent around
      // 标记被引用对象，同时将被引用的对象放到栈中，这样被标记后的对象就不会被垃圾回收。
      iter.make_referent_alive();
      iter.move_to_next();
    } else {
      iter.next();
    }
  }
  // Close the reachable set
  // 对栈中存储的对象进行标记，函数最终会调用MarkSweep::follow_stack() 完成标记过程。
  complete_gc->do_void();
  NOT_PRODUCT(
    if (PrintGCDetails && TraceReferenceGC) {
      gclog_or_tty->print_cr(" Dropped %d dead Refs out of %d "
        "discovered Refs by policy, from list " INTPTR_FORMAT,
        iter.removed(), iter.processed(), (address)refs_list.head());
    }
  )
}

// Traverse the list and remove any Refs that are not active, or
// whose referents are either alive or NULL.
void
ReferenceProcessor::pp2_work(DiscoveredList&    refs_list,
                             BoolObjectClosure* is_alive,
                             OopClosure*        keep_alive) {
  assert(discovery_is_atomic(), "Error");
  DiscoveredListIterator iter(refs_list, keep_alive, is_alive);
  while (iter.has_next()) {
    iter.load_ptrs(DEBUG_ONLY(false /* allow_null_referent */));
    DEBUG_ONLY(oop next = java_lang_ref_Reference::next(iter.obj());)
    assert(next == NULL, "Should not discover inactive Reference");
    if (iter.is_referent_alive()) {
      if (TraceReferenceGC) {
        gclog_or_tty->print_cr("Dropping strongly reachable reference (" INTPTR_FORMAT ": %s)",
                               (void *)iter.obj(), iter.obj()->klass()->internal_name());
      }
      // The referent is reachable after all.
      // Remove Reference object from list.
      iter.remove();
      // Update the referent pointer as necessary: Note that this
      // should not entail any recursive marking because the
      // referent must already have been traversed.
      iter.make_referent_alive();
      iter.move_to_next();
    } else {
      iter.next();
    }
  }
  NOT_PRODUCT(
    if (PrintGCDetails && TraceReferenceGC && (iter.processed() > 0)) {
      gclog_or_tty->print_cr(" Dropped %d active Refs out of %d "
        "Refs in discovered list " INTPTR_FORMAT,
        iter.removed(), iter.processed(), (address)refs_list.head());
    }
  )
}

void
ReferenceProcessor::pp2_work_concurrent_discovery(DiscoveredList&    refs_list,
                                                  BoolObjectClosure* is_alive,
                                                  OopClosure*        keep_alive,
                                                  VoidClosure*       complete_gc) {
  assert(!discovery_is_atomic(), "Error");
  DiscoveredListIterator iter(refs_list, keep_alive, is_alive);
  while (iter.has_next()) {
    iter.load_ptrs(DEBUG_ONLY(true /* allow_null_referent */));
    HeapWord* next_addr = java_lang_ref_Reference::next_addr(iter.obj());
    oop next = java_lang_ref_Reference::next(iter.obj());
    if ((iter.referent() == NULL || iter.is_referent_alive() ||
         next != NULL)) {
      assert(next->is_oop_or_null(), "bad next field");
      // Remove Reference object from list
      iter.remove();
      // Trace the cohorts
      iter.make_referent_alive();
      if (UseCompressedOops) {
        keep_alive->do_oop((narrowOop*)next_addr);
      } else {
        keep_alive->do_oop((oop*)next_addr);
      }
      iter.move_to_next();
    } else {
      iter.next();
    }
  }
  // Now close the newly reachable set
  complete_gc->do_void();
  NOT_PRODUCT(
    if (PrintGCDetails && TraceReferenceGC && (iter.processed() > 0)) {
      gclog_or_tty->print_cr(" Dropped %d active Refs out of %d "
        "Refs in discovered list " INTPTR_FORMAT,
        iter.removed(), iter.processed(), (address)refs_list.head());
    }
  )
}

// Traverse the list and process the referents, by either
// clearing them or keeping them (and their reachable
// closure) alive.
void
ReferenceProcessor::process_phase3(DiscoveredList&    refs_list,
                                   bool               clear_referent,
                                   BoolObjectClosure* is_alive,
                                   OopClosure*        keep_alive,
                                   VoidClosure*       complete_gc) {
  ResourceMark rm;
  DiscoveredListIterator iter(refs_list, keep_alive, is_alive);
  while (iter.has_next()) {
    iter.update_discovered();
    iter.load_ptrs(DEBUG_ONLY(false /* allow_null_referent */));
    if (clear_referent) {
      // NULL out referent pointer
      iter.clear_referent();
    } else {
      // keep the referent around
      iter.make_referent_alive();
    }
    if (TraceReferenceGC) {
      gclog_or_tty->print_cr("Adding %sreference (" INTPTR_FORMAT ": %s) as pending",
                             clear_referent ? "cleared " : "",
                             (void *)iter.obj(), iter.obj()->klass()->internal_name());
    }
    assert(iter.obj()->is_oop(UseConcMarkSweepGC), "Adding a bad reference");
    iter.next();
  }
  // Remember to update the next pointer of the last ref.
  iter.update_discovered();
  // Close the reachable set
  complete_gc->do_void();
}

void
ReferenceProcessor::clear_discovered_references(DiscoveredList& refs_list) {
  oop obj = NULL;
  oop next = refs_list.head();
  while (next != obj) {
    obj = next;
    next = java_lang_ref_Reference::discovered(obj);
    java_lang_ref_Reference::set_discovered_raw(obj, NULL);
  }
  refs_list.set_head(NULL);
  refs_list.set_length(0);
}

void
ReferenceProcessor::abandon_partial_discovered_list(DiscoveredList& refs_list) {
  clear_discovered_references(refs_list);
}

void ReferenceProcessor::abandon_partial_discovery() {
  // loop over the lists
  for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
    if (TraceReferenceGC && PrintGCDetails && ((i % _max_num_q) == 0)) {
      gclog_or_tty->print_cr("\nAbandoning %s discovered list", list_name(i));
    }
    abandon_partial_discovered_list(_discovered_refs[i]);
  }
}

class RefProcPhase1Task: public AbstractRefProcTaskExecutor::ProcessTask {
public:
  RefProcPhase1Task(ReferenceProcessor& ref_processor,
                    DiscoveredList      refs_lists[],
                    ReferencePolicy*    policy,
                    bool                marks_oops_alive)
    : ProcessTask(ref_processor, refs_lists, marks_oops_alive),
      _policy(policy)
  { }
  virtual void work(unsigned int i, BoolObjectClosure& is_alive,
                    OopClosure& keep_alive,
                    VoidClosure& complete_gc)
  {
    Thread* thr = Thread::current();
    int refs_list_index = ((WorkerThread*)thr)->id();
    _ref_processor.process_phase1(_refs_lists[refs_list_index], _policy,
                                  &is_alive, &keep_alive, &complete_gc);
  }
private:
  ReferencePolicy* _policy;
};

class RefProcPhase2Task: public AbstractRefProcTaskExecutor::ProcessTask {
public:
  RefProcPhase2Task(ReferenceProcessor& ref_processor,
                    DiscoveredList      refs_lists[],
                    bool                marks_oops_alive)
    : ProcessTask(ref_processor, refs_lists, marks_oops_alive)
  { }
  virtual void work(unsigned int i, BoolObjectClosure& is_alive,
                    OopClosure& keep_alive,
                    VoidClosure& complete_gc)
  {
    _ref_processor.process_phase2(_refs_lists[i],
                                  &is_alive, &keep_alive, &complete_gc);
  }
};

class RefProcPhase3Task: public AbstractRefProcTaskExecutor::ProcessTask {
public:
  RefProcPhase3Task(ReferenceProcessor& ref_processor,
                    DiscoveredList      refs_lists[],
                    bool                clear_referent,
                    bool                marks_oops_alive)
    : ProcessTask(ref_processor, refs_lists, marks_oops_alive),
      _clear_referent(clear_referent)
  { }
  virtual void work(unsigned int i, BoolObjectClosure& is_alive,
                    OopClosure& keep_alive,
                    VoidClosure& complete_gc)
  {
    // Don't use "refs_list_index" calculated in this way because
    // balance_queues() has moved the Ref's into the first n queues.
    // Thread* thr = Thread::current();
    // int refs_list_index = ((WorkerThread*)thr)->id();
    // _ref_processor.process_phase3(_refs_lists[refs_list_index], _clear_referent,
    _ref_processor.process_phase3(_refs_lists[i], _clear_referent,
                                  &is_alive, &keep_alive, &complete_gc);
  }
private:
  bool _clear_referent;
};

void ReferenceProcessor::set_discovered(oop ref, oop value) {
  if (_discovered_list_needs_barrier) {
    java_lang_ref_Reference::set_discovered(ref, value);
  } else {
    java_lang_ref_Reference::set_discovered_raw(ref, value);
  }
}

// Balances reference queues.
// Move entries from all queues[0, 1, ..., _max_num_q-1] to
// queues[0, 1, ..., _num_q-1] because only the first _num_q
// corresponding to the active workers will be processed.
void ReferenceProcessor::balance_queues(DiscoveredList ref_lists[])
{
  // calculate total length
  size_t total_refs = 0;
  if (TraceReferenceGC && PrintGCDetails) {
    gclog_or_tty->print_cr("\nBalance ref_lists ");
  }

  for (uint i = 0; i < _max_num_q; ++i) {
    total_refs += ref_lists[i].length();
    if (TraceReferenceGC && PrintGCDetails) {
      gclog_or_tty->print("%d ", ref_lists[i].length());
    }
  }
  if (TraceReferenceGC && PrintGCDetails) {
    gclog_or_tty->print_cr(" = %d", total_refs);
  }
  size_t avg_refs = total_refs / _num_q + 1;
  uint to_idx = 0;
  for (uint from_idx = 0; from_idx < _max_num_q; from_idx++) {
    bool move_all = false;
    if (from_idx >= _num_q) {
      move_all = ref_lists[from_idx].length() > 0;
    }
    while ((ref_lists[from_idx].length() > avg_refs) ||
           move_all) {
      assert(to_idx < _num_q, "Sanity Check!");
      if (ref_lists[to_idx].length() < avg_refs) {
        // move superfluous refs
        size_t refs_to_move;
        // Move all the Ref's if the from queue will not be processed.
        if (move_all) {
          refs_to_move = MIN2(ref_lists[from_idx].length(),
                              avg_refs - ref_lists[to_idx].length());
        } else {
          refs_to_move = MIN2(ref_lists[from_idx].length() - avg_refs,
                              avg_refs - ref_lists[to_idx].length());
        }

        assert(refs_to_move > 0, "otherwise the code below will fail");

        oop move_head = ref_lists[from_idx].head();
        oop move_tail = move_head;
        oop new_head  = move_head;
        // find an element to split the list on
        for (size_t j = 0; j < refs_to_move; ++j) {
          move_tail = new_head;
          new_head = java_lang_ref_Reference::discovered(new_head);
        }

        // Add the chain to the to list.
        if (ref_lists[to_idx].head() == NULL) {
          // to list is empty. Make a loop at the end.
          set_discovered(move_tail, move_tail);
        } else {
          set_discovered(move_tail, ref_lists[to_idx].head());
        }
        ref_lists[to_idx].set_head(move_head);
        ref_lists[to_idx].inc_length(refs_to_move);

        // Remove the chain from the from list.
        if (move_tail == new_head) {
          // We found the end of the from list.
          ref_lists[from_idx].set_head(NULL);
        } else {
          ref_lists[from_idx].set_head(new_head);
        }
        ref_lists[from_idx].dec_length(refs_to_move);
        if (ref_lists[from_idx].length() == 0) {
          break;
        }
      } else {
        to_idx = (to_idx + 1) % _num_q;
      }
    }
  }
#ifdef ASSERT
  size_t balanced_total_refs = 0;
  for (uint i = 0; i < _max_num_q; ++i) {
    balanced_total_refs += ref_lists[i].length();
    if (TraceReferenceGC && PrintGCDetails) {
      gclog_or_tty->print("%d ", ref_lists[i].length());
    }
  }
  if (TraceReferenceGC && PrintGCDetails) {
    gclog_or_tty->print_cr(" = %d", balanced_total_refs);
    gclog_or_tty->flush();
  }
  assert(total_refs == balanced_total_refs, "Balancing was incomplete");
#endif
}

void ReferenceProcessor::balance_all_queues() {
  balance_queues(_discoveredSoftRefs);
  balance_queues(_discoveredWeakRefs);
  balance_queues(_discoveredFinalRefs);
  balance_queues(_discoveredPhantomRefs);
}

// process_discovered_reflist()函数的作用就是将不需要被回收的对象从refs_lists中移除，
// refs_lists最后剩下的元素全是需要被回收的元素，最后会将其第一个元素赋值给Reference.pending字段。
size_t
ReferenceProcessor::process_discovered_reflist(
  DiscoveredList               refs_lists[],    // refs_lists数组有多个DiscoveredList（一般多线程的话会多个）存放了本次执行GC时发现的引用类型（虚引用、软引用和弱引用等）。
  ReferencePolicy*             policy,          // 只有处理软引用时才有值，处理其他引用对象时的值为NULL
  bool                         clear_referent,  // ReferenceProcessor处理软引用和弱引用时，clear_referent的值为true，处理最终引用和虚引用时，clear_referent的值为false
  BoolObjectClosure*           is_alive,
  OopClosure*                  keep_alive,
  VoidClosure*                 complete_gc,
  AbstractRefProcTaskExecutor* task_executor)
{
  bool mt_processing = task_executor != NULL && _processing_is_mt;
  // If discovery used MT and a dynamic number of GC threads, then
  // the queues must be balanced for correctness if fewer than the
  // maximum number of queues were used.  The number of queue used
  // during discovery may be different than the number to be used
  // for processing so don't depend of _num_q < _max_num_q as part
  // of the test.
  bool must_balance = _discovery_is_mt;

  if ((mt_processing && ParallelRefProcBalancingEnabled) ||
      must_balance) {
    balance_queues(refs_lists);
  }

  size_t total_list_count = total_count(refs_lists);

  if (PrintReferenceGC && PrintGCDetails) {
    gclog_or_tty->print(", %u refs", total_list_count);
  }

  // Phase 1 (soft refs only):
  // . Traverse the list and remove any SoftReferences whose
  //   referents are not alive, but that should be kept alive for
  //   policy reasons. Keep alive the transitive closure of all
  //   such referents.
  // 第1阶段：因为软引用的policy不为NULL，所以遍历保存软引用的DiscoveredList列表将被引用对象不可达的引用对象Reference从列表中移除。
  // 另外，有些对象虽然不可达，但是根据policy也可能会保留，这样referent及引用的对象都会被标记为活跃，这样不会被回收。
  // 此阶段只针对软引用，主要目的是在内存足够的情况下，将对应的SoftReference对象从refs_list中移除。
  if (policy != NULL) {
    if (mt_processing) {
      RefProcPhase1Task phase1(*this, refs_lists, policy, true /*marks_oops_alive*/);
      task_executor->execute(phase1);
    } else {
      for (uint i = 0; i < _max_num_q; i++) {
        process_phase1(refs_lists[i], policy,
                       is_alive, keep_alive, complete_gc);
      }
    }
  } else { // policy == NULL
    assert(refs_lists != _discoveredSoftRefs,
           "Policy must be specified for soft references.");
  }

  // Phase 2:
  // . Traverse the list and remove any refs whose referents are alive.
  // 第2阶段：遍历所有的DiscoveredList列表，将可达的referent对应的Reference对象从Discovered列表中移除。
  if (mt_processing) {
    RefProcPhase2Task phase2(*this, refs_lists, !discovery_is_atomic() /*marks_oops_alive*/);
    task_executor->execute(phase2);
  } else {
    for (uint i = 0; i < _max_num_q; i++) {
      process_phase2(refs_lists[i], is_alive, keep_alive, complete_gc);
    }
  }

  // Phase 3:
  // . Traverse the list and process referents as appropriate.
  // 第3阶段：遍历所有的DiscoveredList列表，正常处理所有的reference。
  if (mt_processing) {
    RefProcPhase3Task phase3(*this, refs_lists, clear_referent, true /*marks_oops_alive*/);
    task_executor->execute(phase3);
  } else {
    for (uint i = 0; i < _max_num_q; i++) {
      process_phase3(refs_lists[i], clear_referent,
                     is_alive, keep_alive, complete_gc);
    }
  }

  return total_list_count;
}

void ReferenceProcessor::clean_up_discovered_references() {
  // loop over the lists
  for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
    if (TraceReferenceGC && PrintGCDetails && ((i % _max_num_q) == 0)) {
      gclog_or_tty->print_cr(
        "\nScrubbing %s discovered list of Null referents",
        list_name(i));
    }
    clean_up_discovered_reflist(_discovered_refs[i]);
  }
}

void ReferenceProcessor::clean_up_discovered_reflist(DiscoveredList& refs_list) {
  assert(!discovery_is_atomic(), "Else why call this method?");
  DiscoveredListIterator iter(refs_list, NULL, NULL);
  while (iter.has_next()) {
    iter.load_ptrs(DEBUG_ONLY(true /* allow_null_referent */));
    oop next = java_lang_ref_Reference::next(iter.obj());
    assert(next->is_oop_or_null(), "bad next field");
    // If referent has been cleared or Reference is not active,
    // drop it.
    if (iter.referent() == NULL || next != NULL) {
      debug_only(
        if (PrintGCDetails && TraceReferenceGC) {
          gclog_or_tty->print_cr("clean_up_discovered_list: Dropping Reference: "
            INTPTR_FORMAT " with next field: " INTPTR_FORMAT
            " and referent: " INTPTR_FORMAT,
            (void *)iter.obj(), (void *)next, (void *)iter.referent());
        }
      )
      // Remove Reference object from list
      iter.remove();
      iter.move_to_next();
    } else {
      iter.next();
    }
  }
  NOT_PRODUCT(
    if (PrintGCDetails && TraceReferenceGC) {
      gclog_or_tty->print(
        " Removed %d Refs with NULL referents out of %d discovered Refs",
        iter.removed(), iter.processed());
    }
  )
}

inline DiscoveredList* ReferenceProcessor::get_discovered_list(ReferenceType rt) {
  uint id = 0;
  // Determine the queue index to use for this object.
  if (_discovery_is_mt) {
    // During a multi-threaded discovery phase,
    // each thread saves to its "own" list.
    Thread* thr = Thread::current();
    id = thr->as_Worker_thread()->id();
  } else {
    // single-threaded discovery, we save in round-robin
    // fashion to each of the lists.
    if (_processing_is_mt) {
      id = next_id();
    }
  }
  assert(0 <= id && id < _max_num_q, "Id is out-of-bounds (call Freud?)");

  // Get the discovered queue to which we will add
  DiscoveredList* list = NULL;
  switch (rt) {
    case REF_OTHER:
      // Unknown reference type, no special treatment
      break;
    case REF_SOFT:
      list = &_discoveredSoftRefs[id];
      break;
    case REF_WEAK:
      list = &_discoveredWeakRefs[id];
      break;
    case REF_FINAL:
      list = &_discoveredFinalRefs[id];
      break;
    case REF_PHANTOM:
      list = &_discoveredPhantomRefs[id];
      break;
    case REF_NONE:
      // we should not reach here if we are an InstanceRefKlass
    default:
      ShouldNotReachHere();
  }
  if (TraceReferenceGC && PrintGCDetails) {
    gclog_or_tty->print_cr("Thread %d gets list " INTPTR_FORMAT, id, list);
  }
  return list;
}

inline void
ReferenceProcessor::add_to_discovered_list_mt(DiscoveredList& refs_list,
                                              oop             obj,
                                              HeapWord*       discovered_addr) {
  assert(_discovery_is_mt, "!_discovery_is_mt should have been handled by caller");
  // First we must make sure this object is only enqueued once. CAS in a non null
  // discovered_addr.
  oop current_head = refs_list.head();
  // The last ref must have its discovered field pointing to itself.
  oop next_discovered = (current_head != NULL) ? current_head : obj;

  // Note: In the case of G1, this specific pre-barrier is strictly
  // not necessary because the only case we are interested in
  // here is when *discovered_addr is NULL (see the CAS further below),
  // so this will expand to nothing. As a result, we have manually
  // elided this out for G1, but left in the test for some future
  // collector that might have need for a pre-barrier here, e.g.:-
  // _bs->write_ref_field_pre((oop* or narrowOop*)discovered_addr, next_discovered);
  assert(!_discovered_list_needs_barrier || UseG1GC,
         "Need to check non-G1 collector: "
         "may need a pre-write-barrier for CAS from NULL below");
  oop retest = oopDesc::atomic_compare_exchange_oop(next_discovered, discovered_addr,
                                                    NULL);
  if (retest == NULL) {
    // This thread just won the right to enqueue the object.
    // We have separate lists for enqueueing, so no synchronization
    // is necessary.
    refs_list.set_head(obj);
    refs_list.inc_length(1);
    if (_discovered_list_needs_barrier) {
      _bs->write_ref_field((void*)discovered_addr, next_discovered);
    }

    if (TraceReferenceGC) {
      gclog_or_tty->print_cr("Discovered reference (mt) (" INTPTR_FORMAT ": %s)",
                             (void *)obj, obj->klass()->internal_name());
    }
  } else {
    // If retest was non NULL, another thread beat us to it:
    // The reference has already been discovered...
    if (TraceReferenceGC) {
      gclog_or_tty->print_cr("Already discovered reference (" INTPTR_FORMAT ": %s)",
                             (void *)obj, obj->klass()->internal_name());
    }
  }
}

#ifndef PRODUCT
// Non-atomic (i.e. concurrent) discovery might allow us
// to observe j.l.References with NULL referents, being those
// cleared concurrently by mutators during (or after) discovery.
void ReferenceProcessor::verify_referent(oop obj) {
  bool da = discovery_is_atomic();
  oop referent = java_lang_ref_Reference::referent(obj);
  assert(da ? referent->is_oop() : referent->is_oop_or_null(),
         err_msg("Bad referent " INTPTR_FORMAT " found in Reference "
                 INTPTR_FORMAT " during %satomic discovery ",
                 (void *)referent, (void *)obj, da ? "" : "non-"));
}
#endif

// We mention two of several possible choices here:
// #0: if the reference object is not in the "originating generation"
//     (or part of the heap being collected, indicated by our "span"
//     we don't treat it specially (i.e. we scan it as we would
//     a normal oop, treating its references as strong references).
//     This means that references can't be discovered unless their
//     referent is also in the same span. This is the simplest,
//     most "local" and most conservative approach, albeit one
//     that may cause weak references to be enqueued least promptly.
//     We call this choice the "ReferenceBasedDiscovery" policy.
// #1: the reference object may be in any generation (span), but if
//     the referent is in the generation (span) being currently collected
//     then we can discover the reference object, provided
//     the object has not already been discovered by
//     a different concurrently running collector (as may be the
//     case, for instance, if the reference object is in CMS and
//     the referent in DefNewGeneration), and provided the processing
//     of this reference object by the current collector will
//     appear atomic to every other collector in the system.
//     (Thus, for instance, a concurrent collector may not
//     discover references in other generations even if the
//     referent is in its own generation). This policy may,
//     in certain cases, enqueue references somewhat sooner than
//     might Policy #0 above, but at marginally increased cost
//     and complexity in processing these references.
//     We call this choice the "RefeferentBasedDiscovery" policy.
// 以下代码中实现了引用查找的策略RefDiscoveryPolicy，引用类型的处理并不简单，因为Reference和referent可能处在不同的代中。
// 如果Reference和referent都在年轻代，那么referent会被标记，这样就可以处理Reference。如果二者不在一个代中，那么Reference可能无法被处理。
// 如果Reference在年轻代，而referent在老年代，老年代不标记referent，此时无法处理Reference；
// 如果Reference在老年代，那么referent在年轻代会被标记，但是YGC不处理Reference，因此也无法处理。
bool ReferenceProcessor::discover_reference(oop obj, ReferenceType rt) {
  // Make sure we are discovering refs (rather than processing discovered refs).
  // _discovering_refs在执行GC的时候设置为true表示只查找引用类型而不处理。当执行完GC时设置为false，表示要处理引用类型
  // RegisterReferences表示HotSpot VM是否要注册引用类型， 默认值为true
  if (!_discovering_refs || !RegisterReferences) {
    return false;
  }
  // We only discover active references.
  // 我们只查找那些处于Active状态的Reference对象， 也就是next为null的引用类型，
  // 其他状态下的Reference对象表示查找过程已经完成
  oop next = java_lang_ref_Reference::next(obj);
  if (next != NULL) {   // Ref is no longer active
    return false;
  }

  HeapWord* obj_addr = (HeapWord*)obj;
  if (RefDiscoveryPolicy == ReferenceBasedDiscovery &&
      !_span.contains(obj_addr)) {
    // Reference is not in the originating generation;
    // don't treat it specially (i.e. we want to scan it as a normal
    // object with strong references).
    return false;
  }

  // We only discover references whose referents are not (yet)
  // known to be strongly reachable.
  if (is_alive_non_header() != NULL) {
    verify_referent(obj);
    if (is_alive_non_header()->do_object_b(java_lang_ref_Reference::referent(obj))) {
      return false;  // referent is reachable
    }
  }
  // 如果当前对象为软引用，在不考虑referent是否被标记的情况下，根据软引用的回收策略有时也能判断referent不会被回收，
  // 如果referent在最近限定的一段时间被使用过，那么函数会直接返回false，Reference不会被加入DiscoveredList中，也就意味着软引用不会被回收。
  if (rt == REF_SOFT) {
    // For soft refs we can decide now if these are not
    // current candidates for clearing, in which case we
    // can mark through them now, rather than delaying that
    // to the reference-processing phase. Since all current
    // time-stamp policies advance the soft-ref clock only
    // at a major collection cycle, this is always currently
    // accurate.
    if (!_current_soft_ref_policy->should_clear_reference(obj, _soft_ref_timestamp_clock)) {
      return false;
    }
  }

  ResourceMark rm;      // Needed for tracing.

  HeapWord* const discovered_addr = java_lang_ref_Reference::discovered_addr(obj);
  const oop  discovered = java_lang_ref_Reference::discovered(obj);
  assert(discovered->is_oop_or_null(), "bad discovered field");
  if (discovered != NULL) {
    // The reference has already been discovered...
    if (TraceReferenceGC) {
      gclog_or_tty->print_cr("Already discovered reference (" INTPTR_FORMAT ": %s)",
                             (void *)obj, obj->klass()->internal_name());
    }
    if (RefDiscoveryPolicy == ReferentBasedDiscovery) {
      // assumes that an object is not processed twice;
      // if it's been already discovered it must be on another
      // generation's discovered list; so we won't discover it.
      return false;
    } else {
      assert(RefDiscoveryPolicy == ReferenceBasedDiscovery,
             "Unrecognized policy");
      // Check assumption that an object is not potentially
      // discovered twice except by concurrent collectors that potentially
      // trace the same Reference object twice.
      assert(UseConcMarkSweepGC || UseG1GC,
             "Only possible with a concurrent marking collector");
      return true;
    }
  }

  if (RefDiscoveryPolicy == ReferentBasedDiscovery) {
    verify_referent(obj);
    // Discover if and only if EITHER:
    // .. reference is in our span, OR
    // .. we are an atomic collector and referent is in our span
    if (_span.contains(obj_addr) ||
        (discovery_is_atomic() &&
         _span.contains(java_lang_ref_Reference::referent(obj)))) {
      // should_enqueue = true;
    } else {
      return false;
    }
  } else {
    assert(RefDiscoveryPolicy == ReferenceBasedDiscovery &&
           _span.contains(obj_addr), "code inconsistency");
  }

  // Get the right type of discovered queue head.
  // 根据类型获取对应的DiscoveredList， 可能获取的是_discoveredSoftRefs和_discoveredWeakRefs等， 在介绍ReferenceProcessor类时已经详细介绍过
  DiscoveredList* list = get_discovered_list(rt);
  if (list == NULL) {
    return false;   // nothing special needs to be done
  }

  if (_discovery_is_mt) {
    add_to_discovered_list_mt(*list, obj, discovered_addr);
  } else {
    // If "_discovered_list_needs_barrier", we do write barriers when
    // updating the discovered reference list.  Otherwise, we do a raw store
    // here: the field will be visited later when processing the discovered
    // references.
    // 通过单线程的方式处理发现的引用类型， 将引用类型添加到DiscoveredList中
    oop current_head = list->head();
    // The last ref must have its discovered field pointing to itself.
    // 如果current_head为NULL时， 则Reference对象的_discovered变量会指向自己，这样列表中的最后一个对象肯定指向自己
    oop next_discovered = (current_head != NULL) ? current_head : obj;

    // As in the case further above, since we are over-writing a NULL
    // pre-value, we can safely elide the pre-barrier here for the case of G1.
    // e.g.:- _bs->write_ref_field_pre((oop* or narrowOop*)discovered_addr, next_discovered);
    assert(discovered == NULL, "control point invariant");
    assert(!_discovered_list_needs_barrier || UseG1GC,
           "For non-G1 collector, may need a pre-write-barrier for CAS from NULL below");
    oop_store_raw(discovered_addr, next_discovered);
    if (_discovered_list_needs_barrier) {
      _bs->write_ref_field((void*)discovered_addr, next_discovered);
    }
    list->set_head(obj);
    list->inc_length(1);

    if (TraceReferenceGC) {
      gclog_or_tty->print_cr("Discovered reference (" INTPTR_FORMAT ": %s)",
                                (void *)obj, obj->klass()->internal_name());
    }
  }
  assert(obj->is_oop(), "Discovered a bad reference");
  verify_referent(obj);
  return true;
}

// Preclean the discovered references by removing those
// whose referents are alive, and by marking from those that
// are not active. These lists can be handled here
// in any order and, indeed, concurrently.
void ReferenceProcessor::preclean_discovered_references(
  BoolObjectClosure* is_alive,
  OopClosure* keep_alive,
  VoidClosure* complete_gc,
  YieldClosure* yield,
  GCTimer* gc_timer) {

  NOT_PRODUCT(verify_ok_to_handle_reflists());

  // Soft references
  {
    GCTraceTime tt("Preclean SoftReferences", PrintGCDetails && PrintReferenceGC,
              false, gc_timer);
    for (uint i = 0; i < _max_num_q; i++) {
      if (yield->should_return()) {
        return;
      }
      preclean_discovered_reflist(_discoveredSoftRefs[i], is_alive,
                                  keep_alive, complete_gc, yield);
    }
  }

  // Weak references
  {
    GCTraceTime tt("Preclean WeakReferences", PrintGCDetails && PrintReferenceGC,
              false, gc_timer);
    for (uint i = 0; i < _max_num_q; i++) {
      if (yield->should_return()) {
        return;
      }
      preclean_discovered_reflist(_discoveredWeakRefs[i], is_alive,
                                  keep_alive, complete_gc, yield);
    }
  }

  // Final references
  {
    GCTraceTime tt("Preclean FinalReferences", PrintGCDetails && PrintReferenceGC,
              false, gc_timer);
    for (uint i = 0; i < _max_num_q; i++) {
      if (yield->should_return()) {
        return;
      }
      preclean_discovered_reflist(_discoveredFinalRefs[i], is_alive,
                                  keep_alive, complete_gc, yield);
    }
  }

  // Phantom references
  {
    GCTraceTime tt("Preclean PhantomReferences", PrintGCDetails && PrintReferenceGC,
              false, gc_timer);
    for (uint i = 0; i < _max_num_q; i++) {
      if (yield->should_return()) {
        return;
      }
      preclean_discovered_reflist(_discoveredPhantomRefs[i], is_alive,
                                  keep_alive, complete_gc, yield);
    }
  }
}

// Walk the given discovered ref list, and remove all reference objects
// whose referents are still alive, whose referents are NULL or which
// are not active (have a non-NULL next field). NOTE: When we are
// thus precleaning the ref lists (which happens single-threaded today),
// we do not disable refs discovery to honour the correct semantics of
// java.lang.Reference. As a result, we need to be careful below
// that ref removal steps interleave safely with ref discovery steps
// (in this thread).
void
ReferenceProcessor::preclean_discovered_reflist(DiscoveredList&    refs_list,
                                                BoolObjectClosure* is_alive,
                                                OopClosure*        keep_alive,
                                                VoidClosure*       complete_gc,
                                                YieldClosure*      yield) {
  DiscoveredListIterator iter(refs_list, keep_alive, is_alive);
  while (iter.has_next()) {
    iter.load_ptrs(DEBUG_ONLY(true /* allow_null_referent */));
    oop obj = iter.obj();
    oop next = java_lang_ref_Reference::next(obj);
    if (iter.referent() == NULL || iter.is_referent_alive() ||
        next != NULL) {
      // The referent has been cleared, or is alive, or the Reference is not
      // active; we need to trace and mark its cohort.
      if (TraceReferenceGC) {
        gclog_or_tty->print_cr("Precleaning Reference (" INTPTR_FORMAT ": %s)",
                               (void *)iter.obj(), iter.obj()->klass()->internal_name());
      }
      // Remove Reference object from list
      iter.remove();
      // Keep alive its cohort.
      iter.make_referent_alive();
      if (UseCompressedOops) {
        narrowOop* next_addr = (narrowOop*)java_lang_ref_Reference::next_addr(obj);
        keep_alive->do_oop(next_addr);
      } else {
        oop* next_addr = (oop*)java_lang_ref_Reference::next_addr(obj);
        keep_alive->do_oop(next_addr);
      }
      iter.move_to_next();
    } else {
      iter.next();
    }
  }
  // Close the reachable set
  complete_gc->do_void();

  NOT_PRODUCT(
    if (PrintGCDetails && PrintReferenceGC && (iter.processed() > 0)) {
      gclog_or_tty->print_cr(" Dropped %d Refs out of %d "
        "Refs in discovered list " INTPTR_FORMAT,
        iter.removed(), iter.processed(), (address)refs_list.head());
    }
  )
}

const char* ReferenceProcessor::list_name(uint i) {
   assert(i >= 0 && i <= _max_num_q * number_of_subclasses_of_ref(),
          "Out of bounds index");

   int j = i / _max_num_q;
   switch (j) {
     case 0: return "SoftRef";
     case 1: return "WeakRef";
     case 2: return "FinalRef";
     case 3: return "PhantomRef";
   }
   ShouldNotReachHere();
   return NULL;
}

#ifndef PRODUCT
void ReferenceProcessor::verify_ok_to_handle_reflists() {
  // empty for now
}
#endif

#ifndef PRODUCT
void ReferenceProcessor::clear_discovered_references() {
  guarantee(!_discovering_refs, "Discovering refs?");
  for (uint i = 0; i < _max_num_q * number_of_subclasses_of_ref(); i++) {
    clear_discovered_references(_discovered_refs[i]);
  }
}

#endif // PRODUCT
