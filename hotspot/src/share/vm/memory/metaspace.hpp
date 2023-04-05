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
#ifndef SHARE_VM_MEMORY_METASPACE_HPP
#define SHARE_VM_MEMORY_METASPACE_HPP

#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "runtime/virtualspace.hpp"
#include "utilities/exceptions.hpp"

// Metaspace
//
// Metaspaces are Arenas for the VM's metadata.
// They are allocated one per class loader object, and one for the null
// bootstrap class loader
// Eventually for bootstrap loader we'll have a read-only section and read-write
// to write for DumpSharedSpaces and read for UseSharedSpaces
//
//    block X ---+       +-------------------+
//               |       |  Virtualspace     |
//               |       |                   |
//               |       |                   |
//               |       |-------------------|
//               |       || Chunk            |
//               |       ||                  |
//               |       ||----------        |
//               +------>||| block 0 |       |
//                       ||----------        |
//                       ||| block 1 |       |
//                       ||----------        |
//                       ||                  |
//                       |-------------------|
//                       |                   |
//                       |                   |
//                       +-------------------+
//

class ChunkManager;
class ClassLoaderData;
class Metablock;
class Metachunk;
class MetaWord;
class Mutex;
class outputStream;
class SpaceManager;
class VirtualSpaceList;

// Metaspaces each have a  SpaceManager and allocations
// are done by the SpaceManager.  Allocations are done
// out of the current Metachunk.  When the current Metachunk
// is exhausted, the SpaceManager gets a new one from
// the current VirtualSpace.  When the VirtualSpace is exhausted
// the SpaceManager gets a new one.  The SpaceManager
// also manages freelists of available Chunks.
//
// Currently the space manager maintains the list of
// virtual spaces and the list of chunks in use.  Its
// allocate() method returns a block for use as a
// quantum of metadata.

// 类的元数据信息被转移到了元空间中，保存类的元数据信息的Klass、Method、ConstMethod与ConstantPool等实例都是在元空间上分配内存。
// Metaspace区域位于堆外，因此它的内存大小取决于系统内存而不是堆大小，我们可以指定MaxMetaspaceSize参数来限定它的最大内存。
// Metaspace用来存放类的元数据信息，元数据信息用于记录一个Java类在JVM中的信息，包括以下几类信息：
// Klass结构： 可以理解为类在HotSpot VM内部的对等表示。
// Method与ConstMethod： 保存Java方法的相关信息，包括方法的字节码、局部变量表、异常表和参数信息等。
// ConstantPool： 保存常量池信息。
// 注解： 提供与程序有关的元数据信息，但是这些信息并不属于程序本身。
// 方法计数器： 记录方法被执行的次数，用来辅助JIT决策。

// 除了以上最主要的5项信息外，还有一些占用内存比较小的元数据信息也存放在Metaspace里。是什么呢？？？

// 虽然每个Java类都关联了一个java.lang.Class对象，而且是一个保存在堆中的Java对象，但是类的元数据信息不是一个Java对象，它不在堆中而是在Metaspace中。

// 元空间主要用来存储Java类的元数据信息，每个类加载器都会在元空间得到自己的存储区域。当一个类加载器被垃圾收集器标记为不再存活时，这块存储区域将会被回收。

// 在元空间中会保存一个VirtualSpace的单链表，每个链表的节点为VirtualSpaceNode，单链表之间通过VirtualSpaceNode类中定义的next属性连接。
// 每个VirtualSpaceNode节点在实际使用过程中会分出4种不同大小的Metachunk块(1K/4K/64K/4MB)，主要是增加灵活分配的同时便于复用，方便管理。

// 一个块只归属一个类加载器。类加载器可能会使用特定大小的块，如反射或者匿名类加载器，也可能根据其内部条目的数量使用小型或者中型的组块，如用户类加载器。

