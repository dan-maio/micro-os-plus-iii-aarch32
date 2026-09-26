# -----------------------------------------------------------------------------
# What this board's tests need.
#
# Which tests exist is the directory listing beside this file. This file is
# only for what a directory listing cannot say: which tests link the board's
# driver set, which bring their own start-up code, and -- for the six carried
# over from the predecessor that drive the USB gadget, the C++ FatFs or the
# Cortex-M0 -- exactly which sources they compile.
#
# That last one has to be written down. smp_test6 and smp_test7 each ship a
# DIFFERENT usb_vendor_gadget.cpp, and smp_test_int4 a different sdmmc.cpp,
# from the ones the board carries in test/boards/luckfox-lyra/. Which copy a
# test compiles is a decision the predecessor's Makefiles made per test, and
# this file makes it the same way. A glob would link both and the build would
# fail on duplicate symbols -- or, worse, pick one.
#
# smp_test_int and smp_test_int2 are this board's own, carried over byte for
# byte: they exercise the GIC-400 SGI paths, which no other board has.
# -----------------------------------------------------------------------------

# Where this file is, captured now: inside a function, CMAKE_CURRENT_LIST_DIR
# is the directory of whoever is calling.
set (_lyra_tests "${CMAKE_CURRENT_LIST_DIR}")

# micro-os-plus::devices-rk3506 -- the RK3506 SD host under the flatfs/FatFs
# layers the Pi shares. smp_test5 and smp_test7 take it for sdcard::SdMmc,
# which the C++ FatFs disk glue calls. smp_test_int4 does NOT: it drives the
# card from its own interrupt-driven sdmmc.cpp, and linking both copies would
# define sdcard::SdMmc twice.
set (BOARD_TEST_NEED_DEVICES
     sd_test smp-mat-sdcard-test smp-num-test smp-pipeline-test
     smp_test5 smp_test7)

# Carried over whole from the predecessor, each with its own
# smp_install_boot_threads(). Adding the board's shared copy from src/ would
# be a duplicate definition.
set (BOARD_TEST_SELF_CONTAINED
     smp_test_int smp_test_int2
     smp_test5 smp_test6 smp_test7
     smp_test_int3 smp_test_int4 smp_test_int5)

# The -D flags the predecessor's Makefiles carried for these three. They are
# not defaults the sources fall back on: each selects a diagnostic or a
# scheduling behaviour the test was run and reported with.
function (board_test_defines _app _out)
  set (_d "")
  # The USB tests keep their console on the UART (UOS_CONSOLE_UART_ONLY, see
  # the board's uart.hpp): only the verdict goes through semihosting.
  if (_app STREQUAL "smp_test6" OR _app STREQUAL "smp_test7")
    set (_d UOS_CONSOLE_UART_ONLY)
  elseif (_app STREQUAL "smp_test_int3")
    set (_d SMP_TEST_INT3_DIAG SMP_TEST_INT3_DYNAMIC_AFFINITY
            UOS_CONSOLE_UART_ONLY)
  elseif (_app STREQUAL "smp_test_int4")
    set (_d SMP_TEST_INT4_DIAG SMP_TEST_INT4_DYNAMIC_AFFINITY
            UOS_CONSOLE_UART_ONLY)
  elseif (_app STREQUAL "smp_test_int5")
    set (_d SMP_TEST_INT5_SELFTRIG)
  endif ()
  set (${_out} "${_d}" PARENT_SCOPE)
endfunction ()

# Beyond each test's own main.cpp: the board's carried code it takes, and the
# subdirectory it brings itself. One entry per test, from that test's Makefile.
#
#   smp_test5      C++ FatFs over the board's SD driver
#   smp_test6      USB vendor gadget: the eight shared DWC2 sources plus its
#                  own gadget, DMA pool and environment
#   smp_test7      the same eight, its own three, and the C++ FatFs: it
#                  streams a file off the card over USB
#   smp_test_int3  none of the shared USB sources -- its usb1_int3.cpp drives
#                  the DWC2 registers directly, from the interrupt
#   smp_test_int4  the same, plus its own interrupt-driven SD driver and disk
#                  glue under the shared C++ FatFs
#   smp_test_int5  m0_blob.S, which embeds the Cortex-M0 firmware built below
function (board_test_sources _app _out)
  set (_s "")
  if (_app STREQUAL "smp_test5")
    set (_s ${UOS_BOARD_FATFS_CPP_SOURCES} ${UOS_BOARD_FATFS_CPP_DISKIO})
  elseif (_app STREQUAL "smp_test6")
    set (_s ${UOS_BOARD_USB_CORE_SOURCES}
            "${_lyra_tests}/smp_test6/usb/usb_vendor_gadget.cpp"
            ${UOS_BOARD_USB_GADGET_SOURCES})
  elseif (_app STREQUAL "smp_test7")
    set (_s ${UOS_BOARD_USB_CORE_SOURCES}
            "${_lyra_tests}/smp_test7/usb/usb_vendor_gadget.cpp"
            ${UOS_BOARD_USB_GADGET_SOURCES}
            ${UOS_BOARD_FATFS_CPP_SOURCES} ${UOS_BOARD_FATFS_CPP_DISKIO})
  elseif (_app STREQUAL "smp_test_int3")
    set (_s ${UOS_BOARD_USB_INT_SOURCES}
            "${_lyra_tests}/smp_test_int3/usb/usb_env_int3.cpp")
  elseif (_app STREQUAL "smp_test_int4")
    set (_s ${UOS_BOARD_USB_INT_SOURCES}
            "${_lyra_tests}/smp_test_int4/usb/usb_env_int3.cpp"
            "${_lyra_tests}/smp_test_int4/sd/sdmmc.cpp"
            "${_lyra_tests}/smp_test_int4/sd/diskio_sd_irq.cpp"
            ${UOS_BOARD_FATFS_CPP_SOURCES})
  elseif (_app STREQUAL "smp_test_int5")
    set (_s "${_lyra_tests}/smp_test_int5/m0_blob.S")
  endif ()
  set (${_out} "${_s}" PARENT_SCOPE)
