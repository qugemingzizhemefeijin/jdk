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
#include "precompiled.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "memory/allocation.hpp"
#include "memory/binaryTreeDictionary.hpp"
#include "memory/freeList.hpp"
#include "memory/collectorPolicy.hpp"
#include "memory/filemap.hpp"
#include "memory/freeList.hpp"
#include "memory/gcLocker.hpp"
#include "memory/metachunk.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "runtime/atomic.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/mutex.hpp"
#include "runtime/orderAccess.hpp"
#include "services/memTracker.hpp"
#include "services/memoryService.hpp"
#include "utilities/copy.hpp"
#include "utilities/debug.hpp"

typedef BinaryTreeDictionary<Metablock, FreeList> BlockTreeDictionary;
typedef BinaryTreeDictionary<Metachunk, FreeList> ChunkTreeDictionary;

// Set this constant to enable slow integrity checking of the free chunk lists
const bool metaspace_slow_verify = false;

size_t const allocation_from_dictionary_limit = 4 * K;

MetaWord* last_allocated = 0;

size_t Metaspace::_compressed_class_space_size;

// Used in declarations in SpaceManager and ChunkManager
enum ChunkIndex {
  ZeroIndex = 0,
  SpecializedIndex = ZeroIndex,
  SmallIndex = SpecializedIndex + 1,
  MediumIndex = SmallIndex + 1,
  HumongousIndex = MediumIndex + 1,
  NumberOfFreeLists = 3,
  NumberOfInUseLists = 4
};

enum ChunkSizes {    // in words.   // 注意，这个是字，一般一个字为8字节
  ClassSpecializedChunk = 128,      // 1K
  SpecializedChunk = 128,           // 1K
  ClassSmallChunk = 256,            // 2K
  SmallChunk = 512,                 // 4K
  ClassMediumChunk = 4 * K,         // 32K
  MediumChunk = 8 * K               // 64K
};

static ChunkIndex next_chunk_index(ChunkIndex i) {
  assert(i < NumberOfInUseLists, "Out of bound");
  return (ChunkIndex) (i+1);
}

volatile intptr_t MetaspaceGC::_capacity_until_GC = 0;
uint MetaspaceGC::_shrink_factor = 0;
bool MetaspaceGC::_should_concurrent_collect = false;

typedef class FreeList<Metachunk> ChunkList;

// Manages the global free lists of chunks.
// 管理着所有类加载器卸载后释放的内存块Metachunk。
class ChunkManager : public CHeapObj<mtInternal> {
  friend class TestVirtualSpaceNodeTest;

  // Free list of chunks of different sizes.    // 空闲列表中含有以下4种尺寸的块：
  //   SpecializedChunk 128K
  //   SmallChunk       512K
  //   MediumChunk      8M
  //   HumongousChunk   8M+                     // 书上说是不管理超大块，超大块由 _humongous_dictionary 属性管理
  ChunkList _free_chunks[NumberOfFreeLists];

  //   HumongousChunk
  // 超大的块通过字典来保存
  ChunkTreeDictionary _humongous_dictionary;

  // ChunkManager in all lists of this type
  size_t _free_chunks_total;
  size_t _free_chunks_count;

  void dec_free_chunks_total(size_t v) {
    assert(_free_chunks_count > 0 &&
             _free_chunks_total > 0,
             "About to go negative");
    // 总个数减1
    Atomic::add_ptr(-1, &_free_chunks_count);
    jlong minus_v = (jlong) - (jlong) v;
    // 总内存空间减少v
    Atomic::add_ptr(minus_v, &_free_chunks_total);
  }

  // Debug support

  size_t sum_free_chunks();
  size_t sum_free_chunks_count();

  void locked_verify_free_chunks_total();
  void slow_locked_verify_free_chunks_total() {
    if (metaspace_slow_verify) {
      locked_verify_free_chunks_total();
    }
  }
  void locked_verify_free_chunks_count();
  void slow_locked_verify_free_chunks_count() {
    if (metaspace_slow_verify) {
      locked_verify_free_chunks_count();
    }
  }
  void verify_free_chunks_count();

 public:

  ChunkManager(size_t specialized_size, size_t small_size, size_t medium_size)
      : _free_chunks_total(0), _free_chunks_count(0) {
    _free_chunks[SpecializedIndex].set_size(specialized_size);
    _free_chunks[SmallIndex].set_size(small_size);
    _free_chunks[MediumIndex].set_size(medium_size);
  }

  // add or delete (return) a chunk to the global freelist.
  Metachunk* chunk_freelist_allocate(size_t word_size);

  // Map a size to a list index assuming that there are lists
  // for special, small, medium, and humongous chunks.
  static ChunkIndex list_index(size_t size);

  // Remove the chunk from its freelist.  It is
  // expected to be on one of the _free_chunks[] lists.
  void remove_chunk(Metachunk* chunk);

  // Add the simple linked list of chunks to the freelist of chunks
  // of type index.
  void return_chunks(ChunkIndex index, Metachunk* chunks);

  // Total of the space in the free chunks list
  size_t free_chunks_total_words();
  size_t free_chunks_total_bytes();

  // Number of chunks in the free chunks list
  size_t free_chunks_count();

  void inc_free_chunks_total(size_t v, size_t count = 1) {
    Atomic::add_ptr(count, &_free_chunks_count);
    Atomic::add_ptr(v, &_free_chunks_total);
  }
  ChunkTreeDictionary* humongous_dictionary() {
    return &_humongous_dictionary;
  }

  ChunkList* free_chunks(ChunkIndex index);

  // Returns the list for the given chunk word size.
  ChunkList* find_free_chunks_list(size_t word_size);

  // Remove from a list by size.  Selects list based on size of chunk.
  Metachunk* free_chunks_get(size_t chunk_word_size);

  // Debug support
  void verify();
  void slow_verify() {
    if (metaspace_slow_verify) {
      verify();
    }
  }
  void locked_verify();
  void slow_locked_verify() {
    // metaspace_slow_verify默认为false，dubug模式下为true
    if (metaspace_slow_verify) {
      locked_verify();
    }
  }
  void verify_free_chunks_total();

  void locked_print_free_chunks(outputStream* st);
  void locked_print_sum_free_chunks(outputStream* st);

  void print_on(outputStream* st) const;
};

// Used to manage the free list of Metablocks (a block corresponds
// to the allocation of a quantum of metadata).
// 用来管理空闲的Metablock，所有空闲的Metablock都被添加到支持按照空闲空间大小排序和查找的二叉树BlockTreeDictionary中。
class BlockFreelist VALUE_OBJ_CLASS_SPEC {

  // 属性会在第一次调用return_block函数时归还空闲Metablock时初始化
  BlockTreeDictionary* _dictionary;

  // Only allocate and split from freelist if the size of the allocation
  // is at least 1/4th the size of the available block.
  // 调用get_block()获取满足指定大小的空闲Metablock时使用，要求查找到的空闲Metablock的大小不能超过目标大小的WasteMultiplier倍
  const static int WasteMultiplier = 4;

  // Accessors
  BlockTreeDictionary* dictionary() const { return _dictionary; }

 public:
  BlockFreelist();
  ~BlockFreelist();

  // Get and return a block to the free list
  // 获取一个空闲的Block
  MetaWord* get_block(size_t word_size);
  // 将空闲的内存空间存放到 BlockFreelist 中，在此方法内会构造一个 Metablock 实例
  void return_block(MetaWord* p, size_t word_size);

  size_t total_size() {
  if (dictionary() == NULL) {
    return 0;
  } else {
    return dictionary()->total_size();
  }
}

  void print_on(outputStream* st) const;
};

// A VirtualSpaceList node.
class VirtualSpaceNode : public CHeapObj<mtClass> {
  friend class VirtualSpaceList;

  // Link to next VirtualSpaceNode
  VirtualSpaceNode* _next;

  // total in the VirtualSpace
  MemRegion _reserved;
  ReservedSpace _rs;
  VirtualSpace _virtual_space;
  MetaWord* _top;
  // count of chunks contained in this VirtualSpace
  uintx _container_count;

  // Convenience functions to access the _virtual_space
  char* low()  const { return virtual_space()->low(); }
  char* high() const { return virtual_space()->high(); }

  // The first Metachunk will be allocated at the bottom of the
  // VirtualSpace
  Metachunk* first_chunk() { return (Metachunk*) bottom(); }

  // Committed but unused space in the virtual space
  size_t free_words_in_vs() const;
 public:

  VirtualSpaceNode(size_t byte_size);
  VirtualSpaceNode(ReservedSpace rs) : _top(NULL), _next(NULL), _rs(rs), _container_count(0) {}
  ~VirtualSpaceNode();

  // Convenience functions for logical bottom and end
  MetaWord* bottom() const { return (MetaWord*) _virtual_space.low(); }
  MetaWord* end() const { return (MetaWord*) _virtual_space.high(); }

  size_t reserved_words() const  { return _virtual_space.reserved_size() / BytesPerWord; }
  size_t committed_words() const { return _virtual_space.actual_committed_size() / BytesPerWord; }

  bool is_pre_committed() const { return _virtual_space.special(); }

  // address of next available space in _virtual_space;
  // Accessors
  VirtualSpaceNode* next() { return _next; }
  void set_next(VirtualSpaceNode* v) { _next = v; }

  void set_reserved(MemRegion const v) { _reserved = v; }
  void set_top(MetaWord* v) { _top = v; }

  // Accessors
  MemRegion* reserved() { return &_reserved; }
  VirtualSpace* virtual_space() const { return (VirtualSpace*) &_virtual_space; }

  // Returns true if "word_size" is available in the VirtualSpace
  bool is_available(size_t word_size) { return _top + word_size <= end(); }

  MetaWord* top() const { return _top; }
  void inc_top(size_t word_size) { _top += word_size; }

  uintx container_count() { return _container_count; }
  void inc_container_count();
  void dec_container_count();
#ifdef ASSERT
  uint container_count_slow();
  void verify_container_count();
#endif

  // used and capacity in this single entry in the list
  size_t used_words_in_vs() const;
  size_t capacity_words_in_vs() const;

  bool initialize();

  // get space from the virtual space
  Metachunk* take_from_committed(size_t chunk_word_size);

  // Allocate a chunk from the virtual space and return it.
  // 从virtual_space中分配一段指定大小的内存空间
  Metachunk* get_chunk_vs(size_t chunk_word_size);

  // Expands/shrinks the committed space in a virtual space.  Delegates
  // to Virtualspace
  bool expand_by(size_t min_words, size_t preferred_words);

  // In preparation for deleting this node, remove all the chunks
  // in the node from any freelist.
  void purge(ChunkManager* chunk_manager);

  // If an allocation doesn't fit in the current node a new node is created.
  // Allocate chunks out of the remaining committed space in this node
  // to avoid wasting that memory.
  // This always adds up because all the chunk sizes are multiples of
  // the smallest chunk size.
  void retire(ChunkManager* chunk_manager);

#ifdef ASSERT
  // Debug support
  void mangle();
#endif

  void print_on(outputStream* st) const;
};

#define assert_is_ptr_aligned(ptr, alignment) \
  assert(is_ptr_aligned(ptr, alignment),      \
    err_msg(PTR_FORMAT " is not aligned to "  \
      SIZE_FORMAT, ptr, alignment))

#define assert_is_size_aligned(size, alignment) \
  assert(is_size_aligned(size, alignment),      \
    err_msg(SIZE_FORMAT " is not aligned to "   \
       SIZE_FORMAT, size, alignment))


// Decide if large pages should be committed when the memory is reserved.
// 确定在保留内存时是否应提交大页
static bool should_commit_large_pages_when_reserving(size_t bytes) {
  if (UseLargePages && UseLargePagesInMetaspace && !os::can_commit_large_page_memory()) {
    size_t words = bytes / BytesPerWord;
    bool is_class = false; // We never reserve large pages for the class space.
    // 如果当前Metaspace的剩余容量允许扩展指定大小
    if (MetaspaceGC::can_expand(words, is_class) &&
        MetaspaceGC::allowed_expansion() >= words) {
      return true;
    }
  }

  return false;
}

  // byte_size is the size of the associated virtualspace.
VirtualSpaceNode::VirtualSpaceNode(size_t bytes) : _top(NULL), _next(NULL), _rs(), _container_count(0) {
  assert_is_size_aligned(bytes, Metaspace::reserve_alignment());

  // This allocates memory with mmap.  For DumpSharedspaces, try to reserve
  // configurable address, generally at the top of the Java heap so other
  // memory addresses don't conflict.
  // DumpSharedSpaces表示将加载的类Dump到一个文件中给其他的JVM使用，默认为false，如果为true则申请一段连续的内存时需要
  // 从Java堆空间的顶部申请，避免地址冲突
  if (DumpSharedSpaces) {
    bool large_pages = false; // No large pages when dumping the CDS archive.
    // SharedBaseAddress表示共享内存区域的基地址，64位下是32G，即从32G往后尝试申请一段连续的内存空间
    char* shared_base = (char*)align_ptr_up((char*)SharedBaseAddress, Metaspace::reserve_alignment());

    _rs = ReservedSpace(bytes, Metaspace::reserve_alignment(), large_pages, shared_base, 0);
    if (_rs.is_reserved()) {
      // 分配成功
      assert(shared_base == 0 || _rs.base() == shared_base, "should match");
    } else {
      // Get a mmap region anywhere if the SharedBaseAddress fails.
      // 在SharedBaseAddress上分配失败，则重试，不指定起始分配地址
      _rs = ReservedSpace(bytes, Metaspace::reserve_alignment(), large_pages);
    }
    MetaspaceShared::set_shared_rs(&_rs);
  } else {
    bool large_pages = should_commit_large_pages_when_reserving(bytes);

    _rs = ReservedSpace(bytes, Metaspace::reserve_alignment(), large_pages);
  }

  // 如果申请成功
  if (_rs.is_reserved()) {
    // 校验分配的地址空间是否符合要求
    assert(_rs.base() != NULL, "Catch if we get a NULL address");
    assert(_rs.size() != 0, "Catch if we get a 0 size");
    assert_is_ptr_aligned(_rs.base(), Metaspace::reserve_alignment());
    assert_is_size_aligned(_rs.size(), Metaspace::reserve_alignment());
    // 记录日志
    MemTracker::record_virtual_memory_type((address)_rs.base(), mtClass);
  }
}

// 用于将此VirtualSpaceNode保存的所有的Metachunk从ChunkManager管理的Metachunk freeList中移除，删除此VirtualSpaceNode时调用
void VirtualSpaceNode::purge(ChunkManager* chunk_manager) {
  Metachunk* chunk = first_chunk();
  Metachunk* invalid_chunk = (Metachunk*) top();
  // 按照起始内存地址遍历所有的Chunk，因为他们是地址连续的
  while (chunk < invalid_chunk ) {
    assert(chunk->is_tagged_free(), "Should be tagged free");
    MetaWord* next = ((MetaWord*)chunk) + chunk->word_size();
    // 移除目标Metachunk
    chunk_manager->remove_chunk(chunk);
    // 校验移除是否正常完成
    assert(chunk->next() == NULL &&
           chunk->prev() == NULL,
           "Was not removed from its list");
    chunk = (Metachunk*) next;
  }
}

#ifdef ASSERT
uint VirtualSpaceNode::container_count_slow() {
  uint count = 0;
  Metachunk* chunk = first_chunk();
  Metachunk* invalid_chunk = (Metachunk*) top();
  // 根据内存地址遍历所有的Metachunk，统计Metachunk的个数
  while (chunk < invalid_chunk ) {
    MetaWord* next = ((MetaWord*)chunk) + chunk->word_size();
    // Don't count the chunks on the free lists.  Those are
    // still part of the VirtualSpaceNode but not currently
    // counted.
    if (!chunk->is_tagged_free()) {
      count++;
    }
    chunk = (Metachunk*) next;
  }
  return count;
}
#endif

// List of VirtualSpaces for metadata allocation.
class VirtualSpaceList : public CHeapObj<mtClass> {
  friend class VirtualSpaceNode;

  enum VirtualSpaceSizes {
    VirtualSpaceSize = 256 * K
  };

  // Head of the list
  VirtualSpaceNode* _virtual_space_list;
  // virtual space currently being used for allocations
  VirtualSpaceNode* _current_virtual_space;

  // Is this VirtualSpaceList used for the compressed class space
  bool _is_class;

  // Sum of reserved and committed memory in the virtual spaces
  size_t _reserved_words;
  size_t _committed_words;

  // Number of virtual spaces
  size_t _virtual_space_count;

  ~VirtualSpaceList();

  VirtualSpaceNode* virtual_space_list() const { return _virtual_space_list; }

  void set_virtual_space_list(VirtualSpaceNode* v) {
    _virtual_space_list = v;
  }
  void set_current_virtual_space(VirtualSpaceNode* v) {
    _current_virtual_space = v;
  }

  void link_vs(VirtualSpaceNode* new_entry);

  // Get another virtual space and add it to the list.  This
  // is typically prompted by a failed attempt to allocate a chunk
  // and is typically followed by the allocation of a chunk.
  bool create_new_virtual_space(size_t vs_word_size);

  // Chunk up the unused committed space in the current
  // virtual space and add the chunks to the free list.
  void retire_current_virtual_space();

 public:
  VirtualSpaceList(size_t word_size);
  VirtualSpaceList(ReservedSpace rs);

  size_t free_bytes();

  // 从 VirtualSpaceList 中获取Metachunk块
  Metachunk* get_new_chunk(size_t word_size,
                           size_t grow_chunks_by_words,
                           size_t medium_chunk_bunch);

  bool expand_node_by(VirtualSpaceNode* node,
                      size_t min_words,
                      size_t preferred_words);

  bool expand_by(size_t min_words,
                 size_t preferred_words);

  VirtualSpaceNode* current_virtual_space() {
    return _current_virtual_space;
  }

  bool is_class() const { return _is_class; }

  bool initialization_succeeded() { return _virtual_space_list != NULL; }

  size_t reserved_words()  { return _reserved_words; }
  size_t reserved_bytes()  { return reserved_words() * BytesPerWord; }
  size_t committed_words() { return _committed_words; }
  size_t committed_bytes() { return committed_words() * BytesPerWord; }

  void inc_reserved_words(size_t v);
  void dec_reserved_words(size_t v);
  void inc_committed_words(size_t v);
  void dec_committed_words(size_t v);
  void inc_virtual_space_count();
  void dec_virtual_space_count();

  // Unlink empty VirtualSpaceNodes and free it.
  void purge(ChunkManager* chunk_manager);

  bool contains(const void *ptr);

  void print_on(outputStream* st) const;

  class VirtualSpaceListIterator : public StackObj {
    VirtualSpaceNode* _virtual_spaces;
   public:
    VirtualSpaceListIterator(VirtualSpaceNode* virtual_spaces) :
      _virtual_spaces(virtual_spaces) {}

    bool repeat() {
      return _virtual_spaces != NULL;
    }

    VirtualSpaceNode* get_next() {
      VirtualSpaceNode* result = _virtual_spaces;
      if (_virtual_spaces != NULL) {
        _virtual_spaces = _virtual_spaces->next();
      }
      return result;
    }
  };
};

class Metadebug : AllStatic {
  // Debugging support for Metaspaces
  static int _allocation_fail_alot_count;

 public:

  static void init_allocation_fail_alot_count();
#ifdef ASSERT
  static bool test_metadata_failure();
#endif
};

