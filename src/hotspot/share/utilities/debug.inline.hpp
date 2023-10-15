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


#ifndef SHARE_UTILITIES_DEBUG_INLINE_HPP
#define SHARE_UTILITIES_DEBUG_INLINE_HPP

#include "runtime/atomic.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "services/heapDumper.hpp"
#include "utilities/vmError.hpp"

extern int g_out_of_memory_reported;
// out of memory reporting
// Function printout writes out extra information before terminating VM.
//
template<typename Function>
void report_java_out_of_memory(const char* message, Function printout) {
  // A number of threads may attempt to report OutOfMemoryError at around the
  // same time. To avoid dumping the heap or executing the data collection
  // commands multiple times we just do it once when the first threads reports
  // the error.
  if (Atomic::cmpxchg(&g_out_of_memory_reported, 0, 1) == 0) {
    // create heap dump before OnOutOfMemoryError commands are executed
    if (HeapDumpOnOutOfMemoryError) {
      tty->print_cr("java.lang.OutOfMemoryError: %s", message);
      HeapDumper::dump_heap_from_oome();
    }

    if (OnOutOfMemoryError && OnOutOfMemoryError[0]) {
      printout(); // it's possible that OnOutOfMemoryError is 'kill -9 %p', which kills itself, so we also invoke printout().
      VMError::report_java_out_of_memory(message);
    }

    if (CrashOnOutOfMemoryError) {
      printout();
      tty->print_cr("Aborting due to java.lang.OutOfMemoryError: %s", message);
      report_fatal(OOM_JAVA_HEAP_FATAL, __FILE__, __LINE__, "OutOfMemory encountered: %s", message);
    }

    if (ExitOnOutOfMemoryError) {
      printout();
      tty->print_cr("Terminating due to java.lang.OutOfMemoryError: %s", message);
      os::_exit(3); // quick exit with no cleanup hooks run
    }
  }
}
#endif //SHARE_UTILITIES_DEBUG_INLINE_HPP
