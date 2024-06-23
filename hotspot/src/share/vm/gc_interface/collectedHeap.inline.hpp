/*
 * Copyright (c) 2001, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_INTERFACE_COLLECTEDHEAP_INLINE_HPP
#define SHARE_VM_GC_INTERFACE_COLLECTEDHEAP_INLINE_HPP

#include "gc_interface/allocTracer.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "memory/threadLocalAllocBuffer.inline.hpp"
#include "memory/universe.hpp"
#include "oops/arrayOop.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.inline.hpp"
#include "services/lowMemoryDetector.hpp"
#include "utilities/copy.hpp"

// Inline allocation implementations.

// 根据是否使用偏向锁设置对象头信息等，即初始化oop的_mark字段
void CollectedHeap::post_allocation_setup_common(KlassHandle klass,
                                                 HeapWord* obj) {
  // 初始化对象头markOop
  post_allocation_setup_no_klass_install(klass, obj);
  // 初始化对象头中的_klass或_compressed_klass属性
  post_allocation_install_obj_klass(klass, oop(obj));
}

// 设置对象头
void CollectedHeap::post_allocation_setup_no_klass_install(KlassHandle klass,
                                                           HeapWord* objPtr) {
  oop obj = (oop)objPtr;

  assert(obj != NULL, "NULL object pointer");
  // 在允许使用偏向锁的情况下，获取Klass中的 _prototype_header 属性值，其中的锁状态一般为偏向锁状态，
  // 而markOopDesc::prototype()函数初始化的对象头，其锁状态一般为无锁状态Klass中的 _prototype_header 完全是为了支持偏向锁增加的属性，
  if (UseBiasedLocking && (klass() != NULL)) {
    obj->set_mark(klass->prototype_header());
  } else {
    // May be bootstrapping
    obj->set_mark(markOopDesc::prototype());    // 锁的状态为正常
  }
}

// 为对象设置_klass属性的值，int数组的_klass为TypeArrayKlass实例，Object_klass为InstanceKlass实例
void CollectedHeap::post_allocation_install_obj_klass(KlassHandle klass,
                                                   oop obj) {
  // These asserts are kind of complicated because of klassKlass
  // and the beginning of the world.
  assert(klass() != NULL || !Universe::is_fully_initialized(), "NULL klass");
  assert(klass() == NULL || klass()->is_klass(), "not a klass");
  assert(obj != NULL, "NULL object pointer");
  // 初始化对象头中的_klass或_compressed_klass属性
  obj->set_klass(klass());
  assert(!Universe::is_fully_initialized() || obj->klass() != NULL,
         "missing klass");
}

// Support for jvmti and dtrace
inline void post_allocation_notify(KlassHandle klass, oop obj) {
  // support low memory notifications (no-op if not enabled)
  LowMemoryDetector::detect_low_memory_for_collected_pools();

  // support for JVMTI VMObjectAlloc event (no-op if not enabled)
  JvmtiExport::vm_object_alloc_event_collector(obj);

  if (DTraceAllocProbes) {
    // support for Dtrace object alloc event (no-op most of the time)
    if (klass() != NULL && klass()->name() != NULL) {
      SharedRuntime::dtrace_object_alloc(obj);
    }
  }
}

void CollectedHeap::post_allocation_setup_obj(KlassHandle klass,
                                              HeapWord* obj) {
  post_allocation_setup_common(klass, obj);
  assert(Universe::is_bootstrapping() ||
         !((oop)obj)->is_array(), "must not be an array");
  // notify jvmti and dtrace
  // 通知jvmti对象创建，如果DTraceAllocProbes为true则打印日志
  post_allocation_notify(klass, (oop)obj);
}

// 设置数组的length，_mark和_metadata属性
void CollectedHeap::post_allocation_setup_array(KlassHandle klass,
                                                HeapWord* obj,
                                                int length) {
  // Set array length before setting the _klass field
  // in post_allocation_setup_common() because the klass field
  // indicates that the object is parsable by concurrent GC.
  assert(length >= 0, "length should be non-negative");
  // 初始化数组中的length值
  ((arrayOop)obj)->set_length(length);
  post_allocation_setup_common(klass, obj);
  assert(((oop)obj)->is_array(), "must be an array");
  // notify jvmti and dtrace (must be after length is set for dtrace)
  post_allocation_notify(klass, (oop)obj);
}

/**
 * 申请一块非永久性存储空间，若分配失败则抛出异常
 *
 * 	1).从本地线程分配缓冲[TLAB]中申请存储空间
 * 	2).从堆[Heap]中申请存储空间
 */