int Metadebug::_allocation_fail_alot_count = 0;

//  SpaceManager - used by Metaspace to handle allocations
// 管理每个类加载器正在使用的Metachunk块
class SpaceManager : public CHeapObj<mtClass> {
  friend class Metaspace;
  friend class Metadebug;

 private:

  // protects allocations and contains.
  Mutex* const _lock;

  // Type of metadata allocated.
  // 分配的元数据区类型，分为类相关metadata和非类相关metadata
  Metaspace::MetadataType _mdtype;

  // SpaceManager类定义中的_chunks_in_use是一个存储4个Metachunk*类型的数组，每个槽上存储一个链表结构，链表结构中的Metachunk块大小相同。
  // 例如下标索引为SpecializedIndex的槽位上存储的是指定的1K的Metachunk块。
  // 参见枚举 ChunkSizes。
  //

  // List of chunks in use by this SpaceManager.  Allocations
  // are done from the current chunk.  The list is used for deallocating
  // chunks when the SpaceManager is freed.
  // 保存所有当前类加载器使用的Metachunk块，NumberOfInUseLists是ChunkIndex枚举类型，默认值为4。
  // hotspot/src/share/vm/memory/metaspace.cpp 64行
  Metachunk* _chunks_in_use[NumberOfInUseLists];
  // 当前用来分配内存的Metachunk块
  Metachunk* _current_chunk;

  // Number of small chunks to allocate to a manager
  // If class space manager, small chunks are unlimited
  static uint const _small_chunk_limit;         // SpaceManager所能分配的small chunks的数量上限，ClassType类型的Metaspace没有此限制

  // Sum of all space in allocated chunks
  size_t _allocated_blocks_words;               // 所有block所占的内存大小

  // Sum of all allocated chunks
  size_t _allocated_chunks_words;               // 所有chunk所占的内存大小
  size_t _allocated_chunks_count;               // 已分配的chunk的个数

  // 下面属性会保存许多空闲的内存块，为了加快查找速度，这些块底层使用了字典结构这些内存块并不是一整个Metachunk，
  // 也不是为了复用Metachunk块，而是为了充分复用Metachunk块的剩余空间。

  // 例如，当前正在负责为类加载器分配内存的Metachunk块的剩余空间不够大，这次的分配请求超过了剩余的内存空间，
  // 那只能从管理空闲块的ChunkManager中申请空闲块，或从VirtualSpaceNode中声明一个新的空闲块，然后在新的空闲块中分配。
  // 这样会带来一个问题，即原Metachunk块中剩余的空间会被浪费，为了充分利用这部分空间，只能将剩余空间作为一个小的空闲块保存到_block_freelists中，
  // 这样下次申请分配相对较小的内存时直接从_block_freelists中分配即可。

  // Free lists of blocks are per SpaceManager since they
  // are assumed to be in chunks in use by the SpaceManager
  // and all chunks in use by a SpaceManager are freed when
  // the class loader using the SpaceManager is collected.
  BlockFreelist _block_freelists;               // 空闲的chunk列表，这里面的chunk再分配时会被重新使用

  // protects virtualspace and chunk expansions
  static const char*  _expand_lock_name;        // _expand_lock的name属性
  static const int    _expand_lock_rank;        // _expand_lock的rank属性
  static Mutex* const _expand_lock;

 private:
  // Accessors
  Metachunk* chunks_in_use(ChunkIndex index) const { return _chunks_in_use[index]; }
  void set_chunks_in_use(ChunkIndex index, Metachunk* v) { _chunks_in_use[index] = v; }

  BlockFreelist* block_freelists() const {
    return (BlockFreelist*) &_block_freelists;
  }

  Metaspace::MetadataType mdtype() { return _mdtype; }

  VirtualSpaceList* vs_list()   const { return Metaspace::get_space_list(_mdtype); }
  ChunkManager* chunk_manager() const { return Metaspace::get_chunk_manager(_mdtype); }

  Metachunk* current_chunk() const { return _current_chunk; }
  void set_current_chunk(Metachunk* v) {
    _current_chunk = v;
  }

  Metachunk* find_current_chunk(size_t word_size);

  // Add chunk to the list of chunks in use
  void add_chunk(Metachunk* v, bool make_current);
  void retire_current_chunk();

  Mutex* lock() const { return _lock; }

  const char* chunk_size_name(ChunkIndex index) const;

 protected:
  void initialize();

 public:
  SpaceManager(Metaspace::MetadataType mdtype,
               Mutex* lock);
  ~SpaceManager();

  enum ChunkMultiples {
    MediumChunkMultiple = 4
  };

  bool is_class() { return _mdtype == Metaspace::ClassType; }

  // Accessors
  size_t specialized_chunk_size() { return (size_t) is_class() ? ClassSpecializedChunk : SpecializedChunk; }
  size_t small_chunk_size()       { return (size_t) is_class() ? ClassSmallChunk : SmallChunk; }
  size_t medium_chunk_size()      { return (size_t) is_class() ? ClassMediumChunk : MediumChunk; }
  size_t medium_chunk_bunch()     { return medium_chunk_size() * MediumChunkMultiple; }

  size_t smallest_chunk_size()  { return specialized_chunk_size(); }

  size_t allocated_blocks_words() const { return _allocated_blocks_words; }
  size_t allocated_blocks_bytes() const { return _allocated_blocks_words * BytesPerWord; }
  size_t allocated_chunks_words() const { return _allocated_chunks_words; }
  size_t allocated_chunks_count() const { return _allocated_chunks_count; }

  bool is_humongous(size_t word_size) { return word_size > medium_chunk_size(); }

  static Mutex* expand_lock() { return _expand_lock; }

  // Increment the per Metaspace and global running sums for Metachunks
  // by the given size.  This is used when a Metachunk to added to
  // the in-use list.
  void inc_size_metrics(size_t words);
  // Increment the per Metaspace and global running sums Metablocks by the given
  // size.  This is used when a Metablock is allocated.
  void inc_used_metrics(size_t words);
  // Delete the portion of the running sums for this SpaceManager. That is,
  // the globals running sums for the Metachunks and Metablocks are
  // decremented for all the Metachunks in-use by this SpaceManager.
  void dec_total_from_size_metrics();

  // Set the sizes for the initial chunks.
  void get_initial_chunk_sizes(Metaspace::MetaspaceType type,
                               size_t* chunk_word_size,
                               size_t* class_chunk_word_size);

  size_t sum_capacity_in_chunks_in_use() const;
  size_t sum_used_in_chunks_in_use() const;
  size_t sum_free_in_chunks_in_use() const;
  size_t sum_waste_in_chunks_in_use() const;
  size_t sum_waste_in_chunks_in_use(ChunkIndex index ) const;

  size_t sum_count_in_chunks_in_use();
  size_t sum_count_in_chunks_in_use(ChunkIndex i);

  // 从空闲列表中或者VirtualSpaceNode中获取Metachunk块
  Metachunk* get_new_chunk(size_t word_size, size_t grow_chunks_by_words);

  // Block allocation and deallocation.
  // Allocates a block from the current chunk
  MetaWord* allocate(size_t word_size);

  // Helper for allocations
  MetaWord* allocate_work(size_t word_size);

  // Returns a block to the per manager freelist
  void deallocate(MetaWord* p, size_t word_size);

  // Based on the allocation size and a minimum chunk size,
  // returned chunk size (for expanding space for chunk allocation).
  // 根据分配大小和最小块大小，返回块大小（用于扩展块分配空间）。
  size_t calc_chunk_size(size_t allocation_word_size);

  // Called when an allocation from the current chunk fails.
  // Gets a new chunk (may require getting a new virtual space),
  // and allocates from that chunk.
  MetaWord* grow_and_allocate(size_t word_size);

  // Notify memory usage to MemoryService.
  void track_metaspace_memory_usage();

  // debugging support.

  void dump(outputStream* const out) const;
  void print_on(outputStream* st) const;
  void locked_print_chunks_in_use_on(outputStream* st) const;

  void verify();
  void verify_chunk_size(Metachunk* chunk);
  NOT_PRODUCT(void mangle_freed_chunks();)
#ifdef ASSERT
  void verify_allocated_blocks_words();
#endif

  size_t get_raw_word_size(size_t word_size) {
    size_t byte_size = word_size * BytesPerWord;
    // byte_size最低大于Metablock的大小
    size_t raw_bytes_size = MAX2(byte_size, sizeof(Metablock));
    // 向上内存取整
    raw_bytes_size = align_size_up(raw_bytes_size, Metachunk::object_alignment());

    size_t raw_word_size = raw_bytes_size / BytesPerWord;
    assert(raw_word_size * BytesPerWord == raw_bytes_size, "Size problem");

    return raw_word_size;
  }
};

uint const SpaceManager::_small_chunk_limit = 4;

const char* SpaceManager::_expand_lock_name =
  "SpaceManager chunk allocation lock";
const int SpaceManager::_expand_lock_rank = Monitor::leaf - 1;
Mutex* const SpaceManager::_expand_lock =
  new Mutex(SpaceManager::_expand_lock_rank,
            SpaceManager::_expand_lock_name,
            Mutex::_allow_vm_block_flag);

void VirtualSpaceNode::inc_container_count() {
  assert_lock_strong(SpaceManager::expand_lock());
  _container_count++;
  assert(_container_count == container_count_slow(),
         err_msg("Inconsistency in countainer_count _container_count " SIZE_FORMAT
                 " container_count_slow() " SIZE_FORMAT,
                 _container_count, container_count_slow()));
}

void VirtualSpaceNode::dec_container_count() {
  assert_lock_strong(SpaceManager::expand_lock());
  _container_count--;
}

#ifdef ASSERT
void VirtualSpaceNode::verify_container_count() {
  assert(_container_count == container_count_slow(),
    err_msg("Inconsistency in countainer_count _container_count " SIZE_FORMAT
            " container_count_slow() " SIZE_FORMAT, _container_count, container_count_slow()));
}
#endif

// BlockFreelist methods

BlockFreelist::BlockFreelist() : _dictionary(NULL) {}

BlockFreelist::~BlockFreelist() {
  if (_dictionary != NULL) {
    if (Verbose && TraceMetadataChunkAllocation) {
      _dictionary->print_free_lists(gclog_or_tty);
    }
    delete _dictionary;
  }
}

// 将空闲内存维护到 BlockFreelist 中
void BlockFreelist::return_block(MetaWord* p, size_t word_size) {
  // 根据内存块的起始地址和大小构造一个新的Metablock
  Metablock* free_chunk = ::new (p) Metablock(word_size);
  if (dictionary() == NULL) {
   // 初始化_dictionary
   _dictionary = new BlockTreeDictionary();
  }
  // 添加到二叉树中保存
  dictionary()->return_chunk(free_chunk);
}

// 从空闲列表中查找一个空闲内存块
MetaWord* BlockFreelist::get_block(size_t word_size) {
  // _dictionary未初始化，肯定没有空闲的
  if (dictionary() == NULL) {
    return NULL;
  }

  // TreeChunk是一个模板类，BlockTreeDictionary的实现会用到，如果word_size太小则返回NULL
  if (word_size < TreeChunk<Metablock, FreeList>::min_size()) {
    // Dark matter.  Too small for dictionary.
    return NULL;
  }

  // 查找大于等于目标大小的空闲Metablock
  Metablock* free_block =
    dictionary()->get_chunk(word_size, FreeBlockDictionary<Metablock>::atLeast);
  if (free_block == NULL) {
    return NULL;
  }

  const size_t block_size = free_block->size();
  // 如果找到的空闲Metablock的大小大于目标大小的4倍，则将其归还，返回NULL，避免浪费
  if (block_size > WasteMultiplier * word_size) {
    return_block((MetaWord*)free_block, block_size);
    return NULL;
  }

  // 如果小于目标大小的4倍
  MetaWord* new_block = (MetaWord*)free_block;
  assert(block_size >= word_size, "Incorrect size of block from freelist");
  // 计算多余的空间
  const size_t unused = block_size - word_size;
  if (unused >= TreeChunk<Metablock, FreeList>::min_size()) {
    // 将多余的空间归还
    return_block(new_block + word_size, unused);
  }

  return new_block;
}

void BlockFreelist::print_on(outputStream* st) const {
  if (dictionary() == NULL) {
    return;
  }
  dictionary()->print_free_lists(st);
}

// VirtualSpaceNode methods

VirtualSpaceNode::~VirtualSpaceNode() {
  _rs.release();
#ifdef ASSERT
  size_t word_size = sizeof(*this) / BytesPerWord;
  Copy::fill_to_words((HeapWord*) this, word_size, 0xf1f1f1f1);
#endif
}

size_t VirtualSpaceNode::used_words_in_vs() const {
  return pointer_delta(top(), bottom(), sizeof(MetaWord));
}

// Space committed in the VirtualSpace
size_t VirtualSpaceNode::capacity_words_in_vs() const {
  return pointer_delta(end(), bottom(), sizeof(MetaWord));
}

size_t VirtualSpaceNode::free_words_in_vs() const {
  return pointer_delta(end(), top(), sizeof(MetaWord));
}

// Allocates the chunk from the virtual space only.
// This interface is also used internally for debugging.  Not all
// chunks removed here are necessarily used for allocation.
Metachunk* VirtualSpaceNode::take_from_committed(size_t chunk_word_size) {
  // Bottom of the new chunk
  // 获取未分配内存的起始地址
  MetaWord* chunk_limit = top();
  assert(chunk_limit != NULL, "Not safe to call this method");

  // The virtual spaces are always expanded by the
  // commit granularity to enforce the following condition.
  // Without this the is_available check will not work correctly.
  // 校验_virtual_space是否按照期望的方式expand，如果为false，则下面的is_available可能返回错误的结果
  assert(_virtual_space.committed_size() == _virtual_space.actual_committed_size(),
      "The committed memory doesn't match the expanded memory.");
  // 如果剩余空间不足，返回NULL
  if (!is_available(chunk_word_size)) {
    if (TraceMetadataChunkAllocation) {
      gclog_or_tty->print("VirtualSpaceNode::take_from_committed() not available %d words ", chunk_word_size);
      // Dump some information about the virtual space that is nearly full
      print_on(gclog_or_tty);
    }
    return NULL;
  }

  // Take the space  (bump top on the current virtual space).
  // 将top指针往高地址移动
  inc_top(chunk_word_size);

  // Initialize the chunk
  // 初始化Metachunk
  Metachunk* result = ::new (chunk_limit) Metachunk(chunk_word_size, this);
  return result;
}


// Expand the virtual space (commit more of the reserved space)
bool VirtualSpaceNode::expand_by(size_t min_words, size_t preferred_words) {
  size_t min_bytes = min_words * BytesPerWord;
  size_t preferred_bytes = preferred_words * BytesPerWord;

  size_t uncommitted = virtual_space()->reserved_size() - virtual_space()->actual_committed_size();

  if (uncommitted < min_bytes) {
    return false;
  }

  size_t commit = MIN2(preferred_bytes, uncommitted);
  bool result = virtual_space()->expand_by(commit, false);

  assert(result, "Failed to commit memory");

  return result;
}

Metachunk* VirtualSpaceNode::get_chunk_vs(size_t chunk_word_size) {
  // 校验已获取锁
  assert_lock_strong(SpaceManager::expand_lock());
  // 从commited区域的内存分配一个Metachunk
  Metachunk* result = take_from_committed(chunk_word_size);
  if (result != NULL) {
    // 分配成功，增加计数器
    inc_container_count();
  }
  return result;
}

bool VirtualSpaceNode::initialize() {
  // _rs申请内存地址空间失败，返回false
  if (!_rs.is_reserved()) {
    return false;
  }

  // These are necessary restriction to make sure that the virtual space always
  // grows in steps of Metaspace::commit_alignment(). If both base and size are
  // aligned only the middle alignment of the VirtualSpace is used.
  // 校验申请的地址空间是否合法
  assert_is_ptr_aligned(_rs.base(), Metaspace::commit_alignment());
  assert_is_size_aligned(_rs.size(), Metaspace::commit_alignment());

  // ReservedSpaces marked as special will have the entire memory
  // pre-committed. Setting a committed size will make sure that
  // committed_size and actual_committed_size agrees.
  // 如果rs支持pre-committed，则设置pre_committed_size为rs的大小
  size_t pre_committed_size = _rs.special() ? _rs.size() : 0;
  // 初始化virtual_space
  bool result = virtual_space()->initialize_with_granularity(_rs, pre_committed_size,
                                            Metaspace::commit_alignment());
  // 申请内存成功
  if (result) {
    assert(virtual_space()->committed_size() == virtual_space()->actual_committed_size(),
        "Checking that the pre-committed memory was registered by the VirtualSpace");
    // 设置其他属性
    set_top((MetaWord*)virtual_space()->low());
    set_reserved(MemRegion((HeapWord*)_rs.base(),
                 (HeapWord*)(_rs.base() + _rs.size())));

    assert(reserved()->start() == (HeapWord*) _rs.base(),
      err_msg("Reserved start was not set properly " PTR_FORMAT
        " != " PTR_FORMAT, reserved()->start(), _rs.base()));
    assert(reserved()->word_size() == _rs.size() / BytesPerWord,
      err_msg("Reserved size was not set properly " SIZE_FORMAT
        " != " SIZE_FORMAT, reserved()->word_size(),
        _rs.size() / BytesPerWord));
  }

  return result;
}

void VirtualSpaceNode::print_on(outputStream* st) const {
  size_t used = used_words_in_vs();
  size_t capacity = capacity_words_in_vs();
  VirtualSpace* vs = virtual_space();
  st->print_cr("   space @ " PTR_FORMAT " " SIZE_FORMAT "K, %3d%% used "
           "[" PTR_FORMAT ", " PTR_FORMAT ", "
           PTR_FORMAT ", " PTR_FORMAT ")",
           vs, capacity / K,
           capacity == 0 ? 0 : used * 100 / capacity,
           bottom(), top(), end(),
           vs->high_boundary());
}

#ifdef ASSERT
void VirtualSpaceNode::mangle() {
  size_t word_size = capacity_words_in_vs();
  Copy::fill_to_words((HeapWord*) low(), word_size, 0xf1f1f1f1);
}
#endif // ASSERT

// VirtualSpaceList methods
// Space allocated from the VirtualSpace

VirtualSpaceList::~VirtualSpaceList() {
  // 从链表头元素开始遍历，释放所有的VirtualSpaceNode
  VirtualSpaceListIterator iter(virtual_space_list());
  while (iter.repeat()) {
    VirtualSpaceNode* vsl = iter.get_next();
    delete vsl;
  }
}

void VirtualSpaceList::inc_reserved_words(size_t v) {
  assert_lock_strong(SpaceManager::expand_lock());
  _reserved_words = _reserved_words + v;
}
void VirtualSpaceList::dec_reserved_words(size_t v) {
  assert_lock_strong(SpaceManager::expand_lock());
  _reserved_words = _reserved_words - v;
}

#define assert_committed_below_limit()                             \
  assert(MetaspaceAux::committed_bytes() <= MaxMetaspaceSize,      \
      err_msg("Too much committed memory. Committed: " SIZE_FORMAT \
              " limit (MaxMetaspaceSize): " SIZE_FORMAT,           \
          MetaspaceAux::committed_bytes(), MaxMetaspaceSize));

