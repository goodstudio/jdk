/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "classfile/javaClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "jvm_io.h"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/handshake.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/osThread.hpp"
#include "runtime/stackWatermarkSet.hpp"
#include "runtime/task.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/filterQueue.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/preserveException.hpp"
#include "utilities/systemMemoryBarrier.hpp"

class HandshakeOperation : public CHeapObj<mtThread> {
  friend class HandshakeState;
 protected:
  HandshakeClosure*   _handshake_cl;
  // Keeps track of emitted and completed handshake operations.
  // Once it reaches zero all handshake operations have been performed.
  int32_t             _pending_threads;
  JavaThread*         _target;
  Thread*             _requester;

  // Must use AsyncHandshakeOperation when using AsyncHandshakeClosure.
  HandshakeOperation(AsyncHandshakeClosure* cl, JavaThread* target, Thread* requester) :
    _handshake_cl(cl),
    _pending_threads(1),
    _target(target),
    _requester(requester) {}

 public:
  HandshakeOperation(HandshakeClosure* cl, JavaThread* target, Thread* requester) :
    _handshake_cl(cl),
    _pending_threads(1),
    _target(target),
    _requester(requester) {}
  virtual ~HandshakeOperation() {}
  void prepare(JavaThread* current_target, Thread* executing_thread);
  void do_handshake(JavaThread* thread);
  bool is_completed() {
    int32_t val = Atomic::load(&_pending_threads);
    assert(val >= 0, "_pending_threads=%d cannot be negative", val);
    return val == 0;
  }
  void add_target_count(int count) { Atomic::add(&_pending_threads, count); }
  int32_t pending_threads()        { return Atomic::load(&_pending_threads); }
  const char* name()               { return _handshake_cl->name(); }
  bool is_async()                  { return _handshake_cl->is_async(); }
  bool is_suspend()                { return _handshake_cl->is_suspend(); }
  bool is_async_exception()        { return _handshake_cl->is_async_exception(); }
};

class AsyncHandshakeOperation : public HandshakeOperation {
 private:
  jlong _start_time_ns;
 public:
  AsyncHandshakeOperation(AsyncHandshakeClosure* cl, JavaThread* target, jlong start_ns)
    : HandshakeOperation(cl, target, nullptr), _start_time_ns(start_ns) {}
  virtual ~AsyncHandshakeOperation() { delete _handshake_cl; }
  jlong start_time() const           { return _start_time_ns; }
};

// Performing handshakes requires a custom yielding strategy because without it
// there is a clear performance regression vs plain spinning. We keep track of
// when we last saw progress by looking at why each targeted thread has not yet
// completed its handshake. After spinning for a while with no progress we will
// yield, but as long as there is progress, we keep spinning. Thus we avoid
// yielding when there is potential work to be done or the handshake is close
// to being finished.
class HandshakeSpinYield : public StackObj {
 private:
  jlong _start_time_ns;
  jlong _last_spin_start_ns;
  jlong _spin_time_ns;

  int _result_count[2][HandshakeState::_number_states];
  int _prev_result_pos;

  int current_result_pos() { return (_prev_result_pos + 1) & 0x1; }

  void wait_raw(jlong now) {
    // We start with fine-grained nanosleeping until a millisecond has
    // passed, at which point we resort to plain naked_short_sleep.
    if (now - _start_time_ns < NANOSECS_PER_MILLISEC) {
      os::naked_short_nanosleep(10 * (NANOUNITS / MICROUNITS));
    } else {
      os::naked_short_sleep(1);
    }
  }

  void wait_blocked(JavaThread* self, jlong now) {
    ThreadBlockInVM tbivm(self);
    wait_raw(now);
  }

  bool state_changed() {
    for (int i = 0; i < HandshakeState::_number_states; i++) {
      if (_result_count[0][i] != _result_count[1][i]) {
        return true;
      }
    }
    return false;
  }

  void reset_state() {
    _prev_result_pos++;
    for (int i = 0; i < HandshakeState::_number_states; i++) {
      _result_count[current_result_pos()][i] = 0;
    }
  }

 public:
  HandshakeSpinYield(jlong start_time) :
    _start_time_ns(start_time), _last_spin_start_ns(start_time),
    _spin_time_ns(0), _result_count(), _prev_result_pos(0) {

    const jlong max_spin_time_ns = 100 /* us */ * (NANOUNITS / MICROUNITS);
    int free_cpus = os::active_processor_count() - 1;
    _spin_time_ns = (5 /* us */ * (NANOUNITS / MICROUNITS)) * free_cpus; // zero on UP
    _spin_time_ns = _spin_time_ns > max_spin_time_ns ? max_spin_time_ns : _spin_time_ns;
  }

