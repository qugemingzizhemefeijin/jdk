/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_CLASSFILE_CLASSLOADERDATA_HPP
#define SHARE_VM_CLASSFILE_CLASSLOADERDATA_HPP

#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceCounters.hpp"
#include "runtime/mutex.hpp"
#include "utilities/growableArray.hpp"

#if INCLUDE_TRACE
# include "utilities/ticks.hpp"
#endif

//
// A class loader represents a linkset. Conceptually, a linkset identifies
// the complete transitive closure of resolved links that a dynamic linker can
// produce.
//
// A ClassLoaderData also encapsulates the allocation space, called a metaspace,
// used by the dynamic linker to allocate the runtime representation of all
// the types it defines.
//
// ClassLoaderData are stored in the runtime representation of classes and the
// system dictionary, are roots of garbage collection, and provides iterators
// for root tracing and other GC operations.

class ClassLoaderData;
class JNIMethodBlock;
class JNIHandleBlock;
class Metadebug;

// GC root for walking class loader data created

// 相当于ClassLoaderData的一个管理类，方便遍历所有的ClassLoaderData，其定义的属性和方法都是静态的
class ClassLoaderDataGraph : public AllStatic {
  friend class ClassLoaderData;
  friend class ClassLoaderDataGraphMetaspaceIterator;
  friend class VMStructs;
 private:
  // All CLDs (except the null CLD) can be reached by walking _head->_next->...
  // 表示当前活跃的ClassLoaderData链表
  static ClassLoaderData* _head;
  // 表示即将被卸载的ClassLoaderData链表
  static ClassLoaderData* _unloading;
  // CMS support.
  static ClassLoaderData* _saved_head;

  static ClassLoaderData* add(Handle class_loader, bool anonymous, TRAPS);
  static void post_class_unload_events(void);
 public:
  // 用于查找某个java/lang/ClassLoader实例对应的ClassLoaderData，如果不存在则为该实例创建一个新的ClassLoaderData实例并添加到ClassLoaderDataGraph管理的ClassLoaderData链表中。
  // 注意ClassLoaderData指针的保存位置比较特殊，不是在ClassLoader实例的内存中，而是内存外，内存上方的8字节处。
  // 为什么这8字节在没有保存ClassLoaderData指针时是NULL了？因为Java对象创建的时候会保证对象间有8字节的空隙。
  static ClassLoaderData* find_or_create(Handle class_loader, TRAPS);
  // 触发unloading链表中所有ClassLoaderData的内存释放。
  static void purge();
  static void clear_claimed_marks();
  static void oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim);
  static void always_strong_oops_do(OopClosure* blk, KlassClosure* klass_closure, bool must_claim);
  static void keep_alive_oops_do(OopClosure* blk, KlassClosure* klass_closure, bool must_claim);
  static void classes_do(KlassClosure* klass_closure);
  static void classes_do(void f(Klass* const));
  static void loaded_classes_do(KlassClosure* klass_closure);
  static void classes_unloading_do(void f(Klass* const));
  // 找出失效的类加载器，并通过_unloading静态属性保存，多个失效的类加载器会形成一个单链表。
  // 遍历所有的活跃ClassLoaderData，判断其是否活跃，如果不再活跃则将其从活跃链表中移除，加入到不活跃的ClassLoaderData链表中，并通知该ClassLoaderData加载的所有Klass类加载器被卸载。
  static bool do_unloading(BoolObjectClosure* is_alive);

  // CMS support.
  static void remember_new_clds(bool remember) { _saved_head = (remember ? _head : NULL); }
  static GrowableArray<ClassLoaderData*>* new_clds();

  static void dump_on(outputStream * const out) PRODUCT_RETURN;
  static void dump() { dump_on(tty); }
  static void verify();

#ifndef PRODUCT
  // expensive test for pointer in metaspace for debugging
  static bool contains(address x);
  static bool contains_loader_data(ClassLoaderData* loader_data);
#endif

#if INCLUDE_TRACE
 private:
  static Ticks _class_unload_time;
  static void class_unload_event(Klass* const k);
#endif
};

// ClassLoaderData class