void VirtualSpaceList::inc_committed_words(size_t v) {
  assert_lock_strong(SpaceManager::expand_lock());
  _committed_words = _committed_words + v;

  assert_committed_below_limit();
}
void VirtualSpaceList::dec_committed_words(size_t v) {
  assert_lock_strong(SpaceManager::expand_lock());
  _committed_words = _committed_words - v;

  assert_committed_below_limit();
}

void VirtualSpaceList::inc_virtual_space_count() {
  assert_lock_strong(SpaceManager::expand_lock());
  _virtual_space_count++;
}
void VirtualSpaceList::dec_virtual_space_count() {
  assert_lock_strong(SpaceManager::expand_lock());
  _virtual_space_count--;
}

// remove_chunk是将某个Metachunk从ChunkManager中移除
void ChunkManager::remove_chunk(Metachunk* chunk) {
  size_t word_size = chunk->word_size();
  // 根据大小匹配对应的空闲chunk链表，然后从链表中移除
  ChunkIndex index = list_index(word_size);
  if (index != HumongousIndex) {
    free_chunks(index)->remove_chunk(chunk);
  } else {
    humongous_dictionary()->remove_chunk(chunk);
  }

  // Chunk is being removed from the chunks free list.
  // 减少计数器
  dec_free_chunks_total(chunk->word_size());
}

// Walk the list of VirtualSpaceNodes and delete
// nodes with a 0 container_count.  Remove Metachunks in
// the node from their respective freelists.
void VirtualSpaceList::purge(ChunkManager* chunk_manager) {
  assert_lock_strong(SpaceManager::expand_lock());
  // Don't use a VirtualSpaceListIterator because this
  // list is being changed and a straightforward use of an iterator is not safe.
  VirtualSpaceNode* purged_vsl = NULL;
  VirtualSpaceNode* prev_vsl = virtual_space_list();
  VirtualSpaceNode* next_vsl = prev_vsl;
  // 遍历VirtualSpaceNode
  while (next_vsl != NULL) {
    VirtualSpaceNode* vsl = next_vsl;
    next_vsl = vsl->next();
    // Don't free the current virtual space since it will likely
    // be needed soon.
    if (vsl->container_count() == 0 && vsl != current_virtual_space()) {
      // Unlink it from the list
      // 将当前的VirtualSpaceNode从列表中移除
      if (prev_vsl == vsl) {
        // This is the case of the current node being the first node.
        assert(vsl == virtual_space_list(), "Expected to be the first node");
        set_virtual_space_list(vsl->next());
      } else {
        prev_vsl->set_next(vsl->next());
      }
      // 将当前的VirtualSpaceNode中使用的chunks从空闲列表中移除
      vsl->purge(chunk_manager);
      // 将VirtualSpaceNode使用的内存归还给操作系统。
      dec_reserved_words(vsl->reserved_words());
      dec_committed_words(vsl->committed_words());
      dec_virtual_space_count();
      purged_vsl = vsl;
      delete vsl;
    } else {
      prev_vsl = vsl;
    }
  }
#ifdef ASSERT
  if (purged_vsl != NULL) {
  // List should be stable enough to use an iterator here.
  VirtualSpaceListIterator iter(virtual_space_list());
    while (iter.repeat()) {
      VirtualSpaceNode* vsl = iter.get_next();
      assert(vsl != purged_vsl, "Purge of vsl failed");
    }
  }
#endif
}

void VirtualSpaceList::retire_current_virtual_space() {
  assert_lock_strong(SpaceManager::expand_lock());

  VirtualSpaceNode* vsn = current_virtual_space();

  ChunkManager* cm = is_class() ? Metaspace::chunk_manager_class() :
                                  Metaspace::chunk_manager_metadata();
  // 回收当前节点
  vsn->retire(cm);
}

// 当前VirtualSpaceNode的剩余空间不足需要申请一个新的VirtualSpaceNode时调用的。
// 此方法会在当前VirtualSpaceNode的剩余空间内申请新的Metachunk，并将其添加到ChunkManager中，避免空间浪费。
void VirtualSpaceNode::retire(ChunkManager* chunk_manager) {
  for (int i = (int)MediumIndex; i >= (int)ZeroIndex; --i) {
    ChunkIndex index = (ChunkIndex)i;
    // 获取不同规格的Metachunk的大小
    size_t chunk_size = chunk_manager->free_chunks(index)->size();
    // 如果剩余空间充足
    while (free_words_in_vs() >= chunk_size) {
      DEBUG_ONLY(verify_container_count();)
      // 申请一个新的Metachunk
      Metachunk* chunk = get_chunk_vs(chunk_size);
      assert(chunk != NULL, "allocation should have been successful");
      // 申请成功将其交给ChunkManager
      chunk_manager->return_chunks(index, chunk);
      chunk_manager->inc_free_chunks_total(chunk_size);
      DEBUG_ONLY(verify_container_count();)
    }
  }
  assert(free_words_in_vs() == 0, "should be empty now");
}

VirtualSpaceList::VirtualSpaceList(size_t word_size) :
                                   _is_class(false),
                                   _virtual_space_list(NULL),
                                   _current_virtual_space(NULL),
                                   _reserved_words(0),
                                   _committed_words(0),
                                   _virtual_space_count(0) {
  // 获取锁expand_lock
  MutexLockerEx cl(SpaceManager::expand_lock(),
                   Mutex::_no_safepoint_check_flag);
  // 创建一个新的virtual_space
  create_new_virtual_space(word_size);
}

VirtualSpaceList::VirtualSpaceList(ReservedSpace rs) :
                                   _is_class(true),
                                   _virtual_space_list(NULL),
                                   _current_virtual_space(NULL),
                                   _reserved_words(0),
                                   _committed_words(0),
                                   _virtual_space_count(0) {
  MutexLockerEx cl(SpaceManager::expand_lock(),
                   Mutex::_no_safepoint_check_flag);
  VirtualSpaceNode* class_entry = new VirtualSpaceNode(rs);
  bool succeeded = class_entry->initialize();
  if (succeeded) {
    link_vs(class_entry);
  }
}

size_t VirtualSpaceList::free_bytes() {
  return virtual_space_list()->free_words_in_vs() * BytesPerWord;
}

// Allocate another meta virtual space and add it to the list.
bool VirtualSpaceList::create_new_virtual_space(size_t vs_word_size) {
  assert_lock_strong(SpaceManager::expand_lock());

  // 创建compressed class的VirtualSpace不会走到此分支
  if (is_class()) {
    assert(false, "We currently don't support more than one VirtualSpace for"
                  " the compressed class space. The initialization of the"
                  " CCS uses another code path and should not hit this path.");
    return false;
  }

  if (vs_word_size == 0) {
    assert(false, "vs_word_size should always be at least _reserve_alignment large.");
    return false;
  }

  // Reserve the space
  size_t vs_byte_size = vs_word_size * BytesPerWord;
  // 内存取整
  assert_is_size_aligned(vs_byte_size, Metaspace::reserve_alignment());

  // Allocate the meta virtual space and initialize it.
  // 创建一个新的节点
  VirtualSpaceNode* new_entry = new VirtualSpaceNode(vs_byte_size);
  if (!new_entry->initialize()) {
    // 初始化失败，返回false
    delete new_entry;
    return false;
  } else {
    // 初始化成功，校验结果
    assert(new_entry->reserved_words() == vs_word_size,
        "Reserved memory size differs from requested memory size");
    // ensure lock-free iteration sees fully initialized node
    OrderAccess::storestore();
    link_vs(new_entry);
    return true;
  }
}

void VirtualSpaceList::link_vs(VirtualSpaceNode* new_entry) {
  // 插入到链表中
  if (virtual_space_list() == NULL) {
      set_virtual_space_list(new_entry);
  } else {
    current_virtual_space()->set_next(new_entry);
  }
  set_current_virtual_space(new_entry);
  // 增加计数
  inc_reserved_words(new_entry->reserved_words());
  inc_committed_words(new_entry->committed_words());
  inc_virtual_space_count();
#ifdef ASSERT
  new_entry->mangle();
#endif
  if (TraceMetavirtualspaceAllocation && Verbose) {
    // 打印日志
    VirtualSpaceNode* vsl = current_virtual_space();
    vsl->print_on(gclog_or_tty);
  }
}

bool VirtualSpaceList::expand_node_by(VirtualSpaceNode* node,
                                      size_t min_words,
                                      size_t preferred_words) {
  size_t before = node->committed_words();
  // 节点expand事假调用VirtualSpace::expand_by方法扩展，如果成功返回true
  bool result = node->expand_by(min_words, preferred_words);

  size_t after = node->committed_words();

  // after and before can be the same if the memory was pre-committed.
  assert(after >= before, "Inconsistency");
  // 增加已提交的内存量
  inc_committed_words(after - before);

  return result;
}

bool VirtualSpaceList::expand_by(size_t min_words, size_t preferred_words) {
  // 校验参数
  assert_is_size_aligned(min_words,       Metaspace::commit_alignment_words());
  assert_is_size_aligned(preferred_words, Metaspace::commit_alignment_words());
  assert(min_words <= preferred_words, "Invalid arguments");

  // MetaspaceGC根据当前已经提交的总内存量和Metaspace最大内存量判断能否扩展
  if (!MetaspaceGC::can_expand(min_words, this->is_class())) {
    return  false;
  }

  size_t allowed_expansion_words = MetaspaceGC::allowed_expansion();
  if (allowed_expansion_words < min_words) {
    return false;
  }

  // 因为preferred_words和allowed_expansion_words都是大于或者等于min_words，所以取两者的最小值也能满足要求
  size_t max_expansion_words = MIN2(preferred_words, allowed_expansion_words);

  // Commit more memory from the the current virtual space.
  // 尝试当前节点扩展
  bool vs_expanded = expand_node_by(current_virtual_space(),
                                    min_words,
                                    max_expansion_words);
  // 扩展成功
  if (vs_expanded) {
    return true;
  }

  // 节点创建时申请的reserved_size的剩余空间不足导致扩展失败，回收当前节点
  retire_current_virtual_space();

  // Get another virtual space.
  // 取两者间的最大值，并做内存取整
  size_t grow_vs_words = MAX2((size_t)VirtualSpaceSize, preferred_words);
  grow_vs_words = align_size_up(grow_vs_words, Metaspace::reserve_alignment_words());

  // 创建一个新的节点
  if (create_new_virtual_space(grow_vs_words)) {
    // pre_committed即创建的时候已经完成commited
    if (current_virtual_space()->is_pre_committed()) {
      // The memory was pre-committed, so we are done here.
      assert(min_words <= current_virtual_space()->committed_words(),
          "The new VirtualSpace was pre-committed, so it"
          "should be large enough to fit the alloc request.");
      return true;
    }

    // 非pre_committed，需要手动commited
    return expand_node_by(current_virtual_space(),
                          min_words,
                          max_expansion_words);
  }

  return false;
}

Metachunk* VirtualSpaceList::get_new_chunk(size_t word_size,
                                           size_t grow_chunks_by_words,
                                           size_t medium_chunk_bunch) {

  // Allocate a chunk out of the current virtual space.
  // 从当前的VirtualSpaceNode节点分配一个Metachunk
  Metachunk* next = current_virtual_space()->get_chunk_vs(grow_chunks_by_words);

  if (next != NULL) {
    // 分配成功则返回
    return next;
  }

  // The expand amount is currently only determined by the requested sizes
  // and not how much committed memory is left in the current virtual space.

  // 当前节点内存不足，需要扩展创建一个新的节点，扩展的量是根据要求分配的chunk_word_size内存大小计算的，而不是当前节点剩余的已提交内存
  // 对chunk_word_size做内存取整
  size_t min_word_size       = align_size_up(grow_chunks_by_words, Metaspace::commit_alignment_words());
  size_t preferred_word_size = align_size_up(medium_chunk_bunch,   Metaspace::commit_alignment_words());
  if (min_word_size >= preferred_word_size) {
    // Can happen when humongous chunks are allocated.
    preferred_word_size = min_word_size;
  }

  // 按照min_word_size重新创建一个新的VirtualSpaceNode
  bool expanded = expand_by(min_word_size, preferred_word_size);
  if (expanded) {
    // 如果创建成功，则使用新的节点创建一个Metachunk
    next = current_virtual_space()->get_chunk_vs(grow_chunks_by_words);
    assert(next != NULL, "The allocation was expected to succeed after the expansion");
  }

   return next;
}

void VirtualSpaceList::print_on(outputStream* st) const {
  if (TraceMetadataChunkAllocation && Verbose) {
    VirtualSpaceListIterator iter(virtual_space_list());
    while (iter.repeat()) {
      VirtualSpaceNode* node = iter.get_next();
      node->print_on(st);
    }
  }
}

bool VirtualSpaceList::contains(const void *ptr) {
  VirtualSpaceNode* list = virtual_space_list();
  VirtualSpaceListIterator iter(list);
  while (iter.repeat()) {
    VirtualSpaceNode* node = iter.get_next();
    if (node->reserved()->contains(ptr)) {
      return true;
    }
  }
  return false;
}


// MetaspaceGC methods

// VM_CollectForMetadataAllocation is the vm operation used to GC.
// Within the VM operation after the GC the attempt to allocate the metadata
// should succeed.  If the GC did not free enough space for the metaspace
// allocation, the HWM is increased so that another virtualspace will be
// allocated for the metadata.  With perm gen the increase in the perm
// gen had bounds, MinMetaspaceExpansion and MaxMetaspaceExpansion.  The
// metaspace policy uses those as the small and large steps for the HWM.
//
// After the GC the compute_new_size() for MetaspaceGC is called to
// resize the capacity of the metaspaces.  The current implementation
// is based on the flags MinMetaspaceFreeRatio and MaxMetaspaceFreeRatio used
// to resize the Java heap by some GC's.  New flags can be implemented
// if really needed.  MinMetaspaceFreeRatio is used to calculate how much
// free space is desirable in the metaspace capacity to decide how much
// to increase the HWM.  MaxMetaspaceFreeRatio is used to decide how much
// free space is desirable in the metaspace capacity before decreasing
// the HWM.

// Calculate the amount to increase the high water mark (HWM).
// Increase by a minimum amount (MinMetaspaceExpansion) so that
// another expansion is not requested too soon.  If that is not
// enough to satisfy the allocation, increase by MaxMetaspaceExpansion.
// If that is still not enough, expand by the size of the allocation
// plus some.
size_t MetaspaceGC::delta_capacity_until_GC(size_t bytes) {
  size_t min_delta = MinMetaspaceExpansion;
  size_t max_delta = MaxMetaspaceExpansion;
  size_t delta = align_size_up(bytes, Metaspace::commit_alignment());

  if (delta <= min_delta) {
    delta = min_delta;
  } else if (delta <= max_delta) {
    // Don't want to hit the high water mark on the next
    // allocation so make the delta greater than just enough
    // for this allocation.
    delta = max_delta;
  } else {
    // This allocation is large but the next ones are probably not
    // so increase by the minimum.
    delta = delta + min_delta;
  }

  assert_is_size_aligned(delta, Metaspace::commit_alignment());

  return delta;
}

size_t MetaspaceGC::capacity_until_GC() {
  size_t value = (size_t)OrderAccess::load_ptr_acquire(&_capacity_until_GC);
  assert(value >= MetaspaceSize, "Not initialied properly?");
  return value;
}

size_t MetaspaceGC::inc_capacity_until_GC(size_t v) {
  assert_is_size_aligned(v, Metaspace::commit_alignment());

  return (size_t)Atomic::add_ptr(v, &_capacity_until_GC);
}

size_t MetaspaceGC::dec_capacity_until_GC(size_t v) {
  assert_is_size_aligned(v, Metaspace::commit_alignment());

  return (size_t)Atomic::add_ptr(-(intptr_t)v, &_capacity_until_GC);
}

bool MetaspaceGC::can_expand(size_t word_size, bool is_class) {
  // Check if the compressed class space is full.
  if (is_class && Metaspace::using_class_space()) {
    size_t class_committed = MetaspaceAux::committed_bytes(Metaspace::ClassType);
    if (class_committed + word_size * BytesPerWord > CompressedClassSpaceSize) {
      return false;
    }
  }

  // Check if the user has imposed a limit on the metaspace memory.
  size_t committed_bytes = MetaspaceAux::committed_bytes();
  if (committed_bytes + word_size * BytesPerWord > MaxMetaspaceSize) {
    return false;
  }

  return true;
}

size_t MetaspaceGC::allowed_expansion() {
  size_t committed_bytes = MetaspaceAux::committed_bytes();

  size_t left_until_max  = MaxMetaspaceSize - committed_bytes;

  // Always grant expansion if we are initiating the JVM,
  // or if the GC_locker is preventing GCs.
  if (!is_init_completed() || GC_locker::is_active_and_needs_gc()) {
    return left_until_max / BytesPerWord;
  }

  size_t capacity_until_gc = capacity_until_GC();

  if (capacity_until_gc <= committed_bytes) {
    return 0;
  }

  size_t left_until_GC = capacity_until_gc - committed_bytes;
  size_t left_to_commit = MIN2(left_until_GC, left_until_max);

  return left_to_commit / BytesPerWord;
}