  void add_result(HandshakeState::ProcessResult pr) {
    _result_count[current_result_pos()][pr]++;
  }

  void process() {
    jlong now = os::javaTimeNanos();
    if (state_changed()) {
      reset_state();
      // We spin for x amount of time since last state change.
      _last_spin_start_ns = now;
      return;
    }
    jlong wait_target = _last_spin_start_ns + _spin_time_ns;
    if (wait_target < now) {
      // On UP this is always true.
      Thread* self = Thread::current();
      if (self->is_Java_thread()) {
        wait_blocked(JavaThread::cast(self), now);
      } else {
        wait_raw(now);
      }
      _last_spin_start_ns = os::javaTimeNanos();
    }
    reset_state();
  }
};

static void handle_timeout(HandshakeOperation* op, JavaThread* target) {
  JavaThreadIteratorWithHandle jtiwh;

  log_error(handshake)("Handshake timeout: %s(" INTPTR_FORMAT "), pending threads: " INT32_FORMAT,
                       op->name(), p2i(op), op->pending_threads());

  if (target == nullptr) {
    for ( ; JavaThread* thr = jtiwh.next(); ) {
      if (thr->handshake_state()->operation_pending(op)) {
        log_error(handshake)("JavaThread " INTPTR_FORMAT " has not cleared handshake op: " INTPTR_FORMAT, p2i(thr), p2i(op));
        // Remember the last one found for more diagnostics below.
        target = thr;
      }
    }
  } else {
    log_error(handshake)("JavaThread " INTPTR_FORMAT " has not cleared handshake op: " INTPTR_FORMAT, p2i(target), p2i(op));
  }

  if (target != nullptr) {
    if (os::signal_thread(target, SIGILL, "cannot be handshaked")) {
      // Give target a chance to report the error and terminate the VM.
      os::naked_sleep(3000);
    }
  } else {
    log_error(handshake)("No thread with an unfinished handshake op(" INTPTR_FORMAT ") found.", p2i(op));
  }
  fatal("Handshake timeout");
}

static void check_handshake_timeout(jlong start_time, HandshakeOperation* op, JavaThread* target = nullptr) {
  // Check if handshake operation has timed out
  jlong timeout_ns = millis_to_nanos(HandshakeTimeout);
  if (timeout_ns > 0) {
    if (os::javaTimeNanos() >= (start_time + timeout_ns)) {
      handle_timeout(op, target);
    }
  }
}

static void log_handshake_info(jlong start_time_ns, const char* name, int targets, int emitted_handshakes_executed, const char* extra = nullptr) {
  if (log_is_enabled(Info, handshake)) {
    jlong completion_time = os::javaTimeNanos() - start_time_ns;
    log_info(handshake)("Handshake \"%s\", Targeted threads: %d, Executed by requesting thread: %d, Total completion time: " JLONG_FORMAT " ns%s%s",
                        name, targets,
                        emitted_handshakes_executed,
                        completion_time,
                        extra != nullptr ? ", " : "",
                        extra != nullptr ? extra : "");
  }
}

class VM_HandshakeAllThreads: public VM_Operation {
  HandshakeOperation* const _op;
 public:
  VM_HandshakeAllThreads(HandshakeOperation* op) : _op(op) {}

  const char* cause() const { return _op->name(); }

  bool evaluate_at_safepoint() const { return false; }

