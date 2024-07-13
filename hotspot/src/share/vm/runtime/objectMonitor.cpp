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

#include "precompiled.hpp"
#include "classfile/vmSymbols.hpp"
#include "memory/resourceArea.hpp"
#include "oops/markOop.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/osThread.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "services/threadService.hpp"
#include "trace/tracing.hpp"
#include "trace/traceMacros.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/macros.hpp"
#include "utilities/preserveException.hpp"
#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "os_solaris.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "os_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "os_bsd.inline.hpp"
#endif

#if defined(__GNUC__) && !defined(IA64)
  // Need to inhibit inlining for older versions of GCC to avoid build-time failures
  #define ATTR __attribute__((noinline))
#else
  #define ATTR
#endif


#ifdef DTRACE_ENABLED

// Only bother with this argument setup if dtrace is available
// TODO-FIXME: probes should not fire when caller is _blocked.  assert() accordingly.


#define DTRACE_MONITOR_PROBE_COMMON(obj, thread)                           \
  char* bytes = NULL;                                                      \
  int len = 0;                                                             \
  jlong jtid = SharedRuntime::get_java_tid(thread);                        \
  Symbol* klassname = ((oop)obj)->klass()->name();                         \
  if (klassname != NULL) {                                                 \
    bytes = (char*)klassname->bytes();                                     \
    len = klassname->utf8_length();                                        \
  }

#ifndef USDT2

HS_DTRACE_PROBE_DECL4(hotspot, monitor__notify,
  jlong, uintptr_t, char*, int);
HS_DTRACE_PROBE_DECL4(hotspot, monitor__notifyAll,
  jlong, uintptr_t, char*, int);
HS_DTRACE_PROBE_DECL4(hotspot, monitor__contended__enter,
  jlong, uintptr_t, char*, int);
HS_DTRACE_PROBE_DECL4(hotspot, monitor__contended__entered,
  jlong, uintptr_t, char*, int);
HS_DTRACE_PROBE_DECL4(hotspot, monitor__contended__exit,
  jlong, uintptr_t, char*, int);

#define DTRACE_MONITOR_WAIT_PROBE(monitor, obj, thread, millis)       \
  {                                                                        \
    if (DTraceMonitorProbes) {                                            \
      DTRACE_MONITOR_PROBE_COMMON(obj, thread);                       \
      HS_DTRACE_PROBE5(hotspot, monitor__wait, jtid,                       \
                       (monitor), bytes, len, (millis));                   \
    }                                                                      \
  }

#define DTRACE_MONITOR_PROBE(probe, monitor, obj, thread)             \
  {                                                                        \
    if (DTraceMonitorProbes) {                                            \
      DTRACE_MONITOR_PROBE_COMMON(obj, thread);                       \
      HS_DTRACE_PROBE4(hotspot, monitor__##probe, jtid,                    \
                       (uintptr_t)(monitor), bytes, len);                  \
    }                                                                      \
  }

#else /* USDT2 */

#define DTRACE_MONITOR_WAIT_PROBE(monitor, obj, thread, millis)            \
  {                                                                        \
    if (DTraceMonitorProbes) {                                            \
      DTRACE_MONITOR_PROBE_COMMON(obj, thread);                            \
      HOTSPOT_MONITOR_WAIT(jtid,                                           \
                       (monitor), bytes, len, (millis));                   \
    }                                                                      \
  }

#define HOTSPOT_MONITOR_contended__enter HOTSPOT_MONITOR_CONTENDED_ENTER
#define HOTSPOT_MONITOR_contended__entered HOTSPOT_MONITOR_CONTENDED_ENTERED
#define HOTSPOT_MONITOR_contended__exit HOTSPOT_MONITOR_CONTENDED_EXIT
#define HOTSPOT_MONITOR_notify HOTSPOT_MONITOR_NOTIFY
#define HOTSPOT_MONITOR_notifyAll HOTSPOT_MONITOR_NOTIFYALL

#define DTRACE_MONITOR_PROBE(probe, monitor, obj, thread)                  \
  {                                                                        \
    if (DTraceMonitorProbes) {                                            \
      DTRACE_MONITOR_PROBE_COMMON(obj, thread);                            \
      HOTSPOT_MONITOR_##probe(jtid,                                               \
                       (uintptr_t)(monitor), bytes, len);                  \
    }                                                                      \
  }

#endif /* USDT2 */
#else //  ndef DTRACE_ENABLED

#define DTRACE_MONITOR_WAIT_PROBE(obj, thread, millis, mon)    {;}
#define DTRACE_MONITOR_PROBE(probe, obj, thread, mon)          {;}

#endif // ndef DTRACE_ENABLED

// Tunables ...
// The knob* variables are effectively final.  Once set they should
// never be modified hence.  Consider using __read_mostly with GCC.

int ObjectMonitor::Knob_Verbose    = 0 ;
int ObjectMonitor::Knob_SpinLimit  = 5000 ;    // derived by an external tool -
static int Knob_LogSpins           = 0 ;       // enable jvmstat tally for spins
static int Knob_HandOff            = 0 ;
static int Knob_ReportSettings     = 0 ;

static int Knob_SpinBase           = 0 ;       // Floor AKA SpinMin
static int Knob_SpinBackOff        = 0 ;       // spin-loop backoff
static int Knob_CASPenalty         = -1 ;      // Penalty for failed CAS
static int Knob_OXPenalty          = -1 ;      // Penalty for observed _owner change
static int Knob_SpinSetSucc        = 1 ;       // spinners set the _succ field
static int Knob_SpinEarly          = 1 ;
static int Knob_SuccEnabled        = 1 ;       // futile wake throttling
static int Knob_SuccRestrict       = 0 ;       // Limit successors + spinners to at-most-one
static int Knob_MaxSpinners        = -1 ;      // Should be a function of # CPUs
static int Knob_Bonus              = 100 ;     // spin success bonus
static int Knob_BonusB             = 100 ;     // spin success bonus
static int Knob_Penalty            = 200 ;     // spin failure penalty
static int Knob_Poverty            = 1000 ;
static int Knob_SpinAfterFutile    = 1 ;       // Spin after returning from park()
static int Knob_FixedSpin          = 0 ;
static int Knob_OState             = 3 ;       // Spinner checks thread state of _owner
static int Knob_UsePause           = 1 ;
static int Knob_ExitPolicy         = 0 ;
static int Knob_PreSpin            = 10 ;      // 20-100 likely better
static int Knob_ResetEvent         = 0 ;
static int BackOffMask             = 0 ;

static int Knob_FastHSSEC          = 0 ;
static int Knob_MoveNotifyee       = 2 ;       // notify() - disposition of notifyee
static int Knob_QMode              = 0 ;       // EntryList-cxq policy - queue discipline
static volatile int InitDone       = 0 ;

#define TrySpin TrySpin_VaryDuration

// -----------------------------------------------------------------------------
// Theory of operations -- Monitors lists, thread residency, etc:
//
// * A thread acquires ownership of a monitor by successfully
//   CAS()ing the _owner field from null to non-null.
//
// * Invariant: A thread appears on at most one monitor list --
//   cxq, EntryList or WaitSet -- at any one time.
//
// * Contending threads "push" themselves onto the cxq with CAS
//   and then spin/park.
//
// * After a contending thread eventually acquires the lock it must
//   dequeue itself from either the EntryList or the cxq.
//
// * The exiting thread identifies and unparks an "heir presumptive"
//   tentative successor thread on the EntryList.  Critically, the
//   exiting thread doesn't unlink the successor thread from the EntryList.
//   After having been unparked, the wakee will recontend for ownership of
//   the monitor.   The successor (wakee) will either acquire the lock or
//   re-park itself.
//
//   Succession is provided for by a policy of competitive handoff.
//   The exiting thread does _not_ grant or pass ownership to the
//   successor thread.  (This is also referred to as "handoff" succession").
//   Instead the exiting thread releases ownership and possibly wakes
//   a successor, so the successor can (re)compete for ownership of the lock.
//   If the EntryList is empty but the cxq is populated the exiting
//   thread will drain the cxq into the EntryList.  It does so by
//   by detaching the cxq (installing null with CAS) and folding
//   the threads from the cxq into the EntryList.  The EntryList is
//   doubly linked, while the cxq is singly linked because of the
//   CAS-based "push" used to enqueue recently arrived threads (RATs).
//
// * Concurrency invariants:
//
//   -- only the monitor owner may access or mutate the EntryList.
//      The mutex property of the monitor itself protects the EntryList
//      from concurrent interference.
//   -- Only the monitor owner may detach the cxq.
//
// * The monitor entry list operations avoid locks, but strictly speaking
//   they're not lock-free.  Enter is lock-free, exit is not.
//   See http://j2se.east/~dice/PERSIST/040825-LockFreeQueues.html
//
// * The cxq can have multiple concurrent "pushers" but only one concurrent
//   detaching thread.  This mechanism is immune from the ABA corruption.
//   More precisely, the CAS-based "push" onto cxq is ABA-oblivious.
//
// * Taken together, the cxq and the EntryList constitute or form a
//   single logical queue of threads stalled trying to acquire the lock.
//   We use two distinct lists to improve the odds of a constant-time
//   dequeue operation after acquisition (in the ::enter() epilog) and
//   to reduce heat on the list ends.  (c.f. Michael Scott's "2Q" algorithm).
//   A key desideratum is to minimize queue & monitor metadata manipulation
//   that occurs while holding the monitor lock -- that is, we want to
//   minimize monitor lock holds times.  Note that even a small amount of
//   fixed spinning will greatly reduce the # of enqueue-dequeue operations
//   on EntryList|cxq.  That is, spinning relieves contention on the "inner"
//   locks and monitor metadata.
//
//   Cxq points to the the set of Recently Arrived Threads attempting entry.
//   Because we push threads onto _cxq with CAS, the RATs must take the form of
//   a singly-linked LIFO.  We drain _cxq into EntryList  at unlock-time when
//   the unlocking thread notices that EntryList is null but _cxq is != null.
//
//   The EntryList is ordered by the prevailing queue discipline and
//   can be organized in any convenient fashion, such as a doubly-linked list or
//   a circular doubly-linked list.  Critically, we want insert and delete operations
//   to operate in constant-time.  If we need a priority queue then something akin
//   to Solaris' sleepq would work nicely.  Viz.,
//   http://agg.eng/ws/on10_nightly/source/usr/src/uts/common/os/sleepq.c.
//   Queue discipline is enforced at ::exit() time, when the unlocking thread
//   drains the cxq into the EntryList, and orders or reorders the threads on the
//   EntryList accordingly.
//
//   Barring "lock barging", this mechanism provides fair cyclic ordering,
//   somewhat similar to an elevator-scan.
//
// * The monitor synchronization subsystem avoids the use of native
//   synchronization primitives except for the narrow platform-specific
//   park-unpark abstraction.  See the comments in os_solaris.cpp regarding
//   the semantics of park-unpark.  Put another way, this monitor implementation
//   depends only on atomic operations and park-unpark.  The monitor subsystem
//   manages all RUNNING->BLOCKED and BLOCKED->READY transitions while the
//   underlying OS manages the READY<->RUN transitions.
//
// * Waiting threads reside on the WaitSet list -- wait() puts
//   the caller onto the WaitSet.
//
// * notify() or notifyAll() simply transfers threads from the WaitSet to
//   either the EntryList or cxq.  Subsequent exit() operations will
//   unpark the notifyee.  Unparking a notifee in notify() is inefficient -
//   it's likely the notifyee would simply impale itself on the lock held
//   by the notifier.
//
// * An interesting alternative is to encode cxq as (List,LockByte) where
//   the LockByte is 0 iff the monitor is owned.  _owner is simply an auxiliary
//   variable, like _recursions, in the scheme.  The threads or Events that form
//   the list would have to be aligned in 256-byte addresses.  A thread would
//   try to acquire the lock or enqueue itself with CAS, but exiting threads
//   could use a 1-0 protocol and simply STB to set the LockByte to 0.
//   Note that is is *not* word-tearing, but it does presume that full-word
//   CAS operations are coherent with intermix with STB operations.  That's true
//   on most common processors.
//
// * See also http://blogs.sun.com/dave


// -----------------------------------------------------------------------------
// Enter support

// try_enter用于实现Unsafe类的tryMonitorEnter方法，会尝试获取锁，如果获取失败则直接返回false。
bool ObjectMonitor::try_enter(Thread* THREAD) {
  if (THREAD != _owner) {
    if (THREAD->is_lock_owned ((address)_owner)) {
       // 如果该线程已经占有了该锁，该锁由轻量级锁膨胀而来
       assert(_recursions == 0, "internal state error");
       // 修改owner等属性
       _owner = THREAD ;
       _recursions = 1 ;
       OwnerIsThread = 1 ;
       return true;
    }
    if (Atomic::cmpxchg_ptr (THREAD, &_owner, NULL) != NULL) {
      // 原子的设置owner属性，修改失败
      return false;
    }
    // 修改成功
    return true;
  } else {
    // 当前线程已经占有该锁，将记录嵌套加锁的计数器加1
    _recursions++;
    return true;
  }
}

