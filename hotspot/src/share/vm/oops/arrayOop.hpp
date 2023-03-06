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

#ifndef SHARE_VM_OOPS_ARRAYOOP_HPP
#define SHARE_VM_OOPS_ARRAYOOP_HPP

#include "memory/universe.inline.hpp"
#include "oops/oop.hpp"

// arrayOopDesc is the abstract baseclass for all arrays.  It doesn't
// declare pure virtual to enforce this because that would allocate a vtbl
// in each instance, which we don't want.

// The layout of array Oops is:
//
//  markOop
//  Klass*    // 32 bits if compressed but declared 64 in LP64.
//  length    // shares klass memory or allocated after declared fields.

// arrayOopDesc类的实例表示Java数组对象。 具体的基本类型数组或对象类型数组由具体的C++中定义的子类实例表示。
// 在HotSpot VM中，数组对象在内存中的布局可以分为三个区域：对象头（header）、对象字段数据（field data）和对齐填充（padding）。
// |------------------|
// |       _mark      |
// |     _metadata    |   对象头
// |     length       |
// |------------------|
// |                  |
// |  field data...   |   对象字段数据
// |                  |
// |------------------|
// |      padding     |   对齐填充
// |------------------|

// 与Java对象内存布局唯一不同的是，数组对象的对象头中还会存储数组的长度length，它占用的内存空间为4字节。
// 在64位下，存放_metadata的空间是8字节，_mark是8字节，length是4字节，对象头为20字节，由于要按8字节对齐，所以会填充4字节，最终占用24字节。
// 64位开启指针压缩的情况下，存放_metadata的空间是4字节，_mark是8字节，length是4字节，对象头为16字节。

// arrayOopDesc类有两个子类：
//   1:表示组件类型为基本类型的typeArrayOopDesc；
//   2:表示组件类型为对象类型的objArrayOopDesc；二维及二维以上的数组都用objArrayOopDesc的实例来表示
// 当需要创建typeArrayOopDesc/objArrayOopDesc实例时，通常会调用oopFactory类中的工厂方法。hotspot/src/share/vm/memory/oopFactory.hpp
class arrayOopDesc : public oopDesc {
  friend class VMStructs;

  // Interpreter/Compiler offsets

  // Header size computation.
  // The header is considered the oop part of this type plus the length.
  // Returns the aligned header_size_in_bytes.  This is not equivalent to
  // sizeof(arrayOopDesc) which should not appear in the code.
  // 在默认参数下， 存放_metadata的空间容量是8字节， _mark是8字节
  // length是4字节， 对象头为20字节， 由于要按8字节对齐， 所以会填充4字节
  // 最终占用24字节
  static int header_size_in_bytes() {
    size_t hs = align_size_up(length_offset_in_bytes() + sizeof(int),
                              HeapWordSize);
#ifdef ASSERT
    // make sure it isn't called before UseCompressedOops is initialized.
    static size_t arrayoopdesc_hs = 0;
    if (arrayoopdesc_hs == 0) arrayoopdesc_hs = hs;
    assert(arrayoopdesc_hs == hs, "header size can't change");
#endif // ASSERT
    return (int)hs;
  }

 public:
  // The _length field is not declared in C++.  It is allocated after the
  // declared nonstatic fields in arrayOopDesc if not compressed, otherwise
  // it occupies the second half of the _klass field in oopDesc.
  // 使用-XX:+UseCompressedClassPointers选项来压缩类指针， 默认值为true。
  // sizeof(arrayOopDesc)的返回值为16， 其中_mark和_metadata._klass各占用8字节。
  // 在压缩指针的情况下， _mark占用8字节，_metadata._narrowKlass占用4字节， 共12字节。
  static int length_offset_in_bytes() {
    return UseCompressedClassPointers ? klass_gap_offset_in_bytes() :
                               sizeof(arrayOopDesc);
  }

  // Returns the offset of the first element.
  // 获取数组头元素的字节数
  static int base_offset_in_bytes(BasicType type) {
    return header_size(type) * HeapWordSize;
  }

  // Returns the address of the first element.
  void* base(BasicType type) const {
    return (void*) (((intptr_t) this) + base_offset_in_bytes(type));
  }

  // Tells whether index is within bounds.
  bool is_within_bounds(int index) const        { return 0 <= index && index < length(); }

  // Accessors for instance variable which is not a C++ declared nonstatic
  // field.
  int length() const {
    return *(int*)(((intptr_t)this) + length_offset_in_bytes());
  }
  void set_length(int length) {
    *(int*)(((intptr_t)this) + length_offset_in_bytes()) = length;
  }

  // Should only be called with constants as argument
  // (will not constant fold otherwise)
  // Returns the header size in words aligned to the requirements of the
  // array object type.
  static int header_size(BasicType type) {
    size_t typesize_in_bytes = header_size_in_bytes();
    return (int)(Universe::element_type_should_be_aligned(type)
      ? align_object_offset(typesize_in_bytes/HeapWordSize)
      : typesize_in_bytes/HeapWordSize);
  }

  // Return the maximum length of an array of BasicType.  The length can passed
  // to typeArrayOop::object_size(scale, length, header_size) without causing an
  // overflow. We also need to make sure that this will not overflow a size_t on
  // 32 bit platforms when we convert it to a byte size.
  // 返回 BasicType 数组的最大长度。长度可以通过到 typeArrayOop::object_size(scale, length, header_size）计算而不会导致溢出。
  // 当我们将其转换为字节大小时，我们还需要确保在32位平台也不会溢出。
  static int32_t max_array_length(BasicType type) {
    assert(type >= 0 && type < T_CONFLICT, "wrong type");
    assert(type2aelembytes(type) != 0, "wrong type");

    const size_t max_element_words_per_size_t =
      align_size_down((SIZE_MAX/HeapWordSize - header_size(type)), MinObjAlignment);
    const size_t max_elements_per_size_t =
      HeapWordSize * max_element_words_per_size_t / type2aelembytes(type);
    if ((size_t)max_jint < max_elements_per_size_t) {
      // It should be ok to return max_jint here, but parts of the code
      // (CollectedHeap, Klass::oop_oop_iterate(), and more) uses an int for
      // passing around the size (in words) of an object. So, we need to avoid
      // overflowing an int when we add the header. See CRs 4718400 and 7110613.
      return align_size_down(max_jint - header_size(type), MinObjAlignment);
    }
    return (int32_t)max_elements_per_size_t;
  }

// for unit testing
#ifndef PRODUCT
  static bool check_max_length_overflow(BasicType type);
  static int32_t old_max_array_length(BasicType type);
  static void test_max_array_length();
#endif
};

#endif // SHARE_VM_OOPS_ARRAYOOP_HPP