// 分出4种不同大小的Metachunk块，那么Metaspace的分配器是怎么知道一个类加载器每次要多大的chunk呢？这当然是基于猜测的。
// - 一个标准的类加载器在第一次申请空间时会得到一个4KB的chunk块，直到它达到了一个随意设置的阈值，此时分配器失去了耐心，会一次性分配一个64KB的chunk块。
// - bootstrap classloader是一个公认的会加载大量类的加载器，因此分配器会给它一个巨大的chunk，一开始就给它分配4MB。
//   可以通过InitialBootClassLoaderMetaspaceSize进行调优。
// - 反射类类加载器（jdk.internal.reflect.DelegatingClassLoader）和匿名类类加载器只会加载一个类，
//   所以一开始只会给它们一个非常小的chunk块（1KB），因为给太多是一种浪费。
// - 类加载器申请空间的时候，元空间每次都给类加载器分配一个chunk块，这种优化是建立在假设它们马上就需要新的空间的基础上。
//   这种假设可能正确也可能错误，可能在拿到一个很大的chunk后，这个类加载器恰巧不再需要加载新的类了。

// 链表VirtualSpaceList和节点Node是全局的，而Node内部的MetaChunk块是分配给每个类加载器的，因此一个Node通常由分配给多个类加载器的chunks组成。

// 只有当64位平台上启用了类指针压缩后才会存在类指针压缩空间（Compressed Class Pointer Space）。对于64位平台，为了压缩JVM对象中的_klass指针的大小，
// 引入了类指针压缩空间。对象中指向类元数据的指针会被压缩成32位。
// 元空间和类指针压缩空间的区别如下：
// - 类指针压缩空间只包含类的元数据，如InstanceKlass和ArrayKlass，虚拟机仅在打开了UseCompressedClassPointers选项时才生效。
//   为了提高性能，Java中的虚方法表也存放到这里。
// 元空间包含的是类里比较大的元数据，如方法、字节码和常量池等。


// 1.当一个VirtualSpaceListNode中的所有chunk块都处于空闲状态的时候，这个Virtual-SpaceListNode就会从链表VirtualSpaceList中移除，
// VirtualSpaceList中的所有chunk块也会从空闲列表中移除。此时这个VirtualSpaceListNode就不会再使用了，其内存会归还给操作系统。
// 2.对于一个空闲的VirtualSpaceListNode来说，拥有此空闲VirtualSpaceListNode中的chunk块的所有类加载器必然都已经被卸载了。
// 至于是否能让VirtualSpaceListNode中的所有chunk块都空闲，主要取决于碎片化的程度。
// 3.一个VirtualSpaceListNode的大小是2MB，chunk块的大小为1KB、4KB或64KB，因此一个VirtualSpaceListNode上通常有约150~200个chunk块。
// 如果这些chunk块全部归属于同一个类加载器，那么回收这个类加载器就可以一次性回收这个VirtualSpaceListNode，并且可以将其空间还给操作系统。
// 如果将这些chunk块分配给不同的类加载器，每个类加载器都有不同的生命周期，那么什么都不会被释放。
// 这是在告诉我们要小心对待大量的小的类加载器，如那些负责加载匿名类或反射类的加载器。

// 请注意，部分 Metaspace（压缩类空间）将永远不会释放回操作系统。
// - 内存由操作系统在 2MB 大小的区域中保留，并保存在全局链接列表中。这些地区承诺按需提供服务。
// - 这些区域被分割成块，然后交给类装入器。块属于一个类装入器。
// - 块被进一步分割成微小的分配，称为块。这些是分发给呼叫者的分配单元。
// - 当一个全局块被重新使用时，它拥有一个全局块。部分内存可能会被释放到操作系统中，但这在很大程度上取决于碎片化和运气。
class Metaspace : public CHeapObj<mtClass> {
  friend class VMStructs;
  friend class SpaceManager;
  friend class VM_CollectForMetadataAllocation;
  friend class MetaspaceGC;
  friend class MetaspaceAux;

 public:
  enum MetadataType {
    ClassType,
    NonClassType,
    MetadataTypeCount
  };
  enum MetaspaceType {
    StandardMetaspaceType,
    BootMetaspaceType,
    ROMetaspaceType,
    ReadWriteMetaspaceType,
    AnonymousMetaspaceType,
    ReflectionMetaspaceType
  };

 private:
  void initialize(Mutex* lock, MetaspaceType type);