// enter方法用于获取某个ObjectMonitor对应的重量级锁，为了尽可能减少对系统互斥量的使用，减少锁抢占的性能损耗，
// ObjectMonitor多次调用TrySpin方法让当前线程自旋抢占锁，进入EnterI方法前会调用一次，进入EnterI后会调用一次，然后park和TrySpin在一个for循环中先后执行，直到成功获取锁为止。
// 重量级锁入口函数（可能多个线程同时进入此方法）
void ATTR ObjectMonitor::enter(TRAPS) {
  // The following code is ordered to check the most common cases first
  // and to reduce RTS->RTO cache line upgrades on SPARC and IA32 processors.
  // 获取当前线程指针
  Thread * const Self = THREAD ;
  void * cur ;
  // 1.通过CAS（原子操作）操作尝试把monitor的_owner字段设置为当前线程(开始竞争)
  cur = Atomic::cmpxchg_ptr (Self, &_owner, NULL) ;
  // 获取锁失败
  if (cur == NULL) {
     // 设置成功，说明该Monitor没有被人占用
     // Either ASSERT _recursions == 0 or explicitly set _recursions = 0.
     assert (_recursions == 0   , "invariant") ;
     assert (_owner      == Self, "invariant") ;
     // CONSIDER: set or assert OwnerIsThread == 1
     return ;
  }
  // 2.若是以前的_owner指向该THREAD，那么该线程是重入，_recursions++
  if (cur == Self) {
     // TODO-FIXME: check for integer overflow!  BUGID 6557169.
     // 设置失败，说明该Monitor就是当前线程占用的，此处进入enter是嵌套加锁情形
     _recursions ++ ;
     return ;
  }

  // 轻量级锁膨胀成重量级锁时，将owner设置为lock属性
  if (Self->is_lock_owned ((address)cur)) { // 若是当前线程是第一次进入该monitor，设置_recursions为1，_owner为当前线程
    assert (_recursions == 0, "internal state error");
    // 正常轻量级膨胀成重量级锁时，之前已经获取轻量级锁的线程不需要二次调用enter方法
    // 此时再调用enter方法说明是锁嵌套情形，将_recursions置为1
    _recursions = 1 ;
    // Commute owner from a thread-specific on-stack BasicLockObject address to
    // a full-fledged "Thread *".
    // 将owner置为当前线程
    _owner = Self ;
    // 表明当前线程是获取轻量级锁的
    OwnerIsThread = 1 ;
    return ;
  }

  // 该Monitor被其他某个线程占用了，需要抢占
  // We've encountered genuine contention.
  assert (Self->_Stalled == 0, "invariant") ;
  // 记录需要抢占的Monitor指针
  Self->_Stalled = intptr_t(this) ; // 标记线程处在阻塞，并将本线程封装成 ObjectWaiter 对象插入到等待队列的尾部

  // Try one round of spinning *before* enqueueing Self
  // and before going through the awkward and expensive state
  // transitions.  The following spin is strictly optional ...
  // Note that if we acquire the monitor from an initial spin
  // we forgo posting JVMTI events and firing DTRACE probes.
  // Knob_SpinEarly默认为1，即为true
  // TrySpin让当前线程自旋，自旋的次数默认可以自适应调整，如果进入安全点同步则退出自旋，返回1表示抢占成功
  if (Knob_SpinEarly && TrySpin (Self) > 0) { // 自旋锁
     assert (_owner == Self      , "invariant") ;
     assert (_recursions == 0    , "invariant") ;
     assert (((oop)(object()))->mark() == markOopDesc::encode(this), "invariant") ;
     Self->_Stalled = 0 ; // 解除阻塞状态
     return ;
  }

  // 自旋若干次数后依然抢占失败
  assert (_owner != Self          , "invariant") ;
  assert (_succ  != Self          , "invariant") ;
  assert (Self->is_Java_thread()  , "invariant") ;
  JavaThread * jt = (JavaThread *) Self ;
  // 校验安全点同步未完成
  assert (!SafepointSynchronize::is_at_safepoint(), "invariant") ;
  assert (jt->thread_state() != _thread_blocked   , "invariant") ;
  assert (this->object() != NULL  , "invariant") ;
  assert (_count >= 0, "invariant") ;

  // Prevent deflation at STW-time.  See deflate_idle_monitors() and is_busy().
  // Ensure the object-monitor relationship remains stable while there's contention.
  // 原子的将_count属性加1，表示增加了一个抢占该锁的线程
  Atomic::inc_ptr(&_count);

  EventJavaMonitorEnter event;

  { // Change java thread status to indicate blocked on monitor enter.
    // 修改Java线程状态为BLOCKED_ON_MONITOR_ENTER，此代码块退出后还原成原来的
    JavaThreadBlockedOnMonitorEnterState jtbmes(jt, this);

    DTRACE_MONITOR_PROBE(contended__enter, this, object(), jt);
    if (JvmtiExport::should_post_monitor_contended_enter()) {
      JvmtiExport::post_monitor_contended_enter(jt, this);
    }

    // 修改OS线程状态为MONITOR_WAIT，此代码块退出后还原成原来的
    OSThreadContendState osts(Self->osthread());
    // 让当前线程的调用栈帧可以walkable，即可以被遍历，需要记录上一次执行的Java字节码
    // 然后切换线程的运行状态，从_thread_in_vm切换成_thread_blocked，切换的过程如果进入安全点同步则会被阻塞，
    // 此代码块退出将状态从_thread_blocked切换成_thread_in_vm，同样切换过程中如果进入安全点同步则被阻塞
    ThreadBlockInVM tbivm(jt);

    Self->set_current_pending_monitor(this);

    // TODO-FIXME: change the following for(;;) loop to straight-line code.
    for (;;) {
      // 将线程的_suspend_equivalent属性置为true，该属性表明当前线程处于悬浮状态
      jt->set_suspend_equivalent();
      // cleared by handle_special_suspend_equivalent_condition()
      // or java_suspend_self()
      // 3.获取锁失败的线程，则等待！（会通过自旋，park等方式不断循环尝试获取锁，直到成功获取锁为止）
      EnterI (THREAD) ;

      if (!ExitSuspendEquivalent(jt)) break ; // 如果线程长时间处于suspend_equivalent状态，那么就会调用ExitSuspendEquivalent函数退出该状态

      //
      // We have acquired the contended monitor, but while we were
      // waiting another thread suspended us. We don't want to enter
      // the monitor while suspended because that would surprise the
      // thread that suspended us.
      //
      // 等待suspended当前线程的线程
          _recursions = 0 ;
      _succ = NULL ;
      exit (false, Self) ;

      jt->java_suspend_self();
    }
    // 将关联的ObjectMonitor置为null，表示当前线程已经不在阻塞状态了
    Self->set_current_pending_monitor(NULL);
  }

  // 原子的将count属性减1，表示已经有一个线程成功获取锁
  Atomic::dec_ptr(&_count);
  assert (_count >= 0, "invariant") ;
  Self->_Stalled = 0 ;

  // Must either set _recursions = 0 or ASSERT _recursions == 0.
  assert (_recursions == 0     , "invariant") ;
  assert (_owner == Self       , "invariant") ;
  assert (_succ  != Self       , "invariant") ;
  assert (((oop)(object()))->mark() == markOopDesc::encode(this), "invariant") ;

  // The thread -- now the owner -- is back in vm mode.
  // Report the glorious news via TI,DTrace and jvmstat.
  // The probe effect is non-trivial.  All the reportage occurs
  // while we hold the monitor, increasing the length of the critical
  // section.  Amdahl's parallel speedup law comes vividly into play.
  //
  // Another option might be to aggregate the events (thread local or
  // per-monitor aggregation) and defer reporting until a more opportune
  // time -- such as next time some thread encounters contention but has
  // yet to acquire the lock.  While spinning that thread could
  // spinning we could increment JVMStat counters, etc.

  DTRACE_MONITOR_PROBE(contended__entered, this, object(), jt);
  if (JvmtiExport::should_post_monitor_contended_entered()) {
    JvmtiExport::post_monitor_contended_entered(jt, this);
  }

  if (event.should_commit()) {
    event.set_klass(((oop)this->object())->klass());
    event.set_previousOwner((TYPE_JAVALANGTHREAD)_previous_owner_tid);
    event.set_address((TYPE_ADDRESS)(uintptr_t)(this->object_addr()));
    event.commit();
  }

  if (ObjectMonitor::_sync_ContendedLockAttempts != NULL) {
     // 增加计数
     ObjectMonitor::_sync_ContendedLockAttempts->inc() ;
  }
}


// Caveat: TryLock() is not necessarily serializing if it returns failure.
// Callers must compensate as needed.

// 线程尝试获取锁（or 线程被唤醒后获取）
int ObjectMonitor::TryLock (Thread * Self) {
   for (;;) {
      void * own = _owner ;
      if (own != NULL) return 0 ; // 若是有线程还拥有着重量级锁，退出
      // CAS操做将_owner修改成当前线程，操做成功return>0
      if (Atomic::cmpxchg_ptr (Self, &_owner, NULL) == NULL) {
         // Either guarantee _recursions == 0 or set _recursions = 0.
         assert (_recursions == 0, "invariant") ;
         assert (_owner == Self, "invariant") ;
         // CONSIDER: set or assert that OwnerIsThread == 1
         // 尝试拿到锁返回1
         return 1 ;
      }
      // The lock had been free momentarily, but we lost the race to the lock.
      // Interference -- the CAS failed.
      // We can either return -1 or retry.
      // Retry doesn't make as much sense because the lock was just acquired.
      // CAS更新失败return<0
      if (true) return -1 ;
   }
}

// EnterI方法会初始化线程自旋相关配置，然后自旋尝试获取锁，获取失败后将当前线程加入到ObjectWaiter队列中，
// 然后借助底层操作系统的互斥量让当前线程处于休眠状态，如果持有锁的线程释放了锁就会唤醒该线程，被唤醒后该线程会尝试获取锁，
// 获取失败再自旋，依然获取失败再次进入休眠状态，如此循环直到获取锁为止，获取成功后将当前线程对应的ObjectWaiter从队列中移除。
void ATTR ObjectMonitor::EnterI (TRAPS) {
    Thread * Self = THREAD ;
    assert (Self->is_Java_thread(), "invariant") ;
    // 校验线程状态已经处于阻塞中
    assert (((JavaThread *) Self)->thread_state() == _thread_blocked   , "invariant") ;

    // 没拿到锁，还是要尝试TryLock一次
    // Try the lock - TATAS
    if (TryLock (Self) > 0) {
        // 拿到锁执行，在返回
        assert (_succ != Self              , "invariant") ;
        assert (_owner == Self             , "invariant") ;
        assert (_Responsible != Self       , "invariant") ;
        return ;
    }
    // 初始化自旋相关配置参数
    DeferredInitialize () ;

    // We try one round of spinning *before* enqueueing Self.
    //
    // If the _owner is ready but OFFPROC we could use a YieldTo()
    // operation to donate the remainder of this thread's quantum
    // to the owner.  This has subtle but beneficial affinity
    // effects.
    // 没拿到锁，开始TrySpin自旋（CAS,while循环）
    if (TrySpin (Self) > 0) {
        // 再次尝试自旋，获取锁成功则返回
        assert (_owner == Self        , "invariant") ;
        assert (_succ != Self         , "invariant") ;
        assert (_Responsible != Self  , "invariant") ;
        return ;
    }

    // The Spin failed -- Enqueue and park the thread ...
    // 自旋获取锁失败，将当前线程加入到等待队列中并且park
    assert (_succ  != Self            , "invariant") ;
    assert (_owner != Self            , "invariant") ;
    assert (_Responsible != Self      , "invariant") ;

    // Enqueue "Self" on ObjectMonitor's _cxq.
    //
    // Node acts as a proxy for Self.
    // As an aside, if were to ever rewrite the synchronization code mostly
    // in Java, WaitNodes, ObjectMonitors, and Events would become 1st-class
    // Java objects.  This would avoid awkward lifecycle and liveness issues,
    // as well as eliminate a subset of ABA issues.
    // TODO: eliminate ObjectWaiter and enqueue either Threads or Events.
    // 到此，自旋终于全失败了，要入队挂起了
    // 实在拿不到锁；当前线程被封装成ObjectWaiter对象node，状态设置成ObjectWaiter::TS_CXQ。即将放入竞争队列
    ObjectWaiter node(Self) ; // 将Thread封装成ObjectWaiter结点
    Self->_ParkEvent->reset() ;
    node._prev   = (ObjectWaiter *) 0xBAD ;
    node.TState  = ObjectWaiter::TS_CXQ ;

    // Push "Self" onto the front of the _cxq.
    // Once on cxq/EntryList, Self stays on-queue until it acquires the lock.
    // Note that spinning tends to reduce the rate at which threads
    // enqueue and dequeue on EntryList|cxq.
    ObjectWaiter * nxt ;
    for (;;) { // 循环，保证将node插入队列
        node._next = nxt = _cxq ; // 将node插入到_cxq队列的首部
        // 使用内核函数cmpxchg_ptr 将没有拿到锁线程（node）放到竞争队列
        if (Atomic::cmpxchg_ptr (&node, &_cxq, nxt) == nxt) break ;

        // Interference - the CAS failed because _cxq changed.  Just retry.
        // As an optional optimization we retry the lock.
        if (TryLock (Self) > 0) { // 我再默默的TryLock一下，真的是不想挂起呀！
            // 再次尝试获取锁，获取成功则返回
            assert (_succ != Self         , "invariant") ;
            assert (_owner == Self        , "invariant") ;
            assert (_Responsible != Self  , "invariant") ;
            return ;
        }
    }

    // Check for cxq|EntryList edge transition to non-null.  This indicates
    // the onset of contention.  While contention persists exiting threads
    // will use a ST:MEMBAR:LD 1-1 exit protocol.  When contention abates exit
    // operations revert to the faster 1-0 mode.  This enter operation may interleave
    // (race) a concurrent 1-0 exit operation, resulting in stranding, so we
    // arrange for one of the contending thread to use a timed park() operations
    // to detect and recover from the race.  (Stranding is form of progress failure
    // where the monitor is unlocked but all the contending threads remain parked).
    // That is, at least one of the contended threads will periodically poll _owner.
    // One of the contending threads will become the designated "Responsible" thread.
    // The Responsible thread uses a timed park instead of a normal indefinite park
    // operation -- it periodically wakes and checks for and recovers from potential
    // strandings admitted by 1-0 exit operations.   We need at most one Responsible
    // thread per-monitor at any given moment.  Only threads on cxq|EntryList may
    // be responsible for a monitor.
    //
    // Currently, one of the contended threads takes on the added role of "Responsible".
    // A viable alternative would be to use a dedicated "stranding checker" thread
    // that periodically iterated over all the threads (or active monitors) and unparked
    // successors where there was risk of stranding.  This would help eliminate the
    // timer scalability issues we see on some platforms as we'd only have one thread
    // -- the checker -- parked on a timer.
    // SyncFlags的默认值是0
    if ((SyncFlags & 16) == 0 && nxt == NULL && _EntryList == NULL) {
        // Try to assume the role of responsible thread for the monitor.
        // CONSIDER:  ST vs CAS vs { if (Responsible==null) Responsible=Self }
        // nxt或者_EntryList为NULL，说明当前线程是第一个阻塞的线程，将_Responsible原子的修改为当前线程
        Atomic::cmpxchg_ptr (Self, &_Responsible, NULL) ;
    }

    // The lock have been released while this thread was occupied queueing
    // itself onto _cxq.  To close the race and avoid "stranding" and
    // progress-liveness failure we must resample-retry _owner before parking.
    // Note the Dekker/Lamport duality: ST cxq; MEMBAR; LD Owner.
    // In this case the ST-MEMBAR is accomplished with CAS().
    //
    // TODO: Defer all thread state transitions until park-time.
    // Since state transitions are heavy and inefficient we'd like
    // to defer the state transitions until absolutely necessary,
    // and in doing so avoid some transitions ...

    TEVENT (Inflated enter - Contention) ;
    int nWakeups = 0 ;
    int RecheckInterval = 1 ;
    // 将竞争队列线程挂起
    for (;;) {
        // 线程在被挂起前做一下挣扎，看能不能获取到锁
        if (TryLock (Self) > 0) break ;
        assert (_owner != Self, "invariant") ;

        if ((SyncFlags & 2) && _Responsible == NULL) {
           // 原子的将_Responsible置为Self
           Atomic::cmpxchg_ptr (Self, &_Responsible, NULL) ;
        }

        // park self
        // 将目标线程park掉，底层通过操作系统的互斥量实现，让当前线程休眠
        if (_Responsible == Self || (SyncFlags & 1)) {
            TEVENT (Inflated enter - park TIMED) ;
            Self->_ParkEvent->park ((jlong) RecheckInterval) ;
            // Increase the RecheckInterval, but clamp the value.
            // 增加等待时间，最大不超过1s
            RecheckInterval *= 8 ;
            if (RecheckInterval > 1000) RecheckInterval = 1000 ;
        } else {
            TEVENT (Inflated enter - park UNTIMED) ;
            // 挂起!!!!!!：：通过park将当前线程挂起（不被执行了），等待被唤醒！
            Self->_ParkEvent->park() ;
        }
        // 当该线程被唤醒时，执行TryLock----->ObjectMonitor::TryLock
        if (TryLock(Self) > 0) break ;

        // The lock is still contested.
        // Keep a tally of the # of futile wakeups.
        // Note that the counter is not protected by a lock or updated by atomics.
        // That is by design - we trade "lossy" counters which are exposed to
        // races during updates for a lower probe effect.
        TEVENT (Inflated enter - Futile wakeup) ;
        if (ObjectMonitor::_sync_FutileWakeups != NULL) {
           // 增加计数
           ObjectMonitor::_sync_FutileWakeups->inc() ;
        }
        ++ nWakeups ;

        // Assuming this is not a spurious wakeup we'll normally find _succ == Self.
        // We can defer clearing _succ until after the spin completes
        // TrySpin() must tolerate being called with _succ == Self.
        // Try yet another round of adaptive spinning.
        // Knob_SpinAfterFutile默认值是1，此时会再次尝试自旋获取锁
        if ((Knob_SpinAfterFutile & 1) && TrySpin (Self) > 0) break ;

        // We can find that we were unpark()ed and redesignated _succ while
        // we were spinning.  That's harmless.  If we iterate and call park(),
        // park() will consume the event and return immediately and we'll
        // just spin again.  This pattern can repeat, leaving _succ to simply
        // spin on a CPU.  Enable Knob_ResetEvent to clear pending unparks().
        // Alternately, we can sample fired() here, and if set, forgo spinning
        // in the next iteration.
        // Knob_ResetEvent默认值是0
        if ((Knob_ResetEvent & 1) && Self->_ParkEvent->fired()) {
           Self->_ParkEvent->reset() ;
           OrderAccess::fence() ;
        }
        if (_succ == Self) _succ = NULL ;

        // Invariant: after clearing _succ a thread *must* retry _owner before parking.
        // 强制所有修改立即生效
        OrderAccess::fence() ;
    }

    // Egress :
    // Self has acquired the lock -- Unlink Self from the cxq or EntryList.
    // Normally we'll find Self on the EntryList .
    // From the perspective of the lock owner (this thread), the
    // EntryList is stable and cxq is prepend-only.
    // The head of cxq is volatile but the interior is stable.
    // In addition, Self.TState is stable.
    // for循环结束，当前线程已经获取了锁
    assert (_owner == Self      , "invariant") ;
    assert (object() != NULL    , "invariant") ;
    // I'd like to write:
    //   guarantee (((oop)(object()))->mark() == markOopDesc::encode(this), "invariant") ;
    // but as we're at a safepoint that's not safe.
    // 将其从EntryList或者cxq链表中移除
    UnlinkAfterAcquire (Self, &node) ;
    if (_succ == Self) _succ = NULL ;

    assert (_succ != Self, "invariant") ;
    if (_Responsible == Self) {
        // 将_Responsible置为NULL
        _Responsible = NULL ;
        OrderAccess::fence(); // Dekker pivot-point

        // We may leave threads on cxq|EntryList without a designated
        // "Responsible" thread.  This is benign.  When this thread subsequently
        // exits the monitor it can "see" such preexisting "old" threads --
        // threads that arrived on the cxq|EntryList before the fence, above --
        // by LDing cxq|EntryList.  Newly arrived threads -- that is, threads
        // that arrive on cxq after the ST:MEMBAR, above -- will set Responsible
        // non-null and elect a new "Responsible" timer thread.
        //
        // This thread executes:
        //    ST Responsible=null; MEMBAR    (in enter epilog - here)
        //    LD cxq|EntryList               (in subsequent exit)
        //
        // Entering threads in the slow/contended path execute:
        //    ST cxq=nonnull; MEMBAR; LD Responsible (in enter prolog)
        //    The (ST cxq; MEMBAR) is accomplished with CAS().
        //
        // The MEMBAR, above, prevents the LD of cxq|EntryList in the subsequent
        // exit operation from floating above the ST Responsible=null.
    }

    // We've acquired ownership with CAS().
    // CAS is serializing -- it has MEMBAR/FENCE-equivalent semantics.
    // But since the CAS() this thread may have also stored into _succ,
    // EntryList, cxq or Responsible.  These meta-data updates must be
    // visible __before this thread subsequently drops the lock.
    // Consider what could occur if we didn't enforce this constraint --
    // STs to monitor meta-data and user-data could reorder with (become
    // visible after) the ST in exit that drops ownership of the lock.
    // Some other thread could then acquire the lock, but observe inconsistent
    // or old monitor meta-data and heap data.  That violates the JMM.
    // To that end, the 1-0 exit() operation must have at least STST|LDST
    // "release" barrier semantics.  Specifically, there must be at least a
    // STST|LDST barrier in exit() before the ST of null into _owner that drops
    // the lock.   The barrier ensures that changes to monitor meta-data and data
    // protected by the lock will be visible before we release the lock, and
    // therefore before some other thread (CPU) has a chance to acquire the lock.
    // See also: http://gee.cs.oswego.edu/dl/jmm/cookbook.html.
    //
    // Critically, any prior STs to _succ or EntryList must be visible before
    // the ST of null into _owner in the *subsequent* (following) corresponding
    // monitorexit.  Recall too, that in 1-0 mode monitorexit does not necessarily
    // execute a serializing instruction.

    if (SyncFlags & 8) {
       OrderAccess::fence() ;
    }
    return ;
}

