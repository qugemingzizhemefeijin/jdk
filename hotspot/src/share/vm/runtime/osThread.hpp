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

#ifndef SHARE_VM_RUNTIME_OSTHREAD_HPP
#define SHARE_VM_RUNTIME_OSTHREAD_HPP

#include "runtime/frame.hpp"
#include "runtime/handles.hpp"
#include "runtime/javaFrameAnchor.hpp"
#include "runtime/objectMonitor.hpp"
#include "utilities/top.hpp"

// The OSThread class holds OS-specific thread information.  It is equivalent
// to the sys_thread_t structure of the classic JVM implementation.

// The thread states represented by the ThreadState values are platform-specific
// and are likely to be only approximate, because most OSes don't give you access
// to precise thread state information.

// Note: the ThreadState is legacy code and is not correctly implemented.
// Uses of ThreadState need to be replaced by the state in the JavaThread.

enum ThreadState {
  ALLOCATED,                    // Memory has been allocated but not initialized
  INITIALIZED,                  // The thread has been initialized but yet started
  RUNNABLE,                     // Has been started and is runnable, but not necessarily running
  MONITOR_WAIT,                 // Waiting on a contended monitor lock
  CONDVAR_WAIT,                 // Waiting on a condition variable
  OBJECT_WAIT,                  // Waiting on an Object.wait() call
  BREAKPOINTED,                 // Suspended at breakpoint
  SLEEPING,                     // Thread.sleep()
  ZOMBIE                        // All done, but not reclaimed yet
};

// I'd make OSThread a ValueObj embedded in Thread to avoid an indirection, but
// the assembler test in java.cpp expects that it can install the OSThread of
// the main thread into its own Thread at will.

// OSThread主要负责把Java线程映射到操作系统线程上，实现Java线程与操作系统线程之间的交互和通信。
// OSThread类是HotSpot虚拟机中非常重要的一个类，它负责将Java线程映射到操作系统线程上，并实现Java线程与操作系统线程之间的交互和通信。
// OSThread类的作用主要包括以下几个方面：
//   1.创建和销毁操作系统线程：OSThread类封装了操作系统线程的创建和销毁接口，并提供了对应的静态方法create和destroy，
//     用于创建和销毁操作系统线程。
//   2.管理操作系统线程的状态：OSThread类维护了操作系统线程的状态，包括线程是否已启动、线程是否已结束等信息。
//     它还提供了相应的方法，用于查询和修改线程状态。
//   3.管理Java线程与操作系统线程之间的映射关系：OSThread类将Java线程和操作系统线程之间建立映射关系，并提供了相应的方法，
//     用于查询和修改Java线程和操作系统线程之间的关系。
//   4.管理操作系统线程的优先级：OSThread类提供了相应的方法，用于查询和修改操作系统线程的优先级。
class OSThread: public CHeapObj<mtThread> {
  friend class VMStructs;
 private:
  OSThreadStartFunc _start_proc;  // Thread start routine               // 线程启动时调用的函数指针
  void* _start_parm;              // Thread start routine parameter     // 线程启动时传递给函数的参数
  volatile ThreadState _state;    // Thread state *hint*                // 线程状态
  volatile jint _interrupted;     // Thread.isInterrupted state         // 线程是否被中断

  // Note:  _interrupted must be jint, so that Java intrinsics can access it.
  // The value stored there must be either 0 or 1.  It must be possible
  // for Java to emulate Thread.currentThread().isInterrupted() by performing
  // the double indirection Thread::current()->_osthread->_interrupted.

  // Methods
 public:
  void set_state(ThreadState state)                { _state = state; }
  ThreadState get_state()                          { return _state; }

  OSThread(OSThreadStartFunc start_proc, void* start_parm);
  ~OSThread();

  // Accessors
  OSThreadStartFunc start_proc() const              { return _start_proc; }
  void set_start_proc(OSThreadStartFunc start_proc) { _start_proc = start_proc; }
  void* start_parm() const                          { return _start_parm; }
  void set_start_parm(void* start_parm)             { _start_parm = start_parm; }

  volatile bool interrupted() const                 { return _interrupted != 0; }
  void set_interrupted(bool z)                      { _interrupted = z ? 1 : 0; }

  // Printing
  void print_on(outputStream* st) const;
  void print() const                                { print_on(tty); }

  // For java intrinsics:
  static ByteSize interrupted_offset()            { return byte_offset_of(OSThread, _interrupted); }

  // Platform dependent stuff
#ifdef TARGET_OS_FAMILY_linux
# include "osThread_linux.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "osThread_solaris.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "osThread_windows.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "osThread_bsd.hpp"
#endif

 public:
  static ByteSize thread_id_offset()              { return byte_offset_of(OSThread, _thread_id); }
  static size_t thread_id_size()                  { return sizeof(thread_id_t); }

  thread_id_t thread_id() const                   { return _thread_id; }

  void set_thread_id(thread_id_t id)              { _thread_id = id; }

 private:
  // _thread_id is kernel thread id (similar to LWP id on Solaris). Each
  // thread has a unique thread_id (BsdThreads or NPTL). It can be used
  // to access /proc.
  thread_id_t _thread_id;                          // 操作系统中线程的唯一标识符
};


// Utility class for use with condition variables:
class OSThreadWaitState : public StackObj {
  OSThread*   _osthread;
  ThreadState _old_state;
 public:
  OSThreadWaitState(OSThread* osthread, bool is_object_wait) {
    _osthread  = osthread;
    _old_state = osthread->get_state();
    if (is_object_wait) {
      osthread->set_state(OBJECT_WAIT);
    } else {
      osthread->set_state(CONDVAR_WAIT);
    }
  }
  ~OSThreadWaitState() {
    _osthread->set_state(_old_state);
  }
};


// Utility class for use with contended monitors:
class OSThreadContendState : public StackObj {
  OSThread*   _osthread;
  ThreadState _old_state;
 public:
  OSThreadContendState(OSThread* osthread) {
    _osthread  = osthread;
    _old_state = osthread->get_state();
    osthread->set_state(MONITOR_WAIT);
  }
  ~OSThreadContendState() {
    _osthread->set_state(_old_state);
  }
};

#endif // SHARE_VM_RUNTIME_OSTHREAD_HPP