HeapWord* CollectedHeap::common_mem_allocate_noinit(KlassHandle klass, size_t size, TRAPS) {

  // Clear unhandled oops for memory allocation.  Memory allocation might
  // not take out a lock if from tlab, so clear here.
  // 清理当前线程TLAB中未使用的opp
  CHECK_UNHANDLED_OOPS_ONLY(THREAD->clear_unhandled_oops();)
  // 判断是否发生异常
  if (HAS_PENDING_EXCEPTION) {
    NOT_PRODUCT(guarantee(false, "Should not allocate with exception pending"));
    return NULL;  // caller does a CHECK_0 too
  }

  HeapWord* result = NULL;
  if (UseTLAB) {
    // 在TLAB中分配内存
    result = allocate_from_tlab(klass, THREAD, size);
    if (result != NULL) {
      assert(!HAS_PENDING_EXCEPTION,
             "Unexpected exception, will result in uninitialized storage");
      return result;
    }
  }
  // 在堆中分配内存
  bool gc_overhead_limit_was_exceeded = false;
  // 这个方法里面涉及到堆扩容，GC等等内容
  result = Universe::heap()->mem_allocate(size,
                                          &gc_overhead_limit_was_exceeded);
  // 分配成功
  if (result != NULL) {
    NOT_PRODUCT(Universe::heap()->
      check_for_non_bad_heap_word_value(result, size));
    assert(!HAS_PENDING_EXCEPTION,
           "Unexpected exception, will result in uninitialized storage");
    // 增加当前线程记录已分配的内存大小的属性
    THREAD->incr_allocated_bytes(size * HeapWordSize);
    // 发布堆内存对象分配事件
    AllocTracer::send_allocation_outside_tlab_event(klass, size * HeapWordSize);

    return result;
  }

  // 分配失败
  if (!gc_overhead_limit_was_exceeded) {
    // -XX:+HeapDumpOnOutOfMemoryError and -XX:OnOutOfMemoryError support
    // 异常处理，Java heap space表示当前堆内存严重不足
    report_java_out_of_memory("Java heap space");
    // 通知JVMTI
    if (JvmtiExport::should_post_resource_exhausted()) {
      JvmtiExport::post_resource_exhausted(
        JVMTI_RESOURCE_EXHAUSTED_OOM_ERROR | JVMTI_RESOURCE_EXHAUSTED_JAVA_HEAP,
        "Java heap space");
    }
    // 抛出异常 Java heap space
    THROW_OOP_0(Universe::out_of_memory_error_java_heap());
  } else {
    // -XX:+HeapDumpOnOutOfMemoryError and -XX:OnOutOfMemoryError support
    // 同上，异常处理，GC overhead limit exceeded表示执行GC后仍不能有效回收内存导致内存不足
    report_java_out_of_memory("GC overhead limit exceeded");

    if (JvmtiExport::should_post_resource_exhausted()) {
      JvmtiExport::post_resource_exhausted(
        JVMTI_RESOURCE_EXHAUSTED_OOM_ERROR | JVMTI_RESOURCE_EXHAUSTED_JAVA_HEAP,
        "GC overhead limit exceeded");
    }
    // 抛出异常 GC overhead limit exceeded
    THROW_OOP_0(Universe::out_of_memory_error_gc_overhead_limit());
  }
}

/**
 * 申请一块非永久性存储空间并初始化分配的对象内存块
 */
HeapWord* CollectedHeap::common_mem_allocate_init(KlassHandle klass, size_t size, TRAPS) {
  // 分配内存
  HeapWord* obj = common_mem_allocate_noinit(klass, size, CHECK_NULL);
  // 对分配的内存进行部分初始化
  init_obj(obj, size);
  return obj;
}

// 从TLAB中分配内存
HeapWord* CollectedHeap::allocate_from_tlab(KlassHandle klass, Thread* thread, size_t size) {
  assert(UseTLAB, "should use UseTLAB");
  // 从tlab中分配指定大小的内存
  HeapWord* obj = thread->tlab().allocate(size);
  if (obj != NULL) {
    return obj;
  }
  // Otherwise...
  // 可能分配新的TLAB，然后在新的TLAB中分配内存
  return allocate_from_tlab_slow(klass, thread, size);
}