// ReenterI() is a specialized inline form of the latter half of the
// contended slow-path from EnterI().  We use ReenterI() only for
// monitor reentry in wait().
//
// In the future we should reconcile EnterI() and ReenterI(), adding
// Knob_Reset and Knob_SpinAfterFutile support and restructuring the
// loop accordingly.

// ReenterI和EnterI的逻辑基本相同，用于获取对象锁
void ATTR ObjectMonitor::ReenterI (Thread * Self, ObjectWaiter * SelfNode) {
    assert (Self != NULL                , "invariant") ;
    assert (SelfNode != NULL            , "invariant") ;
    assert (SelfNode->_thread == Self   , "invariant") ;
    assert (_waiters > 0                , "invariant") ;
    // 校验目标对象的对象头就是当前ObjectMonitor的指针
    assert (((oop)(object()))->mark() == markOopDesc::encode(this) , "invariant") ;
    assert (((JavaThread *)Self)->thread_state() != _thread_blocked, "invariant") ;
    JavaThread * jt = (JavaThread *) Self ;

    int nWakeups = 0 ;
    for (;;) {
        ObjectWaiter::TStates v = SelfNode->TState ;
        // 校验状态
        guarantee (v == ObjectWaiter::TS_ENTER || v == ObjectWaiter::TS_CXQ, "invariant") ;
        assert    (_owner != Self, "invariant") ;

        // 尝试获取锁
        if (TryLock (Self) > 0) break ;
        // 尝试自旋获取锁
        if (TrySpin (Self) > 0) break ;

        TEVENT (Wait Reentry - parking) ;

        // State transition wrappers around park() ...
        // ReenterI() wisely defers state transitions until
        // it's clear we must park the thread.
        {
           // 修改线程状态
           OSThreadContendState osts(Self->osthread());
           ThreadBlockInVM tbivm(jt);

           // cleared by handle_special_suspend_equivalent_condition()
           // or java_suspend_self()
           jt->set_suspend_equivalent();
           // SyncFlags默认是0
           if (SyncFlags & 1) {
              Self->_ParkEvent->park ((jlong)1000) ;
           } else {
              Self->_ParkEvent->park () ;
           }

           // were we externally suspended while we were waiting?
           for (;;) {
              // ExitSuspendEquivalent默认返回false
              if (!ExitSuspendEquivalent (jt)) break ;
              if (_succ == Self) { _succ = NULL; OrderAccess::fence(); }
              jt->java_suspend_self();
              jt->set_suspend_equivalent();
           }
        }

        // Try again, but just so we distinguish between futile wakeups and
        // successful wakeups.  The following test isn't algorithmically
        // necessary, but it helps us maintain sensible statistics.
        // 尝试获取锁
        if (TryLock(Self) > 0) break ;

        // The lock is still contested.
        // Keep a tally of the # of futile wakeups.
        // Note that the counter is not protected by a lock or updated by atomics.
        // That is by design - we trade "lossy" counters which are exposed to
        // races during updates for a lower probe effect.
        TEVENT (Wait Reentry - futile wakeup) ;
        ++ nWakeups ;

        // Assuming this is not a spurious wakeup we'll normally
        // find that _succ == Self.
        if (_succ == Self) _succ = NULL ;

        // Invariant: after clearing _succ a contending thread
        // *must* retry  _owner before parking.
        OrderAccess::fence() ;

        if (ObjectMonitor::_sync_FutileWakeups != NULL) {
          ObjectMonitor::_sync_FutileWakeups->inc() ;
        }
    }

    // Self has acquired the lock -- Unlink Self from the cxq or EntryList .
    // Normally we'll find Self on the EntryList.
    // Unlinking from the EntryList is constant-time and atomic-free.
    // From the perspective of the lock owner (this thread), the
    // EntryList is stable and cxq is prepend-only.
    // The head of cxq is volatile but the interior is stable.
    // In addition, Self.TState is stable.

    // for循环结束，已经获取了锁
    assert (_owner == Self, "invariant") ;
    assert (((oop)(object()))->mark() == markOopDesc::encode(this), "invariant") ;
    // 从链表中移除
    UnlinkAfterAcquire (Self, SelfNode) ;
    if (_succ == Self) _succ = NULL ;
    assert (_succ != Self, "invariant") ;
    // 修改状态为TS_RUN
    SelfNode->TState = ObjectWaiter::TS_RUN ;
    OrderAccess::fence() ;      // see comments at the end of EnterI()
}

// after the thread acquires the lock in ::enter().  Equally, we could defer
// unlinking the thread until ::exit()-time.

void ObjectMonitor::UnlinkAfterAcquire (Thread * Self, ObjectWaiter * SelfNode)
{
    assert (_owner == Self, "invariant") ;
    assert (SelfNode->_thread == Self, "invariant") ;

    if (SelfNode->TState == ObjectWaiter::TS_ENTER) {
        // Normal case: remove Self from the DLL EntryList .
        // This is a constant-time operation.
        // 正常情况走此分支，将SelfNode从_EntryList中移除
        // 默认配置下，cxq链表中的节点会被转移到EntryList链表中，状态就置为TS_ENTER
        ObjectWaiter * nxt = SelfNode->_next ;
        ObjectWaiter * prv = SelfNode->_prev ;
        if (nxt != NULL) nxt->_prev = prv ;
        if (prv != NULL) prv->_next = nxt ;
        if (SelfNode == _EntryList ) _EntryList = nxt ;
        assert (nxt == NULL || nxt->TState == ObjectWaiter::TS_ENTER, "invariant") ;
        assert (prv == NULL || prv->TState == ObjectWaiter::TS_ENTER, "invariant") ;
        TEVENT (Unlink from EntryList) ;
    } else {
        guarantee (SelfNode->TState == ObjectWaiter::TS_CXQ, "invariant") ;
        // Inopportune interleaving -- Self is still on the cxq.
        // This usually means the enqueue of self raced an exiting thread.
        // Normally we'll find Self near the front of the cxq, so
        // dequeueing is typically fast.  If needbe we can accelerate
        // this with some MCS/CHL-like bidirectional list hints and advisory
        // back-links so dequeueing from the interior will normally operate
        // in constant-time.
        // Dequeue Self from either the head (with CAS) or from the interior
        // with a linear-time scan and normal non-atomic memory operations.
        // CONSIDER: if Self is on the cxq then simply drain cxq into EntryList
        // and then unlink Self from EntryList.  We have to drain eventually,
        // so it might as well be now.

        ObjectWaiter * v = _cxq ;
        assert (v != NULL, "invariant") ;
        // 如果v不等于SelfNode直接进入下面的分支，如果等于执行后面的CAS逻辑，将_cxq修改为next，如果修改失败会进入if分支
        if (v != SelfNode || Atomic::cmpxchg_ptr (SelfNode->_next, &_cxq, v) != v) {
            // The CAS above can fail from interference IFF a "RAT" arrived.
            // In that case Self must be in the interior and can no longer be
            // at the head of cxq.
            if (v == SelfNode) {
                assert (_cxq != v, "invariant") ;
                // 修改失败，说明有其他线程修改了cxq，这里重新获取cxq
                v = _cxq ;          // CAS above failed - start scan at head of list
            }
            ObjectWaiter * p ;
            ObjectWaiter * q = NULL ;
            // 遍历找到SelfNode，将其移除
            for (p = v ; p != NULL && p != SelfNode; p = p->_next) {
                q = p ;
                assert (p->TState == ObjectWaiter::TS_CXQ, "invariant") ;
            }
            assert (v != SelfNode,  "invariant") ;
            assert (p == SelfNode,  "Node not found on cxq") ;
            assert (p != _cxq,      "invariant") ;
            assert (q != NULL,      "invariant") ;
            assert (q->_next == p,  "invariant") ;
            q->_next = p->_next ;
        }
        TEVENT (Unlink from cxq) ;
    }

    // prev和next属性置为null
    // Diagnostic hygiene ...
    SelfNode->_prev  = (ObjectWaiter *) 0xBAD ;
    SelfNode->_next  = (ObjectWaiter *) 0xBAD ;
    SelfNode->TState = ObjectWaiter::TS_RUN ;
}

// -----------------------------------------------------------------------------
// Exit support
//
// exit()
// ~~~~~~
// Note that the collector can't reclaim the objectMonitor or deflate
// the object out from underneath the thread calling ::exit() as the
// thread calling ::exit() never transitions to a stable state.
// This inhibits GC, which in turn inhibits asynchronous (and
// inopportune) reclamation of "this".
//
// We'd like to assert that: (THREAD->thread_state() != _thread_blocked) ;
// There's one exception to the claim above, however.  EnterI() can call
// exit() to drop a lock if the acquirer has been externally suspended.
// In that case exit() is called with _thread_state as _thread_blocked,
// but the monitor's _count field is > 0, which inhibits reclamation.
//
// 1-0 exit
// ~~~~~~~~
// ::exit() uses a canonical 1-1 idiom with a MEMBAR although some of
// the fast-path operators have been optimized so the common ::exit()
// operation is 1-0.  See i486.ad fast_unlock(), for instance.
// The code emitted by fast_unlock() elides the usual MEMBAR.  This
// greatly improves latency -- MEMBAR and CAS having considerable local
// latency on modern processors -- but at the cost of "stranding".  Absent the
// MEMBAR, a thread in fast_unlock() can race a thread in the slow
// ::enter() path, resulting in the entering thread being stranding
// and a progress-liveness failure.   Stranding is extremely rare.
// We use timers (timed park operations) & periodic polling to detect
// and recover from stranding.  Potentially stranded threads periodically
// wake up and poll the lock.  See the usage of the _Responsible variable.
//
// The CAS() in enter provides for safety and exclusion, while the CAS or
// MEMBAR in exit provides for progress and avoids stranding.  1-0 locking
// eliminates the CAS/MEMBAR from the exist path, but it admits stranding.
// We detect and recover from stranding with timers.
//
// If a thread transiently strands it'll park until (a) another
// thread acquires the lock and then drops the lock, at which time the
// exiting thread will notice and unpark the stranded thread, or, (b)
// the timer expires.  If the lock is high traffic then the stranding latency
// will be low due to (a).  If the lock is low traffic then the odds of
// stranding are lower, although the worst-case stranding latency
// is longer.  Critically, we don't want to put excessive load in the
// platform's timer subsystem.  We want to minimize both the timer injection
// rate (timers created/sec) as well as the number of timers active at
// any one time.  (more precisely, we want to minimize timer-seconds, which is
// the integral of the # of active timers at any instant over time).
// Both impinge on OS scalability.  Given that, at most one thread parked on
// a monitor will use a timer.


// exit用于释放锁，即将owner属性置为NULL，默认配置下会通过unpark唤醒_EntryList链表头部节点对应的等待线程，
// 如果EntryList链表为空，则将cxq链表中的元素加入到EntryList链表中且顺序保持不变，即优先唤醒最近等待的线程。
// 注意exit方法并不会因为安全点同步而阻塞，exit方法退出后继续执行，无论解释执行或者编译执行则会都被阻塞；
// exit方式释放锁后，被唤醒的线程占用了该锁，在enter方法获取锁准备切换线程状态时会被阻塞。