  void doit() {
    jlong start_time_ns = os::javaTimeNanos();

    JavaThreadIteratorWithHandle jtiwh;
    int number_of_threads_issued = 0;
    for (JavaThread* thr = jtiwh.next(); thr != nullptr; thr = jtiwh.next()) {
      thr->handshake_state()->add_operation(_op);
      number_of_threads_issued++;
    }

    // Separate the arming of the poll in add_operation() above from
    // the read of JavaThread state in the try_process() call below.
    if (UseSystemMemoryBarrier) {
      SystemMemoryBarrier::emit();
    } else {
      OrderAccess::fence();
    }

    if (number_of_threads_issued < 1) {
      log_handshake_info(start_time_ns, _op->name(), 0, 0, "no threads alive");
      return;
    }
    // _op was created with a count == 1 so don't double count.
    _op->add_target_count(number_of_threads_issued - 1);

    log_trace(handshake)("Threads signaled, begin processing blocked threads by VMThread");
    HandshakeSpinYield hsy(start_time_ns);
    // Keeps count on how many of own emitted handshakes
    // this thread execute.
    int emitted_handshakes_executed = 0;
    do {
      // Check if handshake operation has timed out
      check_handshake_timeout(start_time_ns, _op);

      // Have VM thread perform the handshake operation for blocked threads.
      // Observing a blocked state may of course be transient but the processing is guarded
      // by mutexes and we optimistically begin by working on the blocked threads
      jtiwh.rewind();
      for (JavaThread* thr = jtiwh.next(); thr != nullptr; thr = jtiwh.next()) {
        // A new thread on the ThreadsList will not have an operation,
        // hence it is skipped in handshake_try_process.
        HandshakeState::ProcessResult pr = thr->handshake_state()->try_process(_op);
        hsy.add_result(pr);
        if (pr == HandshakeState::_succeeded) {
          emitted_handshakes_executed++;
        }
      }
      hsy.process();
    } while (!_op->is_completed());

    // This pairs up with the release store in do_handshake(). It prevents future
    // loads from floating above the load of _pending_threads in is_completed()
    // and thus prevents reading stale data modified in the handshake closure
    // by the Handshakee.
    OrderAccess::acquire();

    log_handshake_info(start_time_ns, _op->name(), number_of_threads_issued, emitted_handshakes_executed);
  }

  VMOp_Type type() const { return VMOp_HandshakeAllThreads; }
};

void HandshakeOperation::prepare(JavaThread* current_target, Thread* executing_thread) {
  if (current_target->is_terminated()) {
    // Will never execute any handshakes on this thread.
    return;
  }
  if (current_target != executing_thread) {
    // Only when the target is not executing the handshake itself.
    StackWatermarkSet::start_processing(current_target, StackWatermarkKind::gc);
  }
  if (_requester != nullptr && _requester != executing_thread && _requester->is_Java_thread()) {
    // The handshake closure may contain oop Handles from the _requester.
    // We must make sure we can use them.
    StackWatermarkSet::start_processing(JavaThread::cast(_requester), StackWatermarkKind::gc);
  }
}

void HandshakeOperation::do_handshake(JavaThread* thread) {
  jlong start_time_ns = 0;
  if (log_is_enabled(Debug, handshake, task)) {
    start_time_ns = os::javaTimeNanos();
  }

  // Only actually execute the operation for non terminated threads.
  if (!thread->is_terminated()) {
    _handshake_cl->do_thread(thread);
  }

  if (start_time_ns != 0) {
    jlong completion_time = os::javaTimeNanos() - start_time_ns;
    log_debug(handshake, task)("Operation: %s for thread " PTR_FORMAT ", is_vm_thread: %s, completed in " JLONG_FORMAT " ns",
                               name(), p2i(thread), BOOL_TO_STR(Thread::current()->is_VM_thread()), completion_time);
  }

  // Inform VMThread/Handshaker that we have completed the operation.
  // When this is executed by the Handshakee we need a release store
  // here to make sure memory operations executed in the handshake
  // closure are visible to the VMThread/Handshaker after it reads
  // that the operation has completed.
  Atomic::dec(&_pending_threads);
  // Trailing fence, used to make sure removal of the operation strictly
  // happened after we completed the operation.

  // It is no longer safe to refer to 'this' as the VMThread/Handshaker may have destroyed this operation
}

void Handshake::execute(HandshakeClosure* hs_cl) {
  HandshakeOperation cto(hs_cl, nullptr, Thread::current());
  VM_HandshakeAllThreads handshake(&cto);
  VMThread::execute(&handshake);
}

void Handshake::execute(HandshakeClosure* hs_cl, JavaThread* target) {
  // tlh == nullptr means we rely on a ThreadsListHandle somewhere
  // in the caller's context (and we sanity check for that).
  Handshake::execute(hs_cl, nullptr, target);
}