// GC之后用于计算一个新的 _capacity_until_GC 属性值
void MetaspaceGC::compute_new_size() {
  assert(_shrink_factor <= 100, "invalid shrink factor");
  // 此值用于控制因为 System.gc() 的前几次做到缓慢的缩容
  uint current_shrink_factor = _shrink_factor;
  _shrink_factor = 0;

  // 当前已使用的内存大小
  const size_t used_after_gc = MetaspaceAux::allocated_capacity_bytes();
  // 当前已分配的内存大小
  const size_t capacity_until_GC = MetaspaceGC::capacity_until_GC();

  // MinMetaspaceFreeRatio是GC完成后需要保证的Metaspace最低的空闲空间比例，默认是40，为了避免Metaspace因为内存不足再次触发GC
  const double minimum_free_percentage = MinMetaspaceFreeRatio / 100.0;
  const double maximum_used_percentage = 1.0 - minimum_free_percentage;

  // 计算按照MinMetaspaceFreeRatio计算后需要的最低内存值(让其已使用空间保持在60%以上)
  const double min_tmp = used_after_gc / maximum_used_percentage;
  // 如果min_tmp大于MaxMetaspaceSize则取MaxMetaspaceSize，保证扩容后不超过最大值
  size_t minimum_desired_capacity =
    (size_t)MIN2(min_tmp, double(max_uintx));
  // Don't shrink less than the initial generation size
  // 如果MetaspaceSize大于minimum_desired_capacity则取MetaspaceSize，保证缩容后不低于初始值
  minimum_desired_capacity = MAX2(minimum_desired_capacity,
                                  MetaspaceSize);

  // 打印GC日志
  if (PrintGCDetails && Verbose) {
    gclog_or_tty->print_cr("\nMetaspaceGC::compute_new_size: ");
    gclog_or_tty->print_cr("  "
                  "  minimum_free_percentage: %6.2f"
                  "  maximum_used_percentage: %6.2f",
                  minimum_free_percentage,
                  maximum_used_percentage);
    gclog_or_tty->print_cr("  "
                  "   used_after_gc       : %6.1fKB",
                  used_after_gc / (double) K);
  }


  size_t shrink_bytes = 0;
  // 如果当前的capacity_until_GC小于期望值，则扩容，增加capacity_until_GC
  if (capacity_until_GC < minimum_desired_capacity) {
    // If we have less capacity below the metaspace HWM, then
    // increment the HWM.
    // 计算需要增加的值
    size_t expand_bytes = minimum_desired_capacity - capacity_until_GC;
    // 取整
    expand_bytes = align_size_up(expand_bytes, Metaspace::commit_alignment());
    // Don't expand unless it's significant
    // MinMetaspaceExpansion表示扩容时最低的扩展值，默认是332k，低于此值不扩容
    if (expand_bytes >= MinMetaspaceExpansion) {
      // 增加capacity_until_GC
      MetaspaceGC::inc_capacity_until_GC(expand_bytes);
    }
    if (PrintGCDetails && Verbose) {
      size_t new_capacity_until_GC = capacity_until_GC;
      gclog_or_tty->print_cr("    expanding:"
                    "  minimum_desired_capacity: %6.1fKB"
                    "  expand_bytes: %6.1fKB"
                    "  MinMetaspaceExpansion: %6.1fKB"
                    "  new metaspace HWM:  %6.1fKB",
                    minimum_desired_capacity / (double) K,
                    expand_bytes / (double) K,
                    MinMetaspaceExpansion / (double) K,
                    new_capacity_until_GC / (double) K);
    }
    return;
  }

  // No expansion, now see if we want to shrink
  // We would never want to shrink more than this
  // 如果当前的capacity_until_GC大于最低期望值，需要判断capacity_until_GC是否大于最高期望值，如果是则缩容，减少capacity_until_GC计算需要缩容的空间
  size_t max_shrink_bytes = capacity_until_GC - minimum_desired_capacity;
  assert(max_shrink_bytes >= 0, err_msg("max_shrink_bytes " SIZE_FORMAT,
    max_shrink_bytes));

  // Should shrinking be considered?
  // MaxMetaspaceFreeRatio表示GC结束后Metaspace的最大空闲比例，默认是70
  if (MaxMetaspaceFreeRatio < 100) {
    // 根据MaxMetaspaceFreeRatio计算允许的最大的空间值，不能低于MetaspaceSize初始值，不能大于最大值MaxMetaspaceSize
    const double maximum_free_percentage = MaxMetaspaceFreeRatio / 100.0;
    const double minimum_used_percentage = 1.0 - maximum_free_percentage;
    // 计算按照MaxMetaspaceFreeRatio计算后需要的最高内存值(让其已使用空间保持在30%以下)
    const double max_tmp = used_after_gc / minimum_used_percentage;
    size_t maximum_desired_capacity = (size_t)MIN2(max_tmp, double(max_uintx));
    maximum_desired_capacity = MAX2(maximum_desired_capacity,
                                    MetaspaceSize);
    if (PrintGCDetails && Verbose) {
      gclog_or_tty->print_cr("  "
                             "  maximum_free_percentage: %6.2f"
                             "  minimum_used_percentage: %6.2f",
                             maximum_free_percentage,
                             minimum_used_percentage);
      gclog_or_tty->print_cr("  "
                             "  minimum_desired_capacity: %6.1fKB"
                             "  maximum_desired_capacity: %6.1fKB",
                             minimum_desired_capacity / (double) K,
                             maximum_desired_capacity / (double) K);
    }

    // /合理校验，要求的最小内存值必须小于或者等于最大内存值
    assert(minimum_desired_capacity <= maximum_desired_capacity,
           "sanity check");

    // 如果capacity_until_GC大于根据最大空闲比例计算出的允许的最大内存值，即当前的空间比例大于设置的最大比例，需要缩容
    if (capacity_until_GC > maximum_desired_capacity) {
      // Capacity too large, compute shrinking size
      // 计算需要缩容的大小
      shrink_bytes = capacity_until_GC - maximum_desired_capacity;
      // We don't want shrink all the way back to initSize if people call
      // System.gc(), because some programs do that between "phases" and then
      // we'd just have to grow the heap up again for the next phase.  So we
      // damp the shrinking: 0% on the first call, 10% on the second call, 40%
      // on the third call, and 100% by the fourth call.  But if we recompute
      // size without shrinking, it goes back to 0%.
      // 为了避免程序调用System.gc()触发的GC导致堆空间的再次分配，增加参数_shrink_factor，第一次GC时是0，即第一次GC时不会触发缩容
      // 第二次是10，即最多只缩容10%,第三次是40%,第四次是100%
      shrink_bytes = shrink_bytes / 100 * current_shrink_factor;
      // 内存取整
      shrink_bytes = align_size_down(shrink_bytes, Metaspace::commit_alignment());

      assert(shrink_bytes <= max_shrink_bytes,
        err_msg("invalid shrink size " SIZE_FORMAT " not <= " SIZE_FORMAT,
          shrink_bytes, max_shrink_bytes));
      // 更新 _shrink_factor
      if (current_shrink_factor == 0) {
        _shrink_factor = 10;
      } else {
        _shrink_factor = MIN2(current_shrink_factor * 4, (uint) 100);
      }
      if (PrintGCDetails && Verbose) {
        gclog_or_tty->print_cr("  "
                      "  shrinking:"
                      "  initSize: %.1fK"
                      "  maximum_desired_capacity: %.1fK",
                      MetaspaceSize / (double) K,
                      maximum_desired_capacity / (double) K);
        gclog_or_tty->print_cr("  "
                      "  shrink_bytes: %.1fK"
                      "  current_shrink_factor: %d"
                      "  new shrink factor: %d"
                      "  MinMetaspaceExpansion: %.1fK",
                      shrink_bytes / (double) K,
                      current_shrink_factor,
                      _shrink_factor,
                      MinMetaspaceExpansion / (double) K);
      }
    }
  }

  // Don't shrink unless it's significant
  // 如果大于最低扩容空间，且缩容后大于初始值MetaspaceSize，则缩容
  if (shrink_bytes >= MinMetaspaceExpansion &&
      ((capacity_until_GC - shrink_bytes) >= MetaspaceSize)) {
    MetaspaceGC::dec_capacity_until_GC(shrink_bytes);
  }
}

// Metadebug methods

void Metadebug::init_allocation_fail_alot_count() {
  if (MetadataAllocationFailALot) {
    _allocation_fail_alot_count =
      1+(long)((double)MetadataAllocationFailALotInterval*os::random()/(max_jint+1.0));
  }
}

#ifdef ASSERT
bool Metadebug::test_metadata_failure() {
  if (MetadataAllocationFailALot &&
      Threads::is_vm_complete()) {
    if (_allocation_fail_alot_count > 0) {
      _allocation_fail_alot_count--;
    } else {
      if (TraceMetadataChunkAllocation && Verbose) {
        gclog_or_tty->print_cr("Metadata allocation failing for "
                               "MetadataAllocationFailALot");
      }
      init_allocation_fail_alot_count();
      return true;
    }
  }
  return false;
}
#endif

// ChunkManager methods

size_t ChunkManager::free_chunks_total_words() {
  return _free_chunks_total;
}

size_t ChunkManager::free_chunks_total_bytes() {
  return free_chunks_total_words() * BytesPerWord;
}

size_t ChunkManager::free_chunks_count() {
#ifdef ASSERT
  if (!UseConcMarkSweepGC && !SpaceManager::expand_lock()->is_locked()) {
    MutexLockerEx cl(SpaceManager::expand_lock(),
                     Mutex::_no_safepoint_check_flag);
    // This lock is only needed in debug because the verification
    // of the _free_chunks_totals walks the list of free chunks
    slow_locked_verify_free_chunks_count();
  }
#endif
  return _free_chunks_count;
}

void ChunkManager::locked_verify_free_chunks_total() {
  assert_lock_strong(SpaceManager::expand_lock());
  // 校验_free_chunks_total正确
  assert(sum_free_chunks() == _free_chunks_total,
    err_msg("_free_chunks_total " SIZE_FORMAT " is not the"
           " same as sum " SIZE_FORMAT, _free_chunks_total,
           sum_free_chunks()));
}

void ChunkManager::verify_free_chunks_total() {
  MutexLockerEx cl(SpaceManager::expand_lock(),
                     Mutex::_no_safepoint_check_flag);
  locked_verify_free_chunks_total();
}

void ChunkManager::locked_verify_free_chunks_count() {
  assert_lock_strong(SpaceManager::expand_lock());
  // 校验_free_chunks_count正确
  assert(sum_free_chunks_count() == _free_chunks_count,
    err_msg("_free_chunks_count " SIZE_FORMAT " is not the"
           " same as sum " SIZE_FORMAT, _free_chunks_count,
           sum_free_chunks_count()));
}

void ChunkManager::verify_free_chunks_count() {
#ifdef ASSERT
  MutexLockerEx cl(SpaceManager::expand_lock(),
                     Mutex::_no_safepoint_check_flag);
  locked_verify_free_chunks_count();
#endif
}

void ChunkManager::verify() {
  MutexLockerEx cl(SpaceManager::expand_lock(),
                     Mutex::_no_safepoint_check_flag);
  locked_verify();
}

void ChunkManager::locked_verify() {
  locked_verify_free_chunks_count();
  locked_verify_free_chunks_total();
}

void ChunkManager::locked_print_free_chunks(outputStream* st) {
  assert_lock_strong(SpaceManager::expand_lock());
  st->print_cr("Free chunk total " SIZE_FORMAT "  count " SIZE_FORMAT,
                _free_chunks_total, _free_chunks_count);
}

void ChunkManager::locked_print_sum_free_chunks(outputStream* st) {
  assert_lock_strong(SpaceManager::expand_lock());
  st->print_cr("Sum free chunk total " SIZE_FORMAT "  count " SIZE_FORMAT,
                sum_free_chunks(), sum_free_chunks_count());
}
ChunkList* ChunkManager::free_chunks(ChunkIndex index) {
  return &_free_chunks[index];
}

// These methods that sum the free chunk lists are used in printing
// methods that are used in product builds.
// 返回所有空闲chunk的总的内存大小
size_t ChunkManager::sum_free_chunks() {
  assert_lock_strong(SpaceManager::expand_lock());
  size_t result = 0;
  // 遍历数组_free_chunks
  for (ChunkIndex i = ZeroIndex; i < NumberOfFreeLists; i = next_chunk_index(i)) {
    ChunkList* list = free_chunks(i);

    if (list == NULL) {
      continue;
    }
    // 链表中每个chunk的大小都是list->size()
    result = result + list->count() * list->size();
  }
  result = result + humongous_dictionary()->total_size();
  return result;
}

// 统计总的空闲chunk数量
size_t ChunkManager::sum_free_chunks_count() {
  assert_lock_strong(SpaceManager::expand_lock());
  size_t count = 0;
  // 遍历数组_free_chunks
  for (ChunkIndex i = ZeroIndex; i < NumberOfFreeLists; i = next_chunk_index(i)) {
    ChunkList* list = free_chunks(i);
    if (list == NULL) {
      continue;
    }
    count = count + list->count();
  }
  count = count + humongous_dictionary()->total_free_blocks();
  return count;
}

ChunkList* ChunkManager::find_free_chunks_list(size_t word_size) {
  ChunkIndex index = list_index(word_size);
  assert(index < HumongousIndex, "No humongous list");
  return free_chunks(index);
}

Metachunk* ChunkManager::free_chunks_get(size_t word_size) {
  // 校验获取锁
  assert_lock_strong(SpaceManager::expand_lock());
  // 校验计数器
  slow_locked_verify();

  Metachunk* chunk = NULL;
  // 如果是通用规格的Metachunk
  if (list_index(word_size) != HumongousIndex) {
    // 获取对应的链表
    ChunkList* free_list = find_free_chunks_list(word_size);
    assert(free_list != NULL, "Sanity check");
    // 获取链表头元素
    chunk = free_list->head();

    if (chunk == NULL) {
      return NULL;
    }

    // Remove the chunk as the head of the list.
    // 不为空，则将头元素从链表中移除
    free_list->remove_chunk(chunk);

    if (TraceMetadataChunkAllocation && Verbose) {
      // 打印日志
      gclog_or_tty->print_cr("ChunkManager::free_chunks_get: free_list "
                             PTR_FORMAT " head " PTR_FORMAT " size " SIZE_FORMAT,
                             free_list, chunk, chunk->word_size());
    }
  } else {
    // 如果是特殊规格，则在humongous链表中查找大于等于word_size的Metachunk
    chunk = humongous_dictionary()->get_chunk(
      word_size,
      FreeBlockDictionary<Metachunk>::atLeast);

    if (chunk == NULL) {
      return NULL;
    }

    if (TraceMetadataHumongousAllocation) {
      // 查找成功，打印日志
      size_t waste = chunk->word_size() - word_size;
      gclog_or_tty->print_cr("Free list allocate humongous chunk size "
                             SIZE_FORMAT " for requested size " SIZE_FORMAT
                             " waste " SIZE_FORMAT,
                             chunk->word_size(), word_size, waste);
    }
  }

  // Chunk is being removed from the chunks free list.
  // 修改计数器
  dec_free_chunks_total(chunk->word_size());

  // Remove it from the links to this freelist
  // 删除对前后节点的引用
  chunk->set_next(NULL);
  chunk->set_prev(NULL);
#ifdef ASSERT
  // Chunk is no longer on any freelist. Setting to false make container_count_slow()
  // work.
  chunk->set_is_tagged_free(false);
#endif
  // 增加Metachunk所属的VirtualSpaceNode的计数器
  chunk->container()->inc_container_count();

  slow_locked_verify();
  return chunk;
}

Metachunk* ChunkManager::chunk_freelist_allocate(size_t word_size) {
  // 获取锁
  assert_lock_strong(SpaceManager::expand_lock());
  // 校验两个计数器的正确
  slow_locked_verify();

  // Take from the beginning of the list
  // 查找满足要求的Metachunk
  Metachunk* chunk = free_chunks_get(word_size);
  if (chunk == NULL) {
    return NULL;
  }

  // 查找成功，校验结果
  assert((word_size <= chunk->word_size()) ||
         list_index(chunk->word_size() == HumongousIndex),
         "Non-humongous variable sized chunk");
  if (TraceMetadataChunkAllocation) {
    size_t list_count;
    // 判断word_size对应哪种类型的Metachunk，获取对应类型的Metachunk链表的长度
    if (list_index(word_size) < HumongousIndex) {
      ChunkList* list = find_free_chunks_list(word_size);
      list_count = list->count();
    } else {
      list_count = humongous_dictionary()->total_count();
    }
    // 打印日志
    gclog_or_tty->print("ChunkManager::chunk_freelist_allocate: " PTR_FORMAT " chunk "
                        PTR_FORMAT "  size " SIZE_FORMAT " count " SIZE_FORMAT " ",
                        this, chunk, chunk->word_size(), list_count);
    locked_print_free_chunks(gclog_or_tty);
  }

  return chunk;
}

void ChunkManager::print_on(outputStream* out) const {
  if (PrintFLSStatistics != 0) {
    const_cast<ChunkManager *>(this)->humongous_dictionary()->report_statistics();
  }
}

// SpaceManager methods

void SpaceManager::get_initial_chunk_sizes(Metaspace::MetaspaceType type,
                                           size_t* chunk_word_size,
                                           size_t* class_chunk_word_size) {
  switch (type) {
  case Metaspace::BootMetaspaceType:
    *chunk_word_size = Metaspace::first_chunk_word_size();
    *class_chunk_word_size = Metaspace::first_class_chunk_word_size();
    break;
  case Metaspace::ROMetaspaceType:
    *chunk_word_size = SharedReadOnlySize / wordSize;
    *class_chunk_word_size = ClassSpecializedChunk;
    break;
  case Metaspace::ReadWriteMetaspaceType:
    *chunk_word_size = SharedReadWriteSize / wordSize;
    *class_chunk_word_size = ClassSpecializedChunk;
    break;
  case Metaspace::AnonymousMetaspaceType:
  case Metaspace::ReflectionMetaspaceType:
    *chunk_word_size = SpecializedChunk;
    *class_chunk_word_size = ClassSpecializedChunk;
    break;
  default:
    *chunk_word_size = SmallChunk;
    *class_chunk_word_size = ClassSmallChunk;
    break;
  }
  assert(*chunk_word_size != 0 && *class_chunk_word_size != 0,
    err_msg("Initial chunks sizes bad: data  " SIZE_FORMAT
            " class " SIZE_FORMAT,
            *chunk_word_size, *class_chunk_word_size));
}

size_t SpaceManager::sum_free_in_chunks_in_use() const {
  MutexLockerEx cl(lock(), Mutex::_no_safepoint_check_flag);
  size_t free = 0;
  for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
    Metachunk* chunk = chunks_in_use(i);
    while (chunk != NULL) {
      free += chunk->free_word_size();
      chunk = chunk->next();
    }
  }
  return free;
}

size_t SpaceManager::sum_waste_in_chunks_in_use() const {
  MutexLockerEx cl(lock(), Mutex::_no_safepoint_check_flag);
  size_t result = 0;
  for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
   result += sum_waste_in_chunks_in_use(i);
  }

  return result;
}

size_t SpaceManager::sum_waste_in_chunks_in_use(ChunkIndex index) const {
  size_t result = 0;
  Metachunk* chunk = chunks_in_use(index);
  // Count the free space in all the chunk but not the
  // current chunk from which allocations are still being done.
  while (chunk != NULL) {
    if (chunk != current_chunk()) {
      result += chunk->free_word_size();
    }
    chunk = chunk->next();
  }
  return result;
}

size_t SpaceManager::sum_capacity_in_chunks_in_use() const {
  // For CMS use "allocated_chunks_words()" which does not need the
  // Metaspace lock.  For the other collectors sum over the
  // lists.  Use both methods as a check that "allocated_chunks_words()"
  // is correct.  That is, sum_capacity_in_chunks() is too expensive
  // to use in the product and allocated_chunks_words() should be used
  // but allow for  checking that allocated_chunks_words() returns the same
  // value as sum_capacity_in_chunks_in_use() which is the definitive
  // answer.
  if (UseConcMarkSweepGC) {
    // CMS不需要使用Metaspace lock，所以不用遍历校验
    return allocated_chunks_words();
  } else {
    MutexLockerEx cl(lock(), Mutex::_no_safepoint_check_flag);
    size_t sum = 0;
    for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
      Metachunk* chunk = chunks_in_use(i);
      // 遍历所有的Metachunk
      while (chunk != NULL) {
        sum += chunk->word_size();
        chunk = chunk->next();
      }
    }
  return sum;
  }
}

size_t SpaceManager::sum_count_in_chunks_in_use() {
  size_t count = 0;
  for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
    count = count + sum_count_in_chunks_in_use(i);
  }

  return count;
}

size_t SpaceManager::sum_count_in_chunks_in_use(ChunkIndex i) {
  size_t count = 0;
  Metachunk* chunk = chunks_in_use(i);
  while (chunk != NULL) {
    count++;
    chunk = chunk->next();
  }
  return count;
}


size_t SpaceManager::sum_used_in_chunks_in_use() const {
  MutexLockerEx cl(lock(), Mutex::_no_safepoint_check_flag);
  size_t used = 0;
  for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
    Metachunk* chunk = chunks_in_use(i);
    while (chunk != NULL) {
      used += chunk->used_word_size();
      chunk = chunk->next();
    }
  }
  return used;
}