// 第一个参数not_suspended用于debug的，可以忽略
void ATTR ObjectMonitor::exit(bool not_suspended, TRAPS) {
   Thread * Self = THREAD ;
   if (THREAD != _owner) { // 如果线程没有锁会进行修复
     if (THREAD->is_lock_owned((address) _owner)) {
       // Transmute _owner from a BasicLock pointer to a Thread address.
       // We don't need to hold _mutex for this transition.
       // Non-null to Non-null is safe as long as all readers can
       // tolerate either flavor.
       // 如果owner位于当前线程调用栈帧，说明该锁是轻量级锁膨胀来的
       assert (_recursions == 0, "invariant") ;
       // 修改owner属性
       _owner = THREAD ;
       _recursions = 0 ;
       OwnerIsThread = 1 ;
     } else {
       // NOTE: we need to handle unbalanced monitor enter/exit
       // in native code by throwing an exception.
       // TODO: Throw an IllegalMonitorStateException ?
       // 其他线程占用该锁，直接返回
       TEVENT (Exit - Throw IMSX) ;
       assert(false, "Non-balanced monitor enter/exit!");
       if (false) {
          THROW(vmSymbols::java_lang_IllegalMonitorStateException());
       }
       return;
     }
   }
   // _recursions计数不等于0；说明还没出代码块；进入减减操作。
   if (_recursions != 0) {
     _recursions--;        // this is simple recursive enter
     TEVENT (Inflated exit - recursive) ;
     return ;
   }

   // Invariant: after setting Responsible=null an thread must execute
   // a MEMBAR or other serializing instruction before fetching EntryList|cxq
   // SyncFlags默认值是0.
   if ((SyncFlags & 4) == 0) { // 没有等待线程直接设为0
      _Responsible = NULL ;
   }

#if INCLUDE_TRACE
   // get the owner's thread id for the MonitorEnter event
   // if it is enabled and the thread isn't suspended
   if (not_suspended && Tracing::is_event_enabled(TraceJavaMonitorEnterEvent)) {
     _previous_owner_tid = SharedRuntime::get_java_tid(Self);
   }
#endif

   for (;;) {
      assert (THREAD == _owner, "invariant") ;

      // Knob_ExitPolicy默认值是0
      if (Knob_ExitPolicy == 0) { // 简单释放锁策略
         // release semantics: prior loads and stores from within the critical section
         // must not float (reorder) past the following store that drops the lock.
         // On SPARC that requires MEMBAR #loadstore|#storestore.
         // But of course in TSO #loadstore|#storestore is not required.
         // I'd like to write one of the following:
         // A.  OrderAccess::release() ; _owner = NULL
         // B.  OrderAccess::loadstore(); OrderAccess::storestore(); _owner = NULL;
         // Unfortunately OrderAccess::release() and OrderAccess::loadstore() both
         // store into a _dummy variable.  That store is not needed, but can result
         // in massive wasteful coherency traffic on classic SMP systems.
         // Instead, I use release_store(), which is implemented as just a simple
         // ST on x64, x86 and SPARC.
         // 将_owner属性置为NULL，释放锁，如果某个线程正在自旋抢占该锁，则会抢占成功
         // 即这种策略会优先保证通过自旋抢占锁的线程获取锁，而其他处于等待队列中的线程则靠后
         OrderAccess::release_store_ptr (&_owner, NULL) ;   // drop the lock
         // 让修改立即生效
         OrderAccess::storeload() ;                         // See if we need to wake a successor  // 清除缓存
         if ((intptr_t(_EntryList)|intptr_t(_cxq)) == 0 || _succ != NULL) { // 有一个线程在等待锁
            // 如果_EntryList或者cxq链表都是空的，则直接返回
            TEVENT (Inflated exit - simple egress) ;
            return ;
         }
         TEVENT (Inflated exit - complex egress) ; // 释放锁

         // Normally the exiting thread is responsible for ensuring succession,
         // but if other successors are ready or other entering threads are spinning
         // then this thread can simply store NULL into _owner and exit without
         // waking a successor.  The existence of spinners or ready successors
         // guarantees proper succession (liveness).  Responsibility passes to the
         // ready or running successors.  The exiting thread delegates the duty.
         // More precisely, if a successor already exists this thread is absolved
         // of the responsibility of waking (unparking) one.
         //
         // The _succ variable is critical to reducing futile wakeup frequency.
         // _succ identifies the "heir presumptive" thread that has been made
         // ready (unparked) but that has not yet run.  We need only one such
         // successor thread to guarantee progress.
         // See http://www.usenix.org/events/jvm01/full_papers/dice/dice.pdf
         // section 3.3 "Futile Wakeup Throttling" for details.
         //
         // Note that spinners in Enter() also set _succ non-null.
         // In the current implementation spinners opportunistically set
         // _succ so that exiting threads might avoid waking a successor.
         // Another less appealing alternative would be for the exiting thread
         // to drop the lock and then spin briefly to see if a spinner managed
         // to acquire the lock.  If so, the exiting thread could exit
         // immediately without waking a successor, otherwise the exiting
         // thread would need to dequeue and wake a successor.
         // (Note that we'd need to make the post-drop spin short, but no
         // shorter than the worst-case round-trip cache-line migration time.
         // The dropped lock needs to become visible to the spinner, and then
         // the acquisition of the lock by the spinner must become visible to
         // the exiting thread).
         //

         // It appears that an heir-presumptive (successor) must be made ready.
         // Only the current lock owner can manipulate the EntryList or
         // drain _cxq, so we need to reacquire the lock.  If we fail
         // to reacquire the lock the responsibility for ensuring succession
         // falls to the new owner.
         // 当owner不为null时返回false，因为已经释放锁了，所以owner不为null说明有线程获取锁
         if (Atomic::cmpxchg_ptr (THREAD, &_owner, NULL) != NULL) {
            // 抢占失败则返回，等占用该锁的线程释放后再处理链表中的等待线程
            return ;
         }
         TEVENT (Exit - Reacquired) ; // 前面的判断获取到了锁，再释放锁
      } else {
         if ((intptr_t(_EntryList)|intptr_t(_cxq)) == 0 || _succ != NULL) {
            OrderAccess::release_store_ptr (&_owner, NULL) ;   // drop the lock
            OrderAccess::storeload() ;
            // Ratify the previously observed values.
            if (_cxq == NULL || _succ != NULL) {
                TEVENT (Inflated exit - simple egress) ;
                return ;
            }

            // inopportune interleaving -- the exiting thread (this thread)
            // in the fast-exit path raced an entering thread in the slow-enter
            // path.
            // We have two choices:
            // A.  Try to reacquire the lock.
            //     If the CAS() fails return immediately, otherwise
            //     we either restart/rerun the exit operation, or simply
            //     fall-through into the code below which wakes a successor.
            // B.  If the elements forming the EntryList|cxq are TSM
            //     we could simply unpark() the lead thread and return
            //     without having set _succ.
            // 有可能cxq插入了一个新节点，导致上面的if不成立，需要重新获取锁
            if (Atomic::cmpxchg_ptr (THREAD, &_owner, NULL) != NULL) {
               TEVENT (Inflated exit - reacquired succeeded) ;
               return ;
            }
            TEVENT (Inflated exit - reacquired failed) ;
         } else {
            // 如果_EntryList或者cxq链表不是空的则不释放锁，避免二次抢占锁，即优先处理等待队列中的线程
            TEVENT (Inflated exit - complex egress) ;
         }
      }
      // 上面的for循环确保了，有等待线程获得锁才释放锁并退出函数
      guarantee (_owner == THREAD, "invariant") ;
      // 计数为0；开始唤醒cq竞争队列、enteryList阻塞队列
      ObjectWaiter * w = NULL ;// w就是被唤醒的线程
      // Knob_QMode 是一个全局变量，它是由 Intel 的 PIN 工具框架定义的，通过在 PIN 工具代码中使用命令行参数 -qmode 设置其值。
      // 它可以控制 ObjectMonitor 中的一些机制，具体的取值及其含义如下：
      // 0：默认值，不进行队列优化，采用最基本的等待队列。
      // 1：采用 Ticket Spinlock 算法，即使用公平自旋而不是等待队列，使得等待时间更短、竞争更公平，但会增加 CPU 的占用时间。
      // 2：使用 CXQ（Cache Exclusive Queue）算法，在 CPU 的 L1 缓存中维护一个等待队列。这种算法使用缓存来减少内存访问并提高效率，但会增加内存开销。
      int QMode = Knob_QMode ;

      if (QMode == 2 && _cxq != NULL) { // 直接绕过EntryList阻塞队列，从cxq（竞争）队列中获取线程用于竞争锁
          // QMode == 2 : cxq has precedence over EntryList.
          // Try to directly wake a successor from the cxq.
          // If successful, the successor will need to unlink itself from cxq.
          w = _cxq ;
          assert (w != NULL, "invariant") ;
          assert (w->TState == ObjectWaiter::TS_CXQ, "Invariant") ;
          ExitEpilog (Self, w) ; // 该函数将当前线程从等待队列中移除，并将锁定状态设为无锁状态，以使得其他线程可以获取该锁定。
          return ;
      }

      if (QMode == 3 && _cxq != NULL) { // cxq（竞争）队列插入EntryList（阻塞）尾部；
          // Aggressively drain cxq into EntryList at the first opportunity.
          // This policy ensure that recently-run threads live at the head of EntryList.
          // Drain _cxq into EntryList - bulk transfer.
          // First, detach _cxq.
          // The following loop is tantamount to: w = swap (&cxq, NULL)
          w = _cxq ;
          for (;;) {
             assert (w != NULL, "Invariant") ;
             // CAS操做取出cxq队列首结点
             // 将_cxq原子的置为NULL，如果失败则更新w，重新尝试直到成功为止
             // 置为NULL后，如果有新的节点插入进来就形成了一个新的cxq链表
             ObjectWaiter * u = (ObjectWaiter *) Atomic::cmpxchg_ptr (NULL, &_cxq, w) ;
             if (u == w) break ;
             w = u ; // 更新w，自旋
          }
          assert (w != NULL              , "invariant") ;

          ObjectWaiter * q = NULL ;
          ObjectWaiter * p ;
          // 遍历cxq中的所有节点，将其置为TS_ENTER
          for (p = w ; p != NULL ; p = p->_next) {
              guarantee (p->TState == ObjectWaiter::TS_CXQ, "Invariant") ;
              p->TState = ObjectWaiter::TS_ENTER ;
              // 下面两句为cxq队列反向构造一条链，即将cxq变成双向链表
              p->_prev = q ;
              q = p ;
          }

          // Append the RATs to the EntryList
          // TODO: organize EntryList as a CDLL so we can locate the tail in constant-time.
          ObjectWaiter * Tail ;
          // 遍历_EntryList找到末尾元素，将w插入到后面
          for (Tail = _EntryList ; Tail != NULL && Tail->_next != NULL ; Tail = Tail->_next) ;
          if (Tail == NULL) {
              _EntryList = w ; // _EntryList为空，_EntryList=w
          } else {
              // 将w插入_EntryList队列尾部
              Tail->_next = w ;
              w->_prev = Tail ;
          }

          // Fall thru into code that tries to wake a successor from EntryList
      }

      if (QMode == 4 && _cxq != NULL) { // cxq队列插入到_EntryList头部
          // Aggressively drain cxq into EntryList at the first opportunity.
          // This policy ensure that recently-run threads live at the head of EntryList.

          // Drain _cxq into EntryList - bulk transfer.
          // First, detach _cxq.
          // The following loop is tantamount to: w = swap (&cxq, NULL)
          w = _cxq ;
          for (;;) {
             assert (w != NULL, "Invariant") ;
             // 将_cxq原子的置为NULL，如果失败则更新w，重新尝试直到成功为止
             ObjectWaiter * u = (ObjectWaiter *) Atomic::cmpxchg_ptr (NULL, &_cxq, w) ;
             if (u == w) break ;
             w = u ;
          }
          assert (w != NULL              , "invariant") ;

          ObjectWaiter * q = NULL ;
          ObjectWaiter * p ;
          // 遍历cxq中的所有节点，将其置为TS_ENTER
          for (p = w ; p != NULL ; p = p->_next) {
              guarantee (p->TState == ObjectWaiter::TS_CXQ, "Invariant") ;
              p->TState = ObjectWaiter::TS_ENTER ;
              p->_prev = q ;
              q = p ;
          }

          // Prepend the RATs to the EntryList
          // 插入到_EntryList的头部
          if (_EntryList != NULL) {
              // q为cxq队列最后一个结点
              q->_next = _EntryList ;
              _EntryList->_prev = q ;
          }
          _EntryList = w ;

          // Fall thru into code that tries to wake a successor from EntryList
      }

      w = _EntryList  ;
      if (w != NULL) {
          // I'd like to write: guarantee (w->_thread != Self).
          // But in practice an exiting thread may find itself on the EntryList.
          // Lets say thread T1 calls O.wait().  Wait() enqueues T1 on O's waitset and
          // then calls exit().  Exit release the lock by setting O._owner to NULL.
          // Lets say T1 then stalls.  T2 acquires O and calls O.notify().  The
          // notify() operation moves T1 from O's waitset to O's EntryList. T2 then
          // release the lock "O".  T2 resumes immediately after the ST of null into
          // _owner, above.  T2 notices that the EntryList is populated, so it
          // reacquires the lock and then finds itself on the EntryList.
          // Given all that, we have to tolerate the circumstance where "w" is
          // associated with Self.
          // 通过unpark唤醒w对应的线程，唤醒后会该线程会负责将w从EntryList链表中移除
          assert (w->TState == ObjectWaiter::TS_ENTER, "invariant") ;
          ExitEpilog (Self, w) ; // 从_EntryList中唤醒线程
          return ;
      }

      // If we find that both _cxq and EntryList are null then just
      // re-run the exit protocol from the top.
      // 如果_EntryList为空
      w = _cxq ;
      if (w == NULL) continue ; // 若是_cxq和_EntryList队列都为空，自旋

      // Drain _cxq into EntryList - bulk transfer.
      // First, detach _cxq.
      // The following loop is tantamount to: w = swap (&cxq, NULL)
      // cxq不为NULL
      for (;;) {
          assert (w != NULL, "Invariant") ;
          // 将cxq原子的修改为NULL
          ObjectWaiter * u = (ObjectWaiter *) Atomic::cmpxchg_ptr (NULL, &_cxq, w) ;
          if (u == w) break ;
          w = u ;
      }
      TEVENT (Inflated exit - drain cxq into EntryList) ;

      assert (w != NULL              , "invariant") ;
      assert (_EntryList  == NULL    , "invariant") ;

      // Convert the LIFO SLL anchored by _cxq into a DLL.
      // The list reorganization step operates in O(LENGTH(w)) time.
      // It's critical that this step operate quickly as
      // "Self" still holds the outer-lock, restricting parallelism
      // and effectively lengthening the critical section.
      // Invariant: s chases t chases u.
      // TODO-FIXME: consider changing EntryList from a DLL to a CDLL so
      // we have faster access to the tail.

      // 下面执行的是：cxq不为空，_EntryList为空的状况

      if (QMode == 1) { // 结合前面的代码，若是QMode == 1，_EntryList不为空，直接从_EntryList中唤醒线程
         // QMode == 1 : drain cxq to EntryList, reversing order
         // We also reverse the order of the list.
         // 遍历cxq中的元素将其加入到_EntryList中，注意顺序跟cxq中是返的
         ObjectWaiter * s = NULL ;
         ObjectWaiter * t = w ;
         ObjectWaiter * u = NULL ;
         while (t != NULL) {
             guarantee (t->TState == ObjectWaiter::TS_CXQ, "invariant") ;
             t->TState = ObjectWaiter::TS_ENTER ;
             // 下面的操做是双向链表的倒置
             u = t->_next ;
             t->_prev = u ;
             t->_next = s ;
             s = t;
             t = u ;
         }
         _EntryList  = s ; // _EntryList为倒置后的cxq队列
         assert (s != NULL, "invariant") ;
      } else {
         // QMode == 0 or QMode == 2
         // 遍历cxq中的元素将其加入到_EntryList中，注意此时cxq链表的头元素被赋值给EntryList
         _EntryList = w ;
         ObjectWaiter * q = NULL ;
         ObjectWaiter * p ;
         // cxq中的元素是通过next属性串联起来的，prev属性没有，此处遍历加上prev属性
         // 当EntryList头元素被移除了是取next属性作为EntryList
         for (p = w ; p != NULL ; p = p->_next) {
             guarantee (p->TState == ObjectWaiter::TS_CXQ, "Invariant") ;
             p->TState = ObjectWaiter::TS_ENTER ;
             // 构形成双向的
             p->_prev = q ;
             q = p ;
         }
      }

      // In 1-0 mode we need: ST EntryList; MEMBAR #storestore; ST _owner = NULL
      // The MEMBAR is satisfied by the release_store() operation in ExitEpilog().

      // See if we can abdicate to a spinner instead of waking a thread.
      // A primary goal of the implementation is to reduce the
      // context-switch rate.
      if (_succ != NULL) continue;

      w = _EntryList  ;
      if (w != NULL) {
          guarantee (w->TState == ObjectWaiter::TS_ENTER, "invariant") ;
          ExitEpilog (Self, w) ; // 从_EntryList中唤醒线程
          return ;
      }
   }
}