endfunction ()

# smp_test_int4's own sd/ comes first, so its sdmmc.hpp wins over the board's.
function (board_test_includes _app _out)
  set (_i "")
  if (_app STREQUAL "smp_test5")
    set (_i ${UOS_BOARD_FATFS_CPP_INCLUDE})
  elseif (_app STREQUAL "smp_test6")
    set (_i ${UOS_BOARD_USB_INCLUDE})
  elseif (_app STREQUAL "smp_test7")
    set (_i ${UOS_BOARD_USB_INCLUDE} ${UOS_BOARD_FATFS_CPP_INCLUDE})
  elseif (_app STREQUAL "smp_test_int3")
    set (_i ${UOS_BOARD_USB_INCLUDE})
  elseif (_app STREQUAL "smp_test_int4")
    set (_i "${_lyra_tests}/smp_test_int4/sd"
            ${UOS_BOARD_FATFS_CPP_INCLUDE} ${UOS_BOARD_USB_INCLUDE})
  endif ()
  set (${_out} "${_i}" PARENT_SCOPE)
endfunction ()

# -----------------------------------------------------------------------------
# The Cortex-M0 firmware smp_test_int5 starts.
#
# The RK3506's M0 is not one of the SMP cores -- different ISA, outside the A7
# coherency domain -- so it cannot be scheduled on. smp_test_int5 uses it as
# an interrupt SOURCE: the A7 copies this image to System SRAM at 0xFFF80000,
# releases the M0, and the M0 raises the mailbox interrupt the test counts.
#
# It is a second, tiny firmware inside this build: same arm-none-eabi
# toolchain, -mcpu=cortex-m0, its own linker script. m0_blob.S puts the
# result in .rodata with `.incbin "m0/m0_fw.bin"`, which the assembler
# resolves against its own include path -- hence the -Wa,-I below.
set (_m0_src "${_lyra_tests}/smp_test_int5/m0")
set (_m0_out "${CMAKE_CURRENT_BINARY_DIR}/m0")
set (_m0_bin "${_m0_out}/m0_fw.bin")

add_custom_command (
  OUTPUT "${_m0_bin}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_m0_out}"
  COMMAND "${CMAKE_C_COMPILER}"
          -mcpu=cortex-m0 -mthumb -Os -ffreestanding -fno-builtin
          -ffunction-sections -fdata-sections -Wall -Wextra -g3
          -nostdlib "-T${_m0_src}/linker.ld" -Wl,--gc-sections
          "${_m0_src}/m0_start.c" "${_m0_src}/m0_main.c"
          -o "${_m0_out}/m0_fw.elf"
  COMMAND "${CMAKE_OBJCOPY}" -O binary "${_m0_out}/m0_fw.elf" "${_m0_bin}"
  DEPENDS "${_m0_src}/m0_start.c" "${_m0_src}/m0_main.c" "${_m0_src}/linker.ld"
  COMMENT "Building the Cortex-M0 firmware for smp_test_int5"
  VERBATIM
)
add_custom_target (m0-firmware ALL DEPENDS "${_m0_bin}")

set_source_files_properties (
  "${_lyra_tests}/smp_test_int5/m0_blob.S"
  PROPERTIES
    OBJECT_DEPENDS  "${_m0_bin}"
    COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_BINARY_DIR}"
)

# -----------------------------------------------------------------------------
# The carried DWC2 gadget stack is compiled at -O0, exactly as the predecessor's
# Makefiles compiled it.
#
# This is not a preference and not a workaround we could drop: the source under
# usb/ is BYTE-IDENTICAL to the predecessor's cortex-a7 stack, which enumerates
# and runs on this board -- but only at -O0. Built here at the -O2 every other
# application gets, the same source stops completing USB enumeration: the host
# reads the device descriptor, then resets and reads it again, never advancing
# to the configuration descriptor. Rebuilding just these sources at -O0 removes
# the failure, and the -O0 object code is instruction-for-instruction the
# predecessor's working image (verified with objdump).
#
# The two mechanisms the port already had to work around were only the visible
# edge of this. Driver::instance() is defined inline in usb/include/
# dwc2_driver.hpp while smp_test6/smp_test7 declare it out of line; at -O0 an
# inline function also gets one out-of-line copy, so the tests' declaration
# always linked. At -O2 the copy is not emitted and --fkeep-inline-functions--
# was needed to bring it back. The EP0/descriptor path has the same shape: at
# -O2 the optimizer rewrites it into something the hardware will not enumerate
# with, and -O0 is the only setting under which it was ever validated.
#
# Scoped to the gadget stack alone: the kernel, the ports and every other test
# keep the -O2 this repository builds with. The application's own descriptor
# file (its usb_vendor_gadget.cpp) is part of the same stack and takes the same
# level, so both smp_test6 and smp_test7 are covered.
set_source_files_properties (
  ${UOS_BOARD_USB_CORE_SOURCES}
  ${UOS_BOARD_USB_GADGET_SOURCES}
  "${_lyra_tests}/smp_test6/usb/usb_vendor_gadget.cpp"
  "${_lyra_tests}/smp_test7/usb/usb_vendor_gadget.cpp"
  PROPERTIES COMPILE_OPTIONS "-O0"
)
