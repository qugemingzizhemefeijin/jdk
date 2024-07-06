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

#ifndef SHARE_VM_RUNTIME_SAFEPOINT_HPP
#define SHARE_VM_RUNTIME_SAFEPOINT_HPP

#include "asm/assembler.hpp"
#include "code/nmethod.hpp"
#include "memory/allocation.hpp"
#include "runtime/extendedPC.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "utilities/ostream.hpp"

//
// Safepoint synchronization
////
// The VMThread or CMS_thread uses the SafepointSynchronize::begin/end
// methods to enter/exit a safepoint region. The begin method will roll
// all JavaThreads forward to a safepoint.
//
// JavaThreads must use the ThreadSafepointState abstraction (defined in
// thread.hpp) to indicate that that they are at a safepoint.
//
// The Mutex/Condition variable and ObjectLocker classes calls the enter/
// exit safepoint methods, when a thread is blocked/restarted. Hence, all mutex exter/
// exit points *must* be at a safepoint.


class ThreadSafepointState;
class SnippetCache;
class nmethod;

//
// Implements roll-forward to safepoint (safepoint synchronization)
//
// 安全点会让Mutator线程运行到一些特殊位置后主动暂停，这样就可以让GC线程进行垃圾回收或者导出堆栈等操作。
// 当有垃圾回收任务时，通常会产生一个VM_Operation任务并将其放到VMThread队列中，VMThread会循环处理这个队列中的任务，
// 其中在处理任务时需要有一个进入安全点的操作，任务完成后还要退出安全点。

// 当所有线程都进入安全点后，VMThread才能继续执行后面的代码。
// 进入安全点时Java线程可能存在几种不同的状态，这里需要处理所有可能存在的情况。
// 1 处于解释执行字节码的状态，解释器在通过字节码派发表（Dispatch Table）获取下一条字节码的时候会主动检查安全点的状态。
// 2 处于执行native代码的状态，也就是执行JNI。此时VMThread不会等待线程进入安全点。
//   执行JNI退出后线程需要主动检查安全点状态，如果此时安全点位置被标记了，那么就不能继续执行，需要等待安全点位置被清除后才能继续执行。
// 3 处于编译代码执行状态，编译器会在合适的位置（例如循环、方法调用等）插入读取全局Safepoint Polling内存页的指令，
//   如果此时安全点位置被标记了，那么Safepoint Polling内存页会变成不可读，此时线程会因为读取了不可读的内存页而陷入内核态，
//   事先注册好的信号处理程序就会处理这个信号并让线程进入安全点。
// 4 线程本身处于blocked状态，例如线程在等待锁，那么线程的阻塞状态将不会结束直到安全点标志被清除。
// 5 当线程处于以上(1)至(3)3种状态切换阶段，切换前会先检查安全点的状态，如果此时要求进入安全点，那么切换将不被允许，需要等待，直到安全点状态被清除。
//
class SafepointSynchronize : AllStatic {
 public:
  enum SynchronizeState {
      // 表示所有线程都不在安全点上
      _not_synchronized = 0,                   // Threads not synchronized at a safepoint
                                               // Keep this value 0. See the coment in do_call_back()
      // 表示所有线程在执行安全点同步中
      _synchronizing    = 1,                   // Synchronizing in progress
      // 所有的线程都已经进入安全点，只有VMThread线程在运行
      _synchronized     = 2                    // All Java threads are stopped at a safepoint. Only VM thread is running
  };

  enum SafepointingThread {
      _null_thread  = 0,
      _vm_thread    = 1,
      _other_thread = 2
  };

  enum SafepointTimeoutReason {
    _spinning_timeout = 0,
    _blocking_timeout = 1
  };