// ExitSuspendEquivalent:
// A faster alternate to handle_special_suspend_equivalent_condition()
//
// handle_special_suspend_equivalent_condition() unconditionally
// acquires the SR_lock.  On some platforms uncontended MutexLocker()
// operations have high latency.  Note that in ::enter() we call HSSEC
// while holding the monitor, so we effectively lengthen the critical sections.
//
// There are a number of possible solutions:
//
// A.  To ameliorate the problem we might also defer state transitions
//     to as late as possible -- just prior to parking.
//     Given that, we'd call HSSEC after having returned from park(),
//     but before attempting to acquire the monitor.  This is only a
//     partial solution.  It avoids calling HSSEC while holding the
//     monitor (good), but it still increases successor reacquisition latency --
//     the interval between unparking a successor and the time the successor
//     resumes and retries the lock.  See ReenterI(), which defers state transitions.
//     If we use this technique we can also avoid EnterI()-exit() loop
//     in ::enter() where we iteratively drop the lock and then attempt
//     to reacquire it after suspending.
//
// B.  In the future we might fold all the suspend bits into a
//     composite per-thread suspend flag and then update it with CAS().
//     Alternately, a Dekker-like mechanism with multiple variables
//     would suffice:
//       ST Self->_suspend_equivalent = false
//       MEMBAR
//       LD Self_>_suspend_flags
//


bool ObjectMonitor::ExitSuspendEquivalent (JavaThread * jSelf) {
   int Mode = Knob_FastHSSEC ;
   // Knob_FastHSSEC默认为0，即为false
   if (Mode && !jSelf->is_external_suspend()) {
      assert (jSelf->is_suspend_equivalent(), "invariant") ;
      jSelf->clear_suspend_equivalent() ;
      if (2 == Mode) OrderAccess::storeload() ;
      if (!jSelf->is_external_suspend()) return false ;
      // We raced a suspension -- fall thru into the slow path
      TEVENT (ExitSuspendEquivalent - raced) ;
      jSelf->set_suspend_equivalent() ;
   }
   // 该方法默认返回false
   return jSelf->handle_special_suspend_equivalent_condition() ;
}


void ObjectMonitor::ExitEpilog (Thread * Self, ObjectWaiter * Wakee) {
   assert (_owner == Self, "invariant") ;

   // Exit protocol:
   // 1. ST _succ = wakee
   // 2. membar #loadstore|#storestore;
   // 2. ST _owner = NULL
   // 3. unpark(wakee)

   // Knob_SuccEnabled默认是1，succ表示很有可能占用该锁的线程
   _succ = Knob_SuccEnabled ? Wakee->_thread : NULL ;
   ParkEvent * Trigger = Wakee->_event ;

   // Hygiene -- once we've set _owner = NULL we can't safely dereference Wakee again.
   // The thread associated with Wakee may have grabbed the lock and "Wakee" may be
   // out-of-scope (non-extant).
   Wakee  = NULL ;

   // Drop the lock
   // 将owner属性置为NULL
   OrderAccess::release_store_ptr (&_owner, NULL) ;
   OrderAccess::fence() ;                               // ST _owner vs LD in unpark()

   if (SafepointSynchronize::do_call_back()) {
      TEVENT (unpark before SAFEPOINT) ;
   }

   DTRACE_MONITOR_PROBE(contended__exit, this, object(), Self);
   // 唤醒之前被park()挂起的线程
   Trigger->unpark() ; // invoke ObjectMonitor::EnterI 方法，继续竞争

   // Maintain stats and report events to JVMTI
   if (ObjectMonitor::_sync_Parks != NULL) {
      // 增加计数
      ObjectMonitor::_sync_Parks->inc() ;
   }
}


// -----------------------------------------------------------------------------
// Class Loader deadlock handling.
//
// complete_exit exits a lock returning recursion count
// complete_exit/reenter operate as a wait without waiting
// complete_exit requires an inflated monitor
// The _owner field is not always the Thread addr even with an
// inflated monitor, e.g. the monitor can be inflated by a non-owning
// thread due to contention.

// complete_exit用于释放目标锁，在嵌套加锁的情形下只需要调用complete_exit一次即可，如果是exit则需要调用多次。
intptr_t ObjectMonitor::complete_exit(TRAPS) {
   Thread * const Self = THREAD;
   assert(Self->is_Java_thread(), "Must be Java thread!");
   JavaThread *jt = (JavaThread *)THREAD;

   DeferredInitialize();

   if (THREAD != _owner) {
    if (THREAD->is_lock_owned ((address)_owner)) {
       // 如果是轻量级锁膨胀来的
       assert(_recursions == 0, "internal state error");
       _owner = THREAD ;   /* Convert from basiclock addr to Thread addr */
       _recursions = 0 ;
       OwnerIsThread = 1 ;
    }
   }

   guarantee(Self == _owner, "complete_exit not owner");
   intptr_t save = _recursions; // record the old recursion count
   // _recursions置为0，即嵌套加锁的情形下不需要多次调用exit了
   _recursions = 0;        // set the recursion level to be 0
   // 释放该锁
   exit (true, Self) ;           // exit the monitor
   guarantee (_owner != Self, "invariant");
   return save;
}

// reenter() enters a lock and sets recursion count
// complete_exit/reenter operate as a wait without waiting
void ObjectMonitor::reenter(intptr_t recursions, TRAPS) {
   Thread * const Self = THREAD;
   assert(Self->is_Java_thread(), "Must be Java thread!");
   JavaThread *jt = (JavaThread *)THREAD;

   guarantee(_owner != Self, "reenter already owner");
   enter (THREAD);       // enter the monitor
   guarantee (_recursions == 0, "reenter recursion");
   _recursions = recursions;
   return;
}


// -----------------------------------------------------------------------------
// A macro is used below because there may already be a pending
// exception which should not abort the execution of the routines
// which use this (which is why we don't put this into check_slow and
// call it with a CHECK argument).

#define CHECK_OWNER()                                                             \
  do {                                                                            \
    if (THREAD != _owner) {                                                       \
      // 如果owner属性不是当前线程
      if (THREAD->is_lock_owned((address) _owner)) {                              \
        // 如果owner属性位于当前线程栈帧中，说明该锁是由轻量级锁膨胀来的
        // 修改owner属性为当前线程
        _owner = THREAD ;  /* Convert from basiclock addr to Thread addr */       \
        _recursions = 0;                                                          \
        OwnerIsThread = 1 ;                                                       \
      } else {                                                                    \
        // 当前线程没有获取锁，则抛出异常
        TEVENT (Throw IMSX) ;                                                     \
        THROW(vmSymbols::java_lang_IllegalMonitorStateException());               \
      }                                                                           \
    }                                                                             \
  } while (false)

// check_slow() is a misnomer.  It's called to simply to throw an IMSX exception.
// TODO-FIXME: remove check_slow() -- it's likely dead.

void ObjectMonitor::check_slow(TRAPS) {
  TEVENT (check_slow - throw IMSX) ;
  assert(THREAD != _owner && !THREAD->is_lock_owned((address) _owner), "must not be owner");
  THROW_MSG(vmSymbols::java_lang_IllegalMonitorStateException(), "current thread not owner");
}

static int Adjust (volatile int * adr, int dx) {
  int v ;
  for (v = *adr ; Atomic::cmpxchg (v + dx, adr, v) != v; v = *adr) ;
  return v ;
}

// helper method for posting a monitor wait event
void ObjectMonitor::post_monitor_wait_event(EventJavaMonitorWait* event,
                                                           jlong notifier_tid,
                                                           jlong timeout,
                                                           bool timedout) {
  event->set_klass(((oop)this->object())->klass());
  event->set_timeout((TYPE_ULONG)timeout);
  event->set_address((TYPE_ADDRESS)(uintptr_t)(this->object_addr()));
  event->set_notifier((TYPE_OSTHREAD)notifier_tid);
  event->set_timedOut((TYPE_BOOLEAN)timedout);
  event->commit();
}

// -----------------------------------------------------------------------------
// Wait/Notify/NotifyAll
//
// Note: a subset of changes to ObjectMonitor::wait()
// will need to be replicated in complete_exit above
// wait方法是Object的wait方法的底层实现，该方法会创建一个ObjectWaiter并加入到链表中，然后释放占有的锁，让当前线程休眠，当当前线程因为等待超时，
// 被中断或者被其他线程唤醒时就再次抢占锁，抢占逻辑就是之前的enter方法，抢占成功后wait方法退出。
void ObjectMonitor::wait(jlong millis, bool interruptible, TRAPS) {
   // 获取当前线程
   Thread * const Self = THREAD ;
   assert(Self->is_Java_thread(), "Must be Java thread!");
   JavaThread *jt = (JavaThread *)THREAD;

   // 初始化配置，如果已经初始化则返回
   DeferredInitialize () ;

   // Throw IMSX or IEX.
   // 检查当前线程是否获取了锁，如果没有则抛出异常
   CHECK_OWNER();

   EventJavaMonitorWait event;

   // check for a pending interrupt
   // 如果线程被中断了且不是因为未处理异常导致的
   if (interruptible && Thread::is_interrupted(Self, true) && !HAS_PENDING_EXCEPTION) {
     // post monitor waited event.  Note that this is past-tense, we are done waiting.
     // 发布JVMTI事件
     if (JvmtiExport::should_post_monitor_waited()) {
        // Note: 'false' parameter is passed here because the
        // wait was not timed out due to thread interrupt.
        JvmtiExport::post_monitor_waited(jt, this, false);
     }
     if (event.should_commit()) {
       post_monitor_wait_event(&event, 0, millis, false);
     }
     TEVENT (Wait - Throw IEX) ;
     // 抛出异常
     THROW(vmSymbols::java_lang_InterruptedException());
     return ;
   }

   TEVENT (Wait) ;

   assert (Self->_Stalled == 0, "invariant") ;
   // 设置属性，记录当前线程等待的ObjectMonitor
   Self->_Stalled = intptr_t(this) ;
   jt->set_current_waiting_monitor(this);

   // create a node to be put into the queue
   // Critically, after we reset() the event but prior to park(), we must check
   // for a pending interrupt.
   // 创建ObjectWaiter，将其状态置为TS_WAIT
   ObjectWaiter node(Self);
   node.TState = ObjectWaiter::TS_WAIT ;
   Self->_ParkEvent->reset() ;
   OrderAccess::fence();          // ST into Event; membar ; LD interrupted-flag

   // Enter the waiting queue, which is a circular doubly linked list in this case
   // but it could be a priority queue or any data structure.
   // _WaitSetLock protects the wait queue.  Normally the wait queue is accessed only
   // by the the owner of the monitor *except* in the case where park()
   // returns because of a timeout of interrupt.  Contention is exceptionally rare
   // so we use a simple spin-lock instead of a heavier-weight blocking lock.

   // 获取操作ObjectWaiter链表的锁_WaitSetLock
   Thread::SpinAcquire (&_WaitSetLock, "WaitSet - add") ;
   // 将当前节点插入到ObjectWaiter链表中
   AddWaiter (&node) ;
   // 释放锁
   Thread::SpinRelease (&_WaitSetLock) ;

   // SyncFlags默认为0
   if ((SyncFlags & 4) == 0) {
      _Responsible = NULL ;
   }
   intptr_t save = _recursions; // record the old recursion count
   // 等待的线程数加1
   _waiters++;                  // increment the number of waiters
   _recursions = 0;             // set the recursion level to be 1
   // 释放该锁
   exit (true, Self) ;                    // exit the monitor
   guarantee (_owner != Self, "invariant") ;

   // As soon as the ObjectMonitor's ownership is dropped in the exit()
   // call above, another thread can enter() the ObjectMonitor, do the
   // notify(), and exit() the ObjectMonitor. If the other thread's
   // exit() call chooses this thread as the successor and the unpark()
   // call happens to occur while this thread is posting a
   // MONITOR_CONTENDED_EXIT event, then we run the risk of the event
   // handler using RawMonitors and consuming the unpark().
   //
   // To avoid the problem, we re-post the event. This does no harm
   // even if the original unpark() was not consumed because we are the
   // chosen successor for this monitor.
   if (node._notified != 0 && _succ == Self) {
      node._event->unpark();
   }

   // The thread is on the WaitSet list - now park() it.
   // On MP systems it's conceivable that a brief spin before we park
   // could be profitable.
   //
   // TODO-FIXME: change the following logic to a loop of the form
   //   while (!timeout && !interrupted && _notified == 0) park()

   int ret = OS_OK ;
   int WasNotified = 0 ;
   { // State transition wrappers
     OSThread* osthread = Self->osthread();
     // 修改线程状态为OBJECT_WAIT
     OSThreadWaitState osts(osthread, true);
     {
       // 修改线程状态从_thread_in_vm到_thread_blocked
       ThreadBlockInVM tbivm(jt);
       // Thread is in thread_blocked state and oop access is unsafe.
       jt->set_suspend_equivalent();

       if (interruptible && (Thread::is_interrupted(THREAD, false) || HAS_PENDING_EXCEPTION)) {
           // Intentionally empty
       } else
       if (node._notified == 0) { // _notified为0表示没有其他线程唤醒
         // 将当前线程park，让其处于休眠状态
         if (millis <= 0) {
            Self->_ParkEvent->park () ;
         } else {
            ret = Self->_ParkEvent->park (millis) ;
         }
       }

       // were we externally suspended while we were waiting?
       // 当前线程从park状态被唤醒了
       // ExitSuspendEquivalent默认返回false
       if (ExitSuspendEquivalent (jt)) {
          // TODO-FIXME: add -- if succ == Self then succ = null.
          jt->java_suspend_self();
       }

     } // Exit thread safepoint: transition _thread_blocked -> _thread_in_vm
       // 退出代码块时会切换线程状态 _thread_blocked -> _thread_in_vm


     // Node may be on the WaitSet, the EntryList (or cxq), or in transition
     // from the WaitSet to the EntryList.
     // See if we need to remove Node from the WaitSet.
     // We use double-checked locking to avoid grabbing _WaitSetLock
     // if the thread is not on the wait queue.
     //
     // Note that we don't need a fence before the fetch of TState.
     // In the worst case we'll fetch a old-stale value of TS_WAIT previously
     // written by the is thread. (perhaps the fetch might even be satisfied
     // by a look-aside into the processor's own store buffer, although given
     // the length of the code path between the prior ST and this load that's
     // highly unlikely).  If the following LD fetches a stale TS_WAIT value
     // then we'll acquire the lock and then re-fetch a fresh TState value.
     // That is, we fail toward safety.

     // 如果是线程被中断或者等待超时则状态是TS_WAIT，如果是被nofity唤醒的则应该是TS_RUN
     if (node.TState == ObjectWaiter::TS_WAIT) {
         // 获取锁
         Thread::SpinAcquire (&_WaitSetLock, "WaitSet - unlink") ;
         if (node.TState == ObjectWaiter::TS_WAIT) {
            // 如果是TS_WAIT，则将其从链表中移除
            DequeueSpecificWaiter (&node) ;       // unlink from WaitSet
            assert(node._notified == 0, "invariant");
            // 将状态置为TS_RUN
            node.TState = ObjectWaiter::TS_RUN ;
         }
         // 释放锁
         Thread::SpinRelease (&_WaitSetLock) ;
     }

     // The thread is now either on off-list (TS_RUN),
     // on the EntryList (TS_ENTER), or on the cxq (TS_CXQ).
     // The Node's TState variable is stable from the perspective of this thread.
     // No other threads will asynchronously modify TState.
     guarantee (node.TState != ObjectWaiter::TS_WAIT, "invariant") ;
     // 让修改立即生效
     OrderAccess::loadload() ;
     if (_succ == Self) _succ = NULL ;
     WasNotified = node._notified ;

     // Reentry phase -- reacquire the monitor.
     // re-enter contended monitor after object.wait().
     // retain OBJECT_WAIT state until re-enter successfully completes
     // Thread state is thread_in_vm and oop access is again safe,
     // although the raw address of the object may have changed.
     // (Don't cache naked oops over safepoints, of course).

     // post monitor waited event. Note that this is past-tense, we are done waiting.
     if (JvmtiExport::should_post_monitor_waited()) {
       JvmtiExport::post_monitor_waited(jt, this, ret == OS_TIMEOUT);
     }

     if (event.should_commit()) {
       post_monitor_wait_event(&event, node._notifier_tid, millis, ret == OS_TIMEOUT);
     }

     OrderAccess::fence() ;

     assert (Self->_Stalled != 0, "invariant") ;
     Self->_Stalled = 0 ;

     assert (_owner != Self, "invariant") ;
     ObjectWaiter::TStates v = node.TState ;
     if (v == ObjectWaiter::TS_RUN) {
         // 重新获取该锁
         enter (Self) ;
     } else {
         // 该ObjectWaiter已经被唤醒了，但是等待获取锁的时候线程被中断了
         guarantee (v == ObjectWaiter::TS_ENTER || v == ObjectWaiter::TS_CXQ, "invariant") ;
         ReenterI (Self, &node) ;
         node.wait_reenter_end(this);
     }

     // Self has reacquired the lock.
     // Lifecycle - the node representing Self must not appear on any queues.
     // Node is about to go out-of-scope, but even if it were immortal we wouldn't
     // want residual elements associated with this thread left on any lists.
     guarantee (node.TState == ObjectWaiter::TS_RUN, "invariant") ;
     assert    (_owner == Self, "invariant") ;
     assert    (_succ != Self , "invariant") ;
   } // OSThreadWaitState()

   jt->set_current_waiting_monitor(NULL);

   guarantee (_recursions == 0, "invariant") ;
   _recursions = save;     // restore the old recursion count
   _waiters--;             // decrement the number of waiters

   // Verify a few postconditions
   assert (_owner == Self       , "invariant") ;
   assert (_succ  != Self       , "invariant") ;
   assert (((oop)(object()))->mark() == markOopDesc::encode(this), "invariant") ;

   if (SyncFlags & 32) {
      OrderAccess::fence() ;
   }

   // check if the notification happened
   // 如果不是因为notify被唤醒
   if (!WasNotified) {
     // no, it could be timeout or Thread.interrupt() or both
     // check for interrupt event, otherwise it is timeout
     // 可能因为等待超时或者Thread.interrupt()被唤醒
     if (interruptible && Thread::is_interrupted(Self, true) && !HAS_PENDING_EXCEPTION) {
       TEVENT (Wait - throw IEX from epilog) ;
       // 如果线程中断则抛出异常
       THROW(vmSymbols::java_lang_InterruptedException());
     }
   }

   // NOTE: Spurious wake up will be consider as timeout.
   // Monitor notify has precedence over thread interrupt.
}


