// wifi_startup.hpp - the µOS++ start-up every rpi-zero-2w test carries (the same
// code as sd_test): the UART, the heap, the exception vectors, and the main
// thread. Include it once, at the end of a test's main.cpp.
#pragma once

#include <cmsis-plus/rtos/os.h>

#include <exception_handler.hpp>
#include <uart.hpp>

#include <cstdlib>

extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void
  os_startup_initialize_hardware (void)
  {
    uart::uart1.init ();
    os_startup_initialize_free_store (
        __heap_start, static_cast<std::size_t> (__heap_end - __heap_start));
    exception::init ();
  }

  [[noreturn]] static void
  custom_main_trampoline (void)
  {
    std::exit (os_main (0, nullptr));
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int
  main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*> (__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    os::rtos::scheduler::initialize ();

    static os::rtos::thread::stack::element_t main_stack[32768];
    os::rtos::thread::attributes attr = os::rtos::thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof (main_stack);
    static os::rtos::thread main_thread{
      "main", reinterpret_cast<os::rtos::thread::func_t> (custom_main_trampoline),
      nullptr, attr
    };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    os::rtos::scheduler::start ();
    return 0;
  }
}