void SpaceManager::locked_print_chunks_in_use_on(outputStream* st) const {

  for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
    Metachunk* chunk = chunks_in_use(i);
    st->print("SpaceManager: %s " PTR_FORMAT,
                 chunk_size_name(i), chunk);
    if (chunk != NULL) {
      st->print_cr(" free " SIZE_FORMAT,
                   chunk->free_word_size());
    } else {
      st->print_cr("");
    }
  }

  chunk_manager()->locked_print_free_chunks(st);
  chunk_manager()->locked_print_sum_free_chunks(st);
}

// 根据当前要求分配的内存容量word_size计算分配哪个内存块Metachunk，4种不同大小的内存块。
size_t SpaceManager::calc_chunk_size(size_t word_size) {

  // Decide between a small chunk and a medium chunk.  Up to
  // _small_chunk_limit small chunks can be allocated but
  // once a medium chunk has been allocated, no more small
  // chunks will be allocated.
  // 判断采用MediumChunk还是SmallChunk
  size_t chunk_word_size;
  if (chunks_in_use(MediumIndex) == NULL &&
      sum_count_in_chunks_in_use(SmallIndex) < _small_chunk_limit) {
    chunk_word_size = (size_t) small_chunk_size();
    if (word_size + Metachunk::overhead() > small_chunk_size()) {
      chunk_word_size = medium_chunk_size();
    }
  } else {
    chunk_word_size = medium_chunk_size();
  }

  // Might still need a humongous chunk.  Enforce
  // humongous allocations sizes to be aligned up to
  // the smallest chunk size.
  // 按照SpecializedChunk的内存取整
  size_t if_humongous_sized_chunk =
    align_size_up(word_size + Metachunk::overhead(),
                  smallest_chunk_size());
  chunk_word_size =
    MAX2((size_t) chunk_word_size, if_humongous_sized_chunk);

  // is_humongous为false时，chunk_word_size就是SmallChunk
  // is_humongous为true时，chunk_word_size就是MediumChunk,word_size大于MediumChunk，算出来的if_humongous_sized_chunk肯定大于chunk_word_size
  // 取最大值时，chunk_word_size变成if_humongous_sized_chunk
  assert(!SpaceManager::is_humongous(word_size) ||
         chunk_word_size == if_humongous_sized_chunk,
         err_msg("Size calculation is wrong, word_size " SIZE_FORMAT
                 " chunk_word_size " SIZE_FORMAT,
                 word_size, chunk_word_size));
  if (TraceMetadataHumongousAllocation &&
      SpaceManager::is_humongous(word_size)) {
    gclog_or_tty->print_cr("Metadata humongous allocation:");
    gclog_or_tty->print_cr("  word_size " PTR_FORMAT, word_size);
    gclog_or_tty->print_cr("  chunk_word_size " PTR_FORMAT,
                           chunk_word_size);
    gclog_or_tty->print_cr("    chunk overhead " PTR_FORMAT,
                           Metachunk::overhead());
  }
  return chunk_word_size;
}

void SpaceManager::track_metaspace_memory_usage() {
  if (is_init_completed()) {
    if (is_class()) {
      MemoryService::track_compressed_class_memory_usage();
    }
    MemoryService::track_metaspace_memory_usage();
  }
}

// 从空闲块中找到合适的新块或从VirtualSpaceNode中分配新块
MetaWord* SpaceManager::grow_and_allocate(size_t word_size) {
  // 校验参数
  assert(vs_list()->current_virtual_space() != NULL,
         "Should have been set");
  assert(current_chunk() == NULL ||
         current_chunk()->allocate(word_size) == NULL,
         "Don't need to expand");
  // 获取锁expand_lock
  MutexLockerEx cl(SpaceManager::expand_lock(), Mutex::_no_safepoint_check_flag);

  if (TraceMetadataChunkAllocation && Verbose) {
    size_t words_left = 0;
    size_t words_used = 0;
    if (current_chunk() != NULL) {
      words_left = current_chunk()->free_word_size();
      words_used = current_chunk()->used_word_size();
    }
    // 打印日志
    gclog_or_tty->print_cr("SpaceManager::grow_and_allocate for " SIZE_FORMAT
                           " words " SIZE_FORMAT " words used " SIZE_FORMAT
                           " words left",
                            word_size, words_used, words_left);
  }

  // 计算块的大小并从空闲块中获取，或者从VirtualSpaceNode中新分配一个空闲块
  // Get another chunk out of the virtual space
  size_t grow_chunks_by_words = calc_chunk_size(word_size);
  // 获取Metachunk块
  Metachunk* next = get_new_chunk(word_size, grow_chunks_by_words);

  MetaWord* mem = NULL;

  // If a chunk was available, add it to the in-use chunk list
  // and do an allocation from it.
  // 将分配出的内存块存储到SpaceManager的_chunks_in_use中进行管理，同时从这个新的块中分配请求的内存
  if (next != NULL) {
    // Add to this manager's list of chunks in use.
    // 将新chunk添加到_chunks_in_use数组中
    add_chunk(next, false);
    // 使用新chunk分配内存
    mem = next->allocate(word_size);
  }

  // Track metaspace memory usage statistic.
  track_metaspace_memory_usage();

  return mem;
}

void SpaceManager::print_on(outputStream* st) const {

  for (ChunkIndex i = ZeroIndex;
       i < NumberOfInUseLists ;
       i = next_chunk_index(i) ) {
    st->print_cr("  chunks_in_use " PTR_FORMAT " chunk size " PTR_FORMAT,
                 chunks_in_use(i),
                 chunks_in_use(i) == NULL ? 0 : chunks_in_use(i)->word_size());
  }
  st->print_cr("    waste:  Small " SIZE_FORMAT " Medium " SIZE_FORMAT
               " Humongous " SIZE_FORMAT,
               sum_waste_in_chunks_in_use(SmallIndex),
               sum_waste_in_chunks_in_use(MediumIndex),
               sum_waste_in_chunks_in_use(HumongousIndex));
  // block free lists
  if (block_freelists() != NULL) {
    st->print_cr("total in block free lists " SIZE_FORMAT,
      block_freelists()->total_size());
  }
}

SpaceManager::SpaceManager(Metaspace::MetadataType mdtype,
                           Mutex* lock) :
  _mdtype(mdtype),
  _allocated_blocks_words(0),
  _allocated_chunks_words(0),
  _allocated_chunks_count(0),
  _lock(lock)
{
  initialize();
}

void SpaceManager::inc_size_metrics(size_t words) {
  assert_lock_strong(SpaceManager::expand_lock());
  // Total of allocated Metachunks and allocated Metachunks count
  // for each SpaceManager
  _allocated_chunks_words = _allocated_chunks_words + words;
  _allocated_chunks_count++;
  // Global total of capacity in allocated Metachunks
  MetaspaceAux::inc_capacity(mdtype(), words);
  // Global total of allocated Metablocks.
  // used_words_slow() includes the overhead in each
  // Metachunk so include it in the used when the
  // Metachunk is first added (so only added once per
  // Metachunk).
  MetaspaceAux::inc_used(mdtype(), Metachunk::overhead());
}

void SpaceManager::inc_used_metrics(size_t words) {
  // Add to the per SpaceManager total
  Atomic::add_ptr(words, &_allocated_blocks_words);
  // Add to the global total
  MetaspaceAux::inc_used(mdtype(), words);
}

void SpaceManager::dec_total_from_size_metrics() {
  // 将MetaspaceAux的计数减少
  MetaspaceAux::dec_capacity(mdtype(), allocated_chunks_words());
  // 扣减所有block的内存空间
  MetaspaceAux::dec_used(mdtype(), allocated_blocks_words());
  // Also deduct the overhead per Metachunk
  // 扣减Metachunk自身占用的内存空间
  MetaspaceAux::dec_used(mdtype(), allocated_chunks_count() * Metachunk::overhead());
}

void SpaceManager::initialize() {
  Metadebug::init_allocation_fail_alot_count();
  // 初始化数组元素
  for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
    _chunks_in_use[i] = NULL;
  }
  _current_chunk = NULL;
  if (TraceMetadataChunkAllocation && Verbose) {
    // 打印日志
    gclog_or_tty->print_cr("SpaceManager(): " PTR_FORMAT, this);
  }
}

// return_chunks是将某个Metachunk添加到ChunkManager中
void ChunkManager::return_chunks(ChunkIndex index, Metachunk* chunks) {
  if (chunks == NULL) {
    return;
  }
  // 找到匹配的链表
  ChunkList* list = free_chunks(index);
  assert(list->size() == chunks->word_size(), "Mismatch in chunk sizes");
  assert_lock_strong(SpaceManager::expand_lock());
  Metachunk* cur = chunks;

  // This returns chunks one at a time.  If a new
  // class List can be created that is a base class
  // of FreeList then something like FreeList::prepend()
  // can be used in place of this loop
  while (cur != NULL) {
    assert(cur->container() != NULL, "Container should have been set");
    // 增加Metachunk所属的VirtualSpaceNode的计数器
    cur->container()->dec_container_count();
    // Capture the next link before it is changed
    // by the call to return_chunk_at_head();
    // 获取下一个节点
    Metachunk* next = cur->next();
    DEBUG_ONLY(cur->set_is_tagged_free(true);)
    // 将cur设置为链表头
    list->return_chunk_at_head(cur);
    cur = next;
  }
}

SpaceManager::~SpaceManager() {
  // This call this->_lock which can't be done while holding expand_lock()
  // 校验计数器_allocated_chunks_words的正确
  assert(sum_capacity_in_chunks_in_use() == allocated_chunks_words(),
    err_msg("sum_capacity_in_chunks_in_use() " SIZE_FORMAT
            " allocated_chunks_words() " SIZE_FORMAT,
            sum_capacity_in_chunks_in_use(), allocated_chunks_words()));

  // 获取锁
  MutexLockerEx fcl(SpaceManager::expand_lock(),
                    Mutex::_no_safepoint_check_flag);
  // 校验ChunkManager的计数器的正确
  chunk_manager()->slow_locked_verify();
  // 将MetaspaceAux中的计数器减少
  dec_total_from_size_metrics();

  if (TraceMetadataChunkAllocation && Verbose) {
    // 打印日志
    gclog_or_tty->print_cr("~SpaceManager(): " PTR_FORMAT, this);
    locked_print_chunks_in_use_on(gclog_or_tty);
  }

  // Do not mangle freed Metachunks.  The chunk size inside Metachunks
  // is during the freeing of a VirtualSpaceNodes.

  // Have to update before the chunks_in_use lists are emptied
  // below.
  // 增加空闲的chunk的数量和空闲chunk的内存大小
  chunk_manager()->inc_free_chunks_total(allocated_chunks_words(),
                                         sum_count_in_chunks_in_use());

  // Add all the chunks in use by this space manager
  // to the global list of free chunks.

  // Follow each list of chunks-in-use and add them to the
  // free lists.  Each list is NULL terminated.

  // 将当前SpaceManger使用的所有SpecializedChunk、SmallChunk与MediumChunk交给ChunkManger管理
  for (ChunkIndex i = ZeroIndex; i < HumongousIndex; i = next_chunk_index(i)) {
    if (TraceMetadataChunkAllocation && Verbose) {
      gclog_or_tty->print_cr("returned %d %s chunks to freelist",
                             sum_count_in_chunks_in_use(i),
                             chunk_size_name(i));
    }
    // 遍历_chunks_in_use中的Chunk，将其归还到ChunkManager
    Metachunk* chunks = chunks_in_use(i);
    chunk_manager()->return_chunks(i, chunks);
    set_chunks_in_use(i, NULL);
    if (TraceMetadataChunkAllocation && Verbose) {
      gclog_or_tty->print_cr("updated freelist count %d %s",
                             chunk_manager()->free_chunks(i)->count(),
                             chunk_size_name(i));
    }
    assert(i != HumongousIndex, "Humongous chunks are handled explicitly later");
  }

  // The medium chunk case may be optimized by passing the head and
  // tail of the medium chunk list to add_at_head().  The tail is often
  // the current chunk but there are probably exceptions.

  // Humongous chunks
  if (TraceMetadataChunkAllocation && Verbose) {
    gclog_or_tty->print_cr("returned %d %s humongous chunks to dictionary",
                            sum_count_in_chunks_in_use(HumongousIndex),
                            chunk_size_name(HumongousIndex));
    gclog_or_tty->print("Humongous chunk dictionary: ");
  }
  // Humongous chunks are never the current chunk.
  // 获取Humongous chunk，因为大小不规整，不能通过return_chunks方法添加到ChunkManager中
  Metachunk* humongous_chunks = chunks_in_use(HumongousIndex);

  while (humongous_chunks != NULL) {
#ifdef ASSERT
    humongous_chunks->set_is_tagged_free(true);
#endif
    if (TraceMetadataChunkAllocation && Verbose) {
      gclog_or_tty->print(PTR_FORMAT " (" SIZE_FORMAT ") ",
                          humongous_chunks,
                          humongous_chunks->word_size());
    }
    // 校验Humongous chunk的内存大小是经过内存取整的
    assert(humongous_chunks->word_size() == (size_t)
           align_size_up(humongous_chunks->word_size(),
                             smallest_chunk_size()),
           err_msg("Humongous chunk size is wrong: word size " SIZE_FORMAT
                   " granularity %d",
                   humongous_chunks->word_size(), smallest_chunk_size()));
    // 获取下一个chunk
    Metachunk* next_humongous_chunks = humongous_chunks->next();
    // 减少VirtualSpaceNode中的计数器
    humongous_chunks->container()->dec_container_count();
    // 归还到_humongous_dictionary中
    chunk_manager()->humongous_dictionary()->return_chunk(humongous_chunks);
    humongous_chunks = next_humongous_chunks;
  }
  if (TraceMetadataChunkAllocation && Verbose) {
    gclog_or_tty->print_cr("");
    gclog_or_tty->print_cr("updated dictionary count %d %s",
                     chunk_manager()->humongous_dictionary()->total_count(),
                     chunk_size_name(HumongousIndex));
  }
  chunk_manager()->slow_locked_verify();
}

const char* SpaceManager::chunk_size_name(ChunkIndex index) const {
  switch (index) {
    case SpecializedIndex:
      return "Specialized";
    case SmallIndex:
      return "Small";
    case MediumIndex:
      return "Medium";
    case HumongousIndex:
      return "Humongous";
    default:
      return NULL;
  }
}

ChunkIndex ChunkManager::list_index(size_t size) {
  switch (size) {
    case SpecializedChunk:
      assert(SpecializedChunk == ClassSpecializedChunk,
             "Need branch for ClassSpecializedChunk");
      return SpecializedIndex;
    case SmallChunk:
    case ClassSmallChunk:
      return SmallIndex;
    case MediumChunk:
    case ClassMediumChunk:
      return MediumIndex;
    default:
      assert(size > MediumChunk || size > ClassMediumChunk,
             "Not a humongous chunk");
      return HumongousIndex;
  }
}

// 用于释放这个内存块，将内存块作为MetaBlock归还到BlockFreelist中
void SpaceManager::deallocate(MetaWord* p, size_t word_size) {
  assert_lock_strong(_lock);
  // 获取取整后的字宽数
  size_t raw_word_size = get_raw_word_size(word_size);
  size_t min_size = TreeChunk<Metablock, FreeList>::min_size();
  assert(raw_word_size >= min_size,
         err_msg("Should not deallocate dark matter " SIZE_FORMAT "<" SIZE_FORMAT, word_size, min_size));
  // 将其作为MetaBlock放到BlockFreelist中
  block_freelists()->return_block(p, raw_word_size);
}

// Adds a chunk to the list of chunks in use.
void SpaceManager::add_chunk(Metachunk* new_chunk, bool make_current) {

  assert(new_chunk != NULL, "Should not be NULL");
  assert(new_chunk->next() == NULL, "Should not be on a list");

  // 将其重置成初始状态
  new_chunk->reset_empty();

  // Find the correct list and and set the current
  // chunk for that list.
  // 根据chunk的大小找到匹配的链表
  ChunkIndex index = ChunkManager::list_index(new_chunk->word_size());

  // 如果是规整的chunk
  if (index != HumongousIndex) {
    // 将当前chunk的剩余空间分配成block，放入BlockFreelist中，避免空间浪费
    retire_current_chunk();
    // 将新的chunk设置为当前chunk，原来的chunk作为当前chunk的next节点
    set_current_chunk(new_chunk);
    new_chunk->set_next(chunks_in_use(index));
    set_chunks_in_use(index, new_chunk);
  } else {
    // For null class loader data and DumpSharedSpaces, the first chunk isn't
    // small, so small will be null.  Link this first chunk as the current
    // chunk.
    if (make_current) {
      // Set as the current chunk but otherwise treat as a humongous chunk.
      // 将新chunk设置为当前chunk，SpaceManager::grow_and_allocate中调用时永远传false，Metaspace::initialize_first_chunk中调用时传true
      //只有启动类加载器对应的metaspace才会将_current_chunk设置为一个humongous chunk
      set_current_chunk(new_chunk);
    }
    // Link at head.  The _current_chunk only points to a humongous chunk for
    // the null class loader metaspace (class and data virtual space managers)
    // any humongous chunks so will not point to the tail
    // of the humongous chunks list.
    new_chunk->set_next(chunks_in_use(HumongousIndex));
    set_chunks_in_use(HumongousIndex, new_chunk);

    assert(new_chunk->word_size() > medium_chunk_size(), "List inconsistency");
  }

  // Add to the running sum of capacity // 增加计数
  inc_size_metrics(new_chunk->word_size());

  assert(new_chunk->is_empty(), "Not ready for reuse");
  if (TraceMetadataChunkAllocation && Verbose) {
    gclog_or_tty->print("SpaceManager::add_chunk: %d) ",
                        sum_count_in_chunks_in_use());
    new_chunk->print_on(gclog_or_tty);
    // 打印日志
    chunk_manager()->locked_print_free_chunks(gclog_or_tty);
  }
}

void SpaceManager::retire_current_chunk() {
  if (current_chunk() != NULL) {
    // 获取剩余的内存
    size_t remaining_words = current_chunk()->free_word_size();
    if (remaining_words >= TreeChunk<Metablock, FreeList>::min_size()) {
      // 如果大于最小值，则将其变成block放到BlockFreelist中，避免空间浪费
      block_freelists()->return_block(current_chunk()->allocate(remaining_words), remaining_words);
      inc_used_metrics(remaining_words);
    }
  }
}

// 从空闲列表中获取块或者从VirtualSpaceNode中获取。
Metachunk* SpaceManager::get_new_chunk(size_t word_size,
                                       size_t grow_chunks_by_words) {
  // Get a chunk from the chunk freelist
  // 从空闲列表中获取空闲块
  Metachunk* next = chunk_manager()->chunk_freelist_allocate(grow_chunks_by_words);

  if (next == NULL) {
    // 获取不到，从VirtualSpaceNode中获取。
    next = vs_list()->get_new_chunk(word_size,
                                    grow_chunks_by_words,
                                    medium_chunk_bunch());
  }

  if (TraceMetadataHumongousAllocation && next != NULL &&
      SpaceManager::is_humongous(next->word_size())) {
    gclog_or_tty->print_cr("  new humongous chunk word size "
                           PTR_FORMAT, next->word_size());
  }

  return next;
}