// 每个类加载器都会对应一个ClassLoaderData实例，该实例负责初始化并销毁一个ClassLoader实例对应的Metaspace。
// 类加载器共有4类，分别是根类加载器、反射类加载器、匿名类加载器和普通类加载器，其中反射类加载器和匿名类加载器比较少见，
// 根类加载器就是BootstrapClassLoader，其使用C++编写，其他的扩展类加载器、应用类加载器及自定义的加载器等都属于普通类加载器。
// 每个加载器都会对应一个Metaspace实例，创建出的实例由ClassLoaderData类中定义的_metaspace属性保存，以便进行管理。
class ClassLoaderData : public CHeapObj<mtClass> {
  friend class VMStructs;
 private:
  // 表示引用此ClassLoaderDat的对象，特殊场景下使用，这些引用对象无法通过GC遍历到
  class Dependencies VALUE_OBJ_CLASS_SPEC {
    objArrayOop _list_head;
    void locked_add(objArrayHandle last,
                    objArrayHandle new_dependency,
                    Thread* THREAD);
   public:
    Dependencies() : _list_head(NULL) {}
    Dependencies(TRAPS) : _list_head(NULL) {
      init(CHECK);
    }
    void add(Handle dependency, TRAPS);
    void init(TRAPS);
    void oops_do(OopClosure* f);
  };

  friend class ClassLoaderDataGraph;
  friend class ClassLoaderDataGraphMetaspaceIterator;
  friend class MetaDataFactory;
  friend class Method;

  // 启动类加载器对应的ClassLoaderData
  static ClassLoaderData * _the_null_class_loader_data;

  // 关联的Java ClassLoader实例
  oop _class_loader;          // oop used to uniquely identify a class loader
                              // class loader or a canonical class path
  // 依赖
  Dependencies _dependencies; // holds dependencies from this class loader
                              // data to others.

  // 类加载器对应的Metaspace实例
  Metaspace * _metaspace;  // Meta-space where meta-data defined by the
  // 分配内存的锁
  Mutex* _metaspace_lock;  // Locks the metaspace for allocations and setup.

  // 为true，表示被卸载了
  bool _unloading;         // true if this class loader goes away
  // 为true，表示ClassLoaderData是活跃的但是没有关联的活跃对象，比如匿名类的类加载器和启动类加载器实例，为true则不能被GC回收掉
  bool _keep_alive;        // if this CLD can be unloaded for anonymous loaders
  // 为true，表示ClassLoaderData主要加载匿名类
  bool _is_anonymous;      // if this CLD is for an anonymous class
  // 用来标记该ClassLoaderData已经被遍历过了
  volatile int _claimed;   // true if claimed, for example during GC traces.
                           // To avoid applying oop closure more than once.
                           // Has to be an int because we cas it.
  // 该ClassLoaderData加载的所有Klass
  Klass* _klasses;         // The classes defined by the class loader.
  // 保存所有常量池的引用
  JNIHandleBlock* _handles; // Handles to constant pool arrays

  // These method IDs are created for the class loader and set to NULL when the
  // class loader is unloaded.  They are rarely freed, only for redefine classes
  // and if they lose a data race in InstanceKlass.
  JNIMethodBlock*                  _jmethod_ids;

  // Metadata to be deallocated when it's safe at class unloading, when
  // this class loader isn't unloaded itself.
  // 需要被释放的从 Metaspace 中分配的内存
  GrowableArray<Metadata*>*      _deallocate_list;

  // Support for walking class loader data objects
  // 下一个ClassLoaderData
  ClassLoaderData* _next; /// Next loader_datas created

  // ReadOnly and ReadWrite metaspaces (static because only on the null
  // class loader for now).
  // 启动类加载器使用的只读的Metaspace，DumpSharedSpaces为true时使用
  static Metaspace* _ro_metaspace;
  // 启动类加载器使用的可读写的Metaspace，DumpSharedSpaces为true时使用
  static Metaspace* _rw_metaspace;

  void set_next(ClassLoaderData* next) { _next = next; }
  ClassLoaderData* next() const        { return _next; }

  ClassLoaderData(Handle h_class_loader, bool is_anonymous, Dependencies dependencies);
  ~ClassLoaderData();

  void set_metaspace(Metaspace* m) { _metaspace = m; }

  JNIHandleBlock* handles() const;
  void set_handles(JNIHandleBlock* handles);

  Mutex* metaspace_lock() const { return _metaspace_lock; }

  // GC interface.
  void clear_claimed()          { _claimed = 0; }
  bool claimed() const          { return _claimed == 1; }
  bool claim();

  void unload();
  bool keep_alive() const       { return _keep_alive; }
  // 判断类加载器是否还活着
  bool is_alive(BoolObjectClosure* is_alive_closure) const;
  void classes_do(void f(Klass*));
  void loaded_classes_do(KlassClosure* klass_closure);
  void classes_do(void f(InstanceKlass*));

  // Deallocate free list during class unloading.
  // 类加载器被卸载时调用来释放deallocate_list中的元数据
  void free_deallocate_list();

