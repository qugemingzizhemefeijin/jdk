/*
 * Copyright (c) 1998, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_RUNTIME_SYNCHRONIZER_HPP
#define SHARE_VM_RUNTIME_SYNCHRONIZER_HPP

#include "oops/markOop.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/handles.hpp"
#include "runtime/perfData.hpp"
#include "utilities/top.hpp"


class ObjectMonitor;

// ObjectSynchronizer 在实现管程时扮演着重要的角色，它提供了一种基于 Java 对象锁的同步方法，用于实现 Java 中的管程（Monitor）机制。
// 通过使用 ObjectSynchronizer，可以实现实例方法的同步、静态方法的同步，以及对对象的监视器方法（如 wait、notify、notifyAll）的处理。

// ObjectSynchronizer 可以做到以下几点：
// 负责对象锁的获取和释放：在同步方法或同步块中，对象锁的获取和释放是由 ObjectSynchronizer 来负责的。
// 管理等待队列：当有线程试图获得某个对象的锁，但当前对象的锁已经被另外一个线程占用时，ObjectSynchronizer 会将该线程挂起，并将其放入等待队列中。
// 实现监视器方法：在监视器方法中，ObjectSynchronizer 负责实现 wait、notify 和 notifyAll 方法，以及相应的信号量机制，用于实现线程之间的等待和唤醒操作。
class ObjectSynchronizer : AllStatic {
  friend class VMStructs;   // 声明 VMStructs 类为本类的友元类，VMStructs 可以访问本类的私有成员
 public:
  typedef enum {
    owner_self,
    owner_none,
    owner_other
  } LockOwnership;  // 锁的所有权，包括：owner_self（自身拥有）、owner_none（无所有者）、owner_other（其他线程拥有）
  // exit must be implemented non-blocking, since the compiler cannot easily handle
  // deoptimization at monitor exit. Hence, it does not take a Handle argument.

  // This is full version of monitor enter and exit. I choose not
  // to use enter() and exit() in order to make sure user be ware
  // of the performance and semantics difference. They are normally
  // used by ObjectLocker etc. The interpreter and compiler use
  // assembly copies of these routines. Please keep them synchornized.
  //
  // attempt_rebias flag is used by UseBiasedLocking implementation
  static void fast_enter  (Handle obj, BasicLock* lock, bool attempt_rebias, TRAPS);
  static void fast_exit   (oop obj,    BasicLock* lock, Thread* THREAD);

  // WARNING: They are ONLY used to handle the slow cases. They should
  // only be used when the fast cases failed. Use of these functions
  // without previous fast case check may cause fatal error.
  static void slow_enter  (Handle obj, BasicLock* lock, TRAPS);
  static void slow_exit   (oop obj,    BasicLock* lock, Thread* THREAD);

  // Used only to handle jni locks or other unmatched monitor enter/exit
  // Internally they will use heavy weight monitor.
  static void jni_enter   (Handle obj, TRAPS);
  static bool jni_try_enter(Handle obj, Thread* THREAD); // Implements Unsafe.tryMonitorEnter
  static void jni_exit    (oop obj,    Thread* THREAD);

  // Handle all interpreter, compiler and jni cases
  static void wait               (Handle obj, jlong millis, TRAPS);
  static void notify             (Handle obj,               TRAPS);
  static void notifyall          (Handle obj,               TRAPS);

  // Special internal-use-only method for use by JVM infrastructure
  // that needs to wait() on a java-level object but that can't risk
  // throwing unexpected InterruptedExecutionExceptions.
  static void waitUninterruptibly (Handle obj, jlong Millis, Thread * THREAD) ;

  // used by classloading to free classloader object lock,
  // wait on an internal lock, and reclaim original lock
  // with original recursion count
  static intptr_t complete_exit  (Handle obj,                TRAPS);
  static void reenter            (Handle obj, intptr_t recursion, TRAPS);

  // thread-specific and global objectMonitor free list accessors
//  static void verifyInUse (Thread * Self) ; too slow for general assert/debug
  static ObjectMonitor * omAlloc (Thread * Self) ;
  static void omRelease (Thread * Self, ObjectMonitor * m, bool FromPerThreadAlloc) ;
  static void omFlush   (Thread * Self) ;

  // Inflate light weight monitor to heavy weight monitor
  // 函数实现了Java中对象的监视器的膨胀，即将一个对象的监视器从栈上锁定状态转换为膨胀状态。
  // 膨胀状态是一种共享状态，多个线程可以在同一时间持有该监视器。只有在重量级的锁才申请ObjectMonitor。
  // 函数的具体实现过程如下：
  // 1.首先检查对象的标记（mark），确定对象是否已经被膨胀。如果已经膨胀，直接返回该膨胀状态的监视器。
  // 2.如果对象被栈上锁定，则申请一个对象监视器（ObjectMonitor），将该对象的标记设置为INFLATING，并将申请的对象监视器与该对象关联。
  //   这里需要用到CAS操作（比较交换），确保只有一个线程能够将监视器设置到该对象的标记中。
  // 3.如果对象的标记是NEUTRAL（未加锁），则申请一个对象监视器，将该对象的标记设置为已膨胀，并将申请的对象监视器与该对象关联。
  // 4.如果对象的标记是INFLATING（正在膨胀），则继续等待，直到该对象的标记不再是INFLATING。
  // 5.如果对象的标记是BIASED（偏向锁），则抛出错误，因为对象存在偏向锁不应该被膨胀。
  static ObjectMonitor* inflate(Thread * Self, oop obj);
  // This version is only for internal use
  static ObjectMonitor* inflate_helper(oop obj);

  // Returns the identity hash value for an oop
  // NOTE: It may cause monitor inflation
  static intptr_t identity_hash_value_for(Handle obj);

  // FastHashCode函数实现了获取Java对象的哈希码的功能。
  // 1.检查是否启用偏向锁，并在需要的情况下撤销偏向锁；
  // 2.通过读取对象的稳定标记（stable mark）获取对象的标记（mark）；
  // 3.如果对象的标记是中立（neutral）状态，则生成一个新的哈希码并设置到标记中，如果对象已经有哈希码，则直接返回原哈希码；
  // 4.如果对象的标记是轻量级锁（lightweight monitor）的状态，并且该锁被当前线程所拥有，则检查该锁的撤销标记（displaced mark）是否包含哈希码，
  //   如果有，则返回该哈希码，否则将标记转换为重量级锁（heavyweight monitor）；
  // 5.如果对象的标记是重量级锁（heavyweight monitor）的状态，则检查该锁的头部是否包含哈希码，如果有，则返回该哈希码，否则生成一个新的哈希码并设置到头部中。
  static intptr_t FastHashCode (Thread * Self, oop obj) ;

  // java.lang.Thread support
  static bool current_thread_holds_lock(JavaThread* thread, Handle h_obj);
  static LockOwnership query_lock_ownership(JavaThread * self, Handle h_obj);

  static JavaThread* get_lock_owner(Handle h_obj, bool doLock);

  // JNI detach support
  static void release_monitors_owned_by_thread(TRAPS);
  static void monitors_iterate(MonitorClosure* m);

  // GC: we current use aggressive monitor deflation policy
  // Basically we deflate all monitors that are not busy.
  // An adaptive profile-based deflation policy could be used if needed
  static void deflate_idle_monitors();
  static int walk_monitor_list(ObjectMonitor** listheadp,
                               ObjectMonitor** FreeHeadp,
                               ObjectMonitor** FreeTailp);
  static bool deflate_monitor(ObjectMonitor* mid, oop obj, ObjectMonitor** FreeHeadp,
                              ObjectMonitor** FreeTailp);
  static void oops_do(OopClosure* f);

  // debugging
  static void verify() PRODUCT_RETURN;
  static int  verify_objmon_isinpool(ObjectMonitor *addr) PRODUCT_RETURN0;

  static void RegisterSpinCallback (int (*)(intptr_t, int), intptr_t) ;

 private:
  enum { _BLOCKSIZE = 128 };
  static ObjectMonitor* gBlockList;
  static ObjectMonitor * volatile gFreeList;
  static ObjectMonitor * volatile gOmInUseList; // for moribund thread, so monitors they inflated still get scanned
  static int gOmInUseCount;

};

// ObjectLocker enforced balanced locking and can never thrown an
// IllegalMonitorStateException. However, a pending exception may
// have to pass through, and we must also be able to deal with
// asynchronous exceptions. The caller is responsible for checking
// the threads pending exception if needed.
// doLock was added to support classloading with UnsyncloadClass which
// requires flag based choice of locking the classloader lock.
class ObjectLocker : public StackObj {
 private:
  Thread*   _thread;
  Handle    _obj;
  BasicLock _lock;
  bool      _dolock;   // default true
 public:
  ObjectLocker(Handle obj, Thread* thread, bool doLock = true);
  ~ObjectLocker();

  // Monitor behavior
  void wait      (TRAPS)      { ObjectSynchronizer::wait     (_obj, 0, CHECK); } // wait forever
  void notify_all(TRAPS)      { ObjectSynchronizer::notifyall(_obj,    CHECK); }
  void waitUninterruptibly (TRAPS) { ObjectSynchronizer::waitUninterruptibly (_obj, 0, CHECK);}
  // complete_exit gives up lock completely, returning recursion count
  // reenter reclaims lock with original recursion count
  intptr_t complete_exit(TRAPS) { return  ObjectSynchronizer::complete_exit(_obj, CHECK_0); }
  void reenter(intptr_t recursion, TRAPS) { ObjectSynchronizer::reenter(_obj, recursion, CHECK); }
};

#endif // SHARE_VM_RUNTIME_SYNCHRONIZER_HPP