void Handshake::execute(HandshakeClosure* hs_cl, ThreadsListHandle* tlh, JavaThread* target) {
  JavaThread* self = JavaThread::current();
  HandshakeOperation op(hs_cl, target, Thread::current());

  jlong start_time_ns = os::javaTimeNanos();

  guarantee(target != nullptr, "must be");
  if (tlh == nullptr) {
    guarantee(Thread::is_JavaThread_protected_by_TLH(target),
              "missing ThreadsListHandle in calling context.");
    target->handshake_state()->add_operation(&op);
  } else if (tlh->includes(target)) {
    target->handshake_state()->add_operation(&op);
  } else {
    char buf[128];
    jio_snprintf(buf, sizeof(buf),  "(thread= " INTPTR_FORMAT " dead)", p2i(target));
    log_handshake_info(start_time_ns, op.name(), 0, 0, buf);
    return;
  }

  // Separate the arming of the poll in add_operation() above from
  // the read of JavaThread state in the try_process() call below.
  if (UseSystemMemoryBarrier) {
    SystemMemoryBarrier::emit();
  } else {
    OrderAccess::fence();
  }

  // Keeps count on how many of own emitted handshakes
  // this thread execute.
  int emitted_handshakes_executed = 0;
  HandshakeSpinYield hsy(start_time_ns);
  while (!op.is_completed()) {
    HandshakeState::ProcessResult pr = target->handshake_state()->try_process(&op);
    if (pr == HandshakeState::_succeeded) {
      emitted_handshakes_executed++;
    }
    if (op.is_completed()) {
      break;
    }

    // Check if handshake operation has timed out
    check_handshake_timeout(start_time_ns, &op, target);

    hsy.add_result(pr);
    // Check for pending handshakes to avoid possible deadlocks where our
    // target is trying to handshake us.
    if (SafepointMechanism::should_process(self)) {
      // Will not suspend here.
      ThreadBlockInVM tbivm(self);
    }
    hsy.process();
  }

  // This pairs up with the release store in do_handshake(). It prevents future
  // loads from floating above the load of _pending_threads in is_completed()
  // and thus prevents reading stale data modified in the handshake closure
  // by the Handshakee.
  OrderAccess::acquire();

  log_handshake_info(start_time_ns, op.name(), 1, emitted_handshakes_executed);
}

void Handshake::execute(AsyncHandshakeClosure* hs_cl, JavaThread* target) {
  jlong start_time_ns = os::javaTimeNanos();
  AsyncHandshakeOperation* op = new AsyncHandshakeOperation(hs_cl, target, start_time_ns);

  guarantee(target != nullptr, "must be");

  Thread* current = Thread::current();
  if (current != target) {
    // Another thread is handling the request and it must be protecting
    // the target.
    guarantee(Thread::is_JavaThread_protected_by_TLH(target),
              "missing ThreadsListHandle in calling context.");
  }
  // Implied else:
  // The target is handling the request itself so it can't be dead.

  target->handshake_state()->add_operation(op);
}

// Filters
static bool non_self_executable_filter(HandshakeOperation* op) {
  return !op->is_async();
}
static bool no_async_exception_filter(HandshakeOperation* op) {
  return !op->is_async_exception();
}
static bool async_exception_filter(HandshakeOperation* op) {
  return op->is_async_exception();
}
static bool no_suspend_no_async_exception_filter(HandshakeOperation* op) {
  return !op->is_suspend() && !op->is_async_exception();
}
static bool all_ops_filter(HandshakeOperation* op) {
  return true;
}

HandshakeState::HandshakeState(JavaThread* target) :
  _handshakee(target),
  _queue(),
  _lock(Monitor::nosafepoint, "HandshakeState_lock"),
  _active_handshaker(),
  _async_exceptions_blocked(false) {
}

HandshakeState::~HandshakeState() {
  while (has_operation()) {
    HandshakeOperation* op = _queue.pop(all_ops_filter);
    guarantee(op->is_async(), "Only async operations may still be present on queue");
    delete op;
  }
}

void HandshakeState::add_operation(HandshakeOperation* op) {
  // Adds are done lock free and so is arming.
  _queue.push(op);
  SafepointMechanism::arm_local_poll_release(_handshakee);
}

bool HandshakeState::operation_pending(HandshakeOperation* op) {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  MatchOp mo(op);
  return _queue.contains(mo);
}

HandshakeOperation* HandshakeState::get_op_for_self(bool allow_suspend, bool check_async_exception) {
  assert(_handshakee == Thread::current(), "Must be called by self");
  assert(_lock.owned_by_self(), "Lock must be held");
  assert(allow_suspend || !check_async_exception, "invalid case");
#if INCLUDE_JVMTI
  if (allow_suspend && _handshakee->is_disable_suspend()) {
    // filter out suspend operations while JavaThread is in disable_suspend mode
    allow_suspend = false;
  }
#endif
  if (!allow_suspend) {
    return _queue.peek(no_suspend_no_async_exception_filter);
  } else if (check_async_exception && !_async_exceptions_blocked) {
    return _queue.peek();
  } else {
    return _queue.peek(no_async_exception_filter);
  }
}

