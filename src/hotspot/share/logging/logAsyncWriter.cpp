/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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
#include "logging/logAsyncWriter.hpp"
#include "logging/logConfiguration.hpp"
#include "logging/logFileOutput.hpp"
#include "logging/logHandle.hpp"
#include "runtime/atomic.hpp"

Semaphore AsyncLogWriter::_sem(0);

class AsyncLogLocker : public StackObj {
 private:
  debug_only(static intx _locking_thread_id;)
  static Semaphore _lock;
 public:
  AsyncLogLocker() {
    _lock.wait();
    debug_only(_locking_thread_id = os::current_thread_id());
  }

  ~AsyncLogLocker() {
    debug_only(_locking_thread_id = -1);
    _lock.signal();
  }
};

Semaphore AsyncLogLocker::_lock(1);
debug_only(intx AsyncLogLocker::_locking_thread_id = -1;)

void AsyncLogWriter::enqueue_locked(const AsyncLogMessage& msg) {
  if (_buffer.size() >= _buffer_max_size)  {
    bool p_created;
    uint32_t* counter = _stats.add_if_absent(msg.output(), 0, &p_created);
    *counter = *counter + 1;
    // drop the enqueueing message.
    return;
  }

  assert(_buffer.size() < _buffer_max_size, "_buffer is over-sized.");
  _buffer.push_back(msg);
  _sem.signal();
}

void AsyncLogWriter::enqueue(LogFileOutput& output, const LogDecorations& decorations, const char* msg) {
  AsyncLogMessage m(output, decorations, os::strdup(msg));

  { // critical area
    AsyncLogLocker lock;
    enqueue_locked(m);
  }
}

// LogMessageBuffer consists of a multiple-part/multiple-line messsages.
// the mutex here gurantees its integrity.
void AsyncLogWriter::enqueue(LogFileOutput& output, LogMessageBuffer::Iterator msg_iterator) {
  AsyncLogLocker lock;

  for (; !msg_iterator.is_at_end(); msg_iterator++) {
    AsyncLogMessage m(output, msg_iterator.decorations(), os::strdup(msg_iterator.message()));
    enqueue_locked(m);
  }
}

AsyncLogWriter::AsyncLogWriter()
  : _state(ThreadState::NotReady),
    _stats(17 /*table_size*/) {
  if (os::create_thread(this, os::asynclog_thread)) {
    _state = ThreadState::Initialized;
  }

  log_info(logging)("The maximum entries of AsyncLogBuffer: " SIZE_FORMAT ", estimated memory use: " SIZE_FORMAT " bytes",
                    _buffer_max_size, AsyncLogBufferSize);
}

class AsyncLogMapIterator {
  AsyncLogBuffer& _logs;

 public:
  AsyncLogMapIterator(AsyncLogBuffer& logs) :_logs(logs) {}
  bool do_entry(LogFileOutput* output, uint32_t* counter) {
    using none = LogTagSetMapping<LogTag::__NO_TAG>;

    if (*counter > 0) {
      LogDecorations decorations(LogLevel::Warning, none::tagset(), output->decorators());
      stringStream ss;
      ss.print(UINT32_FORMAT_W(6) " messages dropped due to async logging", *counter);
      AsyncLogMessage msg(*output, decorations, ss.as_string(true /*c_heap*/));
      _logs.push_back(msg);
      *counter = 0;
    }

    return true;
  }
};

void AsyncLogWriter::perform_IO() {
  // Use kind of copy-and-swap idiom here.
  // Empty 'logs' swaps the content with _buffer.
  // Along with logs destruction, all procceeded messages are deleted.
  //
  // The operation 'pop_all()' is done in O(1). All I/O jobs are then performed without
  // lock protection. This guarantees I/O jobs don't block logsites.
  AsyncLogBuffer logs;
  { // critical region
    AsyncLogLocker ml;
    AsyncLogMapIterator dropped_counters_iter(logs);

    _buffer.pop_all(&logs);
    // append meta-messages of dropped counters
    _stats.iterate(&dropped_counters_iter);
  }

  LinkedListIterator<AsyncLogMessage> it(logs.head());
  while (!it.is_empty()) {
    AsyncLogMessage* e = it.next();
    char* msg = e->message();

    if (msg != nullptr) {
      e->output()->write_blocking(e->decorations(), msg);
      os::free(msg);
    }
  }
}

void AsyncLogWriter::run() {
  assert(_state == ThreadState::Running, "sanity check");

  while (_state == ThreadState::Running) {
    _sem.wait();
    perform_IO();
  }

  assert(_state == ThreadState::Terminated, "sanity check");
  perform_IO(); // in case there are some messages left
}

AsyncLogWriter* AsyncLogWriter::_instance = nullptr;

void AsyncLogWriter::initialize() {
  if (!LogConfiguration::is_async_mode()) return;

  if (_instance == nullptr) {
    AsyncLogWriter* self = new AsyncLogWriter();

    if (self->_state == ThreadState::Initialized) {
      Atomic::release_store_fence(&AsyncLogWriter::_instance, self);
      // All readers of _instance after the fence see non-NULL.
      // We make use LogOutputList's RCU counters to ensure all synchronous logsites have completed.
      // After that, we start AsyncLog Thread and it exclusively takee over all logging I/O.
      for (LogTagSet* ts = LogTagSet::first(); ts != NULL; ts = ts->next()) {
        ts->wait_until_no_readers();
      }
      self->_state = ThreadState::Running;
      os::start_thread(self);
      log_debug(logging, thread)("AsyncLogging starts working.");
    } else {
      log_warning(logging, thread)("AsyncLogging failed to launch thread. fall back to synchronous logging.");
    }
  }
}

AsyncLogWriter* AsyncLogWriter::instance() {
  return _instance;
}

void AsyncLogWriter::flush() {
  if (_instance != nullptr) {
    _instance->perform_IO();
  }
}