  // Get the first chunk for a Metaspace.  Used for
  // special cases such as the boot class loader, reflection
  // class loader and anonymous class loader.
  Metachunk* get_initialization_chunk(MetadataType mdtype,
                                      size_t chunk_word_size,
                                      size_t chunk_bunch);

  // Align up the word size to the allocation word size
  static size_t align_word_size_up(size_t);

  // Aligned size of the metaspace.
  static size_t _compressed_class_space_size;

  static size_t compressed_class_space_size() {
    return _compressed_class_space_size;
  }
  static void set_compressed_class_space_size(size_t size) {
    _compressed_class_space_size = size;
  }

  // 以下代码中定义的属性都是成对出现，因为元空间其实还有类指针压缩空间（Compressed Class Pointer Space），这两部分是相互独立的。

  static size_t _first_chunk_word_size;
  static size_t _first_class_chunk_word_size;

  static size_t _commit_alignment;
  static size_t _reserve_alignment;

  // 被管理的SpaceManager实例，为类加载器管理分配的Metachunk块
  SpaceManager* _vsm;
  SpaceManager* vsm() const { return _vsm; }

  SpaceManager* _class_vsm;
  SpaceManager* class_vsm() const { return _class_vsm; }

  // Allocate space for metadata of type mdtype. This is space
  // within a Metachunk and is used by
  //   allocate(ClassLoaderData*, size_t, bool, MetadataType, TRAPS)
  // 从Metaspace实例中申请内存空间
  MetaWord* allocate(size_t word_size, MetadataType mdtype);

  // Virtual Space lists for both classes and other metadata
  static VirtualSpaceList* _space_list;
  static VirtualSpaceList* _class_space_list;

  static ChunkManager* _chunk_manager_metadata;
  static ChunkManager* _chunk_manager_class;

 public:
  static VirtualSpaceList* space_list()       { return _space_list; }
  static VirtualSpaceList* class_space_list() { return _class_space_list; }
  static VirtualSpaceList* get_space_list(MetadataType mdtype) {
    assert(mdtype != MetadataTypeCount, "MetadaTypeCount can't be used as mdtype");
    return mdtype == ClassType ? class_space_list() : space_list();
  }

  static ChunkManager* chunk_manager_metadata() { return _chunk_manager_metadata; }
  static ChunkManager* chunk_manager_class()    { return _chunk_manager_class; }
  static ChunkManager* get_chunk_manager(MetadataType mdtype) {
    assert(mdtype != MetadataTypeCount, "MetadaTypeCount can't be used as mdtype");
    return mdtype == ClassType ? chunk_manager_class() : chunk_manager_metadata();
  }

 private:
  // This is used by DumpSharedSpaces only, where only _vsm is used. So we will
  // maintain a single list for now.
  void record_allocation(void* ptr, MetaspaceObj::Type type, size_t word_size);

#ifdef _LP64
  static void set_narrow_klass_base_and_shift(address metaspace_base, address cds_base);

  // Returns true if can use CDS with metaspace allocated as specified address.
  static bool can_use_cds_with_metaspace_addr(char* metaspace_base, address cds_base);

  static void allocate_metaspace_compressed_klass_ptrs(char* requested_addr, address cds_base);

  static void initialize_class_space(ReservedSpace rs);
#endif

  class AllocRecord : public CHeapObj<mtClass> {
  public:
    AllocRecord(address ptr, MetaspaceObj::Type type, int byte_size)
      : _next(NULL), _ptr(ptr), _type(type), _byte_size(byte_size) {}
    AllocRecord *_next;
    address _ptr;
    MetaspaceObj::Type _type;
    int _byte_size;
  };

  AllocRecord * _alloc_record_head;
  AllocRecord * _alloc_record_tail;

  size_t class_chunk_size(size_t word_size);

 public:

  Metaspace(Mutex* lock, MetaspaceType type);
  ~Metaspace();

  static void ergo_initialize();
  static void global_initialize();

  static size_t first_chunk_word_size() { return _first_chunk_word_size; }
  static size_t first_class_chunk_word_size() { return _first_class_chunk_word_size; }

