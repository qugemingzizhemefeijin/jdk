/*
 * Copyright (c) 1998, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_RUNTIME_VMTHREAD_HPP
#define SHARE_VM_RUNTIME_VMTHREAD_HPP

#include "runtime/perfData.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vm_operations.hpp"

//
// Prioritized queue of VM operations.
//
// Encapsulates both queue management and
// and priority policy
//
// VMOperationQueue类中的_queue保存了SafepointPriority和MediumPriority优先级中的任务列表，
// _queue_length保存了SafepointPriority和MediumPriority优先级中的任务数量。
// 在获取任务时，通常会优先获取SafepointPriority中的任务，也就是需要在安全点中执行的任务。
class VMOperationQueue : public CHeapObj<mtInternal> {
 private:
  enum Priorities {
     SafepointPriority, // Highest priority (operation executed at a safepoint) // 最高优先级，必须在安全点中执行的任务
     MediumPriority,    // Medium priority                                      // 中等优先级，不必在安全点中执行的任务
     nof_priorities                                                             // 优先级的数量
  };

  // We maintain a doubled linked list, with explicit count.
  // 统计SafepointPriority和MediumPriority优先级中的任务数量
  int           _queue_length[nof_priorities];
  int           _queue_counter;
  // 将SafepointPriority和MediumPriority优先级中的任务列表以双链表的形式连接
  VM_Operation* _queue       [nof_priorities];
  // we also allow the vmThread to register the ops it has drained so we
  // can scan them from oops_do
  VM_Operation* _drain_list;

  // Double-linked non-empty list insert.
  void insert(VM_Operation* q,VM_Operation* n);
  void unlink(VM_Operation* q);

  // Basic queue manipulation
  bool queue_empty                (int prio);
  void queue_add_front            (int prio, VM_Operation *op);
  void queue_add_back             (int prio, VM_Operation *op);
  VM_Operation* queue_remove_front(int prio);
  void queue_oops_do(int queue, OopClosure* f);
  void drain_list_oops_do(OopClosure* f);
  VM_Operation* queue_drain(int prio);
  // lock-free query: may return the wrong answer but must not break
  bool queue_peek(int prio) { return _queue_length[prio] > 0; }

 public:
  VMOperationQueue();

  // Highlevel operations. Encapsulates policy
  bool add(VM_Operation *op);
  VM_Operation* remove_next();                        // Returns next or null
  VM_Operation* remove_next_at_safepoint_priority()   { return queue_remove_front(SafepointPriority); }
  // 获取在安全点中执行的任务列表
  VM_Operation* drain_at_safepoint_priority() { return queue_drain(SafepointPriority); }
  void set_drain_list(VM_Operation* list) { _drain_list = list; }
  bool peek_at_safepoint_priority() { return queue_peek(SafepointPriority); }

  // GC support
  void oops_do(OopClosure* f);

  void verify_queue(int prio) PRODUCT_RETURN;
};


//
// A single VMThread (the primordial thread) spawns all other threads
// and is itself used by other threads to offload heavy vm operations
// like scavenge, garbage_collect etc.
//

// VMThread负责调度、执行虚拟机内部的VM线程操作，如GC操作等，在JVM实例创建时进行初始化。
// 继承关系 VMThread -> NamedThread -> Thread -> ThreadShadow

// VMThread主要处理垃圾回收。如果是多线程回收，则启动多个线程回收；如果是单线程回收，使用VMThread回收。
// 在VMThread类中定义了_vm_queue，它是一个队列，任何执行GC的操作都是VM_Operation的一个实例。
// 用户线程JavaThread会通过执行VMThread::execute()函数把相关操作放到队列中，然后由VMThread在run()函数中轮询队列并获取任务

// 调用链：
//   Threads::create_vm()   thread.cpp
//   JNI_CreateJavaVM()     jni.cpp
//   InitializeJVM()        java.c
//   JavaMain()             java.c
//   start_thread()         pthread_create.c

// VMThread、OSThread和pthread线程的关系：
// VMThread._osthread -> OSThread._pthread_id -> pthread
// VMThread中通过_osthread变量保持对OSThread实例的引用，OSThread实例通过_pthread_id来管理pthread线程
class VMThread: public NamedThread {
 private:
  // 当前VMThread线程在执行的虚拟机任务
  static ThreadPriority _current_priority;

  static bool _should_terminate;
  static bool _terminated;
  static Monitor * _terminate_lock;
  static PerfCounter* _perf_accumulated_vm_operation_time;
  // 执行队列中的任务
  void evaluate_operation(VM_Operation* op);
 public:
  // Constructor
  VMThread();

  // Tester
  bool is_VM_thread() const                      { return true; }
  bool is_GC_thread() const                      { return true; }

  // The ever running loop for the VMThread
  // VMThread线程通过循环获取队列任务并执行
  void loop();

  // Called to stop the VM thread
  static void wait_for_vm_thread_exit();
  static bool should_terminate()                  { return _should_terminate; }
  static bool is_terminated()                     { return _terminated == true; }

  // Execution of vm operation
  // 用户线程将任务放到队列中
  static void execute(VM_Operation* op);

  // Returns the current vm operation if any.
  static VM_Operation* vm_operation()             { return _cur_vm_operation;   }

  // Returns the single instance of VMThread.
  static VMThread* vm_thread()                    { return _vm_thread; }

  // GC support
  void oops_do(OopClosure* f, CLDToOopClosure* cld_f, CodeBlobClosure* cf);

  // Debugging
  void print_on(outputStream* st) const;
  void print() const                              { print_on(tty); }
  void verify();

  // Performance measurement
  static PerfCounter* perf_accumulated_vm_operation_time()               { return _perf_accumulated_vm_operation_time; }

  // Entry for starting vm thread
  // 启动VMThread线程
  virtual void run();

  // Creations/Destructions
  static void create();
  static void destroy();

 private:
  // VM_Operation support
  static VM_Operation*     _cur_vm_operation;   // Current VM operation
  // 多个任务放到队列中
  static VMOperationQueue* _vm_queue;           // Queue (w/ policy) of VM operations

  // Pointer to single-instance of VM thread
  static VMThread*     _vm_thread;
};

#endif // SHARE_VM_RUNTIME_VMTHREAD_HPP
