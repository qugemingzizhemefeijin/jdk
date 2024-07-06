/*
 * Copyright (c) 1997, 2013, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "code/pcDesc.hpp"
#include "code/scopeDesc.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/compilationPolicy.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/signature.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/sweeper.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/thread.inline.hpp"
#include "services/memTracker.hpp"
#include "services/runtimeService.hpp"
#include "utilities/events.hpp"
#include "utilities/macros.hpp"
#ifdef TARGET_ARCH_x86
# include "nativeInst_x86.hpp"
# include "vmreg_x86.inline.hpp"
#endif
#ifdef TARGET_ARCH_sparc
# include "nativeInst_sparc.hpp"
# include "vmreg_sparc.inline.hpp"
#endif
#ifdef TARGET_ARCH_zero
# include "nativeInst_zero.hpp"
# include "vmreg_zero.inline.hpp"
#endif
#ifdef TARGET_ARCH_arm
# include "nativeInst_arm.hpp"
# include "vmreg_arm.inline.hpp"
#endif
#ifdef TARGET_ARCH_ppc
# include "nativeInst_ppc.hpp"
# include "vmreg_ppc.inline.hpp"
#endif
#if INCLUDE_ALL_GCS
#include "gc_implementation/concurrentMarkSweep/concurrentMarkSweepThread.hpp"
#include "gc_implementation/shared/concurrentGCThread.hpp"
#endif // INCLUDE_ALL_GCS
#ifdef COMPILER1
#include "c1/c1_globals.hpp"
#endif

// --------------------------------------------------------------------------------------------------
// Implementation of Safepoint begin/end

SafepointSynchronize::SynchronizeState volatile SafepointSynchronize::_state = SafepointSynchronize::_not_synchronized;
volatile int  SafepointSynchronize::_waiting_to_block = 0;
volatile int SafepointSynchronize::_safepoint_counter = 0;
int SafepointSynchronize::_current_jni_active_count = 0;
long  SafepointSynchronize::_end_of_last_safepoint = 0;
static volatile int PageArmed = 0 ;        // safepoint polling page is RO|RW vs PROT_NONE
static volatile int TryingToBlock = 0 ;    // proximate value -- for advisory use only
static bool timeout_error_printed = false;

// Roll all threads forward to a safepoint and suspend them all
// 将所有线程前滚到安全点并挂起所有线程（只有VMThread才能调用当前的函数）
// 开启安全点的方法，会通知所有JavaThread安全点开启了，然后通过循环等待的方式等待所有的JavaThread都变成阻塞状态，安全点同步完成，然后就可以执行必须在安全点下执行的任务了。
void SafepointSynchronize::begin() {

  Thread* myThread = Thread::current();
  // 校验当前线程是VMThread
  assert(myThread->is_VM_thread(), "Only VM thread may execute a safepoint");

  if (PrintSafepointStatistics || PrintSafepointStatisticsTimeout > 0) {
    _safepoint_begin_time = os::javaTimeNanos();
    _ts_of_current_safepoint = tty->time_stamp().seconds();
  }

#if INCLUDE_ALL_GCS
  if (UseConcMarkSweepGC) {
    // In the future we should investigate whether CMS can use the
    // more-general mechanism below.  DLD (01/05).
    // 如果使用CMS算法，则获取CMS Token
    ConcurrentMarkSweepThread::synchronize(false);
  } else if (UseG1GC) {
    ConcurrentGCThread::safepoint_synchronize();
  }
#endif // INCLUDE_ALL_GCS

  // By getting the Threads_lock, we assure that no threads are about to start or
  // exit. It is released again in SafepointSynchronize::end().
  // 获取Threads_lock锁，这个锁直到调用退出安全点的函数SafepointSynchronize::end()时才会释放，因此相关的Mutator线程在获取此锁时阻塞
  Threads_lock->lock();
  // 校验当前安全点的状态为未同步
  assert( _state == _not_synchronized, "trying to safepoint synchronize with wrong state");

  // 获取所有的Mutator线程总数，这些线程在GC执行过程中必须要STW
  int nof_threads = Threads::number_of_threads();

  if (TraceSafepoint) {
    tty->print_cr("Safepoint synchronization initiated. (%d)", nof_threads);
  }
  // 通知RuntimeService 开始进入安全点
  RuntimeService::record_safepoint_begin();
  // 获取Safepoint_lock锁，线程状态流转时必须获取该锁
  MutexLocker mu(Safepoint_lock);

  // Reset the count of active JNI critical threads
  // 重置计数器，用来记录处于JNI关键区的线程数
  _current_jni_active_count = 0;

  // Set number of threads to wait for, before we initiate the callbacks
  // 需要等待暂停的线程数量
  _waiting_to_block = nof_threads;
  // 定义的静态volatile变量，用来记录尝试去阻塞的线程数
  TryingToBlock     = 0 ;
  int still_running = nof_threads;

  // Save the starting time, so that it can be compared to see if this has taken
  // too long to complete.
  jlong safepoint_limit_time;
  timeout_error_printed = false;

  // PrintSafepointStatisticsTimeout can be specified separately. When
  // specified, PrintSafepointStatistics will be set to true in
  // deferred_initialize_stat method. The initialization has to be done
  // early enough to avoid any races. See bug 6880029 for details.
  if (PrintSafepointStatistics || PrintSafepointStatisticsTimeout > 0) {
    // 如果需要打印日志，则初始化_safepoint_stats
    deferred_initialize_stat();
  }

  // Begin the process of bringing the system to a safepoint.
  // Java threads can be in several different states and are
  // stopped by different mechanisms:
  //
  //  1. Running interpreted
  //     The interpeter dispatch table is changed to force it to
  //     check for a safepoint condition between bytecodes.
  //  2. Running in native code
  //     When returning from the native code, a Java thread must check
  //     the safepoint _state to see if we must block.  If the
  //     VM thread sees a Java thread in native, it does
  //     not wait for this thread to block.  The order of the memory
  //     writes and reads of both the safepoint state and the Java
  //     threads state is critical.  In order to guarantee that the
  //     memory writes are serialized with respect to each other,
  //     the VM thread issues a memory barrier instruction
  //     (on MP systems).  In order to avoid the overhead of issuing
  //     a memory barrier for each Java thread making native calls, each Java
  //     thread performs a write to a single memory page after changing
  //     the thread state.  The VM thread performs a sequence of
  //     mprotect OS calls which forces all previous writes from all
  //     Java threads to be serialized.  This is done in the
  //     os::serialize_thread_states() call.  This has proven to be
  //     much more efficient than executing a membar instruction
  //     on every call to native code.
  //  3. Running compiled Code
  //     Compiled code reads a global (Safepoint Polling) page that
  //     is set to fault if we are trying to get to a safepoint.
  //  4. Blocked
  //     A thread which is blocked will not be allowed to return from the
  //     block condition until the safepoint operation is complete.
  //  5. In VM or Transitioning between states
  //     If a Java thread is currently running in the VM or transitioning
  //     between states, the safepointing code will wait for the thread to
  //     block itself when it attempts transitions to a new state.
  //

  // 开始将系统带到安全点的过程。Java 线程可以处于几种不同的状态，并由不同的机制停止：
  // 1. 运行解释
  //    更改了 interpeter 调度表，以强制它检查字节码之间的安全点条件。
  // 2. 在本机代码中运行
  //    从本机代码返回时，Java 线程必须检查安全点_state以查看我们是否必须阻止。 如果 VM 线程看到本机 Java 线程，则不会等待此线程阻塞。
  //    安全点状态和 Java 线程状态的内存写入和读取顺序至关重要。 为了保证内存写入相互序列化，VM 线程发出内存屏障指令（在 MP 系统上）。
  //    为了避免为每个进行本机调用的 Java 线程发出内存屏障的开销，每个 Java 线程在更改线程状态后都会对单个内存页执行写入操作。
  //    VM 线程执行一系列 mprotect OS 调用，强制序列化来自所有 Java 线程的所有先前写入。这是在 os::serialize_thread_states() 调用中完成的。
  //    事实证明，这比在每次调用本机代码时执行 membar 指令要高效得多。
  // 3. 运行已编译的代码
  //    编译的代码读取一个全局（安全点轮询）页面，如果我们试图到达安全点，该页面将设置为错误。
  // 4. 锁住
  //    在安全点操作完成之前，不允许被阻止的线程从阻止条件返回。
  // 5. 在 VM 中或在状态之间转换
  //    如果 Java 线程当前正在 VM 中运行或在状态之间转换，则安全点代码将等待线程在尝试转换到新状态时阻止自身。

  // 将状态设置为需要同步，这样除当前VMThread线程以外的其他正在运行的线程检测到这个状态时，都会在合适的点调用block()函数暂停
  _state            = _synchronizing;
  // 强制高速缓存刷新，在各CPU间同步数据
  OrderAccess::fence();

  // Flush all thread states to memory
  // 刷新所有线程的状态到内存中
  if (!UseMembar) {
    os::serialize_thread_states();
  }

  // Make interpreter safepoint aware
  // 通知解析执行的线程进入安全点
  Interpreter::notice_safepoints();

  // 让编译执行的线程进入安全点
  // 两个参数 UseCompilerSafepoints 和 DeferPollingPageLoopCount在默认情况下的值分别为true和-1
  // 注：当线程正在执行已经被编译成本地代码的代码时，会在一些位置读取Safepoint——Polling内存页，
  //    VMThread在进入安全点的时候会将这个内存页设置为不可读。 这样， 当线程试图去读这个内存页时就会产生错误信号。
  //    在Linux中，错误信号处理器将会处理这个信号。hotspot\src\os_cpu\linux_x86\vm\os_linux_x86.cpp
  //    JVM_handle_linux_signal 函数
  if (UseCompilerSafepoints && DeferPollingPageLoopCount < 0) {
    // Make polling safepoint aware
    // PageArmed是一个静态变量，表示polling_page的状态
    guarantee (PageArmed == 0, "invariant") ;
    PageArmed = 1 ;
    os::make_polling_page_unreadable();
  }

  // Consider using active_processor_count() ... but that call is expensive.
  // 获取当前系统的CPU个数
  int ncpus = os::processor_count() ;

#ifdef ASSERT
  for (JavaThread *cur = Threads::first(); cur != NULL; cur = cur->next()) {
    assert(cur->safepoint_state()->is_running(), "Illegal initial state");
    // Clear the visited flag to ensure that the critical counts are collected properly.
    cur->set_visited_for_critical_count(false);
  }
#endif // ASSERT
  // SafepointTimeout为false，如果为true则表示如果等待进入安全点的总耗时超过SafepointTimeoutDelay就打印失败日志
  // SafepointTimeoutDelay的默认值是10000ms
  if (SafepointTimeout)
    safepoint_limit_time = os::javaTimeNanos() + (jlong)SafepointTimeoutDelay * MICROUNITS; // 计算进入安全点的最迟时间

  // Iterate through all threads until it have been determined how to stop them all at a safepoint
  unsigned int iterations = 0;
  int steps = 0 ;
  // 循环判断still_running
  while(still_running > 0) {
    // 循环检查每个线程的_safepoint_state属性，属性的类型为ThreadSafepointState*
    for (JavaThread *cur = Threads::first(); cur != NULL; cur = cur->next()) {
      assert(!cur->is_ConcurrentGC_thread(), "A concurrent GC thread is unexpectly being suspended");
      ThreadSafepointState *cur_state = cur->safepoint_state();
      if (cur_state->is_running()) {
        // 根据当前线程的状态检查并修改ThreadSafepointState
        cur_state->examine_state_of_thread();
        if (!cur_state->is_running()) {
           // 如果已经不是running，将计数减一
           still_running--;
           // consider adjusting steps downward:
           //   steps = 0
           //   steps -= NNN
           //   steps >>= 1
           //   steps = MIN(steps, 2000-100)
           //   if (iterations != 0) steps -= NNN
        }
        if (TraceSafepoint && Verbose) cur_state->print();
      }
    }

    if (PrintSafepointStatistics && iterations == 0) {
      begin_statistics(nof_threads, still_running);
    }

    if (still_running > 0) {
      // Check for if it takes to long
      // 所有线程遍历完了，依然有处于running状态的线程
      if (SafepointTimeout && safepoint_limit_time < os::javaTimeNanos()) {
        // 等待超时了
        print_safepoint_timeout(_spinning_timeout);
      }

      // Spin to avoid context switching.
      // There's a tension between allowing the mutators to run (and rendezvous)
      // vs spinning.  As the VM thread spins, wasting cycles, it consumes CPU that
      // a mutator might otherwise use profitably to reach a safepoint.  Excessive
      // spinning by the VM thread on a saturated system can increase rendezvous latency.
      // Blocking or yielding incur their own penalties in the form of context switching
      // and the resultant loss of $ residency.
      //
      // Further complicating matters is that yield() does not work as naively expected
      // on many platforms -- yield() does not guarantee that any other ready threads
      // will run.   As such we revert yield_all() after some number of iterations.
      // Yield_all() is implemented as a short unconditional sleep on some platforms.
      // Typical operating systems round a "short" sleep period up to 10 msecs, so sleeping
      // can actually increase the time it takes the VM thread to detect that a system-wide
      // stop-the-world safepoint has been reached.  In a pathological scenario such as that
      // described in CR6415670 the VMthread may sleep just before the mutator(s) become safe.
      // In that case the mutators will be stalled waiting for the safepoint to complete and the
      // the VMthread will be sleeping, waiting for the mutators to rendezvous.  The VMthread
      // will eventually wake up and detect that all mutators are safe, at which point
      // we'll again make progress.
      //
      // Beware too that that the VMThread typically runs at elevated priority.
      // Its default priority is higher than the default mutator priority.
      // Obviously, this complicates spinning.
      //
      // Note too that on Windows XP SwitchThreadTo() has quite different behavior than Sleep(0).
      // Sleep(0) will _not yield to lower priority threads, while SwitchThreadTo() will.
      //
      // See the comments in synchronizer.cpp for additional remarks on spinning.
      //
      // In the future we might:
      // 1. Modify the safepoint scheme to avoid potentally unbounded spinning.
      //    This is tricky as the path used by a thread exiting the JVM (say on
      //    on JNI call-out) simply stores into its state field.  The burden
      //    is placed on the VM thread, which must poll (spin).
      // 2. Find something useful to do while spinning.  If the safepoint is GC-related
      //    we might aggressively scan the stacks of threads that are already safe.
      // 3. Use Solaris schedctl to examine the state of the still-running mutators.
      //    If all the mutators are ONPROC there's no reason to sleep or yield.
      // 4. YieldTo() any still-running mutators that are ready but OFFPROC.
      // 5. Check system saturation.  If the system is not fully saturated then
      //    simply spin and avoid sleep/yield.
      // 6. As still-running mutators rendezvous they could unpark the sleeping
      //    VMthread.  This works well for still-running mutators that become
      //    safe.  The VMthread must still poll for mutators that call-out.
      // 7. Drive the policy on time-since-begin instead of iterations.
      // 8. Consider making the spin duration a function of the # of CPUs:
      //    Spin = (((ncpus-1) * M) + K) + F(still_running)
      //    Alternately, instead of counting iterations of the outer loop
      //    we could count the # of threads visited in the inner loop, above.
      // 9. On windows consider using the return value from SwitchThreadTo()
      //    to drive subsequent spin/SwitchThreadTo()/Sleep(N) decisions.

      if (UseCompilerSafepoints && int(iterations) == DeferPollingPageLoopCount) {
         // DeferPollingPageLoopCount默认值是-1，不会走此逻辑
         // 如果大于0，则表示当循环次数等于该值时，才会通知编译代码进入安全点
         guarantee (PageArmed == 0, "invariant") ;
         PageArmed = 1 ;
         os::make_polling_page_unreadable();
      }

      // Instead of (ncpus > 1) consider either (still_running < (ncpus + EPSILON)) or
      // ((still_running + _waiting_to_block - TryingToBlock)) < ncpus)
      // 以下实现是为了避免线程上下文切换，也为了尽量让其他线程有机会“走到”安全点处暂停自己
      ++steps ;
      if (ncpus > 1 && steps < SafepointSpinBeforeYield) { // SafepointSpinBeforeYield的默认值是2000
        SpinPause() ;     // MP-Polite spin   // 利用系统调度，做短暂的自旋
      } else
      if (steps < DeferThrSuspendLoopCount) { // DeferThrSuspendLoopCount的默认值是4000
        os::NakedYield() ; // 让出当前VMThread的CPU执行权
      } else {
        os::yield_all(steps) ; // linux下的实现和NakedYield一样，都是调用底层的sched_yield
        // Alternately, the VM thread could transiently depress its scheduling priority or
        // transiently increase the priority of the tardy mutator(s).
      }

      iterations ++ ;
    }
    assert(iterations < (uint)max_jint, "We have been iterating in the safepoint loop too long");
  }
  // 所有线程的状态都变成非running了
  assert(still_running == 0, "sanity check");

  if (PrintSafepointStatistics) {
    update_statistics_on_spin_end();
  }

  // wait until all threads are stopped
  // 循环判断_waiting_to_block，等待直到所有的线程停止
  while (_waiting_to_block > 0) {
    if (TraceSafepoint) tty->print_cr("Waiting for %d thread(s) to block", _waiting_to_block);
    if (!SafepointTimeout || timeout_error_printed) {
      // 如果SafepointTimeout为false，直接在Safepoint_lock上等待
      Safepoint_lock->wait(true);  // true, means with no safepoint checks
    } else {
      // Compute remaining time
      // SafepointTimeout为true，计算剩余的时间
      jlong remaining_time = safepoint_limit_time - os::javaTimeNanos();

      // If there is no remaining time, then there is an error
      if (remaining_time < 0 || Safepoint_lock->wait(true, remaining_time / MICROUNITS)) {
        // 如果超时了，打印日志
        print_safepoint_timeout(_blocking_timeout);
      }
    }
  }
  // 校验所有JavaThread都被阻塞了
  assert(_waiting_to_block == 0, "sanity check");

#ifndef PRODUCT
  if (SafepointTimeout) {
    jlong current_time = os::javaTimeNanos();
    if (safepoint_limit_time < current_time) {
      tty->print_cr("# SafepointSynchronize: Finished after "
                    INT64_FORMAT_W(6) " ms",
                    ((current_time - safepoint_limit_time) / MICROUNITS +
                     SafepointTimeoutDelay));
    }
  }
#endif
  // 校验_safepoint_counter必须是偶数
  assert((_safepoint_counter & 0x1) == 0, "must be even");
  // 校验获取了Threads_lock锁
  assert(Threads_lock->owned_by_self(), "must hold Threads_lock");
  _safepoint_counter ++;

  // Record state
  // 逻辑执行到这里时，除VMThread线程外的所有线程已经暂停，因此将状态设置为_synchronized
  _state = _synchronized;

  // 以上代码让除了执行当前函数的VMThread线程之外的所有线程都在到达安全点时暂停。
  // 例如，调用Interpreter::notice_safepoints()函数让解析执行Java方法的线程进入安全点，
  // 调用os::make_polling_page_unreadable()函数让内存页不可读，让编译执行Java方法的线程进入安全点等。

  // 为了确保所有的线程已进入安全点，begin()函数必须保证still_running和_waiting_to_block变量的值为0。

  // begin()函数还通过两个重要的锁Threads_lock和Safepoint_lock来保证线程的同步过程，由于在开始时已经获取了Threads_lock锁，
  // 因此其他相关的线程如果需要“走到”安全点处暂停，其实就是在获取此锁时进行阻塞暂停；
  // Safepoint_lock主要用来保证多线程操作变量时的线程安全。

  OrderAccess::fence(); // 刷新高速缓存

#ifdef ASSERT
  for (JavaThread *cur = Threads::first(); cur != NULL; cur = cur->next()) {
    // make sure all the threads were visited
    assert(cur->was_visited_for_critical_count(), "missed a thread");
  }
#endif // ASSERT

  // Update the count of active JNI critical regions
  // 更新处于JNI关键区中的线程数
  GC_locker::set_jni_lock_count(_current_jni_active_count);

  if (TraceSafepoint) {
    VM_Operation *op = VMThread::vm_operation();
    tty->print_cr("Entering safepoint region: %s", (op != NULL) ? op->name() : "no vm operation");
  }
  // 通知RuntimeService，安全点同步完成
  RuntimeService::record_safepoint_synchronized();
  if (PrintSafepointStatistics) {
    // 更新统计数据SafepointStats
    update_statistics_on_sync_end(os::javaTimeNanos());
  }

  // Call stuff that needs to be run when a safepoint is just about to be completed
  // 执行清理
  do_cleanup_tasks();

  if (PrintSafepointStatistics) {
    // Record how much time spend on the above cleanup tasks
    // 更新统计数据SafepointStats
    update_statistics_on_cleanup_end(os::javaTimeNanos());
  }
}

// Wake up all threads, so they are ready to resume execution after the safepoint
// operation has been carried out
// end方法用于实现安全点退出，将所有JavaThread的ThreadSafepointState重置成初始状态，
// 然后释放锁Threads_lock，所有因为安全点而等待获取Threads_lock锁的线程就会逐一恢复正常执行。
void SafepointSynchronize::end() {
  // 校验当前线程占有锁Threads_lock
  assert(Threads_lock->owned_by_self(), "must hold Threads_lock");
  // 校验_safepoint_counter为奇数，贼begin方法和end方法中都会加1
  assert((_safepoint_counter & 0x1) == 1, "must be odd");
  _safepoint_counter ++;
  // memory fence isn't required here since an odd _safepoint_counter
  // value can do no harm and a fence is issued below anyway.

  DEBUG_ONLY(Thread* myThread = Thread::current();)
  // 校验当前线程是VMThread
  assert(myThread->is_VM_thread(), "Only VM thread can execute a safepoint");

  if (PrintSafepointStatistics) {
    end_statistics(os::javaTimeNanos());
  }

#ifdef ASSERT
  // A pending_exception cannot be installed during a safepoint.  The threads
  // may install an async exception after they come back from a safepoint into
  // pending_exception after they unblock.  But that should happen later.
  for(JavaThread *cur = Threads::first(); cur; cur = cur->next()) {
    assert (!(cur->has_pending_exception() &&
              cur->safepoint_state()->is_at_poll_safepoint()),
            "safepoint installed a pending exception");
  }
#endif // ASSERT

  // 让轮询页可读，清除安全点标志，否则编译执行的线程会再次进入安全点
  if (PageArmed) {
    // Make polling safepoint aware
    // 让polling_page变得可读，表示安全点结束
    os::make_polling_page_readable();
    PageArmed = 0 ;
  }

  // Remove safepoint check from interpreter
  // 清除安全点标志，否则解析执行的线程仍然会进入安全点
  Interpreter::ignore_safepoints();

  {
    MutexLocker mu(Safepoint_lock);
    // 断言当前是已同步状态
    assert(_state == _synchronized, "must be synchronized before ending safepoint synchronization");

    // Set to not synchronized, so the threads will not go into the signal_thread_blocked method
    // when they get restarted.
    // 更新_state为不需要进入安全点状态
    _state = _not_synchronized;
    OrderAccess::fence();

    if (TraceSafepoint) {
       tty->print_cr("Leaving safepoint region");
    }

    // Start suspended threads
    // 遍历所有的线程，重置其ThreadSafepointState
    for(JavaThread *current = Threads::first(); current; current = current->next()) {
      // A problem occurring on Solaris is when attempting to restart threads
      // the first #cpus - 1 go well, but then the VMThread is preempted when we get
      // to the next one (since it has been running the longest).  We then have
      // to wait for a cpu to become available before we can continue restarting
      // threads.
      // FIXME: This causes the performance of the VM to degrade when active and with
      // large numbers of threads.  Apparently this is due to the synchronous nature
      // of suspending threads.
      //
      // TODO-FIXME: the comments above are vestigial and no longer apply.
      // Furthermore, using solaris' schedctl in this particular context confers no benefit
      // VMThreadHintNoPreempt默认为false
      if (VMThreadHintNoPreempt) {
        os::hint_no_preempt();
      }
      ThreadSafepointState* cur_state = current->safepoint_state();
      assert(cur_state->type() != ThreadSafepointState::_running, "Thread not suspended at safepoint");
      // 设置ThreadSafepointState::_type属性的值为_running
      cur_state->restart();
      assert(cur_state->is_running(), "safepoint state has not been reset");
    }
    // 通知RuntimeService 安全点退出
    RuntimeService::record_safepoint_end();

    // Release threads lock, so threads can be created/destroyed again. It will also starts all threads
    // blocked in signal_thread_blocked
    // 释放锁的同时会唤醒所有阻塞在Threads_lock锁上的线程
    Threads_lock->unlock();

  }
#if INCLUDE_ALL_GCS
  // If there are any concurrent GC threads resume them.
  if (UseConcMarkSweepGC) {
    // 释放CMS Token,从而允许CMS Thread正常执行
    ConcurrentMarkSweepThread::desynchronize(false);
  } else if (UseG1GC) {
    ConcurrentGCThread::safepoint_desynchronize();
  }
#endif // INCLUDE_ALL_GCS
  // record this time so VMThread can keep track how much time has elasped
  // since last safepoint.
  // 记录上一次安全点结束的时间
  _end_of_last_safepoint = os::javaTimeMillis();
}

bool SafepointSynchronize::is_cleanup_needed() {
  // Need a safepoint if some inline cache buffers is non-empty
  if (!InlineCacheBuffer::is_empty()) return true;
  return false;
}



// Various cleaning tasks that should be done periodically at safepoints
void SafepointSynchronize::do_cleanup_tasks() {
  {
    TraceTime t1("deflating idle monitors", TraceSafepointCleanupTime);
    ObjectSynchronizer::deflate_idle_monitors();
  }

  {
    TraceTime t2("updating inline caches", TraceSafepointCleanupTime);
    InlineCacheBuffer::update_inline_caches();
  }
  {
    TraceTime t3("compilation policy safepoint handler", TraceSafepointCleanupTime);
    CompilationPolicy::policy()->do_safepoint_work();
  }

  {
    TraceTime t4("mark nmethods", TraceSafepointCleanupTime);
    NMethodSweeper::mark_active_nmethods();
  }

  if (SymbolTable::needs_rehashing()) {
    TraceTime t5("rehashing symbol table", TraceSafepointCleanupTime);
    SymbolTable::rehash_table();
  }

  if (StringTable::needs_rehashing()) {
    TraceTime t6("rehashing string table", TraceSafepointCleanupTime);
    StringTable::rehash_table();
  }

  // rotate log files?
  if (UseGCLogFileRotation) {
    gclog_or_tty->rotate_log();
  }

  if (MemTracker::is_on()) {
    MemTracker::sync();
  }
}


bool SafepointSynchronize::safepoint_safe(JavaThread *thread, JavaThreadState state) {
  switch(state) {
  case _thread_in_native:
    // native threads are safe if they have no java stack or have walkable stack
    // 上一个栈帧不是Java，或者当前栈帧是walkable
    return !thread->has_last_Java_frame() || thread->frame_anchor()->walkable();

   // blocked threads should have already have walkable stack
  case _thread_blocked:
    assert(!thread->has_last_Java_frame() || thread->frame_anchor()->walkable(), "blocked and not walkable");
    return true;

  default:
    return false;
  }
}


// See if the thread is running inside a lazy critical native and
// update the thread critical count if so.  Also set a suspend flag to
// cause the native wrapper to return into the JVM to do the unlock
// once the native finishes.
void SafepointSynchronize::check_for_lazy_critical_native(JavaThread *thread, JavaThreadState state) {
  if (state == _thread_in_native &&
      thread->has_last_Java_frame() &&
      thread->frame_anchor()->walkable()) {
    // 满足条件的线程可能是处于JNI关键区中且是编译代码
    // This thread might be in a critical native nmethod so look at
    // the top of the stack and increment the critical count if it
    // is.
    frame wrapper_frame = thread->last_frame();
    CodeBlob* stub_cb = wrapper_frame.cb();
    if (stub_cb != NULL &&
        stub_cb->is_nmethod() &&
        stub_cb->as_nmethod_or_null()->is_lazy_critical_native()) {
      // A thread could potentially be in a critical native across
      // more than one safepoint, so only update the critical state on
      // the first one.  When it returns it will perform the unlock.
      if (!thread->do_critical_native_unlock()) {
#ifdef ASSERT
        if (!thread->in_critical()) {
          GC_locker::increment_debug_jni_lock_count();
        }
#endif
        // 表示进入了安全区
        thread->enter_critical();
        // Make sure the native wrapper calls back on return to
        // perform the needed critical unlock.
        thread->set_critical_native_unlock();
      }
    }
  }
}



// -------------------------------------------------------------------------------------------------------
// Implementation of Safepoint callback point

// 该方法用于阻塞当前线程直到VMThead从安全点退出，通常与do_call_back方法同时使用，该方法用于判断是否需要调用block方法。

// block方法会将当前线程的状态改成_thread_blocked，并且变成walkable，从而满足SafepointSynchronize::safepoint_safe中的条件，
// examine_state_of_thread再次检查该线程的运行状态时就会将该线程的ThreadSafepointState置为_at_safepoint，然后执行roll_forward的时候会将_waiting_to_block减一；
// 然后block方法将该线程的状态置为_thread_blocked，在Threads_lock上阻塞等待VMThread从安全点退出，释放Threads_lock。
// 需要注意，调用block方法时，如果线程的状态是_thread_in_vm_trans或者_thread_in_Java，则是走另一种逻辑，会先将线程修改成_thread_in_vm，
// 然后examine_state_of_thread执行时会将当前线程的ThreadSafepointState置为_call_back，即非running的状态，然后等待Safepoint_lock；
// 等所有的线程都变成非running状态了，Safepoint_lock锁被释放了，就会减少_waiting_to_block，将线程状态置为_thread_blocked，
// 然后在Threads_lock上阻塞等待VMThread从安全点退出，释放Threads_lock。
void SafepointSynchronize::block(JavaThread *thread) {
  assert(thread != NULL, "thread must be set");
  assert(thread->is_Java_thread(), "not a Java thread");

  // Threads shouldn't block if they are in the middle of printing, but...
  // 停止打印
  ttyLocker::break_tty_lock_for_safepoint(os::current_thread_id());

  // Only bail from the block() call if the thread is gone from the
  // thread list; starting to exit should still block.
  // 如果线程正在退出中
  if (thread->is_terminated()) {
     // block current thread if we come here from native code when VM is gone
     // 如果是因为JVM退出而终止则阻塞当前线程直到JVM进程终止
     thread->block_if_vm_exited();

     // otherwise do nothing
     return;
  }
  // 获取当前线程的状态
  JavaThreadState state = thread->thread_state();
  thread->frame_anchor()->make_walkable(thread);

  // Check that we have a valid thread_state at this point
  switch(state) {
    // 如果正在进行解释执行的线程，在ThreadInVMfromJava构造函数中会传递_thread_in_vm状态，
    // 然后在transition()函数中将状态更新为_thread_in_vm_trans
    case _thread_in_vm_trans:
    // 如果正在进行编译执行的线程，则线程的状态是_thread_in_Java
    case _thread_in_Java:        // From compiled code

      // We are highly likely to block on the Safepoint_lock. In order to avoid blocking in this case,
      // we pretend we are still in the VM.
      // 临时修改线程状态为_thread_in_vm
      thread->set_thread_state(_thread_in_vm);

      if (is_synchronizing()) {
         // 如果正在同步中，增加TryingToBlock计数
         Atomic::inc (&TryingToBlock) ;
      }

      // We will always be holding the Safepoint_lock when we are examine the state
      // of a thread. Hence, the instructions between the Safepoint_lock->lock() and
      // Safepoint_lock->unlock() are happening atomic with regards to the safepoint code
      // 进入begin方法会获取Safepoint_lock锁，直到所有线程的ThreadSafepointState都变成非running了，才会释放该锁
      Safepoint_lock->lock_without_safepoint_check();
      // 线程正在进行同步，也就是配合GC进入安全点状态
      if (is_synchronizing()) {
        // Decrement the number of threads to wait for and signal vm thread
        assert(_waiting_to_block > 0, "sanity check");
        // 减少等待阻塞线程的数量，这样执行GC的线程如果检测到值为0时，就表示所有需要阻塞的线程都进入了block状态，可以开始执行GC了
        _waiting_to_block--;
        // has_called_back置为true，表示已经调用了block方法
        thread->safepoint_state()->set_has_called_back(true);

        DEBUG_ONLY(thread->set_visited_for_critical_count(true));
        if (thread->in_critical()) {
          // Notice that this thread is in a critical section
          // 当前线程在临界区执行
          increment_jni_active_count();
        }

        // Consider (_waiting_to_block < 2) to pipeline the wakeup of the VM thread
        // 当_waiting_to_block的值为0时，说明应该进入阻塞的线程都已经进入，调用notify_all()函数唤醒所有在Safepoint_lock锁上的线程，
        // 这些线程都是需要在安全点下执行的任务
        if (_waiting_to_block == 0) {
          // 唤醒在Safepoint_lock上等待的VMThread
          Safepoint_lock->notify_all();
        }
      }

      // We transition the thread to state _thread_blocked here, but
      // we can't do our usual check for external suspension and then
      // self-suspend after the lock_without_safepoint_check() call
      // below because we are often called during transitions while
      // we hold different locks. That would leave us suspended while
      // holding a resource which results in deadlocks.
      // 设置线程的状态为_thread_blocked
      thread->set_thread_state(_thread_blocked);
      // 释放Safepoint_lock锁
      Safepoint_lock->unlock();

      // We now try to acquire the threads lock. Since this lock is hold by the VM thread during
      // the entire safepoint, the threads will all line up here during the safepoint.
      // 当前线程将在获取Threads_lock锁时阻塞，因为这个锁会被执行安全点中的任务的线程所持有
      Threads_lock->lock_without_safepoint_check();
      // restore original state. This is important if the thread comes from compiled code, so it
      // will continue to execute with the _thread_in_Java state.
      // 已经从安全点退出，恢复原来的线程状态
      thread->set_thread_state(state);
      // 释放锁Threads_lock
      Threads_lock->unlock();
      break;

    case _thread_in_native_trans:
    case _thread_blocked_trans:
    case _thread_new_trans:
      // 在examine_state_of_thread方法中，只有_thread_in_vm状态的线程才会将其ThreadSafepointState置为_call_back
      // 如果是_thread_in_vm，则进入block方法前会将其设置为_thread_in_vm_trans，因此不会出现此种情形
      if (thread->safepoint_state()->type() == ThreadSafepointState::_call_back) {
        thread->print_thread_state();
        fatal("Deadlock in safepoint code.  "
              "Should have called back to the VM before blocking.");
      }

      // We transition the thread to state _thread_blocked here, but
      // we can't do our usual check for external suspension and then
      // self-suspend after the lock_without_safepoint_check() call
      // below because we are often called during transitions while
      // we hold different locks. That would leave us suspended while
      // holding a resource which results in deadlocks.
      // 我们在这里将线程转换为状态 _thread_blocked，但我们不能像往常一样检查外部挂起，然后在下面的 lock_without_safepoint_check()调用后自挂起，
      // 因为我们经常在过渡期间被调用，同时我们持有不同的锁。这将使我们在持有资源时被暂停，从而导致死锁。
      // 设置线程状态为_thread_blocked
      thread->set_thread_state(_thread_blocked);

      // It is not safe to suspend a thread if we discover it is in _thread_in_native_trans. Hence,
      // the safepoint code might still be waiting for it to block. We need to change the state here,
      // so it can see that it is at a safepoint.

      // Block until the safepoint operation is completed.
      // 让当前线程在Threads_lock上被阻塞，直到end方法被调用
      Threads_lock->lock_without_safepoint_check();

      // Restore state // 恢复成原来的状态
      thread->set_thread_state(state);
      // 解锁Threads_lock
      Threads_lock->unlock();
      break;

    default:
     fatal(err_msg("Illegal threadstate encountered: %d", state));
  }

  // Check for pending. async. exceptions or suspends - except if the
  // thread was blocked inside the VM. has_special_runtime_exit_condition()
  // is called last since it grabs a lock and we only want to do that when
  // we must.
  //
  // Note: we never deliver an async exception at a polling point as the
  // compiler may not have an exception handler for it. The polling
  // code will notice the async and deoptimize and the exception will
  // be delivered. (Polling at a return point is ok though). Sure is
  // a lot of bother for a deprecated feature...
  //
  // We don't deliver an async exception if the thread state is
  // _thread_in_native_trans so JNI functions won't be called with
  // a surprising pending exception. If the thread state is going back to java,
  // async exception is checked in check_special_condition_for_native_trans().
  // 已经从安全点退出了
  if (state != _thread_blocked_trans &&
      state != _thread_in_vm_trans &&
      thread->has_special_runtime_exit_condition()) {
    // 如果有异常或者被要求挂起
    thread->handle_special_runtime_exit_condition(
      !thread->is_at_poll_safepoint() && (state != _thread_in_native_trans));
  }
}

// ------------------------------------------------------------------------------------------------------
// Exception handlers

#ifndef PRODUCT

#ifdef SPARC

#ifdef _LP64
#define PTR_PAD ""
#else
#define PTR_PAD "        "
#endif

static void print_ptrs(intptr_t oldptr, intptr_t newptr, bool wasoop) {
  bool is_oop = newptr ? (cast_to_oop(newptr))->is_oop() : false;
  tty->print_cr(PTR_FORMAT PTR_PAD " %s %c " PTR_FORMAT PTR_PAD " %s %s",
                oldptr, wasoop?"oop":"   ", oldptr == newptr ? ' ' : '!',
                newptr, is_oop?"oop":"   ", (wasoop && !is_oop) ? "STALE" : ((wasoop==false&&is_oop==false&&oldptr !=newptr)?"STOMP":"     "));
}

static void print_longs(jlong oldptr, jlong newptr, bool wasoop) {
  bool is_oop = newptr ? (cast_to_oop(newptr))->is_oop() : false;
  tty->print_cr(PTR64_FORMAT " %s %c " PTR64_FORMAT " %s %s",
                oldptr, wasoop?"oop":"   ", oldptr == newptr ? ' ' : '!',
                newptr, is_oop?"oop":"   ", (wasoop && !is_oop) ? "STALE" : ((wasoop==false&&is_oop==false&&oldptr !=newptr)?"STOMP":"     "));
}

static void print_me(intptr_t *new_sp, intptr_t *old_sp, bool *was_oops) {
#ifdef _LP64
  tty->print_cr("--------+------address-----+------before-----------+-------after----------+");
  const int incr = 1;           // Increment to skip a long, in units of intptr_t
#else
  tty->print_cr("--------+--address-+------before-----------+-------after----------+");
  const int incr = 2;           // Increment to skip a long, in units of intptr_t
#endif
  tty->print_cr("---SP---|");
  for( int i=0; i<16; i++ ) {
    tty->print("blob %c%d |"PTR_FORMAT" ","LO"[i>>3],i&7,new_sp); print_ptrs(*old_sp++,*new_sp++,*was_oops++); }
  tty->print_cr("--------|");
  for( int i1=0; i1<frame::memory_parameter_word_sp_offset-16; i1++ ) {
    tty->print("argv pad|"PTR_FORMAT" ",new_sp); print_ptrs(*old_sp++,*new_sp++,*was_oops++); }
  tty->print("     pad|"PTR_FORMAT" ",new_sp); print_ptrs(*old_sp++,*new_sp++,*was_oops++);
  tty->print_cr("--------|");
  tty->print(" G1     |"PTR_FORMAT" ",new_sp); print_longs(*(jlong*)old_sp,*(jlong*)new_sp,was_oops[incr-1]); old_sp += incr; new_sp += incr; was_oops += incr;
  tty->print(" G3     |"PTR_FORMAT" ",new_sp); print_longs(*(jlong*)old_sp,*(jlong*)new_sp,was_oops[incr-1]); old_sp += incr; new_sp += incr; was_oops += incr;
  tty->print(" G4     |"PTR_FORMAT" ",new_sp); print_longs(*(jlong*)old_sp,*(jlong*)new_sp,was_oops[incr-1]); old_sp += incr; new_sp += incr; was_oops += incr;
  tty->print(" G5     |"PTR_FORMAT" ",new_sp); print_longs(*(jlong*)old_sp,*(jlong*)new_sp,was_oops[incr-1]); old_sp += incr; new_sp += incr; was_oops += incr;
  tty->print_cr(" FSR    |"PTR_FORMAT" "PTR64_FORMAT"       "PTR64_FORMAT,new_sp,*(jlong*)old_sp,*(jlong*)new_sp);
  old_sp += incr; new_sp += incr; was_oops += incr;
  // Skip the floats
  tty->print_cr("--Float-|"PTR_FORMAT,new_sp);
  tty->print_cr("---FP---|");
  old_sp += incr*32;  new_sp += incr*32;  was_oops += incr*32;
  for( int i2=0; i2<16; i2++ ) {
    tty->print("call %c%d |"PTR_FORMAT" ","LI"[i2>>3],i2&7,new_sp); print_ptrs(*old_sp++,*new_sp++,*was_oops++); }
  tty->print_cr("");
}
#endif  // SPARC
#endif  // PRODUCT


void SafepointSynchronize::handle_polling_page_exception(JavaThread *thread) {
  // 校验当前线程是Java线程
  assert(thread->is_Java_thread(), "polling reference encountered by VM thread");
  // 校验线程的状态
  assert(thread->thread_state() == _thread_in_Java, "should come from Java code");
  // 校验安全点的状态
  assert(SafepointSynchronize::is_synchronizing(), "polling encountered outside safepoint synchronization");

  // Uncomment this to get some serious before/after printing of the
  // Sparc safepoint-blob frame structure.
  /*
  intptr_t* sp = thread->last_Java_sp();
  intptr_t stack_copy[150];
  for( int i=0; i<150; i++ ) stack_copy[i] = sp[i];
  bool was_oops[150];
  for( int i=0; i<150; i++ )
    was_oops[i] = stack_copy[i] ? ((oop)stack_copy[i])->is_oop() : false;
  */

  if (ShowSafepointMsgs) {
    tty->print("handle_polling_page_exception: ");
  }

  if (PrintSafepointStatistics) {
    inc_page_trap_count();
  }
  // 获取该线程对应的ThreadSafepointState
  ThreadSafepointState* state = thread->safepoint_state();
  // 执行polling_page的异常处理
  state->handle_polling_page_exception();
  // print_me(sp,stack_copy,was_oops);
}