  typedef struct {
    float  _time_stamp;                        // record when the current safepoint occurs in seconds
    int    _vmop_type;                         // type of VM operation triggers the safepoint
    int    _nof_total_threads;                 // total number of Java threads
    int    _nof_initial_running_threads;       // total number of initially seen running threads
    int    _nof_threads_wait_to_block;         // total number of threads waiting for to block
    bool   _page_armed;                        // true if polling page is armed, false otherwise
    int    _nof_threads_hit_page_trap;         // total number of threads hitting the page trap
    jlong  _time_to_spin;                      // total time in millis spent in spinning
    jlong  _time_to_wait_to_block;             // total time in millis spent in waiting for to block
    jlong  _time_to_do_cleanups;               // total time in millis spent in performing cleanups
    jlong  _time_to_sync;                      // total time in millis spent in getting to _synchronized
    jlong  _time_to_exec_vmop;                 // total time in millis spent in vm operation itself
  } SafepointStats;

 private:
  // 线程同步的状态
  static volatile SynchronizeState _state;     // Threads might read this flag directly, without acquireing the Threads_lock
  // VMThread线程要等待阻塞的用户线程数，只有这些线程全部阻塞时，VMThread线程才能在安全点下执行垃圾回收操作。
  static volatile int _waiting_to_block;       // number of threads we are waiting for to block // 等待被阻塞（同步）的线程数
  static int _current_jni_active_count;        // Counts the number of active critical natives during the safepoint // 记录安全点期间处于JNI关键区的线程的总数

  // This counter is used for fast versions of jni_Get<Primitive>Field.
  // An even value means there is no ongoing safepoint operations.
  // The counter is incremented ONLY at the beginning and end of each
  // safepoint. The fact that Threads_lock is held throughout each pair of
  // increments (at the beginning and end of each safepoint) guarantees
  // race freedom.
public:
  static volatile int _safepoint_counter;       // 进入和退出安全点的总次数，进入和退出时都会加1
private:
  static long       _end_of_last_safepoint;     // Time of last safepoint in milliseconds           // 上一次退出安全点的时间

  // statistics
  static jlong            _safepoint_begin_time;     // time when safepoint begins                  // 进入安全点的时间，单位是纳秒
  static SafepointStats*  _safepoint_stats;          // array of SafepointStats struct              // SafepointStats数组，剩下几个参数都是跟SafepointStats配合使用，用来统计安全点相关的数据
  static int              _cur_stat_index;           // current index to the above array
  static julong           _safepoint_reasons[];      // safepoint count for each VM op
  static julong           _coalesced_vmop_count;     // coalesced vmop count
  static jlong            _max_sync_time;            // maximum sync time in nanos
  static jlong            _max_vmop_time;            // maximum vm operation time in nanos
  static float            _ts_of_current_safepoint;  // time stamp of current safepoint in seconds  // 进入安全点的时间，单位是秒

  static void begin_statistics(int nof_threads, int nof_running);
  static void update_statistics_on_spin_end();
  static void update_statistics_on_sync_end(jlong end_time);
  static void update_statistics_on_cleanup_end(jlong end_time);
  static void end_statistics(jlong end_time);
  static void print_statistics();
  inline static void inc_page_trap_count() {
    Atomic::inc(&_safepoint_stats[_cur_stat_index]._nof_threads_hit_page_trap);
  }

  // For debug long safepoint
  static void print_safepoint_timeout(SafepointTimeoutReason timeout_reason);

public:

  // Main entry points

  // Roll all threads forward to safepoint. Must be called by the
  // VMThread or CMS_thread.
  // 进入安全点（将所有线程前滚到安全点。必须由 VMThread 或 CMS_thread 调用。）
  static void begin();
  // 退出安全点（再次启动所有挂起的线程...）
  static void end();                    // Start all suspended threads again...

  static bool safepoint_safe(JavaThread *thread, JavaThreadState state);

  static void check_for_lazy_critical_native(JavaThread *thread, JavaThreadState state);

  // Query
  inline static bool is_at_safepoint()   { return _state == _synchronized;  }
  inline static bool is_synchronizing()  { return _state == _synchronizing;  }

