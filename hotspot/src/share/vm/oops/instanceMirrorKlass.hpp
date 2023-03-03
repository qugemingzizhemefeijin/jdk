/*
 * Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_OOPS_INSTANCEMIRRORKLASS_HPP
#define SHARE_VM_OOPS_INSTANCEMIRRORKLASS_HPP

#include "classfile/systemDictionary.hpp"
#include "oops/instanceKlass.hpp"
#include "runtime/handles.hpp"
#include "utilities/macros.hpp"

// An InstanceMirrorKlass is a specialized InstanceKlass for
// java.lang.Class instances.  These instances are special because
// they contain the static fields of the class in addition to the
// normal fields of Class.  This means they are variable sized
// instances and need special logic for computing their size and for
// iteration of their oops.

// InstanceMirrorKlass类实例表示java.lang.Class类。
// java类及对象在Hotspot VM中的表示形式
// java.lang.Object等类       通过InstanceKlass等实例表示这些类
// java.lang.Class对象        通过oop实例表示这个对象
// java.lang.Class类          通过InstanceMirrorKlass实例表示这个类
class InstanceMirrorKlass: public InstanceKlass {
  friend class VMStructs;
  friend class InstanceKlass;

 private:
  // 正常情况下，HotSpot VM使用Klass表示Java类，用oop表示Java对象。
  // 而Java类中可能定义了静态或非静态字段，因此将非静态字段值存储在oop中，静态字段值存储在表示当前Java类的java.lang.Class对象中。

  // Class类比较特殊，是用InstanceMirrorKlass实例表示，但是Class对象是用oop对象表示的。由于本身Class类也定义了静态字段，这些值同样存储在oop对象中。
  // 这样静态和非静态字段就存储在一个oop上，需要参考 _offset_of_static_fields 属性的值进行偏移来定位静态字段的存储位置。
  static int _offset_of_static_fields;

  // Constructor
  InstanceMirrorKlass(int vtable_len, int itable_len, int static_field_size, int nonstatic_oop_map_size, ReferenceType rt, AccessFlags access_flags,  bool is_anonymous)
    : InstanceKlass(vtable_len, itable_len, static_field_size, nonstatic_oop_map_size, rt, access_flags, is_anonymous) {}

 public:
  InstanceMirrorKlass() { assert(DumpSharedSpaces || UseSharedSpaces, "only for CDS"); }
  // Type testing
  bool oop_is_instanceMirror() const             { return true; }

  // Casting from Klass*
  static InstanceMirrorKlass* cast(Klass* k) {
    assert(k->oop_is_instanceMirror(), "cast to InstanceMirrorKlass");
    return (InstanceMirrorKlass*) k;
  }

  // Returns the size of the instance including the extra static fields.
  virtual int oop_size(oop obj) const;

  // Static field offset is an offset into the Heap, should be converted by
  // based on UseCompressedOop for traversal
  static HeapWord* start_of_static_fields(oop obj) {
    return (HeapWord*)(cast_from_oop<intptr_t>(obj) + offset_of_static_fields());
  }

  // 初始化静态字段的偏移量值
  static void init_offset_of_static_fields() {
    // Cache the offset of the static fields in the Class instance
    assert(_offset_of_static_fields == 0, "once");
    // 调用size_helper()函数获取oop（表示java.lang.Class对象）的大小，左移3位将字转换为字节。 紧接着oop后开始存储静态字段的值。
    _offset_of_static_fields = InstanceMirrorKlass::cast(SystemDictionary::Class_klass())->size_helper() << LogHeapWordSize;
  }

  static int offset_of_static_fields() {
    return _offset_of_static_fields;
  }

  int compute_static_oop_field_count(oop obj);

  // Given a Klass return the size of the instance
  int instance_size(KlassHandle k);

  // allocation
  instanceOop allocate_instance(KlassHandle k, TRAPS);

  // Garbage collection
  int  oop_adjust_pointers(oop obj);
  void oop_follow_contents(oop obj);

  // Parallel Scavenge and Parallel Old
  PARALLEL_GC_DECLS

  int oop_oop_iterate(oop obj, ExtendedOopClosure* blk) {
    return oop_oop_iterate_v(obj, blk);
  }
  int oop_oop_iterate_m(oop obj, ExtendedOopClosure* blk, MemRegion mr) {
    return oop_oop_iterate_v_m(obj, blk, mr);
  }

#define InstanceMirrorKlass_OOP_OOP_ITERATE_DECL(OopClosureType, nv_suffix)           \
  int oop_oop_iterate##nv_suffix(oop obj, OopClosureType* blk);                       \
  int oop_oop_iterate##nv_suffix##_m(oop obj, OopClosureType* blk, MemRegion mr);

  ALL_OOP_OOP_ITERATE_CLOSURES_1(InstanceMirrorKlass_OOP_OOP_ITERATE_DECL)
  ALL_OOP_OOP_ITERATE_CLOSURES_2(InstanceMirrorKlass_OOP_OOP_ITERATE_DECL)

#if INCLUDE_ALL_GCS
#define InstanceMirrorKlass_OOP_OOP_ITERATE_BACKWARDS_DECL(OopClosureType, nv_suffix) \
  int oop_oop_iterate_backwards##nv_suffix(oop obj, OopClosureType* blk);

  ALL_OOP_OOP_ITERATE_CLOSURES_1(InstanceMirrorKlass_OOP_OOP_ITERATE_BACKWARDS_DECL)
  ALL_OOP_OOP_ITERATE_CLOSURES_2(InstanceMirrorKlass_OOP_OOP_ITERATE_BACKWARDS_DECL)
#endif // INCLUDE_ALL_GCS
};

#endif // SHARE_VM_OOPS_INSTANCEMIRRORKLASS_HPP