void SafepointSynchronize::print_safepoint_timeout(SafepointTimeoutReason reason) {
  if (!timeout_error_printed) {
    timeout_error_printed = true;
    // Print out the thread infor which didn't reach the safepoint for debugging
    // purposes (useful when there are lots of threads in the debugger).
    tty->print_cr("");
    tty->print_cr("# SafepointSynchronize::begin: Timeout detected:");
    if (reason ==  _spinning_timeout) {
      tty->print_cr("# SafepointSynchronize::begin: Timed out while spinning to reach a safepoint.");
    } else if (reason == _blocking_timeout) {
      tty->print_cr("# SafepointSynchronize::begin: Timed out while waiting for threads to stop.");
    }

    tty->print_cr("# SafepointSynchronize::begin: Threads which did not reach the safepoint:");
    ThreadSafepointState *cur_state;
    ResourceMark rm;
    for(JavaThread *cur_thread = Threads::first(); cur_thread;
        cur_thread = cur_thread->next()) {
      cur_state = cur_thread->safepoint_state();

      if (cur_thread->thread_state() != _thread_blocked &&
          ((reason == _spinning_timeout && cur_state->is_running()) ||
           (reason == _blocking_timeout && !cur_state->has_called_back()))) {
        tty->print("# ");
        cur_thread->print();
        tty->print_cr("");
      }
    }
    tty->print_cr("# SafepointSynchronize::begin: (End of list)");
  }

  // To debug the long safepoint, specify both DieOnSafepointTimeout &
  // ShowMessageBoxOnError.
  if (DieOnSafepointTimeout) {
    char msg[1024];
    VM_Operation *op = VMThread::vm_operation();
    sprintf(msg, "Safepoint sync time longer than " INTX_FORMAT "ms detected when executing %s.",
            SafepointTimeoutDelay,
            op != NULL ? op->name() : "no vm operation");
    fatal(msg);
  }
}