  static size_t reserve_alignment()       { return _reserve_alignment; }
  static size_t reserve_alignment_words() { return _reserve_alignment / BytesPerWord; }
  static size_t commit_alignment()        { return _commit_alignment; }
  static size_t commit_alignment_words()  { return _commit_alignment / BytesPerWord; }

  char*  bottom() const;
  size_t used_words_slow(MetadataType mdtype) const;
  size_t free_words_slow(MetadataType mdtype) const;
  size_t capacity_words_slow(MetadataType mdtype) const;

  size_t used_bytes_slow(MetadataType mdtype) const;
  size_t capacity_bytes_slow(MetadataType mdtype) const;

  // 从ClassLoaderData维护的元空间中申请内存（最后会调用到Metaspace实例方法的allocate函数）
  static MetaWord* allocate(ClassLoaderData* loader_data, size_t word_size,
                            bool read_only, MetaspaceObj::Type type, TRAPS);
  void deallocate(MetaWord* ptr, size_t byte_size, bool is_class);

  MetaWord* expand_and_allocate(size_t size,
                                MetadataType mdtype);

  static bool contains(const void *ptr);
  void dump(outputStream* const out) const;

  // Free empty virtualspaces
  static void purge(MetadataType mdtype);
  static void purge();

  static void report_metadata_oome(ClassLoaderData* loader_data, size_t word_size,
                                   MetadataType mdtype, TRAPS);

  void print_on(outputStream* st) const;
  // Debugging support
  void verify();

  class AllocRecordClosure :  public StackObj {
  public:
    virtual void doit(address ptr, MetaspaceObj::Type type, int byte_size) = 0;
  };

  void iterate(AllocRecordClosure *closure);

  // Return TRUE only if UseCompressedClassPointers is True and DumpSharedSpaces is False.
  static bool using_class_space() {
    return NOT_LP64(false) LP64_ONLY(UseCompressedClassPointers && !DumpSharedSpaces);
  }

  static bool is_class_space_allocation(MetadataType mdType) {
    return mdType == ClassType && using_class_space();
  }

};

class MetaspaceAux : AllStatic {
  static size_t free_chunks_total_words(Metaspace::MetadataType mdtype);

  // These methods iterate over the classloader data graph
  // for the given Metaspace type.  These are slow.
  static size_t used_bytes_slow(Metaspace::MetadataType mdtype);
  static size_t free_bytes_slow(Metaspace::MetadataType mdtype);
  static size_t capacity_bytes_slow(Metaspace::MetadataType mdtype);
  static size_t capacity_bytes_slow();

  // Running sum of space in all Metachunks that has been
  // allocated to a Metaspace.  This is used instead of
  // iterating over all the classloaders. One for each
  // type of Metadata
  static size_t _allocated_capacity_words[Metaspace:: MetadataTypeCount];
  // Running sum of space in all Metachunks that have
  // are being used for metadata. One for each
  // type of Metadata.
  static size_t _allocated_used_words[Metaspace:: MetadataTypeCount];

 public:
  // Decrement and increment _allocated_capacity_words
  static void dec_capacity(Metaspace::MetadataType type, size_t words);
  static void inc_capacity(Metaspace::MetadataType type, size_t words);

  // Decrement and increment _allocated_used_words
  static void dec_used(Metaspace::MetadataType type, size_t words);
  static void inc_used(Metaspace::MetadataType type, size_t words);

  // Total of space allocated to metadata in all Metaspaces.
  // This sums the space used in each Metachunk by
  // iterating over the classloader data graph
  static size_t used_bytes_slow() {
    return used_bytes_slow(Metaspace::ClassType) +
           used_bytes_slow(Metaspace::NonClassType);
  }

  // Used by MetaspaceCounters
  static size_t free_chunks_total_words();
  static size_t free_chunks_total_bytes();
  static size_t free_chunks_total_bytes(Metaspace::MetadataType mdtype);