// Consider:
// If the lock is cool (cxq == null && succ == null) and we're on an MP system
// then instead of transferring a thread from the WaitSet to the EntryList
// we might just dequeue a thread from the WaitSet and directly unpark() it.

// notify方法时Object的notify方法的底层实现，用于“唤醒”WaitSet链表头对应的线程，即最早加入到该链表的等待线程，
// 注意在默认配置下（默认的处理策略是2，不同策略的处理逻辑不同），并不会直接unpark该线程，而是将其加入到cxq链表的前面，相当于调用了一次EnterI方法。
// 加入到cxq链表后，当关联的锁被释放了就会unpark该线程，注意只是唤醒，然后该线程调用enter方法抢占锁，因此此时可能有其他线程在同时调用enter方法抢占锁。
void ObjectMonitor::notify(TRAPS) {
  // 检查当前线程是否占用该锁，如果没有抛出异常
  CHECK_OWNER();
  if (_WaitSet == NULL) {
     // 如果没有等待的线程则退出
     TEVENT (Empty-Notify) ;
     return ;
  }
  DTRACE_MONITOR_PROBE(notify, this, object(), THREAD);

  // Knob_MoveNotifyee属性默认是2
  int Policy = Knob_MoveNotifyee ;

  // 获取锁
  Thread::SpinAcquire (&_WaitSetLock, "WaitSet - notify") ;
  // 将链表头元素移除并返回
  ObjectWaiter * iterator = DequeueWaiter() ;
  if (iterator != NULL) {
     TEVENT (Notify1 - Transfer) ;
     guarantee (iterator->TState == ObjectWaiter::TS_WAIT, "invariant") ;
     guarantee (iterator->_notified == 0, "invariant") ;
     if (Policy != 4) {
        // 将状态置为TS_ENTER
        iterator->TState = ObjectWaiter::TS_ENTER ;
     }
     // _notified置为1表示该ObjectWaiter被唤醒了
     iterator->_notified = 1 ;
     Thread * Self = THREAD;
     // 记录当前线程ID
     iterator->_notifier_tid = Self->osthread()->thread_id();

     ObjectWaiter * List = _EntryList ;
     if (List != NULL) {
        assert (List->_prev == NULL, "invariant") ;
        assert (List->TState == ObjectWaiter::TS_ENTER, "invariant") ;
        assert (List != iterator, "invariant") ;
     }

     // 根据不同的策略执行不同的处理
     if (Policy == 0) {       // prepend to EntryList   // 将iterator插入到_EntryList头元素的前面
         if (List == NULL) {
             iterator->_next = iterator->_prev = NULL ;
             _EntryList = iterator ;
         } else {
             List->_prev = iterator ;
             iterator->_next = List ;
             iterator->_prev = NULL ;
             _EntryList = iterator ;
        }
     } else
     if (Policy == 1) {      // append to EntryList     // 将iterator插入到_EntryList链表的末尾
         if (List == NULL) {
             iterator->_next = iterator->_prev = NULL ;
             _EntryList = iterator ;
         } else {
            // CONSIDER:  finding the tail currently requires a linear-time walk of
            // the EntryList.  We can make tail access constant-time by converting to
            // a CDLL instead of using our current DLL.
            ObjectWaiter * Tail ;
            // 不断遍历找到链表最后一个元素
            for (Tail = List ; Tail->_next != NULL ; Tail = Tail->_next) ;
            assert (Tail != NULL && Tail->_next == NULL, "invariant") ;
            Tail->_next = iterator ;
            iterator->_prev = Tail ;
            iterator->_next = NULL ;
        }
     } else
     if (Policy == 2) {      // prepend to cxq      // 将iterator插入到_cxq头元素的前面
         // prepend to cxq
         if (List == NULL) {
             iterator->_next = iterator->_prev = NULL ;
             _EntryList = iterator ;
         } else {
            iterator->TState = ObjectWaiter::TS_CXQ ;
            for (;;) {
                ObjectWaiter * Front = _cxq ;
                iterator->_next = Front ;
                if (Atomic::cmpxchg_ptr (iterator, &_cxq, Front) == Front) {
                    break ;
                }
            }
         }
     } else
     if (Policy == 3) {      // append to cxq       // 将iterator插入到_cxq链表末尾的后面
        iterator->TState = ObjectWaiter::TS_CXQ ;
        for (;;) {
            ObjectWaiter * Tail ;
            Tail = _cxq ;
            if (Tail == NULL) {
                iterator->_next = NULL ;
                if (Atomic::cmpxchg_ptr (iterator, &_cxq, NULL) == NULL) {
                   break ;
                }
            } else {
                // 往后遍历找到最后一个元素
                while (Tail->_next != NULL) Tail = Tail->_next ;
                Tail->_next = iterator ;
                iterator->_prev = Tail ;
                iterator->_next = NULL ;
                break ;
            }
        }
     } else {
        // 将等待的线程直接unpark唤醒
        ParkEvent * ev = iterator->_event ;
        iterator->TState = ObjectWaiter::TS_RUN ;
        OrderAccess::fence() ;
        ev->unpark() ;
     }

     if (Policy < 4) {
       // 修改线程状态，记录锁竞争开始
       iterator->wait_reenter_begin(this);
     }

     // _WaitSetLock protects the wait queue, not the EntryList.  We could
     // move the add-to-EntryList operation, above, outside the critical section
     // protected by _WaitSetLock.  In practice that's not useful.  With the
     // exception of  wait() timeouts and interrupts the monitor owner
     // is the only thread that grabs _WaitSetLock.  There's almost no contention
     // on _WaitSetLock so it's not profitable to reduce the length of the
     // critical section.
  }
  // 释放锁
  Thread::SpinRelease (&_WaitSetLock) ;

  if (iterator != NULL && ObjectMonitor::_sync_Notifications != NULL) {
     // 增加计数
     ObjectMonitor::_sync_Notifications->inc() ;
  }
}

// notifyAll方法就是Object的notifyAll方法的底层实现，对单个ObjectWaiter其处理逻辑跟notify是一致的，
// 相比notify的实现就是增加了一个for循环，会不断的从_WaitSet链表中移除头元素，然后执行notify的处理逻辑，直到_WaitSet链表为空退出循环。
void ObjectMonitor::notifyAll(TRAPS) {
  // 检查当前线程是否占用该锁，如果没有抛出异常
  CHECK_OWNER();
  ObjectWaiter* iterator;
  if (_WaitSet == NULL) {
      // 如果没有等待的线程则退出
      TEVENT (Empty-NotifyAll) ;
      return ;
  }
  DTRACE_MONITOR_PROBE(notifyAll, this, object(), THREAD);

  // Knob_MoveNotifyee属性默认是2
  int Policy = Knob_MoveNotifyee ;
  int Tally = 0 ;
  // 获取锁
  Thread::SpinAcquire (&_WaitSetLock, "WaitSet - notifyall") ;

  for (;;) {
     // 获取头部元素，头部节点为最早加入到链表中的节点
     iterator = DequeueWaiter () ;
     // 如果为空则终止循环
     if (iterator == NULL) break ;
     TEVENT (NotifyAll - Transfer1) ;
     // 增加计数
     ++Tally ;

     // Disposition - what might we do with iterator ?
     // a.  add it directly to the EntryList - either tail or head.
     // b.  push it onto the front of the _cxq.
     // For now we use (a).

     guarantee (iterator->TState == ObjectWaiter::TS_WAIT, "invariant") ;
     guarantee (iterator->_notified == 0, "invariant") ;
     // _notified置为1表示该ObjectWaiter被唤醒了
     iterator->_notified = 1 ;
     Thread * Self = THREAD;
     // 记录当前线程ID
     iterator->_notifier_tid = Self->osthread()->thread_id();
     if (Policy != 4) {
        // 将状态置为TS_ENTER
        iterator->TState = ObjectWaiter::TS_ENTER ;
     }

     // 根据不同的策略执行不同的处理
     ObjectWaiter * List = _EntryList ;
     if (List != NULL) {
        assert (List->_prev == NULL, "invariant") ;
        assert (List->TState == ObjectWaiter::TS_ENTER, "invariant") ;
        assert (List != iterator, "invariant") ;
     }

     if (Policy == 0) {       // prepend to EntryList       // 将iterator插入到_EntryList头元素的前面
         if (List == NULL) {
             iterator->_next = iterator->_prev = NULL ;
             _EntryList = iterator ;
         } else {
             List->_prev = iterator ;
             iterator->_next = List ;
             iterator->_prev = NULL ;
             _EntryList = iterator ;
        }
     } else
     if (Policy == 1) {      // append to EntryList         // 将iterator插入到_EntryList链表的末尾
         if (List == NULL) {
             iterator->_next = iterator->_prev = NULL ;
             _EntryList = iterator ;
         } else {
            // CONSIDER:  finding the tail currently requires a linear-time walk of
            // the EntryList.  We can make tail access constant-time by converting to
            // a CDLL instead of using our current DLL.
            ObjectWaiter * Tail ;
            for (Tail = List ; Tail->_next != NULL ; Tail = Tail->_next) ;
            assert (Tail != NULL && Tail->_next == NULL, "invariant") ;
            Tail->_next = iterator ;
            iterator->_prev = Tail ;
            iterator->_next = NULL ;
        }
     } else
     if (Policy == 2) {      // prepend to cxq              // 将iterator插入到_cxq头元素的前面
         // prepend to cxq
         iterator->TState = ObjectWaiter::TS_CXQ ;
         for (;;) {
             ObjectWaiter * Front = _cxq ;
             iterator->_next = Front ;
             if (Atomic::cmpxchg_ptr (iterator, &_cxq, Front) == Front) {
                 break ;
             }
         }
     } else
     if (Policy == 3) {      // append to cxq               // 将iterator插入到_cxq链表末尾的后面
        iterator->TState = ObjectWaiter::TS_CXQ ;
        for (;;) {
            ObjectWaiter * Tail ;
            Tail = _cxq ;
            if (Tail == NULL) {
                iterator->_next = NULL ;
                if (Atomic::cmpxchg_ptr (iterator, &_cxq, NULL) == NULL) {
                   break ;
                }
            } else {
                while (Tail->_next != NULL) Tail = Tail->_next ;
                Tail->_next = iterator ;
                iterator->_prev = Tail ;
                iterator->_next = NULL ;
                break ;
            }
        }
     } else {
        // 将等待的线程直接unpark唤醒
        ParkEvent * ev = iterator->_event ;
        iterator->TState = ObjectWaiter::TS_RUN ;
        OrderAccess::fence() ;
        ev->unpark() ;
     }

     if (Policy < 4) {
       // 修改线程状态，记录锁竞争开始
       iterator->wait_reenter_begin(this);
     }

     // _WaitSetLock protects the wait queue, not the EntryList.  We could
     // move the add-to-EntryList operation, above, outside the critical section
     // protected by _WaitSetLock.  In practice that's not useful.  With the
     // exception of  wait() timeouts and interrupts the monitor owner
     // is the only thread that grabs _WaitSetLock.  There's almost no contention
     // on _WaitSetLock so it's not profitable to reduce the length of the
     // critical section.
  }

  // 释放锁
  Thread::SpinRelease (&_WaitSetLock) ;

  if (Tally != 0 && ObjectMonitor::_sync_Notifications != NULL) {
     // 增加计数
     ObjectMonitor::_sync_Notifications->inc(Tally) ;
  }
}