// 分配一个指定大小的内存块，优先从BlockFreelist中分配，分配失败从当前chunk中分配，还是失败则创建一个新的chunk，从新chunk中分配
MetaWord* SpaceManager::allocate(size_t word_size) {
  MutexLockerEx cl(lock(), Mutex::_no_safepoint_check_flag);
  // 获取取整后的字宽数
  size_t raw_word_size = get_raw_word_size(word_size);
  BlockFreelist* fl =  block_freelists();
  MetaWord* p = NULL;
  // Allocation from the dictionary is expensive in the sense that
  // the dictionary has to be searched for a size.  Don't allocate
  // from the dictionary until it starts to get fat.  Is this
  // a reasonable policy?  Maybe an skinny dictionary is fast enough
  // for allocations.  Do some profiling.  JJJ

  // 由于从字典中搜索需要时间，所以只有空闲块中的总量大于allocation_from_dictionary_limit变量定义的4KB时，
  // 为了避免内存过度浪费才会从BlockFreeList中分配内存。

  // 当某次请求的内存大小无法从当前的Metachunk块中分配时，如果当前的Metachunk块剩余的空闲空间相对比较大，
  // 就会将这个Metachunk块保存到_block_freelists中，当下一次分配请求到来时，如果请求的内存比较小，这个块中剩余的空闲空间还能进行分配内存的操作。
  if (fl->total_size() > allocation_from_dictionary_limit) {
    p = fl->get_block(raw_word_size);
  }
  if (p == NULL) {
    // 从当前chunk中分配，如果内存不足则创建一个新的chunk，从新chunk中分配
    p = allocate_work(raw_word_size);
  }

  return p;
}

// Returns the address of spaced allocated for "word_size".
// This methods does not know about blocks (Metablocks)
MetaWord* SpaceManager::allocate_work(size_t word_size) {
  assert_lock_strong(_lock);
#ifdef ASSERT
  if (Metadebug::test_metadata_failure()) {
    return NULL;
  }
#endif
  // Is there space in the current chunk?
  MetaWord* result = NULL;

  // For DumpSharedSpaces, only allocate out of the current chunk which is
  // never null because we gave it the size we wanted.   Caller reports out
  // of memory if this returns null.
  // DumpSharedSpaces表示Dump出共享的space到一个文件中，默认为false
  // 如果DumpSharedSpaces为true则只尝试从当前的chunk中分配，如果分配失败，返回NULL由调用方负责处理
  if (DumpSharedSpaces) {
    assert(current_chunk() != NULL, "should never happen");
    inc_used_metrics(word_size);
    return current_chunk()->allocate(word_size); // caller handles null result
  }

  if (current_chunk() != NULL) {
    // 从当前块中分配内存
    result = current_chunk()->allocate(word_size);
  }

  if (result == NULL) {
    // 如果分配失败，从新的块中分配（从空闲块中找到合适的新块或从VirtualSpaceNode中分配新块）内存。
    result = grow_and_allocate(word_size);
  }

  if (result != NULL) {
    // 如果分配成功，增加计数
    inc_used_metrics(word_size);
    assert(result != (MetaWord*) chunks_in_use(MediumIndex),
           "Head of the list is being allocated");
  }

  return result;
}

void SpaceManager::verify() {
  // If there are blocks in the dictionary, then
  // verfication of chunks does not work since
  // being in the dictionary alters a chunk.
  if (block_freelists()->total_size() == 0) {
    for (ChunkIndex i = ZeroIndex; i < NumberOfInUseLists; i = next_chunk_index(i)) {
      Metachunk* curr = chunks_in_use(i);
      while (curr != NULL) {
        curr->verify();
        verify_chunk_size(curr);
        curr = curr->next();
      }
    }
  }
}

void SpaceManager::verify_chunk_size(Metachunk* chunk) {
  assert(is_humongous(chunk->word_size()) ||
         chunk->word_size() == medium_chunk_size() ||
         chunk->word_size() == small_chunk_size() ||
         chunk->word_size() == specialized_chunk_size(),
         "Chunk size is wrong");
  return;
}

#ifdef ASSERT
void SpaceManager::verify_allocated_blocks_words() {
  // Verification is only guaranteed at a safepoint.
  assert(SafepointSynchronize::is_at_safepoint() || !Universe::is_fully_initialized(),
    "Verification can fail if the applications is running");
  assert(allocated_blocks_words() == sum_used_in_chunks_in_use(),
    err_msg("allocation total is not consistent " SIZE_FORMAT
            " vs " SIZE_FORMAT,
            allocated_blocks_words(), sum_used_in_chunks_in_use()));
}

#endif

void SpaceManager::dump(outputStream* const out) const {
  size_t curr_total = 0;
  size_t waste = 0;
  uint i = 0;
  size_t used = 0;
  size_t capacity = 0;

  // Add up statistics for all chunks in this SpaceManager.
  for (ChunkIndex index = ZeroIndex;
       index < NumberOfInUseLists;
       index = next_chunk_index(index)) {
    for (Metachunk* curr = chunks_in_use(index);
         curr != NULL;
         curr = curr->next()) {
      out->print("%d) ", i++);
      curr->print_on(out);
      curr_total += curr->word_size();
      used += curr->used_word_size();
      capacity += curr->word_size();
      waste += curr->free_word_size() + curr->overhead();;
    }
  }

  if (TraceMetadataChunkAllocation && Verbose) {
    block_freelists()->print_on(out);
  }

  size_t free = current_chunk() == NULL ? 0 : current_chunk()->free_word_size();
  // Free space isn't wasted.
  waste -= free;

  out->print_cr("total of all chunks "  SIZE_FORMAT " used " SIZE_FORMAT
                " free " SIZE_FORMAT " capacity " SIZE_FORMAT
                " waste " SIZE_FORMAT, curr_total, used, free, capacity, waste);
}

#ifndef PRODUCT
void SpaceManager::mangle_freed_chunks() {
  for (ChunkIndex index = ZeroIndex;
       index < NumberOfInUseLists;
       index = next_chunk_index(index)) {
    for (Metachunk* curr = chunks_in_use(index);
         curr != NULL;
         curr = curr->next()) {
      curr->mangle();
    }
  }
}
#endif // PRODUCT

// MetaspaceAux


size_t MetaspaceAux::_allocated_capacity_words[] = {0, 0};
size_t MetaspaceAux::_allocated_used_words[] = {0, 0};

size_t MetaspaceAux::free_bytes(Metaspace::MetadataType mdtype) {
  VirtualSpaceList* list = Metaspace::get_space_list(mdtype);
  return list == NULL ? 0 : list->free_bytes();
}

size_t MetaspaceAux::free_bytes() {
  return free_bytes(Metaspace::ClassType) + free_bytes(Metaspace::NonClassType);
}

void MetaspaceAux::dec_capacity(Metaspace::MetadataType mdtype, size_t words) {
  assert_lock_strong(SpaceManager::expand_lock());
  assert(words <= allocated_capacity_words(mdtype),
    err_msg("About to decrement below 0: words " SIZE_FORMAT
            " is greater than _allocated_capacity_words[%u] " SIZE_FORMAT,
            words, mdtype, allocated_capacity_words(mdtype)));
  _allocated_capacity_words[mdtype] -= words;
}

void MetaspaceAux::inc_capacity(Metaspace::MetadataType mdtype, size_t words) {
  assert_lock_strong(SpaceManager::expand_lock());
  // Needs to be atomic
  _allocated_capacity_words[mdtype] += words;
}

void MetaspaceAux::dec_used(Metaspace::MetadataType mdtype, size_t words) {
  assert(words <= allocated_used_words(mdtype),
    err_msg("About to decrement below 0: words " SIZE_FORMAT
            " is greater than _allocated_used_words[%u] " SIZE_FORMAT,
            words, mdtype, allocated_used_words(mdtype)));
  // For CMS deallocation of the Metaspaces occurs during the
  // sweep which is a concurrent phase.  Protection by the expand_lock()
  // is not enough since allocation is on a per Metaspace basis
  // and protected by the Metaspace lock.
  jlong minus_words = (jlong) - (jlong) words;
  Atomic::add_ptr(minus_words, &_allocated_used_words[mdtype]);
}

void MetaspaceAux::inc_used(Metaspace::MetadataType mdtype, size_t words) {
  // _allocated_used_words tracks allocations for
  // each piece of metadata.  Those allocations are
  // generally done concurrently by different application
  // threads so must be done atomically.
  Atomic::add_ptr(words, &_allocated_used_words[mdtype]);
}

size_t MetaspaceAux::used_bytes_slow(Metaspace::MetadataType mdtype) {
  size_t used = 0;
  // ClassLoaderDataGraphMetaspaceIterator的构造方法会获取ClassLoaderData链表的头元素，从头元素开始依次遍历
  ClassLoaderDataGraphMetaspaceIterator iter;
  // 遍历所有的ClassLoader
  while (iter.repeat()) {
    Metaspace* msp = iter.get_next();
    // Sum allocated_blocks_words for each metaspace
    if (msp != NULL) {
      used += msp->used_words_slow(mdtype);
    }
  }
  return used * BytesPerWord;
}

size_t MetaspaceAux::free_bytes_slow(Metaspace::MetadataType mdtype) {
  size_t free = 0;
  ClassLoaderDataGraphMetaspaceIterator iter;
  while (iter.repeat()) {
    Metaspace* msp = iter.get_next();
    if (msp != NULL) {
      free += msp->free_words_slow(mdtype);
    }
  }
  return free * BytesPerWord;
}

size_t MetaspaceAux::capacity_bytes_slow(Metaspace::MetadataType mdtype) {
  if ((mdtype == Metaspace::ClassType) && !Metaspace::using_class_space()) {
    return 0;
  }
  // Don't count the space in the freelists.  That space will be
  // added to the capacity calculation as needed.
  size_t capacity = 0;
  ClassLoaderDataGraphMetaspaceIterator iter;
  while (iter.repeat()) {
    Metaspace* msp = iter.get_next();
    if (msp != NULL) {
      capacity += msp->capacity_words_slow(mdtype);
    }
  }
  return capacity * BytesPerWord;
}

size_t MetaspaceAux::capacity_bytes_slow() {
#ifdef PRODUCT
  // Use allocated_capacity_bytes() in PRODUCT instead of this function.
  guarantee(false, "Should not call capacity_bytes_slow() in the PRODUCT");
#endif
  size_t class_capacity = capacity_bytes_slow(Metaspace::ClassType);
  size_t non_class_capacity = capacity_bytes_slow(Metaspace::NonClassType);
  assert(allocated_capacity_bytes() == class_capacity + non_class_capacity,
      err_msg("bad accounting: allocated_capacity_bytes() " SIZE_FORMAT
        " class_capacity + non_class_capacity " SIZE_FORMAT
        " class_capacity " SIZE_FORMAT " non_class_capacity " SIZE_FORMAT,
        allocated_capacity_bytes(), class_capacity + non_class_capacity,
        class_capacity, non_class_capacity));

  return class_capacity + non_class_capacity;
}

size_t MetaspaceAux::reserved_bytes(Metaspace::MetadataType mdtype) {
  VirtualSpaceList* list = Metaspace::get_space_list(mdtype);
  return list == NULL ? 0 : list->reserved_bytes();
}

size_t MetaspaceAux::committed_bytes(Metaspace::MetadataType mdtype) {
  VirtualSpaceList* list = Metaspace::get_space_list(mdtype);
  return list == NULL ? 0 : list->committed_bytes();
}

size_t MetaspaceAux::min_chunk_size_words() { return Metaspace::first_chunk_word_size(); }

size_t MetaspaceAux::free_chunks_total_words(Metaspace::MetadataType mdtype) {
  ChunkManager* chunk_manager = Metaspace::get_chunk_manager(mdtype);
  if (chunk_manager == NULL) {
    return 0;
  }
  chunk_manager->slow_verify();
  return chunk_manager->free_chunks_total_words();
}

size_t MetaspaceAux::free_chunks_total_bytes(Metaspace::MetadataType mdtype) {
  return free_chunks_total_words(mdtype) * BytesPerWord;
}

size_t MetaspaceAux::free_chunks_total_words() {
  return free_chunks_total_words(Metaspace::ClassType) +
         free_chunks_total_words(Metaspace::NonClassType);
}

size_t MetaspaceAux::free_chunks_total_bytes() {
  return free_chunks_total_words() * BytesPerWord;
}

void MetaspaceAux::print_metaspace_change(size_t prev_metadata_used) {
  gclog_or_tty->print(", [Metaspace:");
  if (PrintGCDetails && Verbose) {
    gclog_or_tty->print(" "  SIZE_FORMAT
                        "->" SIZE_FORMAT
                        "("  SIZE_FORMAT ")",
                        prev_metadata_used,
                        allocated_used_bytes(),
                        reserved_bytes());
  } else {
    gclog_or_tty->print(" "  SIZE_FORMAT "K"
                        "->" SIZE_FORMAT "K"
                        "("  SIZE_FORMAT "K)",
                        prev_metadata_used/K,
                        allocated_used_bytes()/K,
                        reserved_bytes()/K);
  }

  gclog_or_tty->print("]");
}

// This is printed when PrintGCDetails
void MetaspaceAux::print_on(outputStream* out) {
  Metaspace::MetadataType nct = Metaspace::NonClassType;

  out->print_cr(" Metaspace       "
                "used "      SIZE_FORMAT "K, "
                "capacity "  SIZE_FORMAT "K, "
                "committed " SIZE_FORMAT "K, "
                "reserved "  SIZE_FORMAT "K",
                allocated_used_bytes()/K,
                allocated_capacity_bytes()/K,
                committed_bytes()/K,
                reserved_bytes()/K);

  if (Metaspace::using_class_space()) {
    Metaspace::MetadataType ct = Metaspace::ClassType;
    out->print_cr("  class space    "
                  "used "      SIZE_FORMAT "K, "
                  "capacity "  SIZE_FORMAT "K, "
                  "committed " SIZE_FORMAT "K, "
                  "reserved "  SIZE_FORMAT "K",
                  allocated_used_bytes(ct)/K,
                  allocated_capacity_bytes(ct)/K,
                  committed_bytes(ct)/K,
                  reserved_bytes(ct)/K);
  }
}

// Print information for class space and data space separately.
// This is almost the same as above.
void MetaspaceAux::print_on(outputStream* out, Metaspace::MetadataType mdtype) {
  size_t free_chunks_capacity_bytes = free_chunks_total_bytes(mdtype);
  size_t capacity_bytes = capacity_bytes_slow(mdtype);
  size_t used_bytes = used_bytes_slow(mdtype);
  size_t free_bytes = free_bytes_slow(mdtype);
  size_t used_and_free = used_bytes + free_bytes +
                           free_chunks_capacity_bytes;
  out->print_cr("  Chunk accounting: used in chunks " SIZE_FORMAT
             "K + unused in chunks " SIZE_FORMAT "K  + "
             " capacity in free chunks " SIZE_FORMAT "K = " SIZE_FORMAT
             "K  capacity in allocated chunks " SIZE_FORMAT "K",
             used_bytes / K,
             free_bytes / K,
             free_chunks_capacity_bytes / K,
             used_and_free / K,
             capacity_bytes / K);
  // Accounting can only be correct if we got the values during a safepoint
  assert(!SafepointSynchronize::is_at_safepoint() || used_and_free == capacity_bytes, "Accounting is wrong");
}

// Print total fragmentation for class metaspaces
void MetaspaceAux::print_class_waste(outputStream* out) {
  assert(Metaspace::using_class_space(), "class metaspace not used");
  size_t cls_specialized_waste = 0, cls_small_waste = 0, cls_medium_waste = 0;
  size_t cls_specialized_count = 0, cls_small_count = 0, cls_medium_count = 0, cls_humongous_count = 0;
  ClassLoaderDataGraphMetaspaceIterator iter;
  while (iter.repeat()) {
    Metaspace* msp = iter.get_next();
    if (msp != NULL) {
      cls_specialized_waste += msp->class_vsm()->sum_waste_in_chunks_in_use(SpecializedIndex);
      cls_specialized_count += msp->class_vsm()->sum_count_in_chunks_in_use(SpecializedIndex);
      cls_small_waste += msp->class_vsm()->sum_waste_in_chunks_in_use(SmallIndex);
      cls_small_count += msp->class_vsm()->sum_count_in_chunks_in_use(SmallIndex);
      cls_medium_waste += msp->class_vsm()->sum_waste_in_chunks_in_use(MediumIndex);
      cls_medium_count += msp->class_vsm()->sum_count_in_chunks_in_use(MediumIndex);
      cls_humongous_count += msp->class_vsm()->sum_count_in_chunks_in_use(HumongousIndex);
    }
  }
  out->print_cr(" class: " SIZE_FORMAT " specialized(s) " SIZE_FORMAT ", "
                SIZE_FORMAT " small(s) " SIZE_FORMAT ", "
                SIZE_FORMAT " medium(s) " SIZE_FORMAT ", "
                "large count " SIZE_FORMAT,
                cls_specialized_count, cls_specialized_waste,
                cls_small_count, cls_small_waste,
                cls_medium_count, cls_medium_waste, cls_humongous_count);
}

// Print total fragmentation for data and class metaspaces separately
void MetaspaceAux::print_waste(outputStream* out) {
  size_t specialized_waste = 0, small_waste = 0, medium_waste = 0;
  size_t specialized_count = 0, small_count = 0, medium_count = 0, humongous_count = 0;

  ClassLoaderDataGraphMetaspaceIterator iter;
  while (iter.repeat()) {
    Metaspace* msp = iter.get_next();
    if (msp != NULL) {
      specialized_waste += msp->vsm()->sum_waste_in_chunks_in_use(SpecializedIndex);
      specialized_count += msp->vsm()->sum_count_in_chunks_in_use(SpecializedIndex);
      small_waste += msp->vsm()->sum_waste_in_chunks_in_use(SmallIndex);
      small_count += msp->vsm()->sum_count_in_chunks_in_use(SmallIndex);
      medium_waste += msp->vsm()->sum_waste_in_chunks_in_use(MediumIndex);
      medium_count += msp->vsm()->sum_count_in_chunks_in_use(MediumIndex);
      humongous_count += msp->vsm()->sum_count_in_chunks_in_use(HumongousIndex);
    }
  }
  out->print_cr("Total fragmentation waste (words) doesn't count free space");
  out->print_cr("  data: " SIZE_FORMAT " specialized(s) " SIZE_FORMAT ", "
                        SIZE_FORMAT " small(s) " SIZE_FORMAT ", "
                        SIZE_FORMAT " medium(s) " SIZE_FORMAT ", "
                        "large count " SIZE_FORMAT,
             specialized_count, specialized_waste, small_count,
             small_waste, medium_count, medium_waste, humongous_count);
  if (Metaspace::using_class_space()) {
    print_class_waste(out);
  }
}

// Dump global metaspace things from the end of ClassLoaderDataGraph
void MetaspaceAux::dump(outputStream* out) {
  out->print_cr("All Metaspace:");
  out->print("data space: "); print_on(out, Metaspace::NonClassType);
  out->print("class space: "); print_on(out, Metaspace::ClassType);
  print_waste(out);
}

void MetaspaceAux::verify_free_chunks() {
  Metaspace::chunk_manager_metadata()->verify();
  if (Metaspace::using_class_space()) {
    Metaspace::chunk_manager_class()->verify();
  }
}