// -------------------------------------------------------------------------------------------------------
// Implementation of ThreadSafepointState

ThreadSafepointState::ThreadSafepointState(JavaThread *thread) {
  _thread = thread;
  // 初始状态就是_running
  _type   = _running;
  _has_called_back = false;
  _at_poll_safepoint = false;
}

void ThreadSafepointState::create(JavaThread *thread) {
  ThreadSafepointState *state = new ThreadSafepointState(thread);
  thread->set_safepoint_state(state);
}

void ThreadSafepointState::destroy(JavaThread *thread) {
  if (thread->safepoint_state()) {
    // 释放thead的ThreadSafepointState实例
    delete(thread->safepoint_state());
    thread->set_safepoint_state(NULL);
  }
}

// 根据当前线程的状态调整每个线程的ThreadSafepointState
// restart用于将每个线程ThreadSafepointState的状态恢复成初始的running，_has_called_back属性恢复成初始的false
void ThreadSafepointState::examine_state_of_thread() {
  // 校验当前状态时running
  assert(is_running(), "better be running or just have hit safepoint poll");

  // 保存线程原来的状态
  JavaThreadState state = _thread->thread_state();

  // Save the state at the start of safepoint processing.
  _orig_thread_state = state;

  // Check for a thread that is suspended. Note that thread resume tries
  // to grab the Threads_lock which we own here, so a thread cannot be
  // resumed during safepoint synchronization.

  // We check to see if this thread is suspended without locking to
  // avoid deadlocking with a third thread that is waiting for this
  // thread to be suspended. The third thread can notice the safepoint
  // that we're trying to start at the beginning of its SR_lock->wait()
  // call. If that happens, then the third thread will block on the
  // safepoint while still holding the underlying SR_lock. We won't be
  // able to get the SR_lock and we'll deadlock.
  //
  // We don't need to grab the SR_lock here for two reasons:
  // 1) The suspend flags are both volatile and are set with an
  //    Atomic::cmpxchg() call so we should see the suspended
  //    state right away.
  // 2) We're being called from the safepoint polling loop; if
  //    we don't see the suspended state on this iteration, then
  //    we'll come around again.
  //
  // 当线程已经被挂起时表示处于暂停状态，即到达了安全点，如果线程调用了方法suspend()，
  // 为了防止线程在VMThread执行垃圾回收时恢复执行，挂起的线程需要在安全点上暂停
  bool is_suspended = _thread->is_ext_suspended();
  if (is_suspended) {
    // 如果已挂起，通知SafepointSynchronize当前线程已到达安全点
    roll_forward(_at_safepoint);
    return;
  }

  // Some JavaThread states have an initial safepoint state of
  // running, but are actually at a safepoint. We will happily
  // agree and update the safepoint state here.
  // 当线程本身已经处于阻塞状态或线程在执行native代码时表示到达了安全点
  if (SafepointSynchronize::safepoint_safe(_thread, state)) {
    // 检查是否是lazy_critical_native，如果是则将当前线程标识为已进入关键区
    SafepointSynchronize::check_for_lazy_critical_native(_thread, state);
    // 将状态置为_at_safepoint
    roll_forward(_at_safepoint);
    return;
  }
  // 当线程在虚拟机中运行时，需要等待进入安全点
  if (state == _thread_in_vm) {
    // 将状态置为_call_back
    roll_forward(_call_back);
    return;
  }

  // All other thread states will continue to run until they
  // transition and self-block in state _blocked
  // Safepoint polling in compiled code causes the Java threads to do the same.
  // Note: new threads may require a malloc so they must be allowed to finish

  // 线程还处在运行状态，必须在SafepointSynchronize::begin()函数的while循环中等待
  assert(is_running(), "examine_state_of_thread on non-running thread");
  return;
}