// -----------------------------------------------------------------------------
// Adaptive Spinning Support
//
// Adaptive spin-then-block - rational spinning
//
// Note that we spin "globally" on _owner with a classic SMP-polite TATAS
// algorithm.  On high order SMP systems it would be better to start with
// a brief global spin and then revert to spinning locally.  In the spirit of MCS/CLH,
// a contending thread could enqueue itself on the cxq and then spin locally
// on a thread-specific variable such as its ParkEvent._Event flag.
// That's left as an exercise for the reader.  Note that global spinning is
// not problematic on Niagara, as the L2$ serves the interconnect and has both
// low latency and massive bandwidth.
//
// Broadly, we can fix the spin frequency -- that is, the % of contended lock
// acquisition attempts where we opt to spin --  at 100% and vary the spin count
// (duration) or we can fix the count at approximately the duration of
// a context switch and vary the frequency.   Of course we could also
// vary both satisfying K == Frequency * Duration, where K is adaptive by monitor.
// See http://j2se.east/~dice/PERSIST/040824-AdaptiveSpinning.html.
//
// This implementation varies the duration "D", where D varies with
// the success rate of recent spin attempts. (D is capped at approximately
// length of a round-trip context switch).  The success rate for recent
// spin attempts is a good predictor of the success rate of future spin
// attempts.  The mechanism adapts automatically to varying critical
// section length (lock modality), system load and degree of parallelism.
// D is maintained per-monitor in _SpinDuration and is initialized
// optimistically.  Spin frequency is fixed at 100%.
//
// Note that _SpinDuration is volatile, but we update it without locks
// or atomics.  The code is designed so that _SpinDuration stays within
// a reasonable range even in the presence of races.  The arithmetic
// operations on _SpinDuration are closed over the domain of legal values,
// so at worst a race will install and older but still legal value.
// At the very worst this introduces some apparent non-determinism.
// We might spin when we shouldn't or vice-versa, but since the spin
// count are relatively short, even in the worst case, the effect is harmless.
//
// Care must be taken that a low "D" value does not become an
// an absorbing state.  Transient spinning failures -- when spinning
// is overall profitable -- should not cause the system to converge
// on low "D" values.  We want spinning to be stable and predictable
// and fairly responsive to change and at the same time we don't want
// it to oscillate, become metastable, be "too" non-deterministic,
// or converge on or enter undesirable stable absorbing states.
//
// We implement a feedback-based control system -- using past behavior
// to predict future behavior.  We face two issues: (a) if the
// input signal is random then the spin predictor won't provide optimal
// results, and (b) if the signal frequency is too high then the control
// system, which has some natural response lag, will "chase" the signal.
// (b) can arise from multimodal lock hold times.  Transient preemption
// can also result in apparent bimodal lock hold times.
// Although sub-optimal, neither condition is particularly harmful, as
// in the worst-case we'll spin when we shouldn't or vice-versa.
// The maximum spin duration is rather short so the failure modes aren't bad.
// To be conservative, I've tuned the gain in system to bias toward
// _not spinning.  Relatedly, the system can sometimes enter a mode where it
// "rings" or oscillates between spinning and not spinning.  This happens
// when spinning is just on the cusp of profitability, however, so the
// situation is not dire.  The state is benign -- there's no need to add
// hysteresis control to damp the transition rate between spinning and
// not spinning.
//

intptr_t ObjectMonitor::SpinCallbackArgument = 0 ;
int (*ObjectMonitor::SpinCallbackFunction)(intptr_t, int) = NULL ;

// Spinning: Fixed frequency (100%), vary duration

// 默认配置下自旋的次数是会自适应调整的，可以通过参数指定自旋固定的次数，注意在自旋的过程中会判断是否进入安全点同步，如果是则终止自旋。
int ObjectMonitor::TrySpin_VaryDuration (Thread * Self) {

    // Dumb, brutal spin.  Good for comparative measurements against adaptive spinning.
    // Knob_FixedSpin默认是0，表示固定自旋的次数
    int ctr = Knob_FixedSpin ;
    if (ctr != 0) {
        // 每一次while循环都是一次自旋，在指定的次数内抢占成功就是成功，否则失败
        while (--ctr >= 0) {
            // 尝试抢占该锁，如果成功返回1
            if (TryLock (Self) > 0) return 1 ;
            // 抢占失败，该方法直接返回0
            SpinPause () ;
        }
        return 0 ;
    }
    // Knob_PreSpin的默认值是10
    for (ctr = Knob_PreSpin + 1; --ctr >= 0 ; ) {
      if (TryLock(Self) > 0) {
        // Increase _SpinDuration ...
        // Note that we don't clamp SpinDuration precisely at SpinLimit.
        // Raising _SpurDuration to the poverty line is key.
        // 抢占成功
        int x = _SpinDuration ;
        // Knob_SpinLimit的默认值是5000
        if (x < Knob_SpinLimit) {
           // 增加_SpinDuration
           // Knob_Poverty的默认值是1000，Knob_BonusB对的默认值是100
           if (x < Knob_Poverty) x = Knob_Poverty ;
           // 即_SpinDuration的最小值是1100，最大值是5000
           _SpinDuration = x + Knob_BonusB ;
        }
        return 1 ;
      }
      SpinPause () ;
    }

    // Admission control - verify preconditions for spinning
    //
    // We always spin a little bit, just to prevent _SpinDuration == 0 from
    // becoming an absorbing state.  Put another way, we spin briefly to
    // sample, just in case the system load, parallelism, contention, or lock
    // modality changed.
    //
    // Consider the following alternative:
    // Periodically set _SpinDuration = _SpinLimit and try a long/full
    // spin attempt.  "Periodically" might mean after a tally of
    // the # of failed spin attempts (or iterations) reaches some threshold.
    // This takes us into the realm of 1-out-of-N spinning, where we
    // hold the duration constant but vary the frequency.

    ctr = _SpinDuration  ;
    // Knob_SpinBase的默认值是10
    if (ctr < Knob_SpinBase) ctr = Knob_SpinBase ;
    if (ctr <= 0) return 0 ;
    // Knob_SuccRestrict默认为0
    if (Knob_SuccRestrict && _succ != NULL) return 0 ;
    // Knob_OState默认为3，NotRunnable用于判断目标线程是否退出，如果已退出则终止自旋
    if (Knob_OState && NotRunnable (Self, (Thread *) _owner)) {
       TEVENT (Spin abort - notrunnable [TOP]);
       return 0 ;
    }
    // Knob_MaxSpinners默认为-1
    int MaxSpin = Knob_MaxSpinners ;
    if (MaxSpin >= 0) {
       if (_Spinner > MaxSpin) {
          TEVENT (Spin abort -- too many spinners) ;
          return 0 ;
       }
       // Slighty racy, but benign ...
       // 原子的将_Spinner属性加1，不断循环直到修改成功
       Adjust (&_Spinner, 1) ;
    }

    // We're good to spin ... spin ingress.
    // CONSIDER: use Prefetch::write() to avoid RTS->RTO upgrades
    // when preparing to LD...CAS _owner, etc and the CAS is likely
    // to succeed.
    int hits    = 0 ;
    int msk     = 0 ;
    // Knob_CASPenalty默认值是-1
    int caspty  = Knob_CASPenalty ;
    // Knob_OXPenalty默认值是-1
    int oxpty   = Knob_OXPenalty ;
    // Knob_SpinSetSucc默认值是1
    int sss     = Knob_SpinSetSucc ;
    if (sss && _succ == NULL ) _succ = Self ;
    Thread * prv = NULL ;

    // There are three ways to exit the following loop:
    // 1.  A successful spin where this thread has acquired the lock.
    // 2.  Spin failure with prejudice
    // 3.  Spin failure without prejudice

    // 有三种方法可以退出以下循环：
    // 1. 此线程已获得锁定的成功旋转。
    // 2. 带有偏见的旋转失败
    // 3. 不带偏见的旋转失败

    while (--ctr >= 0) {

      // Periodic polling -- Check for pending GC
      // Threads may spin while they're unsafe.
      // We don't want spinning threads to delay the JVM from reaching
      // a stop-the-world safepoint or to steal cycles from GC.
      // If we detect a pending safepoint we abort in order that
      // (a) this thread, if unsafe, doesn't delay the safepoint, and (b)
      // this thread, if safe, doesn't steal cycles from GC.
      // This is in keeping with the "no loitering in runtime" rule.
      // We periodically check to see if there's a safepoint pending.
      // 0xFF就是256，即每自旋256次就需要检查是否开启了安全点同步
      if ((ctr & 0xFF) == 0) {
         if (SafepointSynchronize::do_call_back()) {
            // do_call_back返回true，说明进入了安全点同步
            TEVENT (Spin: safepoint) ;
            // 跳转到Abort
            goto Abort ;           // abrupt spin egress
         }
         // Knob_UsePause默认值是1
         if (Knob_UsePause & 1) SpinPause () ;
         // SpinCallbackFunction默认为NULL
         int (*scb)(intptr_t,int) = SpinCallbackFunction ;
         if (hits > 50 && scb != NULL) {
            int abend = (*scb)(SpinCallbackArgument, 0) ;
         }
      }

      if (Knob_UsePause & 2) SpinPause() ;

      // Exponential back-off ...  Stay off the bus to reduce coherency traffic.
      // This is useful on classic SMP systems, but is of less utility on
      // N1-style CMT platforms.
      //
      // Trade-off: lock acquisition latency vs coherency bandwidth.
      // Lock hold times are typically short.  A histogram
      // of successful spin attempts shows that we usually acquire
      // the lock early in the spin.  That suggests we want to
      // sample _owner frequently in the early phase of the spin,
      // but then back-off and sample less frequently as the spin
      // progresses.  The back-off makes a good citizen on SMP big
      // SMP systems.  Oversampling _owner can consume excessive
      // coherency bandwidth.  Relatedly, if we _oversample _owner we
      // can inadvertently interfere with the the ST m->owner=null.
      // executed by the lock owner.
      if (ctr & msk) continue ;
      ++hits ;
      if ((hits & 0xF) == 0) {
        // The 0xF, above, corresponds to the exponent.
        // Consider: (msk+1)|msk
        // BackOffMask默认值是0
        msk = ((msk << 2)|3) & BackOffMask ;
      }

      // Probe _owner with TATAS
      // If this thread observes the monitor transition or flicker
      // from locked to unlocked to locked, then the odds that this
      // thread will acquire the lock in this spin attempt go down
      // considerably.  The same argument applies if the CAS fails
      // or if we observe _owner change from one non-null value to
      // another non-null value.   In such cases we might abort
      // the spin without prejudice or apply a "penalty" to the
      // spin count-down variable "ctr", reducing it by 100, say.

      Thread * ox = (Thread *) _owner ;
      if (ox == NULL) {
         // 该锁未被占用，通过cas抢占
         ox = (Thread *) Atomic::cmpxchg_ptr (Self, &_owner, NULL) ;
         if (ox == NULL) {
            // The CAS succeeded -- this thread acquired ownership
            // Take care of some bookkeeping to exit spin state.
            // 抢占成功
            if (sss && _succ == Self) {
               _succ = NULL ;
            }
            // 原子的将_Spinner减1
            if (MaxSpin > 0) Adjust (&_Spinner, -1) ;

            // Increase _SpinDuration :
            // The spin was successful (profitable) so we tend toward
            // longer spin attempts in the future.
            // CONSIDER: factor "ctr" into the _SpinDuration adjustment.
            // If we acquired the lock early in the spin cycle it
            // makes sense to increase _SpinDuration proportionally.
            // Note that we don't clamp SpinDuration precisely at SpinLimit.
            // 增加_SpinDuration
            int x = _SpinDuration ;
            if (x < Knob_SpinLimit) {
                if (x < Knob_Poverty) x = Knob_Poverty ;
                _SpinDuration = x + Knob_Bonus ;
            }
            return 1 ;
         }

         // The CAS failed ... we can take any of the following actions:
         // * penalize: ctr -= Knob_CASPenalty
         // * exit spin with prejudice -- goto Abort;
         // * exit spin without prejudice.
         // * Since CAS is high-latency, retry again immediately.
         // CAS抢占失败，caspty默认是-1
         prv = ox ;
         TEVENT (Spin: cas failed) ;
         if (caspty == -2) break ;
         if (caspty == -1) goto Abort ;
         ctr -= caspty ;
         continue ;
      }

      // Did lock ownership change hands ?
      // 如果占有该锁的线程发生改变了，oxpty默认值是-1
      if (ox != prv && prv != NULL ) {
          TEVENT (spin: Owner changed)
          if (oxpty == -2) break ;
          if (oxpty == -1) goto Abort ;
          ctr -= oxpty ;
      }
      // 记录下当前占用锁的线程
      prv = ox ;

      // Abort the spin if the owner is not executing.
      // The owner must be executing in order to drop the lock.
      // Spinning while the owner is OFFPROC is idiocy.
      // Consider: ctr -= RunnablePenalty ;
      // 如果占有该锁的线程退出了，则终止自旋
      if (Knob_OState && NotRunnable (Self, ox)) {
         TEVENT (Spin abort - notrunnable);
         goto Abort ;
      }
      if (sss && _succ == NULL ) _succ = Self ;
   } // while循环结束

   // Spin failed with prejudice -- reduce _SpinDuration.
   // TODO: Use an AIMD-like policy to adjust _SpinDuration.
   // AIMD is globally stable.
   TEVENT (Spin failure) ;
   {
     int x = _SpinDuration ;
     if (x > 0) {
        // Consider an AIMD scheme like: x -= (x >> 3) + 100
        // This is globally sample and tends to damp the response.
        // Knob_Penalty的默认值是200
        x -= Knob_Penalty ;
        if (x < 0) x = 0 ;
        // 实际就是将_SpinDuration减去Knob_Penalty
        _SpinDuration = x ;
     }
   }

 Abort:
   if (MaxSpin >= 0) Adjust (&_Spinner, -1) ;
   if (sss && _succ == Self) {
      _succ = NULL ;
      // Invariant: after setting succ=null a contending thread
      // must recheck-retry _owner before parking.  This usually happens
      // in the normal usage of TrySpin(), but it's safest
      // to make TrySpin() as foolproof as possible.
      OrderAccess::fence() ;
      // 尝试获取锁
      if (TryLock(Self) > 0) return 1 ;
   }
   return 0 ;
}