  static size_t allocated_capacity_words(Metaspace::MetadataType mdtype) {
    return _allocated_capacity_words[mdtype];
  }
  static size_t allocated_capacity_words() {
    return allocated_capacity_words(Metaspace::NonClassType) +
           allocated_capacity_words(Metaspace::ClassType);
  }
  static size_t allocated_capacity_bytes(Metaspace::MetadataType mdtype) {
    return allocated_capacity_words(mdtype) * BytesPerWord;
  }
  static size_t allocated_capacity_bytes() {
    return allocated_capacity_words() * BytesPerWord;
  }

  static size_t allocated_used_words(Metaspace::MetadataType mdtype) {
    return _allocated_used_words[mdtype];
  }
  static size_t allocated_used_words() {
    return allocated_used_words(Metaspace::NonClassType) +
           allocated_used_words(Metaspace::ClassType);
  }
  static size_t allocated_used_bytes(Metaspace::MetadataType mdtype) {
    return allocated_used_words(mdtype) * BytesPerWord;
  }
  static size_t allocated_used_bytes() {
    return allocated_used_words() * BytesPerWord;
  }

  static size_t free_bytes();
  static size_t free_bytes(Metaspace::MetadataType mdtype);

  static size_t reserved_bytes(Metaspace::MetadataType mdtype);
  static size_t reserved_bytes() {
    return reserved_bytes(Metaspace::ClassType) +
           reserved_bytes(Metaspace::NonClassType);
  }

  static size_t committed_bytes(Metaspace::MetadataType mdtype);
  static size_t committed_bytes() {
    return committed_bytes(Metaspace::ClassType) +
           committed_bytes(Metaspace::NonClassType);
  }

  static size_t min_chunk_size_words();
  static size_t min_chunk_size_bytes() {
    return min_chunk_size_words() * BytesPerWord;
  }

  // Print change in used metadata.
  static void print_metaspace_change(size_t prev_metadata_used);
  static void print_on(outputStream * out);
  static void print_on(outputStream * out, Metaspace::MetadataType mdtype);

  static void print_class_waste(outputStream* out);
  static void print_waste(outputStream* out);
  static void dump(outputStream* out);
  static void verify_free_chunks();
  // Checks that the values returned by allocated_capacity_bytes() and
  // capacity_bytes_slow() are the same.
  static void verify_capacity();
  static void verify_used();
  static void verify_metrics();
};

// Metaspace are deallocated when their class loader are GC'ed.
// This class implements a policy for inducing GC's to recover
// Metaspaces.

// 此类不是像类名一样用来对Metaspace执行GC的，仅仅用来维护属性_capacity_until_GC，当Metaspace的已分配内存值达到该属性就会触发GC，
// GC结束后_capacity_until_GC的值会增加直到达到参数MaxMetaspaceSize设置的Metaspace的最大值。
class MetaspaceGC : AllStatic {

  // The current high-water-mark for inducing a GC.
  // When committed memory of all metaspaces reaches this value,
  // a GC is induced and the value is increased. Size is in bytes.
  static volatile intptr_t _capacity_until_GC;

  // For a CMS collection, signal that a concurrent collection should
  // be started.
  static bool _should_concurrent_collect;

  static uint _shrink_factor;

  static size_t shrink_factor() { return _shrink_factor; }
  void set_shrink_factor(uint v) { _shrink_factor = v; }

 public:

  // 此方法是在 universe_init() -> Metaspace::global_initialize() 中被触发
  static void initialize() { _capacity_until_GC = MetaspaceSize; }

  static size_t capacity_until_GC();
  static size_t inc_capacity_until_GC(size_t v);
  static size_t dec_capacity_until_GC(size_t v);

  static bool should_concurrent_collect() { return _should_concurrent_collect; }
  static void set_should_concurrent_collect(bool v) {
    _should_concurrent_collect = v;
  }

  // The amount to increase the high-water-mark (_capacity_until_GC)
  static size_t delta_capacity_until_GC(size_t bytes);

  // Tells if we have can expand metaspace without hitting set limits.
  static bool can_expand(size_t words, bool is_class);

  // Returns amount that we can expand without hitting a GC,
  // measured in words.
  static size_t allowed_expansion();

  // Calculate the new high-water mark at which to induce
  // a GC.
  static void compute_new_size();
};

#endif // SHARE_VM_MEMORY_METASPACE_HPP
