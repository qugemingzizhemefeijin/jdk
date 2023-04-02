/*
 * Copyright (c) 1998, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_RUNTIME_OBJECTMONITOR_HPP
#define SHARE_VM_RUNTIME_OBJECTMONITOR_HPP

#include "runtime/os.hpp"
#include "runtime/park.hpp"
#include "runtime/perfData.hpp"

// ObjectWaiter serves as a "proxy" or surrogate thread.
// TODO-FIXME: Eliminate ObjectWaiter and use the thread-specific
// ParkEvent instead.  Beware, however, that the JVMTI code
// knows about ObjectWaiters, so we'll have to reconcile that code.
// See next_waiter(), first_waiter(), etc.

// ObjectWaiter是一个用于等待唤醒的数据结构。在Java中，Object.wait() 方法调用后，线程会被挂起，
// 直到另一个线程调用Object.notify() 或 Object.notifyAll() 方法，或者线程等待时间到期，或者线程被中断，才会被唤醒。
// 当一个线程调用Object.wait() 方法后，会创建一个ObjectWaiter对象，该对象会被加入到等待队列中。
// 当另一个线程调用Object.notify() 或 Object.notifyAll() 方法时，会从等待队列中取出一个或多个ObjectWaiter对象，
// 并将它们加入到可用队列中，以便在下一次竞争锁时唤醒这些线程。
class ObjectWaiter : public StackObj {
 public:
  enum TStates { TS_UNDEF, TS_READY, TS_RUN, TS_WAIT, TS_ENTER, TS_CXQ } ;
  enum Sorted  { PREPEND, APPEND, SORTED } ;
  ObjectWaiter * volatile _next;    // 指向下一个 ObjectWaiter，通过双向链表将多个 ObjectWaiter 连接起来
  ObjectWaiter * volatile _prev;    // 指向上一个 ObjectWaiter，通过双向链表将多个 ObjectWaiter 连接起来
  Thread*       _thread;            // 指向线程对象，该对象在等待队列中被加入或移除
  jlong         _notifier_tid;      // 记录调用 notify 或 notifyAll 方法的线程 ID
  ParkEvent *   _event;             // 指向一个 ParkEvent 对象，用于等待线程唤醒的信号
  volatile int  _notified ;         // 记录线程是否被唤醒，唤醒后该属性被置为 1
  volatile TStates TState ;         // 指示 ObjectWaiter 的状态，分为6种状态，分别是TS_UNDEF、TS_READY、TS_RUN、TS_WAIT、TS_ENTER、TS_CXQ
  Sorted        _Sorted ;           // List placement disposition // 表示等待队列中的 ObjectWaiter 如何排序，分为 PREPEND、APPEND、SORTED
  bool          _active ;           // Contention monitoring is enabled // 记录是否开启争用监控，开启后可以通过 jcmd 命令查看线程的竞争情况
 public:
  ObjectWaiter(Thread* thread);

  // 函数的作用是将一个线程标记为阻塞在ObjectMonitor上的状态，并设置对应的_blocked_on_monitor_enter_state属性。
  // wait_reenter_begin的作用是标记一个线程进入了重新进入（re-entering）状态，即该线程之前曾经拥有过ObjectMonitor的锁，
  // 现在重新申请锁并被阻塞在了ObjectMonitor上。这个标记的作用是让ObjectMonitor在后续的唤醒操作中优先选择该线程。
  void wait_reenter_begin(ObjectMonitor *mon);

  // 完成线程状态的还原以及唤醒等待线程的工作。将_active状态重置为JavaThreadBlockedOnMonitorEnterState::MonitorIdle。
  void wait_reenter_end(ObjectMonitor *mon);
};

// forward declaration to avoid include tracing.hpp
class EventJavaMonitorWait;

// WARNING:
//   This is a very sensitive and fragile class. DO NOT make any
// change unless you are fully aware of the underlying semantics.

//   This class can not inherit from any other class, because I have
// to let the displaced header be the very first word. Otherwise I
// have to let markOop include this file, which would export the
// monitor data structure to everywhere.
//
// The ObjectMonitor class is used to implement JavaMonitors which have
// transformed from the lightweight structure of the thread stack to a
// heavy weight lock due to contention

// It is also used as RawMonitor by the JVMTI

// objectMoitor在每个Java对象中都具有一个一个ObjectMonitor对象。在Java中，每个对象都可以用作锁来同步多个线程的访问。
// 当线程获取某个对象的锁时，它实际上是获取该对象关联的ObjectMonitor对象的锁。
// 因此，每个对象在Java中都有一个与之关联的ObjectMonitor对象来控制线程对该对象的访问。
class ObjectMonitor {
 public:
  enum {
    OM_OK,                    // no error
    OM_SYSTEM_ERROR,          // operating system error
    OM_ILLEGAL_MONITOR_STATE, // IllegalMonitorStateException
    OM_INTERRUPTED,           // Thread.interrupt()
    OM_TIMED_OUT              // Object.wait() timed out
  };

 public:
  // TODO-FIXME: the "offset" routines should return a type of off_t instead of int ...
  // ByteSize would also be an appropriate type.
  static int header_offset_in_bytes()      { return offset_of(ObjectMonitor, _header);     }
  static int object_offset_in_bytes()      { return offset_of(ObjectMonitor, _object);     }
  static int owner_offset_in_bytes()       { return offset_of(ObjectMonitor, _owner);      }
  static int count_offset_in_bytes()       { return offset_of(ObjectMonitor, _count);      }
  static int recursions_offset_in_bytes()  { return offset_of(ObjectMonitor, _recursions); }
  static int cxq_offset_in_bytes()         { return offset_of(ObjectMonitor, _cxq) ;       }
  static int succ_offset_in_bytes()        { return offset_of(ObjectMonitor, _succ) ;      }
  static int EntryList_offset_in_bytes()   { return offset_of(ObjectMonitor, _EntryList);  }
  static int FreeNext_offset_in_bytes()    { return offset_of(ObjectMonitor, FreeNext);    }
  static int WaitSet_offset_in_bytes()     { return offset_of(ObjectMonitor, _WaitSet) ;   }
  static int Responsible_offset_in_bytes() { return offset_of(ObjectMonitor, _Responsible);}
  static int Spinner_offset_in_bytes()     { return offset_of(ObjectMonitor, _Spinner);    }

 public:
  // Eventaully we'll make provisions for multiple callbacks, but
  // now one will suffice.
  static int (*SpinCallbackFunction)(intptr_t, int) ;
  static intptr_t SpinCallbackArgument ;


 public:
  markOop   header() const;
  void      set_header(markOop hdr);

  intptr_t is_busy() const {
    // TODO-FIXME: merge _count and _waiters.
    // TODO-FIXME: assert _owner == null implies _recursions = 0
    // TODO-FIXME: assert _WaitSet != null implies _count > 0
    return _count|_waiters|intptr_t(_owner)|intptr_t(_cxq)|intptr_t(_EntryList ) ;
  }

  intptr_t  is_entered(Thread* current) const;

  void*     owner() const;
  void      set_owner(void* owner);

  intptr_t  waiters() const;

  intptr_t  count() const;
  void      set_count(intptr_t count);
  intptr_t  contentions() const ;
  intptr_t  recursions() const                                         { return _recursions; }

  // JVM/DI GetMonitorInfo() needs this
  ObjectWaiter* first_waiter()                                         { return _WaitSet; }
  ObjectWaiter* next_waiter(ObjectWaiter* o)                           { return o->_next; }
  Thread* thread_of_waiter(ObjectWaiter* o)                            { return o->_thread; }

  // initialize the monitor, exception the semaphore, all other fields
  // are simple integers or pointers
  ObjectMonitor() {
    _header       = NULL;   // 对象头
    _count        = 0;
    _waiters      = 0,
    _recursions   = 0;      // 锁的重入次数
    _object       = NULL;   // 对应synchronized (object)对应里面的object
    _owner        = NULL;   // 标识拥有该monitor的线程（当前获取锁的线程）
    _WaitSet      = NULL;   // 等待线程（调用wait）组成的双向循环链表，_WaitSet是第一个节点
    _WaitSetLock  = 0 ;
    _Responsible  = NULL ;
    _succ         = NULL ;
    _cxq          = NULL ;  // 多线程竞争锁会先存到这个单向链表中（FILO栈结构）
    FreeNext      = NULL ;
    _EntryList    = NULL ;  // 存放在进入或重新进入时被阻塞(blocked)的线程 (也是存竞争锁失败的线程)
    _SpinFreq     = 0 ;
    _SpinClock    = 0 ;
    OwnerIsThread = 0 ;
    _previous_owner_tid = 0;
  }

  ~ObjectMonitor() {
   // TODO: Add asserts ...
   // _cxq == 0 _succ == NULL _owner == NULL _waiters == 0
   // _count == 0 _EntryList  == NULL etc
  }

private:
  void Recycle () {
    // TODO: add stronger asserts ...
    // _cxq == 0 _succ == NULL _owner == NULL _waiters == 0
    // _count == 0 EntryList  == NULL
    // _recursions == 0 _WaitSet == NULL
    // TODO: assert (is_busy()|_recursions) == 0
    _succ          = NULL ;
    _EntryList     = NULL ;
    _cxq           = NULL ;
    _WaitSet       = NULL ;
    _recursions    = 0 ;
    _SpinFreq      = 0 ;
    _SpinClock     = 0 ;
    OwnerIsThread  = 0 ;
  }

public:

  void*     object() const;
  void*     object_addr();
  void      set_object(void* obj);

  bool      check(TRAPS);       // true if the thread owns the monitor.
  void      check_slow(TRAPS);
  void      clear();
#ifndef PRODUCT
  void      verify();
  void      print();
#endif

  bool      try_enter (TRAPS) ;

  // 在这个函数中主要实现的获取锁的操作。
  // 首先如果没有线程使用这个锁则，直接获取锁，若有线程是会尝试通过原子操作来将当前线程设置成此对象的监视器锁的持有者。
  // 如果原来的持有者是 null，则当前线程成功获取到了锁。如果原来的持有者是当前线程，则说明当前线程已经持有该锁，并且将计数器递增；
  // 如果原来的持有者是其他线程，则说明存在多线程竞争，代码会将当前线程阻塞，并且进入一个等待队列中等待被唤醒。
  // 如果开启了自旋锁，则会尝试自旋一段时间，以避免多线程竞争导致的阻塞开销过大。
  // 如果自旋后仍未获得锁，则当前线程将进入一个等待队列中，并且设置自己为队列的尾部。等待队列中的线程按照LIFO（避免头部饥饿）的顺序进行排队。
  // 当持有者释放锁时，队列头的线程将被唤醒并尝试重新获取锁。
  void      enter(TRAPS);

  // 用于释放当前线程占用的 monitor 并唤醒等待该 monitor 的其他线程。
  // exit是Java虚拟机中用于处理对象锁的代码。它的作用是释放当前线程持有的对象锁，并允许等待该锁的其他线程竞争该锁。具体而言，它执行以下操作：
  // 1.检查当前线程是否持有该锁。如果没有持有该锁，会对其进行修复（假设线程实际上持有该锁，但是由于某些原因，owner字段没有正确更新）
  //   或抛出异常（如果线程没有正确地获取该锁，即不在_owner字段中）。
  // 2.如果当前线程是多次重入该锁，将计数器减1，并直接返回。这是因为线程实际上仍然持有该锁。
  // 3.检查是否有其他线程等待该锁。如果没有等待线程，直接将_owner字段设置为null并返回。如果有等待线程，则释放该锁，并使等待线程之一成为新的owner。
  // 4.如果等待线程中有线程使用了公平自旋（Ticket Spinlock算法），则使用该算法来释放该锁。
  //   否则，使用等待队列或Cache Exclusive Queue（CXQ）算法来释放该锁。这些算法可以更有效地处理多个线程对同一对象锁的竞争，从而提高性能。
  void      exit(bool not_suspended, TRAPS);
  void      wait(jlong millis, bool interruptable, TRAPS);
  void      notify(TRAPS);
  void      notifyAll(TRAPS);

// Use the following at your own risk
  intptr_t  complete_exit(TRAPS);
  void      reenter(intptr_t recursions, TRAPS);

 private:
  void      AddWaiter (ObjectWaiter * waiter) ;
  static    void DeferredInitialize();

  ObjectWaiter * DequeueWaiter () ;
  void      DequeueSpecificWaiter (ObjectWaiter * waiter) ;
  void      EnterI (TRAPS) ;
  void      ReenterI (Thread * Self, ObjectWaiter * SelfNode) ;
  void      UnlinkAfterAcquire (Thread * Self, ObjectWaiter * SelfNode) ;
  int       TryLock (Thread * Self) ;
  int       NotRunnable (Thread * Self, Thread * Owner) ;
  int       TrySpin_Fixed (Thread * Self) ;
  int       TrySpin_VaryFrequency (Thread * Self) ;
  int       TrySpin_VaryDuration  (Thread * Self) ;
  void      ctAsserts () ;
  // ExitEpilog唤醒的是等待时间最长的线程，也就是队列中第一个入队的线程。
  // 具体来说，由于是先入队的线程会排在后面，所以等待时间最长的线程会位于队列的头部。在这里，直接将头部线程从等待队列中移除，然后唤醒它即可。
  void      ExitEpilog (Thread * Self, ObjectWaiter * Wakee) ;
  bool      ExitSuspendEquivalent (JavaThread * Self) ;
  void      post_monitor_wait_event(EventJavaMonitorWait * event,
                                                   jlong notifier_tid,
                                                   jlong timeout,
                                                   bool timedout);

 private:
  friend class ObjectSynchronizer;
  friend class ObjectWaiter;
  friend class VMStructs;

  // WARNING: this must be the very first word of ObjectMonitor
  // This means this class can't use any virtual member functions.
  // TODO-FIXME: assert that offsetof(_header) is 0 or get rid of the
  // implicit 0 offset in emitted code.

  // 表示对象状态的标记。用于与 Java 层面的对象状态 (如 LOCKED 或 UNLOCKED) 对应
  volatile markOop   _header;       // displaced object header word - mark
  // 指向被监视的对象，即 Java 层面的对象
  void*     volatile _object;       // backward object pointer - strong root

  double SharingPad [1] ;           // temp to reduce false sharing

  // All the following fields must be machine word aligned
  // The VM assumes write ordering wrt these fields, which can be
  // read from other threads.

 protected:                         // protected for jvmtiRawMonitor
  // 指向线程或 Lock，表示当前拥有监视器的对象
  void *  volatile _owner;          // pointer to owning thread OR BasicLock
  // 前一个持有监视器的线程的线程 ID
  volatile jlong _previous_owner_tid; // thread id of the previous owner of the monitor
  // 表示进入 monitor 的次数以避免重复进入 monitor
  volatile intptr_t  _recursions;   // recursion count, 0 for first entry
 private:
  int OwnerIsThread ;               // _owner is (Thread *) vs SP/BasicLock
  // 一个 ObjectWaiter 对象的链表，用于存储被阻塞在 monitor 进入的线程。
  // 当一个线程在等待锁时，它会被添加到_cxq中以等待锁资源。当锁被释放时，_cxq中的线程会被从中移除并与其他等待线程竞争锁的拥有权。
  ObjectWaiter * volatile _cxq ;    // LL of recently-arrived threads blocked on entry.
                                    // The list is actually composed of WaitNodes, acting
                                    // as proxies for Threads.
 protected:
  // 一个 ObjectWaiter 对象的列表，用于存储被阻塞的线程等待 monitor 进入。
  // 是一个锁等待队列，用于存储等待锁的线程信息。当一个线程需要获取锁但锁已被其他线程占用时，该线程会被放入_EntryList中并进入等待状态。
  // 当锁被释放时，ObjectMonitor会从_EntryList队列中寻找下一个线程来获取锁。
  ObjectWaiter * volatile _EntryList ;     // Threads blocked on entry or reentry.
 private:
  // 目前所处于锁定状态的锁创建者或是目前正在尝试锁定锁的线程
  Thread * volatile _succ ;          // Heir presumptive thread - used for futile wakeup throttling
  // 目前真正的锁创建者，由于一些原因，前面的 _succ 没有成功锁定，但是又需要处理关于之前线程的唤醒，所以通过 _Responsible 来完成该工作
  Thread * volatile _Responsible ;
  int _PromptDrain ;                // rqst to drain cxq into EntryList ASAP
  // 表示是否允许某个等待线程接管 monitor，可能会降低锁申请的时间，但是也可能存在锁申请时间增加的情况
  volatile int _Spinner ;           // for exit->spinner handoff optimization
  volatile int _SpinFreq ;          // Spin 1-out-of-N attempts: success rate
  volatile int _SpinClock ;
  // 表示可接管等待线程的最长时间，过了这个时间就必须释放 monitor
  volatile int _SpinDuration ;
  volatile intptr_t _SpinState ;    // MCS/CLH list of spinners

  // TODO-FIXME: _count, _waiters and _recursions should be of
  // type int, or int32_t but not intptr_t.  There's no reason
  // to use 64-bit fields for these variables on a 64-bit JVM.

  // 用于防止在全局停滞期间释放或缩小此监视器
  volatile intptr_t  _count;        // reference count to prevent reclaimation/deflation
                                    // at stop-the-world time.  See deflate_idle_monitors().
                                    // _count is approximately |_WaitSet| + |_EntryList|
 protected:
  // 存储 _WaitSet 链表中对象的数量的计数器
  volatile intptr_t  _waiters;      // number of waiting threads
 private:
 protected:
  // 一个 ObjectWaiter 对象的链表，用于存储被阻塞的线程由于 wait() 或 join() 等待 monitor 的状态。
  // 当一个线程调用了Object.wait()方法并进入等待状态时，它会被添加到WaitSet中。
  // 在对象监视器被notify()或notifyAll()方法唤醒时，WaitSet中的线程会被从中移除并与其他等待线程竞争锁的拥有权。
  ObjectWaiter * volatile _WaitSet; // LL of threads wait()ing on the monitor
 private:
  // 保护 _WaitSet 链表的自旋锁
  volatile int _WaitSetLock;        // protects Wait Queue - simple spinlock

 public:
  int _QMix ;                       // Mixed prepend queue discipline
  ObjectMonitor * FreeNext ;        // Free list linkage
  intptr_t StatA, StatsB ;

 public:
  static void Initialize () ;
  static PerfCounter * _sync_ContendedLockAttempts ;
  static PerfCounter * _sync_FutileWakeups ;
  static PerfCounter * _sync_Parks ;
  static PerfCounter * _sync_EmptyNotifications ;
  static PerfCounter * _sync_Notifications ;
  static PerfCounter * _sync_SlowEnter ;
  static PerfCounter * _sync_SlowExit ;
  static PerfCounter * _sync_SlowNotify ;
  static PerfCounter * _sync_SlowNotifyAll ;
  static PerfCounter * _sync_FailedSpins ;
  static PerfCounter * _sync_SuccessfulSpins ;
  static PerfCounter * _sync_PrivateA ;
  static PerfCounter * _sync_PrivateB ;
  static PerfCounter * _sync_MonInCirculation ;
  static PerfCounter * _sync_MonScavenged ;
  static PerfCounter * _sync_Inflations ;
  static PerfCounter * _sync_Deflations ;
  static PerfLongVariable * _sync_MonExtant ;

 public:
  static int Knob_Verbose;
  static int Knob_SpinLimit;
  void* operator new (size_t size) throw() {
    return AllocateHeap(size, mtInternal);
  }
  void* operator new[] (size_t size) throw() {
    return operator new (size);
  }
  void operator delete(void* p) {
    FreeHeap(p, mtInternal);
  }
  void operator delete[] (void *p) {
    operator delete(p);
  }
};

#undef TEVENT
#define TEVENT(nom) {if (SyncVerbose) FEVENT(nom); }

#define FEVENT(nom) { static volatile int ctr = 0 ; int v = ++ctr ; if ((v & (v-1)) == 0) { ::printf (#nom " : %d \n", v); ::fflush(stdout); }}

#undef  TEVENT
#define TEVENT(nom) {;}


#endif // SHARE_VM_RUNTIME_OBJECTMONITOR_HPP