// Returns true is thread could not be rolled forward at present position.
void ThreadSafepointState::roll_forward(suspend_type type) {
  // VMThread线程会更新相关线程的_type变量的值
  _type = type;

  switch(_type) {
    case _at_safepoint:
      // 通知SafepointSynchronize当前线程已经停在安全点上，减少_waiting_to_block
      SafepointSynchronize::signal_thread_at_safepoint();
      DEBUG_ONLY(_thread->set_visited_for_critical_count(true));
      if (_thread->in_critical()) {
        // Notice that this thread is in a critical section
        // 如果线程处于JNI关键区，则增加对应的线程计数
        SafepointSynchronize::increment_jni_active_count();
      }
      break;

    case _call_back:
      // 在_call_back状态下，_waiting_to_block不会减1，线程会在下一个处理_waiting_to_block的循环中继续进行处理。
      set_has_called_back(false);
      break;

    case _running:
    default:
      ShouldNotReachHere();
  }
}

void ThreadSafepointState::restart() {
  switch(type()) {
    case _at_safepoint:
    case _call_back:
      break;

    case _running:
    default:
       tty->print_cr("restart thread "INTPTR_FORMAT" with state %d",
                      _thread, _type);
       _thread->print();
      ShouldNotReachHere();
  }
  // 将状态置为running，has_called_back恢复成初始的false
  _type = _running;
  set_has_called_back(false);
}


