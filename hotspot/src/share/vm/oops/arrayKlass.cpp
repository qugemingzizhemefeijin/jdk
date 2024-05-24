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

#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc_interface/collectedHeap.inline.hpp"
#include "jvmtifiles/jvmti.h"
#include "memory/gcLocker.hpp"
#include "memory/universe.inline.hpp"
#include "oops/arrayKlass.hpp"
#include "oops/arrayOop.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"

// 根据传入的子类的ArrayClass占用的内存空间来计算InstanceKlass需要占用的内存空间
// 注意，header_size属性的值应该是TypeArrayKlass/ObjArrayKlass类自身占用的内存空间，但是现在获取的是InstanceKlass类自身占用的内存空间。
// 这是因为InstanceKlass占用的内存比TypeArrayKlass大，有足够内存来存放相关数据。
// 更重要的是，为了统一从固定的偏移位置获取vtable等信息，在实际操作Klass实例的过程中无须关心是数组还是类，直接偏移固定位置后就可获取。
int ArrayKlass::static_size(int header_size) {
  // size of an array klass object
  assert(header_size <= InstanceKlass::header_size(), "bad header size");
  // If this assert fails, see comments in base_create_array_klass.
  header_size = InstanceKlass::header_size();
  int vtable_len = Universe::base_vtable_size();
#ifdef _LP64
  int size = header_size + align_object_offset(vtable_len);
#else
  int size = header_size + vtable_len;
#endif
  return align_object_size(size);
}


Klass* ArrayKlass::java_super() const {
  if (super() == NULL)  return NULL;  // bootstrap case
  // Array klasses have primary supertypes which are not reported to Java.
  // Example super chain:  String[][] -> Object[][] -> Object[] -> Object
  return SystemDictionary::Object_klass();
}


oop ArrayKlass::multi_allocate(int rank, jint* sizes, TRAPS) {
  ShouldNotReachHere();
  return NULL;
}

Method* ArrayKlass::uncached_lookup_method(Symbol* name, Symbol* signature) const {
  // There are no methods in an array klass but the super class (Object) has some
  assert(super(), "super klass must be present");
  return super()->uncached_lookup_method(name, signature);
}

ArrayKlass::ArrayKlass(Symbol* name) {
  set_name(name);

  // Universe::is_bootstrapping() 表示当前是否处于JVM的引导阶段
  set_super(Universe::is_bootstrapping() ? (Klass*)NULL : SystemDictionary::Object_klass());
  set_layout_helper(Klass::_lh_neutral_value);
  set_dimension(1);
  set_higher_dimension(NULL);
  set_lower_dimension(NULL);
  set_component_mirror(NULL);
  // Arrays don't add any new methods, so their vtable is the same size as
  // the vtable of klass Object.
  int vtable_size = Universe::base_vtable_size();
  set_vtable_length(vtable_size);
  set_is_cloneable(); // All arrays are considered to be cloneable (See JLS 20.1.5)
}


// Initialization of vtables and mirror object is done separatly from base_create_array_klass,
// since a GC can happen. At this point all instance variables of the ArrayKlass must be setup.
// vtables和镜像对象的初始化与 base_create_array_klass 分开完成，因为 GC 可能会发生。此时必须设置 ArrayKlass 的所有实例变量。
void ArrayKlass::complete_create_array_klass(ArrayKlass* k, KlassHandle super_klass, TRAPS) {
  ResourceMark rm(THREAD);
  // 初始化_primary_supers、 _super_check_offset、vtable表等属性。
  k->initialize_supers(super_klass(), CHECK);
  // 初始化虚函数表
  k->vtable()->initialize_vtable(false, CHECK);
  // create_mirror()函数对 _component_mirror 属性进行设置
  // hotspot/src/share/vm/classfile/javaClasses.cpp
  java_lang_Class::create_mirror(k, Handle(NULL), CHECK);
}

// 此方法会在 objArrayKlass中重写，typeArrayKlass没有重写
GrowableArray<Klass*>* ArrayKlass::compute_secondary_supers(int num_extra_slots) {
  // interfaces = { cloneable_klass, serializable_klass };
  assert(num_extra_slots == 0, "sanity of primitive array type");
  // Must share this for correct bootstrapping!
  // 数组类型的默认行为
  // 默认从 hotspot/src/share/vm/memory/universe.hpp 加载 _the_array_interfaces_array 属性
  // 参见 hotspot/src/share/vm/memory/universe.cpp 311行
  // SystemDictionary::Cloneable_klass()
  // SystemDictionary::Serializable_klass()
  set_secondary_supers(Universe::the_array_interfaces_array());
  return NULL;
}

