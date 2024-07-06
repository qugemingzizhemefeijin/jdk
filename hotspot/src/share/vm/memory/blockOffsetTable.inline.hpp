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

#ifndef SHARE_VM_MEMORY_BLOCKOFFSETTABLE_INLINE_HPP
#define SHARE_VM_MEMORY_BLOCKOFFSETTABLE_INLINE_HPP

#include "memory/blockOffsetTable.hpp"
#include "memory/space.hpp"
#include "runtime/safepoint.hpp"

//////////////////////////////////////////////////////////////////////////
// BlockOffsetTable inlines
//////////////////////////////////////////////////////////////////////////
inline HeapWord* BlockOffsetTable::block_start(const void* addr) const {
  if (addr >= _bottom && addr < _end) {
    return block_start_unsafe(addr);
  } else {
    return NULL;
  }
}

//////////////////////////////////////////////////////////////////////////
// BlockOffsetSharedArray inlines
//////////////////////////////////////////////////////////////////////////
inline size_t BlockOffsetSharedArray::index_for(const void* p) const {
  char* pc = (char*)p;
  // 校验p在_reserved对应的内存区域范围内
  assert(pc >= (char*)_reserved.start() &&
         pc <  (char*)_reserved.end(),
         "p not in range.");
  // 算出来的delta是字节的数量
  size_t delta = pointer_delta(pc, _reserved.start(), sizeof(char));
  size_t result = delta >> LogN;
  assert(result < _vs.committed_size(), "bad index from address");
  return result;
}

// 传入_next_offset_index+1或end_index计算对应内存卡页的首地址
inline HeapWord* BlockOffsetSharedArray::address_for_index(size_t index) const {
  assert(index < _vs.committed_size(), "bad index");
  // LogN_words=6，index是字，因此只需要向左移动6位即可
  HeapWord* result = _reserved.start() + (index << LogN_words);
  assert(result >= _reserved.start() && result < _reserved.end(),
         "bad address from index");
  return result;
}

inline void BlockOffsetSharedArray::check_reducing_assertion(bool reducing) {
    assert(reducing || !SafepointSynchronize::is_at_safepoint() || init_to_zero() ||
           Thread::current()->is_VM_thread() ||
           Thread::current()->is_ConcurrentGC_thread() ||
           ((!Thread::current()->is_ConcurrentGC_thread()) &&
            ParGCRareEvent_lock->owned_by_self()), "Crack");
}

//////////////////////////////////////////////////////////////////////////
// BlockOffsetArrayNonContigSpace inlines
//////////////////////////////////////////////////////////////////////////
inline void BlockOffsetArrayNonContigSpace::freed(HeapWord* blk,
                                                  size_t size) {
  freed(blk, blk + size);
}

inline void BlockOffsetArrayNonContigSpace::freed(HeapWord* blk_start,
                                                  HeapWord* blk_end) {
  // Verify that the BOT shows [blk_start, blk_end) to be one block.
  // 校验blk_start, blk_end对应的多个slot已经正确设置偏移了
  verify_single_block(blk_start, blk_end);
  // adjust _unallocated_block upward or downward
  // as appropriate
  // BlockOffsetArrayUseUnallocatedBlock表示否是维护_unallocated_block，默认值是false
  if (BlockOffsetArrayUseUnallocatedBlock) {
    assert(_unallocated_block <= _end,
           "Inconsistent value for _unallocated_block");
    // _unallocated_block在这个内存块的范围内，则更新_unallocated_block
    if (blk_end >= _unallocated_block && blk_start <= _unallocated_block) {
      // CMS-specific note: a block abutting _unallocated_block to
      // its left is being freed, a new block is being added or
      // we are resetting following a compaction
      _unallocated_block = blk_start;
    }
  }
}

#endif // SHARE_VM_MEMORY_BLOCKOFFSETTABLE_INLINE_HPP
