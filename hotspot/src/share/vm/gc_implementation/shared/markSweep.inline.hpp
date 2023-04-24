/*
 * Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_MARKSWEEP_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_MARKSWEEP_INLINE_HPP

#include "gc_implementation/shared/markSweep.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "utilities/stack.inline.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_ALL_GCS
#include "gc_implementation/parallelScavenge/psParallelCompact.hpp"
#endif // INCLUDE_ALL_GCS

inline void MarkSweep::mark_object(oop obj) {
  // some marks may contain information we need to preserve so we store them away
  // and overwrite the mark.  We'll restore it at the end of markSweep.
  // 对活跃对象进行标记
  markOop mark = obj->mark();
  obj->set_mark(markOopDesc::prototype()->set_marked());
  // 返回true时，表示需要保存原markOop，调用preserve_mark()函数保存对象和对应的对象头
  if (mark->must_be_preserved(obj)) {
    preserve_mark(obj, mark);
  }
}

inline void MarkSweep::follow_klass(Klass* klass) {
  oop op = klass->klass_holder();
  MarkSweep::mark_and_push(&op);
}

// 与执行YGC时的标记不同，这里不会进行边界判断，因此会标记年轻代和老年代，而且会调用follow_contents()和follow_stack()函数递归标记对象，是全量标记。
template <class T> inline void MarkSweep::follow_root(T* p) {
  assert(!Universe::heap()->is_in_reserved(p),
         "roots shouldn't be things within the heap");
  // p的类型为oop*，由于oop是oopDesc*类型，则p是oopDesc**类型获取的heap_oop是oop类型，也就是oopDesc*类型
  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop)) {    // 判断oopDesc*是否为NULL
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
    if (!obj->mark()->is_marked()) {
      // 调用mark_object()函数标记对象，借助栈递归标记对象，递归标记的过程就是遍历根集对象并标记，然后压入栈，
      // 再遍历栈中对象，然后标记活跃对象并将其所引用的其他对象压入栈。重复这个过程，直至栈空为止。
      mark_object(obj);
      // follow_contents()函数将当前活跃的对象所引用的对象标记并压入栈
      obj->follow_contents();
    }
  }
  follow_stack();
}

template <class T> inline void MarkSweep::mark_and_push(T* p) {
//  assert(Universe::heap()->is_in_reserved(p), "should be in object space");
  T heap_oop = oopDesc::load_heap_oop(p);
  // p的类型为oopDesc**，而heap_oop为oopDesc*
  if (!oopDesc::is_null(heap_oop)) {
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
    if (!obj->mark()->is_marked()) {
      // 标记对象并压入_mark_stack栈中，_mark_stack是定义在MarkSweep类中类型为Stack <oop, mtGC>的静态变量，递归标记就通过这个栈来完成。
      // 后续在MarkSweep::follow_root()中调用的follow_stack()函数会处理这个栈中存储的数据。
      mark_object(obj);
      _marking_stack.push(obj);
    }
  }
}

void MarkSweep::push_objarray(oop obj, size_t index) {
  ObjArrayTask task(obj, index);
  assert(task.is_valid(), "bad ObjArrayTask");
  _objarray_stack.push(task);
}

// T是oopDesc*类型，则p是oopDesc**类型
template <class T> inline void MarkSweep::adjust_pointer(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop)) {
    oop obj     = oopDesc::decode_heap_oop_not_null(heap_oop);
    oop new_obj = oop(obj->mark()->decode_pointer());
    assert(new_obj != NULL ||                         // is forwarding ptr?
           obj->mark() == markOopDesc::prototype() || // not gc marked?
           (UseBiasedLocking && obj->mark()->has_bias_pattern()),
                                                      // not gc marked?
           "should be forwarded");
    // 只有设置转发指针时，对象的引用地址才需要更新
    if (new_obj != NULL) {
      assert(Universe::heap()->is_in_reserved(new_obj),
             "should be in object space");
      // 执行*p=new_obj操作
      oopDesc::encode_store_heap_oop_not_null(p, new_obj);
    }
  }
}

template <class T> inline void MarkSweep::KeepAliveClosure::do_oop_work(T* p) {
  mark_and_push(p);
}

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_MARKSWEEP_INLINE_HPP