void ThreadSafepointState::print_on(outputStream *st) const {
  const char *s;

  switch(_type) {
    case _running                : s = "_running";              break;
    case _at_safepoint           : s = "_at_safepoint";         break;
    case _call_back              : s = "_call_back";            break;
    default:
      ShouldNotReachHere();
  }

  st->print_cr("Thread: " INTPTR_FORMAT
              "  [0x%2x] State: %s _has_called_back %d _at_poll_safepoint %d",
               _thread, _thread->osthread()->thread_id(), s, _has_called_back,
               _at_poll_safepoint);

  _thread->print_thread_state_on(st);
}


// ---------------------------------------------------------------------------------------------------------------------

// Block the thread at the safepoint poll or poll return.
void ThreadSafepointState::handle_polling_page_exception() {

  // Check state.  block() will set thread state to thread_in_vm which will
  // cause the safepoint state _type to become _call_back.
  // 检查ThreadSafepointState的状态
  assert(type() == ThreadSafepointState::_running,
         "polling page exception on thread not running state");

  // Step 1: Find the nmethod from the return address
  if (ShowSafepointMsgs && Verbose) {
    tty->print_cr("Polling page exception at " INTPTR_FORMAT, thread()->saved_exception_pc());
  }
  // 根据出现异常的地址找到对应的nmethod
  address real_return_addr = thread()->saved_exception_pc();

  CodeBlob *cb = CodeCache::find_blob(real_return_addr);
  assert(cb != NULL && cb->is_nmethod(), "return address should be in nmethod");
  nmethod* nm = (nmethod*)cb;

  // Find frame of caller
  // 找到出现异常的方法的调用方
  frame stub_fr = thread()->last_frame();
  CodeBlob* stub_cb = stub_fr.cb();
  assert(stub_cb->is_safepoint_stub(), "must be a safepoint stub");
  RegisterMap map(thread(), true);
  frame caller_fr = stub_fr.sender(&map);

  // Should only be poll_return or poll
  assert( nm->is_at_poll_or_poll_return(real_return_addr), "should not be at call" );

  // This is a poll immediately before a return. The exception handling code
  // has already had the effect of causing the return to occur, so the execution
  // will continue immediately after the call. In addition, the oopmap at the
  // return point does not mark the return value as an oop (if it is), so
  // it needs a handle here to be updated.
  if( nm->is_at_poll_return(real_return_addr) ) {
    // See if return type is an oop.
    // 判断该方法的返回类型是否是一个oop
    bool return_oop = nm->method()->is_returning_oop();
    Handle return_value;
    if (return_oop) {
      // The oop result has been saved on the stack together with all
      // the other registers. In order to preserve it over GCs we need
      // to keep it in a handle.
      // 如果是则将其放在Hanlder里保护起来
      oop result = caller_fr.saved_oop_result(&map);
      assert(result == NULL || result->is_oop(), "must be oop");
      return_value = Handle(thread(), result);
      assert(Universe::heap()->is_in_or_null(result), "must be heap pointer");
    }

    // Block the thread
    // 阻塞当前线程，直到VMThead从安全点退出
    SafepointSynchronize::block(thread());

    // restore oop result, if any
    if (return_oop) {
      // 设置调用结果oop
      caller_fr.set_saved_oop_result(&map, return_value());
    }
  }

  // This is a safepoint poll. Verify the return address and block.
  else {
    set_at_poll_safepoint(true);

    // verify the blob built the "return address" correctly
    // 校验当前方法的返回地址就是调用方的pc
    assert(real_return_addr == caller_fr.pc(), "must match");

    // Block the thread
    // 阻塞当前线程，直到VMThead从安全点退出
    SafepointSynchronize::block(thread());
    set_at_poll_safepoint(false);

    // If we have a pending async exception deoptimize the frame
    // as otherwise we may never deliver it.
    if (thread()->has_async_condition()) {
      ThreadInVMfromJavaNoAsyncException __tiv(thread());
      Deoptimization::deoptimize_frame(thread(), caller_fr.id());
    }

    // If an exception has been installed we must check for a pending deoptimization
    // Deoptimize frame if exception has been thrown.

    if (thread()->has_pending_exception() ) {
      RegisterMap map(thread(), true);
      frame caller_fr = stub_fr.sender(&map);
      if (caller_fr.is_deoptimized_frame()) {
        // The exception patch will destroy registers that are still
        // live and will be needed during deoptimization. Defer the
        // Async exception should have defered the exception until the
        // next safepoint which will be detected when we get into
        // the interpreter so if we have an exception now things
        // are messed up.

        fatal("Exception installed and deoptimization is pending");
      }
    }
  }
}