bool HandshakeState::has_operation(bool allow_suspend, bool check_async_exception) {
  // We must not block here as that could lead to deadlocks if we already hold an
  // "external" mutex. If the try_lock fails then we assume that there is an operation
  // and force the caller to check more carefully in a safer context. If we can't get
  // the lock it means another thread is trying to handshake with us, so it can't
  // happen during thread termination and destruction.
  bool ret = true;
  if (_lock.try_lock()) {
    ret = get_op_for_self(allow_suspend, check_async_exception) != nullptr;
    _lock.unlock();
  }
  return ret;
}

bool HandshakeState::has_async_exception_operation() {
  if (!has_operation()) return false;
  ConditionalMutexLocker ml(&_lock, !_lock.owned_by_self(), Mutex::_no_safepoint_check_flag);
  return _queue.peek(async_exception_filter) != nullptr;
}

void HandshakeState::clean_async_exception_operation() {
  while (has_async_exception_operation()) {
    MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
    HandshakeOperation* op;
    op = _queue.peek(async_exception_filter);
    remove_op(op);
    delete op;
  }
}

bool HandshakeState::have_non_self_executable_operation() {
  assert(_handshakee != Thread::current(), "Must not be called by self");
  assert(_lock.owned_by_self(), "Lock must be held");
  return _queue.contains(non_self_executable_filter);
}

HandshakeOperation* HandshakeState::get_op() {
  assert(_handshakee != Thread::current(), "Must not be called by self");
  assert(_lock.owned_by_self(), "Lock must be held");
  return _queue.peek(non_self_executable_filter);
};

void HandshakeState::remove_op(HandshakeOperation* op) {
  assert(_lock.owned_by_self(), "Lock must be held");
  MatchOp mo(op);
  HandshakeOperation* ret = _queue.pop(mo);
  assert(ret == op, "Popped op must match requested op");
};

bool HandshakeState::process_by_self(bool allow_suspend, bool check_async_exception) {
  assert(Thread::current() == _handshakee, "should call from _handshakee");
  assert(!_handshakee->is_terminated(), "should not be a terminated thread");

  _handshakee->frame_anchor()->make_walkable();
  // Threads shouldn't block if they are in the middle of printing, but...
  ttyLocker::break_tty_lock_for_safepoint(os::current_thread_id());

  // Separate all the writes above for other threads reading state
  // set by this thread in case the operation is ThreadSuspendHandshake.
  OrderAccess::fence();

  while (has_operation()) {
    // Handshakes cannot safely safepoint. The exceptions to this rule are
    // the asynchronous suspension and unsafe access error handshakes.
    MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);

    HandshakeOperation* op = get_op_for_self(allow_suspend, check_async_exception);
    if (op != nullptr) {
      assert(op->_target == nullptr || op->_target == Thread::current(), "Wrong thread");
      bool async = op->is_async();
      log_trace(handshake)("Proc handshake %s " INTPTR_FORMAT " on " INTPTR_FORMAT " by self",
                           async ? "asynchronous" : "synchronous", p2i(op), p2i(_handshakee));
      op->prepare(_handshakee, _handshakee);
      if (!async) {
        HandleMark hm(_handshakee);
        PreserveExceptionMark pem(_handshakee);
        op->do_handshake(_handshakee); // acquire, op removed after
        remove_op(op);
      } else {
        // An asynchronous handshake may put the JavaThread in blocked state (safepoint safe).
        // The destructor ~PreserveExceptionMark touches the exception oop so it must not be executed,
        // since a safepoint may be in-progress when returning from the async handshake.
        remove_op(op);
        op->do_handshake(_handshakee);
        log_handshake_info(((AsyncHandshakeOperation*)op)->start_time(), op->name(), 1, 0, "asynchronous");
        delete op;
        return true; // Must check for safepoints
      }
    } else {
      return false;
    }
  }
  return false;
}

bool HandshakeState::can_process_handshake() {
  // handshake_safe may only be called with polls armed.
  // Handshaker controls this by first claiming the handshake via claim_handshake().
  return SafepointSynchronize::handshake_safe(_handshakee);
}