void MetaspaceAux::verify_capacity() {
#ifdef ASSERT
  size_t running_sum_capacity_bytes = allocated_capacity_bytes();
  // For purposes of the running sum of capacity, verify against capacity
  size_t capacity_in_use_bytes = capacity_bytes_slow();
  assert(running_sum_capacity_bytes == capacity_in_use_bytes,
    err_msg("allocated_capacity_words() * BytesPerWord " SIZE_FORMAT
            " capacity_bytes_slow()" SIZE_FORMAT,
            running_sum_capacity_bytes, capacity_in_use_bytes));
  for (Metaspace::MetadataType i = Metaspace::ClassType;
       i < Metaspace:: MetadataTypeCount;
       i = (Metaspace::MetadataType)(i + 1)) {
    size_t capacity_in_use_bytes = capacity_bytes_slow(i);
    assert(allocated_capacity_bytes(i) == capacity_in_use_bytes,
      err_msg("allocated_capacity_bytes(%u) " SIZE_FORMAT
              " capacity_bytes_slow(%u)" SIZE_FORMAT,
              i, allocated_capacity_bytes(i), i, capacity_in_use_bytes));
  }
#endif
}

void MetaspaceAux::verify_used() {
#ifdef ASSERT
  size_t running_sum_used_bytes = allocated_used_bytes();
  // For purposes of the running sum of used, verify against used
  size_t used_in_use_bytes = used_bytes_slow();
  assert(allocated_used_bytes() == used_in_use_bytes,
    err_msg("allocated_used_bytes() " SIZE_FORMAT
            " used_bytes_slow()" SIZE_FORMAT,
            allocated_used_bytes(), used_in_use_bytes));
  for (Metaspace::MetadataType i = Metaspace::ClassType;
       i < Metaspace:: MetadataTypeCount;
       i = (Metaspace::MetadataType)(i + 1)) {
    size_t used_in_use_bytes = used_bytes_slow(i);
    assert(allocated_used_bytes(i) == used_in_use_bytes,
      err_msg("allocated_used_bytes(%u) " SIZE_FORMAT
              " used_bytes_slow(%u)" SIZE_FORMAT,
              i, allocated_used_bytes(i), i, used_in_use_bytes));
  }
#endif
}

void MetaspaceAux::verify_metrics() {
  verify_capacity();
  verify_used();
}


// Metaspace methods

size_t Metaspace::_first_chunk_word_size = 0;
size_t Metaspace::_first_class_chunk_word_size = 0;

size_t Metaspace::_commit_alignment = 0;
size_t Metaspace::_reserve_alignment = 0;

Metaspace::Metaspace(Mutex* lock, MetaspaceType type) {
  initialize(lock, type);
}

Metaspace::~Metaspace() {
  // 释放SpaceManager
  delete _vsm;
  if (using_class_space()) {
    delete _class_vsm;
  }
}

VirtualSpaceList* Metaspace::_space_list = NULL;
VirtualSpaceList* Metaspace::_class_space_list = NULL;

ChunkManager* Metaspace::_chunk_manager_metadata = NULL;
ChunkManager* Metaspace::_chunk_manager_class = NULL;

#define VIRTUALSPACEMULTIPLIER 2

#ifdef _LP64
static const uint64_t UnscaledClassSpaceMax = (uint64_t(max_juint) + 1);

void Metaspace::set_narrow_klass_base_and_shift(address metaspace_base, address cds_base) {
  // Figure out the narrow_klass_base and the narrow_klass_shift.  The
  // narrow_klass_base is the lower of the metaspace base and the cds base
  // (if cds is enabled).  The narrow_klass_shift depends on the distance
  // between the lower base and higher address.
  address lower_base;
  address higher_address;
  if (UseSharedSpaces) {
    higher_address = MAX2((address)(cds_base + FileMapInfo::shared_spaces_size()),
                          (address)(metaspace_base + compressed_class_space_size()));
    lower_base = MIN2(metaspace_base, cds_base);
  } else {
    // lower_base是起始地址，higher_address是终止地址
    higher_address = metaspace_base + compressed_class_space_size();
    lower_base = metaspace_base;

    uint64_t klass_encoding_max = UnscaledClassSpaceMax << LogKlassAlignmentInBytes;
    // If compressed class space fits in lower 32G, we don't need a base.
    if (higher_address <= (address)klass_encoding_max) {
      lower_base = 0; // effectively lower base is zero.
    }
  }

  Universe::set_narrow_klass_base(lower_base);

  if ((uint64_t)(higher_address - lower_base) <= UnscaledClassSpaceMax) {
    Universe::set_narrow_klass_shift(0);
  } else {
    assert(!UseSharedSpaces, "Cannot shift with UseSharedSpaces");
    Universe::set_narrow_klass_shift(LogKlassAlignmentInBytes);
  }
}

// Return TRUE if the specified metaspace_base and cds_base are close enough
// to work with compressed klass pointers.
bool Metaspace::can_use_cds_with_metaspace_addr(char* metaspace_base, address cds_base) {
  assert(cds_base != 0 && UseSharedSpaces, "Only use with CDS");
  assert(UseCompressedClassPointers, "Only use with CompressedKlassPtrs");
  address lower_base = MIN2((address)metaspace_base, cds_base);
  address higher_address = MAX2((address)(cds_base + FileMapInfo::shared_spaces_size()),
                                (address)(metaspace_base + compressed_class_space_size()));
  return ((uint64_t)(higher_address - lower_base) <= UnscaledClassSpaceMax);
}

// Try to allocate the metaspace at the requested addr.
void Metaspace::allocate_metaspace_compressed_klass_ptrs(char* requested_addr, address cds_base) {
  // 校验参数
  assert(using_class_space(), "called improperly");
  assert(UseCompressedClassPointers, "Only use with CompressedKlassPtrs");
  assert(compressed_class_space_size() < KlassEncodingMetaspaceMax,
         "Metaspace size is too big");
  assert_is_ptr_aligned(requested_addr, _reserve_alignment);
  assert_is_ptr_aligned(cds_base, _reserve_alignment);
  assert_is_size_aligned(compressed_class_space_size(), _reserve_alignment);

  // Don't use large pages for the class space.
  // 尝试在指定起始地址处申请一段连续的内存空间
  bool large_pages = false;

  ReservedSpace metaspace_rs = ReservedSpace(compressed_class_space_size(),
                                             _reserve_alignment,
                                             large_pages,
                                             requested_addr, 0);
  if (!metaspace_rs.is_reserved()) {
    if (UseSharedSpaces) {
      size_t increment = align_size_up(1*G, _reserve_alignment);

      // Keep trying to allocate the metaspace, increasing the requested_addr
      // by 1GB each time, until we reach an address that will no longer allow
      // use of CDS with compressed klass pointers.
      char *addr = requested_addr;
      while (!metaspace_rs.is_reserved() && (addr + increment > addr) &&
             can_use_cds_with_metaspace_addr(addr + increment, cds_base)) {
        addr = addr + increment;
        metaspace_rs = ReservedSpace(compressed_class_space_size(),
                                     _reserve_alignment, large_pages, addr, 0);
      }
    }

    // If no successful allocation then try to allocate the space anywhere.  If
    // that fails then OOM doom.  At this point we cannot try allocating the
    // metaspace as if UseCompressedClassPointers is off because too much
    // initialization has happened that depends on UseCompressedClassPointers.
    // So, UseCompressedClassPointers cannot be turned off at this point.
    // 忽略起始地址，尝试重新申请，分配失败抛出异常
    if (!metaspace_rs.is_reserved()) {
      metaspace_rs = ReservedSpace(compressed_class_space_size(),
                                   _reserve_alignment, large_pages);
      if (!metaspace_rs.is_reserved()) {
        vm_exit_during_initialization(err_msg("Could not allocate metaspace: %d bytes",
                                              compressed_class_space_size()));
      }
    }
  }

  // If we got here then the metaspace got allocated.
  MemTracker::record_virtual_memory_type((address)metaspace_rs.base(), mtClass);

  // Verify that we can use shared spaces.  Otherwise, turn off CDS.
  if (UseSharedSpaces && !can_use_cds_with_metaspace_addr(metaspace_rs.base(), cds_base)) {
    FileMapInfo::stop_sharing_and_unmap(
        "Could not allocate metaspace at a compatible address");
  }

  // 重置narrow_klass_base和narrow_klass_shift
  set_narrow_klass_base_and_shift((address)metaspace_rs.base(),
                                  UseSharedSpaces ? (address)cds_base : 0);

  initialize_class_space(metaspace_rs);

  // 打印日志
  if (PrintCompressedOopsMode || (PrintMiscellaneous && Verbose)) {
    gclog_or_tty->print_cr("Narrow klass base: " PTR_FORMAT ", Narrow klass shift: " SIZE_FORMAT,
                            Universe::narrow_klass_base(), Universe::narrow_klass_shift());
    gclog_or_tty->print_cr("Compressed class space size: " SIZE_FORMAT " Address: " PTR_FORMAT " Req Addr: " PTR_FORMAT,
                           compressed_class_space_size(), metaspace_rs.base(), requested_addr);
  }
}

// For UseCompressedClassPointers the class space is reserved above the top of
// the Java heap.  The argument passed in is at the base of the compressed space.
void Metaspace::initialize_class_space(ReservedSpace rs) {
  // 初始化_class_space_list和_chunk_manager_class
  // The reserved space size may be bigger because of alignment, esp with UseLargePages
  assert(rs.size() >= CompressedClassSpaceSize,
         err_msg(SIZE_FORMAT " != " UINTX_FORMAT, rs.size(), CompressedClassSpaceSize));
  assert(using_class_space(), "Must be using class space");
  _class_space_list = new VirtualSpaceList(rs);
  _chunk_manager_class = new ChunkManager(SpecializedChunk, ClassSmallChunk, ClassMediumChunk);

  if (!_class_space_list->initialization_succeeded()) {
    vm_exit_during_initialization("Failed to setup compressed class space virtual space list.");
  }
}

#endif

void Metaspace::ergo_initialize() {
  // DumpSharedSpaces表示将共享的Metaspace空间dump到一个文件中，给其他JVM使用，默认为false
  if (DumpSharedSpaces) {
    // Using large pages when dumping the shared archive is currently not implemented.
    FLAG_SET_ERGO(bool, UseLargePagesInMetaspace, false);
  }

  size_t page_size = os::vm_page_size();
  if (UseLargePages && UseLargePagesInMetaspace) {
    page_size = os::large_page_size();
  }

  // 初始化参数
  _commit_alignment  = page_size;
  _reserve_alignment = MAX2(page_size, (size_t)os::vm_allocation_granularity());

  // Do not use FLAG_SET_ERGO to update MaxMetaspaceSize, since this will
  // override if MaxMetaspaceSize was set on the command line or not.
  // This information is needed later to conform to the specification of the
  // java.lang.management.MemoryUsage API.
  //
  // Ideally, we would be able to set the default value of MaxMetaspaceSize in
  // globals.hpp to the aligned value, but this is not possible, since the
  // alignment depends on other flags being parsed.
  MaxMetaspaceSize = align_size_down_bounded(MaxMetaspaceSize, _reserve_alignment);

  if (MetaspaceSize > MaxMetaspaceSize) {
    MetaspaceSize = MaxMetaspaceSize;
  }

  MetaspaceSize = align_size_down_bounded(MetaspaceSize, _commit_alignment);

  assert(MetaspaceSize <= MaxMetaspaceSize, "MetaspaceSize should be limited by MaxMetaspaceSize");

  if (MetaspaceSize < 256*K) {
    vm_exit_during_initialization("Too small initial Metaspace size");
  }

  MinMetaspaceExpansion = align_size_down_bounded(MinMetaspaceExpansion, _commit_alignment);
  MaxMetaspaceExpansion = align_size_down_bounded(MaxMetaspaceExpansion, _commit_alignment);

  CompressedClassSpaceSize = align_size_down_bounded(CompressedClassSpaceSize, _reserve_alignment);
  set_compressed_class_space_size(CompressedClassSpaceSize);
}

void Metaspace::global_initialize() {
  // Initialize the alignment for shared spaces.
  int max_alignment = os::vm_page_size();
  size_t cds_total = 0;

  MetaspaceShared::set_max_alignment(max_alignment);

  // DumpSharedSpaces默认为false
  if (DumpSharedSpaces) {
    SharedReadOnlySize  = align_size_up(SharedReadOnlySize,  max_alignment);
    SharedReadWriteSize = align_size_up(SharedReadWriteSize, max_alignment);
    SharedMiscDataSize  = align_size_up(SharedMiscDataSize,  max_alignment);
    SharedMiscCodeSize  = align_size_up(SharedMiscCodeSize,  max_alignment);

    // Initialize with the sum of the shared space sizes.  The read-only
    // and read write metaspace chunks will be allocated out of this and the
    // remainder is the misc code and data chunks.
    cds_total = FileMapInfo::shared_spaces_size();
    cds_total = align_size_up(cds_total, _reserve_alignment);
    _space_list = new VirtualSpaceList(cds_total/wordSize);
    _chunk_manager_metadata = new ChunkManager(SpecializedChunk, SmallChunk, MediumChunk);

    if (!_space_list->initialization_succeeded()) {
      vm_exit_during_initialization("Unable to dump shared archive.", NULL);
    }

#ifdef _LP64
    if (cds_total + compressed_class_space_size() > UnscaledClassSpaceMax) {
      vm_exit_during_initialization("Unable to dump shared archive.",
          err_msg("Size of archive (" SIZE_FORMAT ") + compressed class space ("
                  SIZE_FORMAT ") == total (" SIZE_FORMAT ") is larger than compressed "
                  "klass limit: " SIZE_FORMAT, cds_total, compressed_class_space_size(),
                  cds_total + compressed_class_space_size(), UnscaledClassSpaceMax));
    }

    // Set the compressed klass pointer base so that decoding of these pointers works
    // properly when creating the shared archive.
    assert(UseCompressedOops && UseCompressedClassPointers,
      "UseCompressedOops and UseCompressedClassPointers must be set");
    Universe::set_narrow_klass_base((address)_space_list->current_virtual_space()->bottom());
    if (TraceMetavirtualspaceAllocation && Verbose) {
      gclog_or_tty->print_cr("Setting_narrow_klass_base to Address: " PTR_FORMAT,
                             _space_list->current_virtual_space()->bottom());
    }

    Universe::set_narrow_klass_shift(0);
#endif

  } else {
    // If using shared space, open the file that contains the shared space
    // and map in the memory before initializing the rest of metaspace (so
    // the addresses don't conflict)
    address cds_address = NULL;
    // UseSharedSpaces默认是false
    if (UseSharedSpaces) {
      FileMapInfo* mapinfo = new FileMapInfo();
      memset(mapinfo, 0, sizeof(FileMapInfo));

      // Open the shared archive file, read and validate the header. If
      // initialization fails, shared spaces [UseSharedSpaces] are
      // disabled and the file is closed.
      // Map in spaces now also
      if (mapinfo->initialize() && MetaspaceShared::map_shared_spaces(mapinfo)) {
        FileMapInfo::set_current_info(mapinfo);
        cds_total = FileMapInfo::shared_spaces_size();
        cds_address = (address)mapinfo->region_base(0);
      } else {
        assert(!mapinfo->is_open() && !UseSharedSpaces,
               "archive file not closed or shared spaces not disabled.");
      }
    }

#ifdef _LP64
    // If UseCompressedClassPointers is set then allocate the metaspace area
    // above the heap and above the CDS area (if it exists).
    // UseCompressedClassPointers为false，DumpSharedSpaces为true时，返回true，64位下默认返回true
    if (using_class_space()) {
      if (UseSharedSpaces) {
        char* cds_end = (char*)(cds_address + cds_total);
        cds_end = (char *)align_ptr_up(cds_end, _reserve_alignment);
        allocate_metaspace_compressed_klass_ptrs(cds_end, cds_address);
      } else {
        // 以堆内存的终止地址作为起始地址申请内存，避免堆内存与Metaspace的内存地址冲突
        char* base = (char*)align_ptr_up(Universe::heap()->reserved_region().end(), _reserve_alignment);
        allocate_metaspace_compressed_klass_ptrs(base, 0);
      }
    }
#endif

    // Initialize these before initializing the VirtualSpaceList
    _first_chunk_word_size = InitialBootClassLoaderMetaspaceSize / BytesPerWord;
    _first_chunk_word_size = align_word_size_up(_first_chunk_word_size);
    // Make the first class chunk bigger than a medium chunk so it's not put
    // on the medium chunk list.   The next chunk will be small and progress
    // from there.  This size calculated by -version.
    _first_class_chunk_word_size = MIN2((size_t)MediumChunk*6,
                                       (CompressedClassSpaceSize/BytesPerWord)*2);
    _first_class_chunk_word_size = align_word_size_up(_first_class_chunk_word_size);
    // Arbitrarily set the initial virtual space to a multiple
    // of the boot class loader size.
    // VIRTUALSPACEMULTIPLIER的取值是2，初始化_space_list和_chunk_manager_metadata
    size_t word_size = VIRTUALSPACEMULTIPLIER * _first_chunk_word_size;
    word_size = align_size_up(word_size, Metaspace::reserve_alignment_words());

    // Initialize the list of virtual spaces.
    _space_list = new VirtualSpaceList(word_size);
    _chunk_manager_metadata = new ChunkManager(SpecializedChunk, SmallChunk, MediumChunk);

    // _space_list初始化失败
    if (!_space_list->initialization_succeeded()) {
      vm_exit_during_initialization("Unable to setup metadata virtual space list.", NULL);
    }
  }

  MetaspaceGC::initialize();
}

Metachunk* Metaspace::get_initialization_chunk(MetadataType mdtype,
                                               size_t chunk_word_size,
                                               size_t chunk_bunch) {
  // Get a chunk from the chunk freelist
  // 从ChunkManager管理的空闲Chunk中分配一个满足大小的chunk
  Metachunk* chunk = get_chunk_manager(mdtype)->chunk_freelist_allocate(chunk_word_size);
  if (chunk != NULL) {
    return chunk;
  }

  // 查找失败从VirtualSpaceList中分配一个新的Chunk
  return get_space_list(mdtype)->get_new_chunk(chunk_word_size, chunk_word_size, chunk_bunch);
}

void Metaspace::initialize(Mutex* lock, MetaspaceType type) {

  assert(space_list() != NULL,
    "Metadata VirtualSpaceList has not been initialized");
  assert(chunk_manager_metadata() != NULL,
    "Metadata ChunkManager has not been initialized");

  // 初始化_vsm和_class_vsm
  _vsm = new SpaceManager(NonClassType, lock);
  if (_vsm == NULL) {
    return;
  }
  size_t word_size;
  size_t class_word_size;
  vsm()->get_initial_chunk_sizes(type, &word_size, &class_word_size);

  if (using_class_space()) {
  assert(class_space_list() != NULL,
    "Class VirtualSpaceList has not been initialized");
  assert(chunk_manager_class() != NULL,
    "Class ChunkManager has not been initialized");

    // Allocate SpaceManager for classes.
    _class_vsm = new SpaceManager(ClassType, lock);
    if (_class_vsm == NULL) {
      return;
    }
  }

  // 获取锁
  MutexLockerEx cl(SpaceManager::expand_lock(), Mutex::_no_safepoint_check_flag);

  // Allocate chunk for metadata objects
  Metachunk* new_chunk = get_initialization_chunk(NonClassType,
                                                  word_size,
                                                  vsm()->medium_chunk_bunch());
  assert(!DumpSharedSpaces || new_chunk != NULL, "should have enough space for both chunks");
  if (new_chunk != NULL) {
    // Add to this manager's list of chunks in use and current_chunk().
    // chunk分配成功，将其添加到SpaceManager中，将其作为当前使用的Chunk
    vsm()->add_chunk(new_chunk, true);
  }

  // Allocate chunk for class metadata objects
  if (using_class_space()) {
    Metachunk* class_chunk = get_initialization_chunk(ClassType,
                                                      class_word_size,
                                                      class_vsm()->medium_chunk_bunch());
    if (class_chunk != NULL) {
      class_vsm()->add_chunk(class_chunk, true);
    }
  }

  _alloc_record_head = NULL;
  _alloc_record_tail = NULL;
}