bool ArrayKlass::compute_is_subtype_of(Klass* k) {
  // An array is a subtype of Serializable, Clonable, and Object
  return    k == SystemDictionary::Object_klass()
         || k == SystemDictionary::Cloneable_klass()
         || k == SystemDictionary::Serializable_klass();
}


inline intptr_t* ArrayKlass::start_of_vtable() const {
  // all vtables start at the same place, that's why we use InstanceKlass::header_size here
  return ((intptr_t*)this) + InstanceKlass::header_size();
}


klassVtable* ArrayKlass::vtable() const {
  KlassHandle kh(Thread::current(), this);
  return new klassVtable(kh, start_of_vtable(), vtable_length() / vtableEntry::size());
}


objArrayOop ArrayKlass::allocate_arrayArray(int n, int length, TRAPS) {
  if (length < 0) {
    THROW_0(vmSymbols::java_lang_NegativeArraySizeException());
  }
  if (length > arrayOopDesc::max_array_length(T_ARRAY)) {
    report_java_out_of_memory("Requested array size exceeds VM limit");
    JvmtiExport::post_array_size_exhausted();
    THROW_OOP_0(Universe::out_of_memory_error_array_size());
  }
  int size = objArrayOopDesc::object_size(length);
  Klass* k = array_klass(n+dimension(), CHECK_0);
  ArrayKlass* ak = ArrayKlass::cast(k);
  objArrayOop o =
    (objArrayOop)CollectedHeap::array_allocate(ak, size, length, CHECK_0);
  // initialization to NULL not necessary, area already cleared
  return o;
}

void ArrayKlass::array_klasses_do(void f(Klass* k, TRAPS), TRAPS) {
  Klass* k = this;
  // Iterate over this array klass and all higher dimensions
  while (k != NULL) {
    f(k, CHECK);
    k = ArrayKlass::cast(k)->higher_dimension();
  }
}

void ArrayKlass::array_klasses_do(void f(Klass* k)) {
  Klass* k = this;
  // Iterate over this array klass and all higher dimensions
  while (k != NULL) {
    f(k);
    k = ArrayKlass::cast(k)->higher_dimension();
  }
}

// GC support

void ArrayKlass::oops_do(OopClosure* cl) {
  Klass::oops_do(cl);

  cl->do_oop(adr_component_mirror());
}

// JVM support

jint ArrayKlass::compute_modifier_flags(TRAPS) const {
  return JVM_ACC_ABSTRACT | JVM_ACC_FINAL | JVM_ACC_PUBLIC;
}

// JVMTI support

jint ArrayKlass::jvmti_class_status() const {
  return JVMTI_CLASS_STATUS_ARRAY;
}

void ArrayKlass::remove_unshareable_info() {
  Klass::remove_unshareable_info();
  // Clear the java mirror
  set_component_mirror(NULL);
}

void ArrayKlass::restore_unshareable_info(TRAPS) {
  Klass::restore_unshareable_info(CHECK);
  // Klass recreates the component mirror also
}

// Printing

void ArrayKlass::print_on(outputStream* st) const {
  assert(is_klass(), "must be klass");
  Klass::print_on(st);
}

void ArrayKlass::print_value_on(outputStream* st) const {
  assert(is_klass(), "must be klass");
  for(int index = 0; index < dimension(); index++) {
    st->print("[]");
  }
}

void ArrayKlass::oop_print_on(oop obj, outputStream* st) {
  assert(obj->is_array(), "must be array");
  Klass::oop_print_on(obj, st);
  st->print_cr(" - length: %d", arrayOop(obj)->length());
}


// Verification

void ArrayKlass::verify_on(outputStream* st, bool check_dictionary) {
  Klass::verify_on(st, check_dictionary);

  if (component_mirror() != NULL) {
    guarantee(component_mirror()->klass() != NULL, "should have a class");
  }
}

void ArrayKlass::oop_verify_on(oop obj, outputStream* st) {
  guarantee(obj->is_array(), "must be array");
  arrayOop a = arrayOop(obj);
  guarantee(a->length() >= 0, "array with negative length?");
}
