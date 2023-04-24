/*
 * Copyright (c) 2001, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_GENMARKSWEEP_HPP
#define SHARE_VM_MEMORY_GENMARKSWEEP_HPP

#include "gc_implementation/shared/markSweep.hpp"

class GenMarkSweep : public MarkSweep {
  friend class VM_MarkSweep;
  friend class G1MarkSweep;
 public:
  // 调用TenuredGeneration::collect()函数时最终会调用GenMarkSweep::invoke_at_safepoint()函数，
  // 该函数分几个阶段通过压缩整理算法实现年轻代和老年代的垃圾回收。
  // 具体就是将年轻代中Eden、From Survivor与To Survivor空间中的对象压缩在年轻代中，而老年代空间中的对象压缩在老年代空间。
  static void invoke_at_safepoint(int level, ReferenceProcessor* rp,
                                  bool clear_all_softrefs);

 private:

  // Mark live objects  // 递归标记所有活跃对象
  static void mark_sweep_phase1(int level, bool clear_all_softrefs);
  // Calculate new addresses
  static void mark_sweep_phase2();
  // Update pointers
  static void mark_sweep_phase3(int level);
  // Move objects to new positions
  static void mark_sweep_phase4();

  // Temporary data structures for traversal and storing/restoring marks
  static void allocate_stacks();
  static void deallocate_stacks();
};

#endif // SHARE_VM_MEMORY_GENMARKSWEEP_HPP