size_t Metaspace::align_word_size_up(size_t word_size) {
  size_t byte_size = word_size * wordSize;
  return ReservedSpace::allocation_align_size_up(byte_size) / wordSize;
}

MetaWord* Metaspace::allocate(size_t word_size, MetadataType mdtype) {
  // DumpSharedSpaces doesn't use class metadata area (yet)
  // Also, don't use class_vsm() unless UseCompressedClassPointers is true.
  // 根据MetadataType决定是在类指针压缩空间分配内存，还是在元数据区分配内存，但最终都会调用对应的SpaceManager实例的allocate()函数。
  if (is_class_space_allocation(mdtype)) {
    return  class_vsm()->allocate(word_size);
  } else {
    return  vsm()->allocate(word_size);
  }
}

MetaWord* Metaspace::expand_and_allocate(size_t word_size, MetadataType mdtype) {
  // 计算允许扩展的空间
  size_t delta_bytes = MetaspaceGC::delta_capacity_until_GC(word_size * BytesPerWord);
  assert(delta_bytes > 0, "Must be");

  size_t after_inc = MetaspaceGC::inc_capacity_until_GC(delta_bytes);
  size_t before_inc = after_inc - delta_bytes;

  if (PrintGCDetails && Verbose) {
    gclog_or_tty->print_cr("Increase capacity to GC from " SIZE_FORMAT
        " to " SIZE_FORMAT, before_inc, after_inc);
  }

  return allocate(word_size, mdtype);
}

// Space allocated in the Metaspace.  This may
// be across several metadata virtual spaces.
char* Metaspace::bottom() const {
  assert(DumpSharedSpaces, "only useful and valid for dumping shared spaces");
  return (char*)vsm()->current_chunk()->bottom();
}

size_t Metaspace::used_words_slow(MetadataType mdtype) const {
  if (mdtype == ClassType) {
    return using_class_space() ? class_vsm()->sum_used_in_chunks_in_use() : 0;
  } else {
    return vsm()->sum_used_in_chunks_in_use();  // includes overhead!
  }
}

size_t Metaspace::free_words_slow(MetadataType mdtype) const {
  if (mdtype == ClassType) {
    return using_class_space() ? class_vsm()->sum_free_in_chunks_in_use() : 0;
  } else {
    return vsm()->sum_free_in_chunks_in_use();
  }
}

// Space capacity in the Metaspace.  It includes
// space in the list of chunks from which allocations
// have been made. Don't include space in the global freelist and
// in the space available in the dictionary which
// is already counted in some chunk.
size_t Metaspace::capacity_words_slow(MetadataType mdtype) const {
  if (mdtype == ClassType) {
    return using_class_space() ? class_vsm()->sum_capacity_in_chunks_in_use() : 0;
  } else {
    return vsm()->sum_capacity_in_chunks_in_use();
  }
}

size_t Metaspace::used_bytes_slow(MetadataType mdtype) const {
  return used_words_slow(mdtype) * BytesPerWord;
}

size_t Metaspace::capacity_bytes_slow(MetadataType mdtype) const {
  return capacity_words_slow(mdtype) * BytesPerWord;
}

void Metaspace::deallocate(MetaWord* ptr, size_t word_size, bool is_class) {
  // 如果在安全点
  if (SafepointSynchronize::is_at_safepoint()) {
    // 校验必须是VM Thread
    assert(Thread::current()->is_VM_thread(), "should be the VM thread");
    // Don't take Heap_lock
    // 获取锁
    MutexLockerEx ml(vsm()->lock(), Mutex::_no_safepoint_check_flag);
    // 如果word_size过小则不处理
    if (word_size < TreeChunk<Metablock, FreeList>::min_size()) {
      // Dark matter.  Too small for dictionary.
#ifdef ASSERT
      Copy::fill_to_words((HeapWord*)ptr, word_size, 0xf5f5f5f5);
#endif
      return;
    }
    // 通过不同的SpaceManager释放，变成MetaBlock放到block_freelists中重复利用
    if (is_class && using_class_space()) {
      class_vsm()->deallocate(ptr, word_size);
    } else {
      vsm()->deallocate(ptr, word_size);
    }
  } else {
    MutexLockerEx ml(vsm()->lock(), Mutex::_no_safepoint_check_flag);

    if (word_size < TreeChunk<Metablock, FreeList>::min_size()) {
      // Dark matter.  Too small for dictionary.
#ifdef ASSERT
      Copy::fill_to_words((HeapWord*)ptr, word_size, 0xf5f5f5f5);
#endif
      return;
    }
    if (is_class && using_class_space()) {
      class_vsm()->deallocate(ptr, word_size);
    } else {
      vsm()->deallocate(ptr, word_size);
    }
  }
}

// 从ClassLoaderData维护的元空间中申请内存
MetaWord* Metaspace::allocate(ClassLoaderData* loader_data, size_t word_size,
                              bool read_only, MetaspaceObj::Type type, TRAPS) {
  if (HAS_PENDING_EXCEPTION) {
    // 不能有未处理异常
    assert(false, "Should not allocate with exception pending");
    return NULL;  // caller does a CHECK_NULL too
  }

  assert(loader_data != NULL, "Should never pass around a NULL loader_data. "
        "ClassLoaderData::the_null_class_loader_data() should have been used.");

  // Allocate in metaspaces without taking out a lock, because it deadlocks
  // with the SymbolTable_lock.  Dumping is single threaded for now.  We'll have
  // to revisit this for application class data sharing.
  // DumpSharedSpaces默认为false
  if (DumpSharedSpaces) {
    assert(type > MetaspaceObj::UnknownType && type < MetaspaceObj::_number_of_types, "sanity");
    Metaspace* space = read_only ? loader_data->ro_metaspace() : loader_data->rw_metaspace();
    MetaWord* result = space->allocate(word_size, NonClassType);
    if (result == NULL) {
      report_out_of_shared_space(read_only ? SharedReadOnly : SharedReadWrite);
    }

    space->record_allocation(result, type, space->vsm()->get_raw_word_size(word_size));

    // Zero initialize.
    Copy::fill_to_aligned_words((HeapWord*)result, word_size, 0);

    return result;
  }

  MetadataType mdtype = (type == MetaspaceObj::ClassType) ? ClassType : NonClassType;

  // Try to allocate metadata.
  // 调用类加载器的metaspace_non_null()函数获取Metaspace实例并尝试分配空间（元空间是惰性创建的，所以可能是空的。这个方法将在需要时分配一个元空间。）
  MetaWord* result = loader_data->metaspace_non_null()->allocate(word_size, mdtype);

  if (result == NULL) {
    // Allocation failed.
    // 分配空间失败
    if (is_init_completed()) {
      // Only start a GC if the bootstrapping has completed.

      // Try to clean out some memory and retry.
      // 如果类加载器当前的内存块无法满足分配，并且ChunkManager中没有空闲块，VirtualSpaceNode中也分配不出新的内存块，
      // 那么会触发GC进行内存回收后再分配。

      // 如果当前使用的是Serial或Serial Old收集器，那么会调用 CollectorPolicy::satisfy_failed_metadata_allocation 函数，
      result = Universe::heap()->collector_policy()->satisfy_failed_metadata_allocation(
          loader_data, word_size, mdtype);
    }
  }

  if (result == NULL) {
    report_metadata_oome(loader_data, word_size, mdtype, CHECK_NULL);
  }

  // Zero initialize.
  // 将分配的内存清零
  Copy::fill_to_aligned_words((HeapWord*)result, word_size, 0);

  return result;
}

size_t Metaspace::class_chunk_size(size_t word_size) {
  assert(using_class_space(), "Has to use class space");
  return class_vsm()->calc_chunk_size(word_size);
}

void Metaspace::report_metadata_oome(ClassLoaderData* loader_data, size_t word_size, MetadataType mdtype, TRAPS) {
  // If result is still null, we are out of memory.
  if (Verbose && TraceMetadataChunkAllocation) {
    gclog_or_tty->print_cr("Metaspace allocation failed for size "
        SIZE_FORMAT, word_size);
    if (loader_data->metaspace_or_null() != NULL) {
      loader_data->dump(gclog_or_tty);
    }
    MetaspaceAux::dump(gclog_or_tty);
  }

  bool out_of_compressed_class_space = false;
  if (is_class_space_allocation(mdtype)) {
    Metaspace* metaspace = loader_data->metaspace_non_null();
    out_of_compressed_class_space =
      MetaspaceAux::committed_bytes(Metaspace::ClassType) +
      (metaspace->class_chunk_size(word_size) * BytesPerWord) >
      CompressedClassSpaceSize;
  }

  // -XX:+HeapDumpOnOutOfMemoryError and -XX:OnOutOfMemoryError support
  const char* space_string = out_of_compressed_class_space ?
    "Compressed class space" : "Metaspace";

  report_java_out_of_memory(space_string);

  if (JvmtiExport::should_post_resource_exhausted()) {
    JvmtiExport::post_resource_exhausted(
        JVMTI_RESOURCE_EXHAUSTED_OOM_ERROR,
        space_string);
  }

  if (!is_init_completed()) {
    vm_exit_during_initialization("OutOfMemoryError", space_string);
  }

  if (out_of_compressed_class_space) {
    THROW_OOP(Universe::out_of_memory_error_class_metaspace());
  } else {
    THROW_OOP(Universe::out_of_memory_error_metaspace());
  }
}

void Metaspace::record_allocation(void* ptr, MetaspaceObj::Type type, size_t word_size) {
  assert(DumpSharedSpaces, "sanity");

  AllocRecord *rec = new AllocRecord((address)ptr, type, (int)word_size * HeapWordSize);
  if (_alloc_record_head == NULL) {
    _alloc_record_head = _alloc_record_tail = rec;
  } else {
    _alloc_record_tail->_next = rec;
    _alloc_record_tail = rec;
  }
}

void Metaspace::iterate(Metaspace::AllocRecordClosure *closure) {
  assert(DumpSharedSpaces, "unimplemented for !DumpSharedSpaces");

  address last_addr = (address)bottom();

  for (AllocRecord *rec = _alloc_record_head; rec; rec = rec->_next) {
    address ptr = rec->_ptr;
    if (last_addr < ptr) {
      closure->doit(last_addr, MetaspaceObj::UnknownType, ptr - last_addr);
    }
    closure->doit(ptr, rec->_type, rec->_byte_size);
    last_addr = ptr + rec->_byte_size;
  }

  address top = ((address)bottom()) + used_bytes_slow(Metaspace::NonClassType);
  if (last_addr < top) {
    closure->doit(last_addr, MetaspaceObj::UnknownType, top - last_addr);
  }
}

void Metaspace::purge(MetadataType mdtype) {
  get_space_list(mdtype)->purge(get_chunk_manager(mdtype));
}

void Metaspace::purge() {
  MutexLockerEx cl(SpaceManager::expand_lock(),
                   Mutex::_no_safepoint_check_flag);
  // 清理元空间
  purge(NonClassType);
  // 如果使用了类指针压缩空间，则清理此空间
  if (using_class_space()) {
    purge(ClassType);
  }
}

void Metaspace::print_on(outputStream* out) const {
  // Print both class virtual space counts and metaspace.
  if (Verbose) {
    vsm()->print_on(out);
    if (using_class_space()) {
      class_vsm()->print_on(out);
    }
  }
}

bool Metaspace::contains(const void * ptr) {
  if (MetaspaceShared::is_in_shared_space(ptr)) {
    return true;
  }
  // This is checked while unlocked.  As long as the virtualspaces are added
  // at the end, the pointer will be in one of them.  The virtual spaces
  // aren't deleted presently.  When they are, some sort of locking might
  // be needed.  Note, locking this can cause inversion problems with the
  // caller in MetaspaceObj::is_metadata() function.
  return space_list()->contains(ptr) ||
         (using_class_space() && class_space_list()->contains(ptr));
}

void Metaspace::verify() {
  vsm()->verify();
  if (using_class_space()) {
    class_vsm()->verify();
  }
}

void Metaspace::dump(outputStream* const out) const {
  out->print_cr("\nVirtual space manager: " INTPTR_FORMAT, vsm());
  vsm()->dump(out);
  if (using_class_space()) {
    out->print_cr("\nClass space manager: " INTPTR_FORMAT, class_vsm());
    class_vsm()->dump(out);
  }
}

/////////////// Unit tests ///////////////

#ifndef PRODUCT

class TestMetaspaceAuxTest : AllStatic {
 public:
  static void test_reserved() {
    size_t reserved = MetaspaceAux::reserved_bytes();

    assert(reserved > 0, "assert");

    size_t committed  = MetaspaceAux::committed_bytes();
    assert(committed <= reserved, "assert");

    size_t reserved_metadata = MetaspaceAux::reserved_bytes(Metaspace::NonClassType);
    assert(reserved_metadata > 0, "assert");
    assert(reserved_metadata <= reserved, "assert");

    if (UseCompressedClassPointers) {
      size_t reserved_class    = MetaspaceAux::reserved_bytes(Metaspace::ClassType);
      assert(reserved_class > 0, "assert");
      assert(reserved_class < reserved, "assert");
    }
  }

  static void test_committed() {
    size_t committed = MetaspaceAux::committed_bytes();

    assert(committed > 0, "assert");

    size_t reserved  = MetaspaceAux::reserved_bytes();
    assert(committed <= reserved, "assert");

    size_t committed_metadata = MetaspaceAux::committed_bytes(Metaspace::NonClassType);
    assert(committed_metadata > 0, "assert");
    assert(committed_metadata <= committed, "assert");

    if (UseCompressedClassPointers) {
      size_t committed_class    = MetaspaceAux::committed_bytes(Metaspace::ClassType);
      assert(committed_class > 0, "assert");
      assert(committed_class < committed, "assert");
    }
  }

  static void test_virtual_space_list_large_chunk() {
    VirtualSpaceList* vs_list = new VirtualSpaceList(os::vm_allocation_granularity());
    MutexLockerEx cl(SpaceManager::expand_lock(), Mutex::_no_safepoint_check_flag);
    // A size larger than VirtualSpaceSize (256k) and add one page to make it _not_ be
    // vm_allocation_granularity aligned on Windows.
    size_t large_size = (size_t)(2*256*K + (os::vm_page_size()/BytesPerWord));
    large_size += (os::vm_page_size()/BytesPerWord);
    vs_list->get_new_chunk(large_size, large_size, 0);
  }

  static void test() {
    test_reserved();
    test_committed();
    test_virtual_space_list_large_chunk();
  }
};

void TestMetaspaceAux_test() {
  TestMetaspaceAuxTest::test();
}

class TestVirtualSpaceNodeTest {
  static void chunk_up(size_t words_left, size_t& num_medium_chunks,
                                          size_t& num_small_chunks,
                                          size_t& num_specialized_chunks) {
    num_medium_chunks = words_left / MediumChunk;
    words_left = words_left % MediumChunk;

    num_small_chunks = words_left / SmallChunk;
    words_left = words_left % SmallChunk;
    // how many specialized chunks can we get?
    num_specialized_chunks = words_left / SpecializedChunk;
    assert(words_left % SpecializedChunk == 0, "should be nothing left");
  }

 public:
  static void test() {
    MutexLockerEx ml(SpaceManager::expand_lock(), Mutex::_no_safepoint_check_flag);
    const size_t vsn_test_size_words = MediumChunk  * 4;
    const size_t vsn_test_size_bytes = vsn_test_size_words * BytesPerWord;

    // The chunk sizes must be multiples of eachother, or this will fail
    STATIC_ASSERT(MediumChunk % SmallChunk == 0);
    STATIC_ASSERT(SmallChunk % SpecializedChunk == 0);

    { // No committed memory in VSN
      ChunkManager cm(SpecializedChunk, SmallChunk, MediumChunk);
      VirtualSpaceNode vsn(vsn_test_size_bytes);
      vsn.initialize();
      vsn.retire(&cm);
      assert(cm.sum_free_chunks_count() == 0, "did not commit any memory in the VSN");
    }

    { // All of VSN is committed, half is used by chunks
      ChunkManager cm(SpecializedChunk, SmallChunk, MediumChunk);
      VirtualSpaceNode vsn(vsn_test_size_bytes);
      vsn.initialize();
      vsn.expand_by(vsn_test_size_words, vsn_test_size_words);
      vsn.get_chunk_vs(MediumChunk);
      vsn.get_chunk_vs(MediumChunk);
      vsn.retire(&cm);
      assert(cm.sum_free_chunks_count() == 2, "should have been memory left for 2 medium chunks");
      assert(cm.sum_free_chunks() == 2*MediumChunk, "sizes should add up");
    }

    { // 4 pages of VSN is committed, some is used by chunks
      ChunkManager cm(SpecializedChunk, SmallChunk, MediumChunk);
      VirtualSpaceNode vsn(vsn_test_size_bytes);
      const size_t page_chunks = 4 * (size_t)os::vm_page_size() / BytesPerWord;
      assert(page_chunks < MediumChunk, "Test expects medium chunks to be at least 4*page_size");
      vsn.initialize();
      vsn.expand_by(page_chunks, page_chunks);
      vsn.get_chunk_vs(SmallChunk);
      vsn.get_chunk_vs(SpecializedChunk);
      vsn.retire(&cm);

      // committed - used = words left to retire
      const size_t words_left = page_chunks - SmallChunk - SpecializedChunk;

      size_t num_medium_chunks, num_small_chunks, num_spec_chunks;
      chunk_up(words_left, num_medium_chunks, num_small_chunks, num_spec_chunks);

      assert(num_medium_chunks == 0, "should not get any medium chunks");
      assert(cm.sum_free_chunks_count() == (num_small_chunks + num_spec_chunks), "should be space for 3 chunks");
      assert(cm.sum_free_chunks() == words_left, "sizes should add up");
    }

    { // Half of VSN is committed, a humongous chunk is used
      ChunkManager cm(SpecializedChunk, SmallChunk, MediumChunk);
      VirtualSpaceNode vsn(vsn_test_size_bytes);
      vsn.initialize();
      vsn.expand_by(MediumChunk * 2, MediumChunk * 2);
      vsn.get_chunk_vs(MediumChunk + SpecializedChunk); // Humongous chunks will be aligned up to MediumChunk + SpecializedChunk
      vsn.retire(&cm);

      const size_t words_left = MediumChunk * 2 - (MediumChunk + SpecializedChunk);
      size_t num_medium_chunks, num_small_chunks, num_spec_chunks;
      chunk_up(words_left, num_medium_chunks, num_small_chunks, num_spec_chunks);

      assert(num_medium_chunks == 0, "should not get any medium chunks");
      assert(cm.sum_free_chunks_count() == (num_small_chunks + num_spec_chunks), "should be space for 3 chunks");
      assert(cm.sum_free_chunks() == words_left, "sizes should add up");
    }

  }
};

void TestVirtualSpaceNode_test() {
  TestVirtualSpaceNodeTest::test();
}

#endif