//
//                     Statistics & Instrumentations
//
SafepointSynchronize::SafepointStats*  SafepointSynchronize::_safepoint_stats = NULL;
jlong  SafepointSynchronize::_safepoint_begin_time = 0;
int    SafepointSynchronize::_cur_stat_index = 0;
julong SafepointSynchronize::_safepoint_reasons[VM_Operation::VMOp_Terminating];
julong SafepointSynchronize::_coalesced_vmop_count = 0;
jlong  SafepointSynchronize::_max_sync_time = 0;
jlong  SafepointSynchronize::_max_vmop_time = 0;
float  SafepointSynchronize::_ts_of_current_safepoint = 0.0f;

static jlong  cleanup_end_time = 0;
static bool   need_to_track_page_armed_status = false;
static bool   init_done = false;

// Helper method to print the header.
static void print_header() {
  tty->print("         vmop                    "
             "[threads: total initially_running wait_to_block]    ");
  tty->print("[time: spin block sync cleanup vmop] ");

  // no page armed status printed out if it is always armed.
  if (need_to_track_page_armed_status) {
    tty->print("page_armed ");
  }

  tty->print_cr("page_trap_count");
}

void SafepointSynchronize::deferred_initialize_stat() {
  // init_done是一个静态属性
  if (init_done) return;

  if (PrintSafepointStatisticsCount <= 0) {
    fatal("Wrong PrintSafepointStatisticsCount");
  }

  // If PrintSafepointStatisticsTimeout is specified, the statistics data will
  // be printed right away, in which case, _safepoint_stats will regress to
  // a single element array. Otherwise, it is a circular ring buffer with default
  // size of PrintSafepointStatisticsCount.
  int stats_array_size;
  if (PrintSafepointStatisticsTimeout > 0) {
    stats_array_size = 1;
    PrintSafepointStatistics = true;
  } else {
    stats_array_size = PrintSafepointStatisticsCount;
  }
  // 初始化_safepoint_stats
  _safepoint_stats = (SafepointStats*)os::malloc(stats_array_size
                                                 * sizeof(SafepointStats), mtInternal);
  guarantee(_safepoint_stats != NULL,
            "not enough memory for safepoint instrumentation data");

  if (UseCompilerSafepoints && DeferPollingPageLoopCount >= 0) {
    // need_to_track_page_armed_status也是一个静态属性
    need_to_track_page_armed_status = true;
  }
  init_done = true;
}