  // Allocate out of this class loader data
  MetaWord* allocate(size_t size);

 public:
  // Accessors
  Metaspace* metaspace_or_null() const     { return _metaspace; }

  static ClassLoaderData* the_null_class_loader_data() {
    return _the_null_class_loader_data;
  }

  bool is_anonymous() const { return _is_anonymous; }

  static void init_null_class_loader_data() {
    assert(_the_null_class_loader_data == NULL, "cannot initialize twice");
    assert(ClassLoaderDataGraph::_head == NULL, "cannot initialize twice");

    // We explicitly initialize the Dependencies object at a later phase in the initialization
    // 初始化 _the_null_class_loader_data
    _the_null_class_loader_data = new ClassLoaderData((oop)NULL, false, Dependencies());
    ClassLoaderDataGraph::_head = _the_null_class_loader_data;
    assert(_the_null_class_loader_data->is_the_null_class_loader_data(), "Must be");
    // DumpSharedSpaces默认为false
    if (DumpSharedSpaces) {
      _the_null_class_loader_data->initialize_shared_metaspaces();
    }
  }

  bool is_the_null_class_loader_data() const {
    return this == _the_null_class_loader_data;
  }
  bool is_ext_class_loader_data() const;

  // The Metaspace is created lazily so may be NULL.  This
  // method will allocate a Metaspace if needed.
  // 元空间是惰性创建的，所以可能是空的。这个方法将在需要时分配一个元空间。
  Metaspace* metaspace_non_null();

  oop class_loader() const      { return _class_loader; }

  // Returns true if this class loader data is for a loader going away.
  bool is_unloading() const     {
    assert(!(is_the_null_class_loader_data() && _unloading), "The null class loader can never be unloaded");
    return _unloading;
  }
  // Anonymous class loader data doesn't have anything to keep them from
  // being unloaded during parsing the anonymous class.
  void set_keep_alive(bool value) { _keep_alive = value; }

  unsigned int identity_hash() {
    return _class_loader == NULL ? 0 : _class_loader->identity_hash();
  }

  // Used when tracing from klasses.
  void oops_do(OopClosure* f, KlassClosure* klass_closure, bool must_claim);

  void classes_do(KlassClosure* klass_closure);

  JNIMethodBlock* jmethod_ids() const              { return _jmethod_ids; }
  void set_jmethod_ids(JNIMethodBlock* new_block)  { _jmethod_ids = new_block; }

  void print_value() { print_value_on(tty); }
  void print_value_on(outputStream* out) const;
  void dump(outputStream * const out) PRODUCT_RETURN;
  void verify();
  const char* loader_name();

  jobject add_handle(Handle h);

  // 用于将已经完成类加载的类对应的Klass加入到ClassLoaderData管理的Klass链表中
  void add_class(Klass* k);

  // 用于类卸载的时候将Klass从ClassLoaderData管理的Klass链表中移除
  void remove_class(Klass* k);

  // 记录引用了当前ClassLoaderData的Klass，这些Klass不能通过正常的GC遍历方式找到
  void record_dependency(Klass* to, TRAPS);
  void init_dependencies(TRAPS);

  // 用于临时保存需要被释放的Klass，Method等元数据的指针，因为这些元数据可能依然被使用，所以不能立即释放，只能等到类加载器被卸载了才释放。
  void add_to_deallocate_list(Metadata* m);

  static ClassLoaderData* class_loader_data(oop loader);
  static ClassLoaderData* class_loader_data_or_null(oop loader);
  static ClassLoaderData* anonymous_class_loader_data(oop loader, TRAPS);
  static void print_loader(ClassLoaderData *loader_data, outputStream *out);

  // CDS support
  Metaspace* ro_metaspace();
  Metaspace* rw_metaspace();
  void initialize_shared_metaspaces();
};

class ClassLoaderDataGraphMetaspaceIterator : public StackObj {
  ClassLoaderData* _data;
 public:
  ClassLoaderDataGraphMetaspaceIterator();
  ~ClassLoaderDataGraphMetaspaceIterator();
  bool repeat() { return _data != NULL; }
  Metaspace* get_next() {
    assert(_data != NULL, "Should not be NULL in call to the iterator");
    Metaspace* result = _data->metaspace_or_null();
    _data = _data->next();
    // This result might be NULL for class loaders without metaspace
    // yet.  It would be nice to return only non-null results but
    // there is no guarantee that there will be a non-null result
    // down the list so the caller is going to have to check.
    return result;
  }
};
#endif // SHARE_VM_CLASSFILE_CLASSLOADERDATA_HPP
