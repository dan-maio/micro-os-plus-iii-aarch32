// The harness suite "mutex-stress".
//
// Its sources are not here: they live in the harness,
// tests/sources/mutex-stress/, and are named by board_test_libs() in
// test/rpi3b/tests.cmake. This file exists because the port's builder builds
// one application per directory that has a *.cpp, and this directory is where
// the application's NAME comes from -- test/rpi3b/mutex-stress/ -- so that the
// platform registers mutex-stress-qemu and mutex-stress-hwd like every other
// test.
//
// It also gives the suite the verdict a board run needs. A suite RETURNS its
// code from os_main(); hw.sh reads a RESULT line. The suite is linked with
// -Wl,--wrap=os_main (aarch32-rpi3b-harness-suite in tests.cmake), so the
// platform trampoline's call to os_main() lands here: run the suite, print the
// line the port's own tests print on both channels, and end the run.

#include <cstdlib>

#include <uart.hpp>
#include <semihosting.hpp>

extern "C" int
__real_os_main (int argc, char* argv[]);

extern "C" int
__wrap_os_main (int argc, char* argv[]);

extern "C" int
__wrap_os_main (int argc, char* argv[])
{
  int code = __real_os_main (argc, argv);

  // The UART terminal sees the verdict...
  uart::uart1.puts_uart (code == 0 ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");

  // ...and OpenOCD, which is what hw.sh greps, sees it on the semihosting
  // channel, because report_result() writes there and then stops the run with
  // the semihosting SYS_EXIT -- exactly what the port's own hw tests do.
  //
  // Do NOT return to the trampoline's std::exit(): on this SMP port that runs
  // the atexit handlers and the C++ static destructors while the suite still
  // has threads and mutexes alive, and the run then blocks instead of ending
  // (observed: QEMU sat until the runner's timeout with RESULT already on the
  // wire). report_result() bypasses all of it, as the port's report_result()
  // tests already did.
#if defined(SEMIHOST)
  semihosting::report_result (code == 0);
#else
  std::exit (code);
#endif
  return code;
}