void SafepointSynchronize::begin_statistics(int nof_threads, int nof_running) {
  assert(init_done, "safepoint statistics array hasn't been initialized");
  SafepointStats *spstat = &_safepoint_stats[_cur_stat_index];

  spstat->_time_stamp = _ts_of_current_safepoint;

  VM_Operation *op = VMThread::vm_operation();
  spstat->_vmop_type = (op != NULL ? op->type() : -1);
  if (op != NULL) {
    _safepoint_reasons[spstat->_vmop_type]++;
  }

  spstat->_nof_total_threads = nof_threads;
  spstat->_nof_initial_running_threads = nof_running;
  spstat->_nof_threads_hit_page_trap = 0;

  // Records the start time of spinning. The real time spent on spinning
  // will be adjusted when spin is done. Same trick is applied for time
  // spent on waiting for threads to block.
  if (nof_running != 0) {
    spstat->_time_to_spin = os::javaTimeNanos();
  }  else {
    spstat->_time_to_spin = 0;
  }
}

void SafepointSynchronize::update_statistics_on_spin_end() {
  SafepointStats *spstat = &_safepoint_stats[_cur_stat_index];

  jlong cur_time = os::javaTimeNanos();

  spstat->_nof_threads_wait_to_block = _waiting_to_block;
  if (spstat->_nof_initial_running_threads != 0) {
    spstat->_time_to_spin = cur_time - spstat->_time_to_spin;
  }

  if (need_to_track_page_armed_status) {
    spstat->_page_armed = (PageArmed == 1);
  }

  // Records the start time of waiting for to block. Updated when block is done.
  if (_waiting_to_block != 0) {
    spstat->_time_to_wait_to_block = cur_time;
  } else {
    spstat->_time_to_wait_to_block = 0;
  }
}