bool HandshakeState::possibly_can_process_handshake() {
  // Note that this method is allowed to produce false positives.
  if (_handshakee->is_terminated()) {
    return true;
  }
  switch (_handshakee->thread_state()) {
  case _thread_in_native:
    // native threads are safe if they have no java stack or have walkable stack
    return !_handshakee->has_last_Java_frame() || _handshakee->frame_anchor()->walkable();

  case _thread_blocked:
    return true;

  default:
    return false;
  }
}

bool HandshakeState::claim_handshake() {
  if (!_lock.try_lock()) {
    return false;
  }
  // Operations are added lock free and then the poll is armed.
  // If all handshake operations for the handshakee are finished and someone
  // just adds an operation we may see it here. But if the handshakee is not
  // armed yet it is not safe to proceed.
  if (have_non_self_executable_operation()) {
    OrderAccess::loadload(); // Matches the implicit storestore in add_operation()
    if (SafepointMechanism::local_poll_armed(_handshakee)) {
      return true;
    }
  }
  _lock.unlock();
  return false;
}

HandshakeState::ProcessResult HandshakeState::try_process(HandshakeOperation* match_op) {
  if (!has_operation()) {
    // JT has already cleared its handshake
    return HandshakeState::_no_operation;
  }

  if (!possibly_can_process_handshake()) {
    // JT is observed in an unsafe state, it must notice the handshake itself
    return HandshakeState::_not_safe;
  }

  // Claim the mutex if there still an operation to be executed.
  if (!claim_handshake()) {
    return HandshakeState::_claim_failed;
  }

  // If we own the mutex at this point and while owning the mutex we
  // can observe a safe state the thread cannot possibly continue without
  // getting caught by the mutex.
  if (!can_process_handshake()) {
    _lock.unlock();
    return HandshakeState::_not_safe;
  }

  Thread* current_thread = Thread::current();

  HandshakeOperation* op = get_op();

  assert(op != nullptr, "Must have an op");
  assert(SafepointMechanism::local_poll_armed(_handshakee), "Must be");
  assert(op->_target == nullptr || _handshakee == op->_target, "Wrong thread");

  log_trace(handshake)("Processing handshake " INTPTR_FORMAT " by %s(%s)", p2i(op),
                       op == match_op ? "handshaker" : "cooperative",
                       current_thread->is_VM_thread() ? "VM Thread" : "JavaThread");

  op->prepare(_handshakee, current_thread);

  set_active_handshaker(current_thread);
  op->do_handshake(_handshakee); // acquire, op removed after
  set_active_handshaker(nullptr);
  remove_op(op);

  _lock.unlock();

  log_trace(handshake)("%s(" INTPTR_FORMAT ") executed an op for JavaThread: " INTPTR_FORMAT " %s target op: " INTPTR_FORMAT,
                       current_thread->is_VM_thread() ? "VM Thread" : "JavaThread",
                       p2i(current_thread), p2i(_handshakee),
                       op == match_op ? "including" : "excluding", p2i(match_op));

  return op == match_op ? HandshakeState::_succeeded : HandshakeState::_processed;
}

void HandshakeState::handle_unsafe_access_error() {
  if (_handshakee->is_suspended()) {
    // A suspend handshake was added to the queue after the
    // unsafe access error. Since the suspender has already
    // considered this JT as suspended and assumes it won't go
    // back to Java until resumed we cannot create the exception
    // object yet. Add a new unsafe access error operation to
    // the end of the queue and try again in the next attempt.
    Handshake::execute(new UnsafeAccessErrorHandshake(), _handshakee);
    log_info(handshake)("JavaThread " INTPTR_FORMAT " skipping unsafe access processing due to suspend.", p2i(_handshakee));
    return;
  }
  // Release the handshake lock before constructing the oop to
  // avoid deadlocks since that can block. This will allow the
  // JavaThread to execute normally as if it was outside a handshake.
  // We will reacquire the handshake lock at return from ~MutexUnlocker.
  MutexUnlocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  // We may be at method entry which requires we save the do-not-unlock flag.
  UnlockFlagSaver fs(_handshakee);
  Handle h_exception = Exceptions::new_exception(_handshakee, vmSymbols::java_lang_InternalError(), "a fault occurred in an unsafe memory access operation");
  if (h_exception()->is_a(vmClasses::InternalError_klass())) {
    java_lang_InternalError::set_during_unsafe_access(h_exception());
  }
  _handshakee->handle_async_exception(h_exception());
}