void CollectedHeap::init_obj(HeapWord* obj, size_t size) {
  assert(obj != NULL, "cannot initialize NULL object");
  const size_t hs = oopDesc::header_size();
  assert(size >= hs, "unexpected object size");
  // 设置GC分代年龄
  ((oop)obj)->set_klass_gap(0);
  // 将分配的对象内存全部初始化为0
  Copy::fill_to_aligned_words(obj + hs, size - hs);
}

oop CollectedHeap::obj_allocate(KlassHandle klass, int size, TRAPS) {
  debug_only(check_for_valid_allocation_state());
  // 检查Java堆是否正在gc
  assert(!Universe::heap()->is_gc_active(), "Allocation during gc not allowed");
  assert(size >= 0, "int won't convert to size_t");
  // 在堆中分配指定size大小的内存并对这块内存进行清零操作
  HeapWord* obj = common_mem_allocate_init(klass, size, CHECK_NULL);
  // 设置对象头和klass属性
  post_allocation_setup_obj(klass, obj);
  // 检查分配的内存是否正常，生产环境不执行
  NOT_PRODUCT(Universe::heap()->check_for_bad_heap_word_value(obj, size));
  return (oop)obj;
}

oop CollectedHeap::array_allocate(KlassHandle klass,
                                  int size,
                                  int length,
                                  TRAPS) {
  debug_only(check_for_valid_allocation_state());
  assert(!Universe::heap()->is_gc_active(), "Allocation during gc not allowed");
  assert(size >= 0, "int won't convert to size_t");
  // 在堆上分配指定size大小的内存。
  HeapWord* obj = common_mem_allocate_init(klass, size, CHECK_NULL);
  // 设置数组的length，_mark和_metadata属性
  post_allocation_setup_array(klass, obj, length);
  NOT_PRODUCT(Universe::heap()->check_for_bad_heap_word_value(obj, size));
  return (oop)obj;
}

oop CollectedHeap::array_allocate_nozero(KlassHandle klass,
                                         int size,
                                         int length,
                                         TRAPS) {
  debug_only(check_for_valid_allocation_state());
  assert(!Universe::heap()->is_gc_active(), "Allocation during gc not allowed");
  assert(size >= 0, "int won't convert to size_t");
  HeapWord* obj = common_mem_allocate_noinit(klass, size, CHECK_NULL);
  ((oop)obj)->set_klass_gap(0);
  // 设置数组的length，_mark和_metadata属性
  post_allocation_setup_array(klass, obj, length);
#ifndef PRODUCT
  const size_t hs = oopDesc::header_size()+1;
  Universe::heap()->check_for_non_bad_heap_word_value(obj+hs, size-hs);
#endif
  return (oop)obj;
}

inline void CollectedHeap::oop_iterate_no_header(OopClosure* cl) {
  NoHeaderExtendedOopClosure no_header_cl(cl);
  oop_iterate(&no_header_cl);
}

#ifndef PRODUCT

inline bool
CollectedHeap::promotion_should_fail(volatile size_t* count) {
  // Access to count is not atomic; the value does not have to be exact.
  if (PromotionFailureALot) {
    const size_t gc_num = total_collections();
    const size_t elapsed_gcs = gc_num - _promotion_failure_alot_gc_number;
    if (elapsed_gcs >= PromotionFailureALotInterval) {
      // Test for unsigned arithmetic wrap-around.
      if (++*count >= PromotionFailureALotCount) {
        *count = 0;
        return true;
      }
    }
  }
  return false;
}

inline bool CollectedHeap::promotion_should_fail() {
  return promotion_should_fail(&_promotion_failure_alot_count);
}

inline void CollectedHeap::reset_promotion_should_fail(volatile size_t* count) {
  if (PromotionFailureALot) {
    _promotion_failure_alot_gc_number = total_collections();
    *count = 0;
  }
}

inline void CollectedHeap::reset_promotion_should_fail() {
  reset_promotion_should_fail(&_promotion_failure_alot_count);
}
#endif  // #ifndef PRODUCT

#endif // SHARE_VM_GC_INTERFACE_COLLECTEDHEAP_INLINE_HPP