  inline static bool do_call_back() {
    return (_state != _not_synchronized);
  }

  inline static void increment_jni_active_count() {
    assert_locked_or_safepoint(Safepoint_lock);
    _current_jni_active_count++;
  }

  // Called when a thread volantary blocks
  static void   block(JavaThread *thread);
  static void   signal_thread_at_safepoint()              { _waiting_to_block--; }

  // Exception handling for page polling
  static void handle_polling_page_exception(JavaThread *thread);

  // VM Thread interface for determining safepoint rate
  static long last_non_safepoint_interval() {
    return os::javaTimeMillis() - _end_of_last_safepoint;
  }
  static long end_of_last_safepoint() {
    return _end_of_last_safepoint;
  }
  static bool is_cleanup_needed();
  static void do_cleanup_tasks();

  // debugging
  static void print_state()                                PRODUCT_RETURN;
  static void safepoint_msg(const char* format, ...)       PRODUCT_RETURN;

  static void deferred_initialize_stat();
  static void print_stat_on_exit();
  inline static void inc_vmop_coalesced_count() { _coalesced_vmop_count++; }

  static void set_is_at_safepoint()                        { _state = _synchronized; }
  static void set_is_not_at_safepoint()                    { _state = _not_synchronized; }

  // assembly support
  static address address_of_state()                        { return (address)&_state; }

  static address safepoint_counter_addr()                  { return (address)&_safepoint_counter; }
};

// State class for a thread suspended at a safepoint
class ThreadSafepointState: public CHeapObj<mtInternal> {
 public:
  // These states are maintained by VM thread while threads are being brought
  // to a safepoint.  After SafepointSynchronize::end(), they are reset to
  // _running.
  enum suspend_type {
    // 线程不在安全点上
    _running                =  0, // Thread state not yet determined (i.e., not at a safepoint yet)
    // 线程已经在安全点上
    _at_safepoint           =  1, // Thread at a safepoint (f.ex., when blocked on a lock)
    // 线程会继续执行，不过在必要时会执行回调
    _call_back              =  2  // Keep executing and wait for callback (if thread is in interpreted or vm)
  };
 private:
  volatile bool _at_poll_safepoint;  // At polling page safepoint (NOT a poll return safepoint) // 为true表示当前线程处于基于poll page实现的安全点上
  // Thread has called back the safepoint code (for debugging)
  bool                           _has_called_back;  // 是否已经执行了block方法

  JavaThread *                   _thread;           // 关键的JavaThread实例
  volatile suspend_type          _type;             // 具体的状态
  JavaThreadState                _orig_thread_state;// 获取原来的状态


 public:
  ThreadSafepointState(JavaThread *thread);

  // examine/roll-forward/restart
  // 状态检查
  void examine_state_of_thread();
  void roll_forward(suspend_type type);
  void restart();

  // Query
  JavaThread*  thread() const         { return _thread; }
  suspend_type type() const           { return _type; }
  bool         is_running() const     { return (_type==_running); }
  JavaThreadState orig_thread_state() const { return _orig_thread_state; }

  // Support for safepoint timeout (debugging)
  bool has_called_back() const                   { return _has_called_back; }
  void set_has_called_back(bool val)             { _has_called_back = val; }
  bool              is_at_poll_safepoint() { return _at_poll_safepoint; }
  void              set_at_poll_safepoint(bool val) { _at_poll_safepoint = val; }

  void handle_polling_page_exception();

  // debugging
  void print_on(outputStream* st) const;
  void print() const                        { print_on(tty); }

  // Initialize
  static void create(JavaThread *thread);
  static void destroy(JavaThread *thread);

  void safepoint_msg(const char* format, ...) {
    if (ShowSafepointMsgs) {
      va_list ap;
      va_start(ap, format);
      tty->vprint_cr(format, ap);
      va_end(ap);
    }
  }
};



#endif // SHARE_VM_RUNTIME_SAFEPOINT_HPP