void SafepointSynchronize::update_statistics_on_sync_end(jlong end_time) {
  SafepointStats *spstat = &_safepoint_stats[_cur_stat_index];

  if (spstat->_nof_threads_wait_to_block != 0) {
    spstat->_time_to_wait_to_block = end_time -
      spstat->_time_to_wait_to_block;
  }

  // Records the end time of sync which will be used to calculate the total
  // vm operation time. Again, the real time spending in syncing will be deducted
  // from the start of the sync time later when end_statistics is called.
  spstat->_time_to_sync = end_time - _safepoint_begin_time;
  if (spstat->_time_to_sync > _max_sync_time) {
    _max_sync_time = spstat->_time_to_sync;
  }

  spstat->_time_to_do_cleanups = end_time;
}

void SafepointSynchronize::update_statistics_on_cleanup_end(jlong end_time) {
  SafepointStats *spstat = &_safepoint_stats[_cur_stat_index];

  // Record how long spent in cleanup tasks.
  spstat->_time_to_do_cleanups = end_time - spstat->_time_to_do_cleanups;

  cleanup_end_time = end_time;
}

void SafepointSynchronize::end_statistics(jlong vmop_end_time) {
  SafepointStats *spstat = &_safepoint_stats[_cur_stat_index];

  // Update the vm operation time.
  spstat->_time_to_exec_vmop = vmop_end_time -  cleanup_end_time;
  if (spstat->_time_to_exec_vmop > _max_vmop_time) {
    _max_vmop_time = spstat->_time_to_exec_vmop;
  }
  // Only the sync time longer than the specified
  // PrintSafepointStatisticsTimeout will be printed out right away.
  // By default, it is -1 meaning all samples will be put into the list.
  if ( PrintSafepointStatisticsTimeout > 0) {
    if (spstat->_time_to_sync > PrintSafepointStatisticsTimeout * MICROUNITS) {
      print_statistics();
    }
  } else {
    // The safepoint statistics will be printed out when the _safepoin_stats
    // array fills up.
    if (_cur_stat_index == PrintSafepointStatisticsCount - 1) {
      print_statistics();
      _cur_stat_index = 0;
    } else {
      _cur_stat_index++;
    }
  }
}

void SafepointSynchronize::print_statistics() {
  SafepointStats* sstats = _safepoint_stats;

  for (int index = 0; index <= _cur_stat_index; index++) {
    if (index % 30 == 0) {
      print_header();
    }
    sstats = &_safepoint_stats[index];
    tty->print("%.3f: ", sstats->_time_stamp);
    tty->print("%-26s       ["
               INT32_FORMAT_W(8)INT32_FORMAT_W(11)INT32_FORMAT_W(15)
               "    ]    ",
               sstats->_vmop_type == -1 ? "no vm operation" :
               VM_Operation::name(sstats->_vmop_type),
               sstats->_nof_total_threads,
               sstats->_nof_initial_running_threads,
               sstats->_nof_threads_wait_to_block);
    // "/ MICROUNITS " is to convert the unit from nanos to millis.
    tty->print("  ["
               INT64_FORMAT_W(6)INT64_FORMAT_W(6)
               INT64_FORMAT_W(6)INT64_FORMAT_W(6)
               INT64_FORMAT_W(6)"    ]  ",
               sstats->_time_to_spin / MICROUNITS,
               sstats->_time_to_wait_to_block / MICROUNITS,
               sstats->_time_to_sync / MICROUNITS,
               sstats->_time_to_do_cleanups / MICROUNITS,
               sstats->_time_to_exec_vmop / MICROUNITS);

    if (need_to_track_page_armed_status) {
      tty->print(INT32_FORMAT"         ", sstats->_page_armed);
    }
    tty->print_cr(INT32_FORMAT"   ", sstats->_nof_threads_hit_page_trap);
  }
}

// This method will be called when VM exits. It will first call
// print_statistics to print out the rest of the sampling.  Then
// it tries to summarize the sampling.
void SafepointSynchronize::print_stat_on_exit() {
  if (_safepoint_stats == NULL) return;

  SafepointStats *spstat = &_safepoint_stats[_cur_stat_index];

  // During VM exit, end_statistics may not get called and in that
  // case, if the sync time is less than PrintSafepointStatisticsTimeout,
  // don't print it out.
  // Approximate the vm op time.
  _safepoint_stats[_cur_stat_index]._time_to_exec_vmop =
    os::javaTimeNanos() - cleanup_end_time;

  if ( PrintSafepointStatisticsTimeout < 0 ||
       spstat->_time_to_sync > PrintSafepointStatisticsTimeout * MICROUNITS) {
    print_statistics();
  }
  tty->print_cr("");

  // Print out polling page sampling status.
  if (!need_to_track_page_armed_status) {
    if (UseCompilerSafepoints) {
      tty->print_cr("Polling page always armed");
    }
  } else {
    tty->print_cr("Defer polling page loop count = %d\n",
                 DeferPollingPageLoopCount);
  }

  for (int index = 0; index < VM_Operation::VMOp_Terminating; index++) {
    if (_safepoint_reasons[index] != 0) {
      tty->print_cr("%-26s"UINT64_FORMAT_W(10), VM_Operation::name(index),
                    _safepoint_reasons[index]);
    }
  }

  tty->print_cr(UINT64_FORMAT_W(5)" VM operations coalesced during safepoint",
                _coalesced_vmop_count);
  tty->print_cr("Maximum sync time  "INT64_FORMAT_W(5)" ms",
                _max_sync_time / MICROUNITS);
  tty->print_cr("Maximum vm operation time (except for Exit VM operation)  "
                INT64_FORMAT_W(5)" ms",
                _max_vmop_time / MICROUNITS);
}

// ------------------------------------------------------------------------------------------------
// Non-product code

#ifndef PRODUCT

void SafepointSynchronize::print_state() {
  if (_state == _not_synchronized) {
    tty->print_cr("not synchronized");
  } else if (_state == _synchronizing || _state == _synchronized) {
    tty->print_cr("State: %s", (_state == _synchronizing) ? "synchronizing" :
                  "synchronized");

    for(JavaThread *cur = Threads::first(); cur; cur = cur->next()) {
       cur->safepoint_state()->print();
    }
  }
}

void SafepointSynchronize::safepoint_msg(const char* format, ...) {
  if (ShowSafepointMsgs) {
    va_list ap;
    va_start(ap, format);
    tty->vprint_cr(format, ap);
    va_end(ap);
  }
}

#endif // !PRODUCT
