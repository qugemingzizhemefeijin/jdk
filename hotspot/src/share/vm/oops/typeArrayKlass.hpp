/*
 * Copyright (c) 1997, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_OOPS_TYPEARRAYKLASS_HPP
#define SHARE_VM_OOPS_TYPEARRAYKLASS_HPP

#include "classfile/classLoaderData.hpp"
#include "oops/arrayKlass.hpp"

// A TypeArrayKlass is the klass of a typeArray
// It contains the type and size of the elements

// ArrayKlass的子类中有表示数组组件类型是Java基本类型的TypeArrayKlass，以及表示组件类型是对象类型的ObjArrayKlass。
// 数组类和普通类不同，数组类没有对应的Class文件，因此数组类是虚拟机直接创建的。
// HotSpot VM在初始化时就会创建Java中8个基本类型的一维数组实例TypeArrayKlass。
// TypeArrayKlass实例的内存布局
// |---------------------------------|
// |    TypeArrayKlass本身占用的内存    |
// |             vtable              |
// |---------------------------------|

// 最终此类初始化之后会与oop形成互联关系
// TypeArrayKlass -> _component_mirror  --->>> oop(表示java.lang.Class对象) -> _array_klass_offset 指向 TypeArrayKlass实例
class TypeArrayKlass : public ArrayKlass {
  friend class VMStructs;
 private:
  // 保存数组允许的最大长度
  jint _max_length;            // maximum number of elements allowed in an array

  // Constructor
  TypeArrayKlass(BasicType type, Symbol* name);
  static TypeArrayKlass* allocate(ClassLoaderData* loader_data, BasicType type, Symbol* name, TRAPS);
 public:
  TypeArrayKlass() {} // For dummy objects.

  // instance variables
  jint max_length()                     { return _max_length; }
  void set_max_length(jint m)           { _max_length = m;    }

  // testers
  bool oop_is_typeArray_slow() const    { return true; }

  // klass allocation 创建TypeArrayKlass实例
  static TypeArrayKlass* create_klass(BasicType type, const char* name_str,
                               TRAPS);
  static inline Klass* create_klass(BasicType type, int scale, TRAPS) {
    // external_name 获取基本类型的描述符 [I,[J等
    TypeArrayKlass* tak = create_klass(type, external_name(type), CHECK_NULL);
    assert(scale == (1 << tak->log2_element_size()), "scale must check out");
    return tak;
  }

  int oop_size(oop obj) const;

  bool compute_is_subtype_of(Klass* k);

  // Allocation
  typeArrayOop allocate_common(int length, bool do_zero, TRAPS);
  typeArrayOop allocate(int length, TRAPS) { return allocate_common(length, true, THREAD); }
  oop multi_allocate(int rank, jint* sizes, TRAPS);

  oop protection_domain() const { return NULL; }

  // Copying
  void  copy_array(arrayOop s, int src_pos, arrayOop d, int dst_pos, int length, TRAPS);

  // Iteration
  int oop_oop_iterate(oop obj, ExtendedOopClosure* blk);
  int oop_oop_iterate_m(oop obj, ExtendedOopClosure* blk, MemRegion mr);

  // Garbage collection
  void oop_follow_contents(oop obj);
  int  oop_adjust_pointers(oop obj);

  // Parallel Scavenge and Parallel Old
  PARALLEL_GC_DECLS

 protected:
  // Find n'th dimensional array
  virtual Klass* array_klass_impl(bool or_null, int n, TRAPS);

  // Returns the array class with this class as element type
  virtual Klass* array_klass_impl(bool or_null, TRAPS);

 public:
  // Casting from Klass*
  static TypeArrayKlass* cast(Klass* k) {
    assert(k->oop_is_typeArray(), "cast to TypeArrayKlass");
    return (TypeArrayKlass*) k;
  }

  // 根据传入的BasicType类型来获取基本类型的字符描述，如[I,[Z,[J
  static const char* external_name(BasicType type);

  // Sizing
  // 获取TypeArrayKlass类自身占用的内存空间。
  static int header_size()  { return sizeof(TypeArrayKlass)/HeapWordSize; }
  int size() const          { return ArrayKlass::static_size(header_size()); }

  // Initialization (virtual from Klass)
  void initialize(TRAPS);

 public:
  // Printing
#ifndef PRODUCT
  void oop_print_on(oop obj, outputStream* st);
#endif

  void print_on(outputStream* st) const;
  void print_value_on(outputStream* st) const;

 public:
  const char* internal_name() const;
};

#endif // SHARE_VM_OOPS_TYPEARRAYKLASS_HPP