// NotRunnable() -- informed spinning
//
// Don't bother spinning if the owner is not eligible to drop the lock.
// Peek at the owner's schedctl.sc_state and Thread._thread_values and
// spin only if the owner thread is _thread_in_Java or _thread_in_vm.
// The thread must be runnable in order to drop the lock in timely fashion.
// If the _owner is not runnable then spinning will not likely be
// successful (profitable).
//
// Beware -- the thread referenced by _owner could have died
// so a simply fetch from _owner->_thread_state might trap.
// Instead, we use SafeFetchXX() to safely LD _owner->_thread_state.
// Because of the lifecycle issues the schedctl and _thread_state values
// observed by NotRunnable() might be garbage.  NotRunnable must
// tolerate this and consider the observed _thread_state value
// as advisory.
//
// Beware too, that _owner is sometimes a BasicLock address and sometimes
// a thread pointer.  We differentiate the two cases with OwnerIsThread.
// Alternately, we might tag the type (thread pointer vs basiclock pointer)
// with the LSB of _owner.  Another option would be to probablistically probe
// the putative _owner->TypeTag value.
//
// Checking _thread_state isn't perfect.  Even if the thread is
// in_java it might be blocked on a page-fault or have been preempted
// and sitting on a ready/dispatch queue.  _thread state in conjunction
// with schedctl.sc_state gives us a good picture of what the
// thread is doing, however.
//
// TODO: check schedctl.sc_state.
// We'll need to use SafeFetch32() to read from the schedctl block.
// See RFE #5004247 and http://sac.sfbay.sun.com/Archives/CaseLog/arc/PSARC/2005/351/
//
// The return value from NotRunnable() is *advisory* -- the
// result is based on sampling and is not necessarily coherent.
// The caller must tolerate false-negative and false-positive errors.
// Spinning, in general, is probabilistic anyway.


int ObjectMonitor::NotRunnable (Thread * Self, Thread * ox) {
    // Check either OwnerIsThread or ox->TypeTag == 2BAD.
    if (!OwnerIsThread) return 0 ;

    if (ox == NULL) return 0 ;

    // Avoid transitive spinning ...
    // Say T1 spins or blocks trying to acquire L.  T1._Stalled is set to L.
    // Immediately after T1 acquires L it's possible that T2, also
    // spinning on L, will see L.Owner=T1 and T1._Stalled=L.
    // This occurs transiently after T1 acquired L but before
    // T1 managed to clear T1.Stalled.  T2 does not need to abort
    // its spin in this circumstance.
    intptr_t BlockedOn = SafeFetchN ((intptr_t *) &ox->_Stalled, intptr_t(1)) ;

    if (BlockedOn == 1) return 1 ;
    if (BlockedOn != 0) {
      return BlockedOn != intptr_t(this) && _owner == ox ;
    }

    assert (sizeof(((JavaThread *)ox)->_thread_state == sizeof(int)), "invariant") ;
    int jst = SafeFetch32 ((int *) &((JavaThread *) ox)->_thread_state, -1) ; ;
    // consider also: jst != _thread_in_Java -- but that's overspecific.
    return jst == _thread_blocked || jst == _thread_in_native ;
}


// -----------------------------------------------------------------------------
// WaitSet management ...

ObjectWaiter::ObjectWaiter(Thread* thread) {
  _next     = NULL;
  _prev     = NULL;
  _notified = 0;
  TState    = TS_RUN ;  // 初始状态
  _thread   = thread;   // 关联的线程
  _event    = thread->_ParkEvent ;
  _active   = false;
  assert (_event != NULL, "invariant") ;
}

void ObjectWaiter::wait_reenter_begin(ObjectMonitor *mon) {
  // 将当前线程转换为JavaThread类型的指针
  JavaThread *jt = (JavaThread *)this->_thread;
  // 将_blocked_on_monitor_enter_state属性设置为阻塞在ObjectMonitor上的状态，
  // 同时返回一个布尔值_active表示是否启用了争用监视（contention monitoring）。
  _active = JavaThreadBlockedOnMonitorEnterState::wait_reenter_begin(jt, mon);
}

void ObjectWaiter::wait_reenter_end(ObjectMonitor *mon) {
  // 将当前线程转换为JavaThread对象
  JavaThread *jt = (JavaThread *)this->_thread;
  // 完成线程状态的还原以及唤醒等待线程的工作
  JavaThreadBlockedOnMonitorEnterState::wait_reenter_end(jt, _active);
}

// 用于将目标ObjectWaiter加入到双向循环链表中
inline void ObjectMonitor::AddWaiter(ObjectWaiter* node) {
  assert(node != NULL, "should not dequeue NULL node");
  assert(node->_prev == NULL, "node already in list");
  assert(node->_next == NULL, "node already in list");
  // put node at end of queue (circular doubly linked list)
  // 将目标节点放入一个双向的循环链表中
  if (_WaitSet == NULL) {
    // 如果_WaitSet还是空的，当前节点就是第一个
    _WaitSet = node;
    node->_prev = node;
    node->_next = node;
  } else {
    // 如果_WaitSet不是空的，将其插入到head的prev节点上
    ObjectWaiter* head = _WaitSet ;
    ObjectWaiter* tail = head->_prev;
    assert(tail->_next == head, "invariant check");
    // 注意tail在初始状态下就是head，所以插入第二个节点时修改next属性，实际是修改head的next属性
    tail->_next = node;
    head->_prev = node;
    node->_next = head;
    node->_prev = tail;
  }
}

// 用于移除链表头_WaitSet对应的节点，该节点是最早加入到链表的，即按照加入链表的先后顺序依次从链表中移除
inline ObjectWaiter* ObjectMonitor::DequeueWaiter() {
  // dequeue the very first waiter
  ObjectWaiter* waiter = _WaitSet;
  if (waiter) {
    // 如果_WaitSet为不空
    DequeueSpecificWaiter(waiter);
  }
  return waiter;
}

// 用于移除指定节点，不一定是_WaitSet对应的节点
inline void ObjectMonitor::DequeueSpecificWaiter(ObjectWaiter* node) {
  assert(node != NULL, "should not dequeue NULL node");
  assert(node->_prev != NULL, "node already removed from list");
  assert(node->_next != NULL, "node already removed from list");
  // when the waiter has woken up because of interrupt,
  // timeout or other spurious wake-up, dequeue the
  // waiter from waiting list
  // 从_WaitSet中取出一个ObjectWaiter，实际就是取出_WaitSet对应的head节点，该节点是最早加入到链表中的
  ObjectWaiter* next = node->_next;
  if (next == node) {
    // _WaitSet只有一个节点
    assert(node->_prev == node, "invariant check");
    _WaitSet = NULL;
  } else {
    // 将node从链表中移除
    ObjectWaiter* prev = node->_prev;
    assert(prev->_next == node, "invariant check");
    assert(next->_prev == node, "invariant check");
    next->_prev = prev;
    prev->_next = next;
    if (_WaitSet == node) {
      // 如果移除的就是_WaitSet，将next置为_WaitSet
      _WaitSet = next;
    }
  }
  node->_next = NULL;
  node->_prev = NULL;
}

// -----------------------------------------------------------------------------
// PerfData support
PerfCounter * ObjectMonitor::_sync_ContendedLockAttempts       = NULL ;
PerfCounter * ObjectMonitor::_sync_FutileWakeups               = NULL ;
PerfCounter * ObjectMonitor::_sync_Parks                       = NULL ;
PerfCounter * ObjectMonitor::_sync_EmptyNotifications          = NULL ;
PerfCounter * ObjectMonitor::_sync_Notifications               = NULL ;
PerfCounter * ObjectMonitor::_sync_PrivateA                    = NULL ;
PerfCounter * ObjectMonitor::_sync_PrivateB                    = NULL ;
PerfCounter * ObjectMonitor::_sync_SlowExit                    = NULL ;
PerfCounter * ObjectMonitor::_sync_SlowEnter                   = NULL ;
PerfCounter * ObjectMonitor::_sync_SlowNotify                  = NULL ;
PerfCounter * ObjectMonitor::_sync_SlowNotifyAll               = NULL ;
PerfCounter * ObjectMonitor::_sync_FailedSpins                 = NULL ;
PerfCounter * ObjectMonitor::_sync_SuccessfulSpins             = NULL ;
PerfCounter * ObjectMonitor::_sync_MonInCirculation            = NULL ;
PerfCounter * ObjectMonitor::_sync_MonScavenged                = NULL ;
PerfCounter * ObjectMonitor::_sync_Inflations                  = NULL ;
PerfCounter * ObjectMonitor::_sync_Deflations                  = NULL ;
PerfLongVariable * ObjectMonitor::_sync_MonExtant              = NULL ;

// One-shot global initialization for the sync subsystem.
// We could also defer initialization and initialize on-demand
// the first time we call inflate().  Initialization would
// be protected - like so many things - by the MonitorCache_lock.

void ObjectMonitor::Initialize () {
  static int InitializationCompleted = 0 ;
  assert (InitializationCompleted == 0, "invariant") ;
  InitializationCompleted = 1 ;
  if (UsePerfData) {
      EXCEPTION_MARK ;
      #define NEWPERFCOUNTER(n)   {n = PerfDataManager::create_counter(SUN_RT, #n, PerfData::U_Events,CHECK); }
      #define NEWPERFVARIABLE(n)  {n = PerfDataManager::create_variable(SUN_RT, #n, PerfData::U_Events,CHECK); }
      NEWPERFCOUNTER(_sync_Inflations) ;
      NEWPERFCOUNTER(_sync_Deflations) ;
      NEWPERFCOUNTER(_sync_ContendedLockAttempts) ;
      NEWPERFCOUNTER(_sync_FutileWakeups) ;
      NEWPERFCOUNTER(_sync_Parks) ;
      NEWPERFCOUNTER(_sync_EmptyNotifications) ;
      NEWPERFCOUNTER(_sync_Notifications) ;
      NEWPERFCOUNTER(_sync_SlowEnter) ;
      NEWPERFCOUNTER(_sync_SlowExit) ;
      NEWPERFCOUNTER(_sync_SlowNotify) ;
      NEWPERFCOUNTER(_sync_SlowNotifyAll) ;
      NEWPERFCOUNTER(_sync_FailedSpins) ;
      NEWPERFCOUNTER(_sync_SuccessfulSpins) ;
      NEWPERFCOUNTER(_sync_PrivateA) ;
      NEWPERFCOUNTER(_sync_PrivateB) ;
      NEWPERFCOUNTER(_sync_MonInCirculation) ;
      NEWPERFCOUNTER(_sync_MonScavenged) ;
      NEWPERFVARIABLE(_sync_MonExtant) ;
      #undef NEWPERFCOUNTER
  }
}


// Compile-time asserts
// When possible, it's better to catch errors deterministically at
// compile-time than at runtime.  The down-side to using compile-time
// asserts is that error message -- often something about negative array
// indices -- is opaque.

#define CTASSERT(x) { int tag[1-(2*!(x))]; printf ("Tag @" INTPTR_FORMAT "\n", (intptr_t)tag); }

void ObjectMonitor::ctAsserts() {
  CTASSERT(offset_of (ObjectMonitor, _header) == 0);
}


static char * kvGet (char * kvList, const char * Key) {
    if (kvList == NULL) return NULL ;
    size_t n = strlen (Key) ;
    char * Search ;
    for (Search = kvList ; *Search ; Search += strlen(Search) + 1) {
        if (strncmp (Search, Key, n) == 0) {
            if (Search[n] == '=') return Search + n + 1 ;
            if (Search[n] == 0)   return (char *) "1" ;
        }
    }
    return NULL ;
}

static int kvGetInt (char * kvList, const char * Key, int Default) {
    char * v = kvGet (kvList, Key) ;
    int rslt = v ? ::strtol (v, NULL, 0) : Default ;
    if (Knob_ReportSettings && v != NULL) {
        ::printf ("  SyncKnob: %s %d(%d)\n", Key, rslt, Default) ;
        ::fflush (stdout) ;
    }
    return rslt ;
}

void ObjectMonitor::DeferredInitialize () {
  // 初始化完成时会将InitDone置为1，即只初始化第一次即可
  if (InitDone > 0) return ;
  if (Atomic::cmpxchg (-1, &InitDone, 0) != 0) {
      // 将其原子的修改为-1，如果修改失败说明有一个线程已经完成了修改
      // 自旋等待该线程完成初始化
      while (InitDone != 1) ;
      return ;
  }

  // One-shot global initialization ...
  // The initialization is idempotent, so we don't need locks.
  // In the future consider doing this via os::init_2().
  // SyncKnobs consist of <Key>=<Value> pairs in the style
  // of environment variables.  Start by converting ':' to NUL.
  // SyncKnobs是一个配置项，用来配置跟自旋等待相关的属性
  if (SyncKnobs == NULL) SyncKnobs = "" ;
  // 获取其字符长度
  size_t sz = strlen (SyncKnobs) ;
  // 分配一个字符数组
  char * knobs = (char *) malloc (sz + 2) ;
  if (knobs == NULL) {
     // 分配失败抛出异常
     vm_exit_out_of_memory (sz + 2, OOM_MALLOC_ERROR, "Parse SyncKnobs") ;
     guarantee (0, "invariant") ;
  }
  // 复制到knobs
  strcpy (knobs, SyncKnobs) ;
  // 加1的字符置为0，表示字符串结束
  knobs[sz+1] = 0 ;
  for (char * p = knobs ; *p ; p++) {
     if (*p == ':') *p = 0 ;
  }

  // 初始化各项配置，kvGetInt负责查找配置项的值
  #define SETKNOB(x) { Knob_##x = kvGetInt (knobs, #x, Knob_##x); }
  SETKNOB(ReportSettings) ;
  SETKNOB(Verbose) ;
  SETKNOB(FixedSpin) ;
  SETKNOB(SpinLimit) ;
  SETKNOB(SpinBase) ;
  SETKNOB(SpinBackOff);
  SETKNOB(CASPenalty) ;
  SETKNOB(OXPenalty) ;
  SETKNOB(LogSpins) ;
  SETKNOB(SpinSetSucc) ;
  SETKNOB(SuccEnabled) ;
  SETKNOB(SuccRestrict) ;
  SETKNOB(Penalty) ;
  SETKNOB(Bonus) ;
  SETKNOB(BonusB) ;
  SETKNOB(Poverty) ;
  SETKNOB(SpinAfterFutile) ;
  SETKNOB(UsePause) ;
  SETKNOB(SpinEarly) ;
  SETKNOB(OState) ;
  SETKNOB(MaxSpinners) ;
  SETKNOB(PreSpin) ;
  SETKNOB(ExitPolicy) ;
  SETKNOB(QMode);
  SETKNOB(ResetEvent) ;
  SETKNOB(MoveNotifyee) ;
  SETKNOB(FastHSSEC) ;
  #undef SETKNOB

  if (os::is_MP()) {
     BackOffMask = (1 << Knob_SpinBackOff) - 1 ;
     if (Knob_ReportSettings) ::printf ("BackOffMask=%X\n", BackOffMask) ;
     // CONSIDER: BackOffMask = ROUNDUP_NEXT_POWER2 (ncpus-1)
  } else {
     Knob_SpinLimit = 0 ;
     Knob_SpinBase  = 0 ;
     Knob_PreSpin   = 0 ;
     Knob_FixedSpin = -1 ;
  }

  if (Knob_LogSpins == 0) {
     ObjectMonitor::_sync_FailedSpins = NULL ;
  }

  // 释放knobs的内存
  free (knobs) ;
  // 让修改立即生效
  OrderAccess::fence() ;
  // 标识初始化完成
  InitDone = 1 ;
}

#ifndef PRODUCT
void ObjectMonitor::verify() {
}

void ObjectMonitor::print() {
}
#endif
