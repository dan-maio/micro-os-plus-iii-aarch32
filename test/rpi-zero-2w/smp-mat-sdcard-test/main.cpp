/*
 * smp-mat-sdcard-test (Raspberry Pi Zero 2W / BCM2837, 4× Cortex-A53) —
 * parallel block linear equation solver, N = 200, B = 20, OS_NCPU cores, with the
 * MATRICES STORED ON THE SD CARD instead of only in RAM. Storage is flatfs
 * over the raw disk.img (QEMU build) or the existing FAT32 boot partition via
 * FatFs with files under /tests (make HW=1, never formatted).
 * Derived from the sibling smp-mat-test (all-in-RAM, N=120/B=20).
 *
 * The coefficient matrix A (200×200 float32 = 160 KB) and the right-hand side b
 * live as files on the SD card:
 *
 *     A.mat   row-major float32 N×N matrix   (1,000,000 bytes)
 *     b.mat   float32 N-vector                (2,000 bytes)
 *     x_block.mat / xclassic.mat  float32 solutions written back to the card
 *
 * Flow (core 0 does all file I/O; the solver cores work in RAM):
 *   1. SD card init (Arasan EMMC/SDHCI) + open the SD volume (format a flatfs
 *      volume under QEMU; mount the EXISTING FAT32 partition on hardware).
 *   2. Generate a diagonally-dominant A and b in RAM, then STORE them to
 *      A.mat / b.mat on the card.
 *   3. LOAD A_block / b_block back from the card (the solver working copy —
 *      proving the solver's data comes off the SD card).
 *   4. PARALLEL BLOCK elimination (B=50, 10 row-blocks over OS_NCPU cores,
 *      generation-based spin barriers, core 0 does the diagonal LU +
 *      back substitution) -> x_block.
 *   5. Classical N×N LU ground truth on core 0, again loading A/b from the
 *      card -> x_classic.
 *   6. Compare x_block vs x_classic (max abs diff), residual ||A x - b||,
 *      times, and STORE both solutions back to the SD card (then read them
 *      back to verify the round trip).
 *
 * rpi-zero-2w adaptations vs the pico2 version:
 *   - the Cortex-A53 has a real multi-core global exclusive monitor, so the
 *     Barrier's internal lock is a plain LDREX/STREX flag via
 *     __atomic_test_and_set (no SIO hardware spinlock needed);
 *   - the microsecond clock is the ARM generic timer physical counter
 *     (CNTPCT via CP15), scaled by CNTFRQ;
 *   - the secondaries are brought up with the standard SMP path
 *     (smp_install_boot_threads + smp::start_secondary_cores);
 *   - SD card I/O uses this port's sd.hpp / flatfs.hpp drivers;
 *   - UART0 (PL011) + a GPIO LED via this port's uart.hpp / led.hpp.
 *
 * With B = 50, N/B = 10 blocks over OS_NCPU cores: the block count need NOT be a
 * multiple of the core count — the round-robin row-block assignment simply
 * distributes 10 blocks unevenly across the cores.
 *
 * Output on UART0 @115200; the LED blinks while the solver runs and becomes a
 * heartbeat after the summary.
 */

#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <led.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>
#include <timer_arm.hpp>
#include <sd.hpp>
#if defined(HW_BUILD)
#include <fatfs_hw.hpp>
#else
#include <flatfs.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see test_smpl/common/src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

using namespace os::rtos;

extern "C" unsigned port_cpu_id (void);

// ----------------------------------------------------------------------------
// Microsecond clock: ARM generic timer physical counter (CNTPCT) via CP15,
// scaled by CNTFRQ. 64-bit read; microseconds fit in 32 bits for this test.
// ----------------------------------------------------------------------------
namespace
{
  inline std::uint64_t
  cntpct (void)
  {
    // The architecture port owns the generic-timer counter; reading it
    // here would hard-code one ISA\'s register form.
    return timer_arm::get_count ();
  }

  std::uint32_t g_ticks_per_us = 19; // set in timer_init() from CNTFRQ

  void
  timer_init (void)
  {
    std::uint32_t f = timer_arm::get_freq (); // Hz (19.2 MHz HW, ~62.5 MHz QEMU)
    g_ticks_per_us = f ? (f / 1000000U) : 19U;
    if (g_ticks_per_us == 0)
      {
        g_ticks_per_us = 1;
      }
  }

  inline std::uint32_t
  timer_us32 (void)
  {
    return static_cast<std::uint32_t> (cntpct () / g_ticks_per_us);
  }

  // ---- Console helpers -----------------------------------------------------
  void
  write_str (const char* s)
  {
    uart::uart1.puts (s);
  }

  void
  led_toggle (void)
  {
    static bool on = false;
    on = !on;
    if (on)
      {
        led::on ();
      }
    else
      {
        led::off ();
      }
  }

  void
  write_fmt (const char* fmt, ...)
  {
    char b[96];
    std::va_list args;
    va_start (args, fmt);
    int n = std::vsnprintf (b, sizeof (b), fmt, args);
    va_end (args);
    if (n < 0)
      {
        n = 0;
      }
    if (n > static_cast<int> (sizeof (b)) - 1)
      {
        n = static_cast<int> (sizeof (b)) - 1;
      }
    uart::uart1.puts (b);
  }

  void
  print_float (float val)
  {
    char b[32];
    if (val < 0.0f)
      {
        uart::uart1.puts ("-");
        val = -val;
      }
    int int_part = static_cast<int> (val);
    int frac_part
        = static_cast<int> ((val - int_part) * 1000000.0f + 0.5f);
    if (frac_part >= 1000000)
      {
        int_part = int_part + 1;
        frac_part = frac_part - 1000000;
      }
    int n = std::snprintf (b, sizeof (b), "%d.%06d", int_part, frac_part);
    uart::uart1.puts (b);
  }

  void
  print_time_us (double time_us)
  {
    char b[32];
    int int_part = static_cast<int> (time_us);
    int frac_part
        = static_cast<int> ((time_us - int_part) * 1000.0 + 0.5);
    if (frac_part >= 1000)
      {
        int_part = int_part + 1;
        frac_part = frac_part - 1000;
      }
    int n = std::snprintf (b, sizeof (b), "%d.%03d us", int_part, frac_part);
    uart::uart1.puts (b);
  }

  void
  print_ratio (double ratio)
  {
    char b[32];
    int int_part = static_cast<int> (ratio);
    int frac_part
        = static_cast<int> ((ratio - int_part) * 1000.0 + 0.5);
    if (frac_part >= 1000)
      {
        int_part = int_part + 1;
        frac_part = frac_part - 1000;
      }
    int n = std::snprintf (b, sizeof (b), "%d.%03d", int_part, frac_part);
    uart::uart1.puts (b);
  }
}

// ----------------------------------------------------------------------------
// Solver dimensions
// ----------------------------------------------------------------------------
#define N 200
#define B 20

// ----------------------------------------------------------------------------
// Generation-based spin barrier. On the Cortex-A53 the global exclusive monitor
// makes LDREX/STREX genuinely cross-core, so the internal lock is a plain
// __atomic_test_and_set flag (compiles to ldaxrb/stlxrb) — no SIO spinlock.
// ----------------------------------------------------------------------------
class Barrier
{
public:
  void
  init (int count, std::uint32_t /*unused_slot*/)
  {
    target_count = count;
    current_count = 0;
    generation = 0;
    lock_ = 0;
  }

  void
  wait (void)
  {
    __asm__ volatile ("dmb ish" ::: "memory");
    int gen = generation;

    while (__atomic_test_and_set (&lock_, __ATOMIC_ACQUIRE))
      {
        this_thread::yield ();
      }

    current_count = current_count + 1;
    if (current_count == target_count)
      {
        current_count = 0;
        generation = generation + 1;
        __atomic_clear (&lock_, __ATOMIC_RELEASE);
      }
    else
      {
        __atomic_clear (&lock_, __ATOMIC_RELEASE);
        while (generation == gen)
          {
            this_thread::yield ();
          }
      }

    __asm__ volatile ("dmb ish" ::: "memory");
  }

private:
  int target_count;
  volatile int current_count;
  volatile int generation;
  volatile unsigned char lock_;
};

// ----------------------------------------------------------------------------
// Thread Pool (10 workers, cross-core tasks)
// ----------------------------------------------------------------------------
typedef void (*task_t) (void* arg);

struct Task
{
  task_t func;
  void* arg;
};

#define POOL_SIZE 10
#define TASK_QUEUE_SIZE 32

class ThreadPool
{
public:
  ThreadPool ()
      : sem_tasks_ ("pool_tasks", TASK_QUEUE_SIZE, 0),
        sem_free_ ("pool_free", TASK_QUEUE_SIZE, TASK_QUEUE_SIZE)
  {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
  }

  void start ();
  bool submit (task_t func, void* arg);
  void worker_loop ();

private:
  Task queue_[TASK_QUEUE_SIZE];
  int head_;
  int tail_;
  int count_;

  mutex mutex_;
  semaphore_counting sem_tasks_;
  semaphore_counting sem_free_;
};

static ThreadPool g_pool;
static char pool_thread_names[POOL_SIZE][16];
static thread::stack::element_t pool_stacks[POOL_SIZE][512];
static thread* pool_threads[POOL_SIZE];

static void*
pool_worker_func (void* arg)
{
  ThreadPool* pool = static_cast<ThreadPool*> (arg);
  pool->worker_loop ();
  return nullptr;
}

void
ThreadPool::start ()
{
  for (int i = 0; i < POOL_SIZE; ++i)
    {
      thread::attributes attr = thread::initializer;
      attr.th_stack_address = pool_stacks[i];
      attr.th_stack_size_bytes = sizeof (pool_stacks[i]);

      std::snprintf (pool_thread_names[i], 16, "pool_%02d", i);

      pool_threads[i]
          = new thread (pool_thread_names[i], pool_worker_func, this, attr);
      // Alternate workers across the two cores.
      pool_threads[i]->cpu_affinity (1u << (i % OS_NCPU));
    }
}

bool
ThreadPool::submit (task_t func, void* arg)
{
  sem_free_.wait ();

  mutex_.lock ();
  queue_[tail_].func = func;
  queue_[tail_].arg = arg;
  tail_ = (tail_ + 1) % TASK_QUEUE_SIZE;
  count_ = count_ + 1;
  mutex_.unlock ();

  sem_tasks_.post ();
  return true;
}

void
ThreadPool::worker_loop ()
{
  while (true)
    {
      sem_tasks_.wait ();

      mutex_.lock ();
      Task task = queue_[head_];
      head_ = (head_ + 1) % TASK_QUEUE_SIZE;
      count_ = count_ - 1;
      mutex_.unlock ();

      sem_free_.post ();

      if (task.func)
        {
          task.func (task.arg);
        }
    }
}

static volatile int g_pool_task_count = 0;
static mutex g_pool_print_mutex;

static void
pool_demo_task (void* arg)
{
  int id = static_cast<int> (reinterpret_cast<intptr_t> (arg));
  volatile int sum = 0;
  for (int i = 0; i < 10000; ++i)
    {
      sum = sum + i;
    }
  g_pool_print_mutex.lock ();
  write_fmt ("Thread Pool: Worker executed task %d on CPU %u\n", id,
             port_cpu_id ());
  g_pool_task_count = g_pool_task_count + 1;
  g_pool_print_mutex.unlock ();
}

// ----------------------------------------------------------------------------
// Solver globals
// ----------------------------------------------------------------------------
static Barrier g_barrier_lu;
static Barrier g_barrier_step;
static volatile bool g_solver_done = false;
static volatile std::uint32_t g_block_start_us = 0;
static volatile std::uint32_t g_block_end_us = 0;

// The generated A / b (also kept for the residual check).
static float A[N][N];
static float b[N];

// Solver working copy - loaded from the SD card (A.mat / b.mat).
static float A_block[N][N];
static float b_block[N];
static float x_block[N];

// Classical-solve copy - also loaded from the SD card.
static float A_classic[N][N];
static float b_classic[N];
static float x_classic[N];

static float LU_k_T[B][B];
static int pivot_k_T[B];

static thread::stack::element_t solver_stacks[OS_NCPU][1024];
static thread* solver_threads[OS_NCPU];
static char solver_thread_names[OS_NCPU][16];

// ----------------------------------------------------------------------------
// SD-card matrix store. All file I/O happens on core 0.
//   - HW build (HW_BUILD): FatFs on the existing FAT32 boot partition, files
//     under /tests (never formatted).
//   - QEMU build (!HW_BUILD): flatfs over the raw sectors of disk.img.
// ----------------------------------------------------------------------------
static sd::SdCard g_card;
#if defined(HW_BUILD)
#else
static flatfs::FlatFs g_fs;
#endif

static const char kFileA[] = "A.mat";
static const char kFileB[] = "b.mat";
static const char kFileXb[] = "x_block.mat";
static const char kFileXc[] = "xclassic.mat"; // 8.3-safe (FatFs, no LFN)

static bool
sd_store (const char* name, const void* data, std::uint32_t bytes)
{
#if defined(HW_BUILD)
  if (!fatfshw::store_file (name, data, bytes))
    {
      write_fmt ("    SD store %s failed: %s\n", name, fatfshw::last_error ());
      return false;
    }
  return true;
#else
  const flatfs::FlatFs::Result r = g_fs.write_file (name, data, bytes);
  if (r != flatfs::FlatFs::Result::ok)
    {
      write_fmt ("    SD store %s failed: %s\n", name,
                 flatfs::FlatFs::result_str (r));
      return false;
    }
  return true;
#endif
}

static bool
sd_load (const char* name, void* data, std::uint32_t bytes)
{
#if defined(HW_BUILD)
  if (!fatfshw::load_file (name, data, bytes))
    {
      write_fmt ("    SD load %s: wrong/missing file\n", name);
      return false;
    }
  return true;
#else
  std::uint32_t size = 0;
  if (g_fs.file_size (name, size) != flatfs::FlatFs::Result::ok
      || size != bytes)
    {
      write_fmt ("    SD load %s: wrong/missing file (%u != %u)\n", name,
                 size, bytes);
      return false;
    }
  if (g_fs.read_file (name, data, bytes, nullptr)
      != flatfs::FlatFs::Result::ok)
    {
      write_fmt ("    SD load %s failed\n", name);
      return false;
    }
  return true;
#endif
}

static void
fatal (const char* msg)
{
  write_fmt ("FATAL: %s\n", msg);
  hw_result::fail ();
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
}

// ----------------------------------------------------------------------------
// LU utilities for B x B blocks
// ----------------------------------------------------------------------------
static bool
lu_decompose (const float M[B][B], float LU[B][B], int pivot[B])
{
  for (int i = 0; i < B; ++i)
    {
      for (int j = 0; j < B; ++j)
        {
          LU[i][j] = M[i][j];
        }
      pivot[i] = i;
    }
  for (int i = 0; i < B; ++i)
    {
      float max_val = 0.0f;
      int max_row = i;
      for (int r = i; r < B; ++r)
        {
          float val = std::abs (LU[r][i]);
          if (val > max_val)
            {
              max_val = val;
              max_row = r;
            }
        }
      if (max_val < 1e-9f)
        {
          return false; // Singular
        }
      if (max_row != i)
        {
          std::swap (pivot[i], pivot[max_row]);
          for (int c = 0; c < B; ++c)
            {
              std::swap (LU[i][c], LU[max_row][c]);
            }
        }
      for (int r = i + 1; r < B; ++r)
        {
          float factor = LU[r][i] / LU[i][i];
          LU[r][i] = factor;
          for (int c = i + 1; c < B; ++c)
            {
              LU[r][c] = LU[r][c] - factor * LU[i][c];
            }
        }
    }
  return true;
}

static void
lu_solve (const float LU[B][B], const int pivot[B], const float Y[B],
          float X[B])
{
  float temp[B];
  for (int i = 0; i < B; ++i)
    {
      temp[i] = Y[pivot[i]];
      for (int j = 0; j < i; ++j)
        {
          temp[i] = temp[i] - LU[i][j] * temp[j];
        }
    }
  for (int i = B - 1; i >= 0; --i)
    {
      X[i] = temp[i];
      for (int j = i + 1; j < B; ++j)
        {
          X[i] = X[i] - LU[i][j] * X[j];
        }
      X[i] = X[i] / LU[i][i];
    }
}

// Solve A_T * M_T = Src_T and transpose the result back to Dest.
static void
solve_block_transpose (const float LU_T[B][B], const int pivot_T[B],
                       const float Src[B][B], float Dest[B][B],
                       float Src_T[B][B], float M_T[B][B])
{
  for (int i = 0; i < B; ++i)
    {
      for (int j = 0; j < B; ++j)
        {
          Src_T[i][j] = Src[j][i];
        }
    }
  for (int col = 0; col < B; ++col)
    {
      float y[B];
      float x[B];
      for (int r = 0; r < B; ++r)
        {
          y[r] = Src_T[r][col];
        }
      lu_solve (LU_T, pivot_T, y, x);
      for (int r = 0; r < B; ++r)
        {
          M_T[r][col] = x[r];
        }
    }
  for (int i = 0; i < B; ++i)
    {
      for (int j = 0; j < B; ++j)
        {
          Dest[i][j] = M_T[j][i];
        }
    }
}

static void
mat_mul_sub (const float M[B][B], const float X[B][B], float Dest[B][B])
{
  for (int i = 0; i < B; ++i)
    {
      for (int j = 0; j < B; ++j)
        {
          float sum = 0.0f;
          for (int k = 0; k < B; ++k)
            {
              sum = sum + M[i][k] * X[k][j];
            }
          Dest[i][j] = Dest[i][j] - sum;
        }
    }
}

static void
vec_mul_sub (const float M[B][B], const float v[B], float Dest[B])
{
  for (int i = 0; i < B; ++i)
    {
      float sum = 0.0f;
      for (int k = 0; k < B; ++k)
        {
          sum = sum + M[i][k] * v[k];
        }
      Dest[i] = Dest[i] - sum;
    }
}

static void
get_matrix_block (const float Src[N][N], int br, int bc, float Dest[B][B])
{
  for (int i = 0; i < B; ++i)
    {
      for (int j = 0; j < B; ++j)
        {
          Dest[i][j] = Src[br * B + i][bc * B + j];
        }
    }
}

static void
set_matrix_block (float Dest[N][N], int br, int bc, const float Src[B][B])
{
  for (int i = 0; i < B; ++i)
    {
      for (int j = 0; j < B; ++j)
        {
          Dest[br * B + i][bc * B + j] = Src[i][j];
        }
    }
}

// ----------------------------------------------------------------------------
// Per-core solver worker (block parallel elimination)
// ----------------------------------------------------------------------------
static void*
solver_thread_func (void* arg)
{
  int core_id = static_cast<int> (reinterpret_cast<intptr_t> (arg));
  int num_blocks = N / B;

  // Per-core workspace (static to keep thread stacks tiny). A_kj doubles as
  // the A_ik scratch: it is only needed again after solve_block_transpose.
  static float workspace_Src_T[OS_NCPU][B][B];
  static float workspace_M_T[OS_NCPU][B][B];
  static float M_local[OS_NCPU][B][B];
  static float A_kj[OS_NCPU][B][B];
  static float A_ij[OS_NCPU][B][B];

  if (core_id == 0)
    {
      g_block_start_us = timer_us32 ();
    }

  for (int k = 0; k < num_blocks - 1; ++k)
    {
      if (core_id == 0)
        {
          // Core 0: LU-decompose the diagonal block A_kk (transposed layout).
          static float A_kk_T[B][B];
          for (int i = 0; i < B; ++i)
            {
              for (int j = 0; j < B; ++j)
                {
                  A_kk_T[i][j] = A_block[k * B + j][k * B + i];
                }
            }
          lu_decompose (A_kk_T, LU_k_T, pivot_k_T);
        }

      g_barrier_lu.wait ();

      // All cores: compute multipliers + update their assigned row blocks.
      for (int i = k + 1; i < num_blocks; ++i)
        {
          if ((i - (k + 1)) % OS_NCPU == core_id)
            {
              // A_ik = A_block[i][k]
              get_matrix_block (A_block, i, k, A_kj[core_id]);
              solve_block_transpose (
                  LU_k_T, pivot_k_T, A_kj[core_id], M_local[core_id],
                  workspace_Src_T[core_id], workspace_M_T[core_id]);

              // A_ij = A_ij - M_ik * A_kj  for j > k
              for (int j = k + 1; j < num_blocks; ++j)
                {
                  get_matrix_block (A_block, k, j, A_kj[core_id]);
                  get_matrix_block (A_block, i, j, A_ij[core_id]);
                  mat_mul_sub (M_local[core_id], A_kj[core_id],
                               A_ij[core_id]);
                  set_matrix_block (A_block, i, j, A_ij[core_id]);
                }

              // b_i = b_i - M_ik * b_k
              vec_mul_sub (M_local[core_id], &b_block[k * B],
                           &b_block[i * B]);
            }
        }

      g_barrier_step.wait ();
    }

  // Final block k = num_blocks - 1: sequential back substitution (core 0).
  if (core_id == 0)
    {
      static float A_ii[B][B];
      static float LU_ii[B][B];
      static float A_ij_temp[B][B];
      static int pivot_ii[B];
      static float temp_bi[B];

      for (int i = num_blocks - 1; i >= 0; --i)
        {
          for (int r = 0; r < B; ++r)
            {
              temp_bi[r] = b_block[i * B + r];
            }
          for (int j = i + 1; j < num_blocks; ++j)
            {
              get_matrix_block (A_block, i, j, A_ij_temp);
              for (int r = 0; r < B; ++r)
                {
                  for (int c = 0; c < B; ++c)
                    {
                      temp_bi[r] = temp_bi[r]
                                   - A_ij_temp[r][c] * x_block[j * B + c];
                    }
                }
            }
          get_matrix_block (A_block, i, i, A_ii);
          lu_decompose (A_ii, LU_ii, pivot_ii);
          lu_solve (LU_ii, pivot_ii, temp_bi, &x_block[i * B]);
        }

      g_block_end_us = timer_us32 ();
      g_solver_done = true;
    }

  g_barrier_lu.wait (); // sync all cores at exit

  for (;;)
    {
      sysclock.sleep_for (10000);
    }
  return nullptr;
}

// ----------------------------------------------------------------------------
// Classical N x N LU solve (ground truth, core 0)
// ----------------------------------------------------------------------------
static bool
solve_classical (const float src_A[N][N], const float src_b[N],
                 float dest_x[N])
{
  static float LU[N][N];
  static int pivot[N];
  float temp[N];

  for (int i = 0; i < N; ++i)
    {
      for (int j = 0; j < N; ++j)
        {
          LU[i][j] = src_A[i][j];
        }
      pivot[i] = i;
    }

  for (int i = 0; i < N; ++i)
    {
      float max_val = 0.0f;
      int max_row = i;
      for (int r = i; r < N; ++r)
        {
          float val = std::abs (LU[r][i]);
          if (val > max_val)
            {
              max_val = val;
              max_row = r;
            }
        }
      if (max_val < 1e-9f)
        {
          return false; // Singular
        }
      if (max_row != i)
        {
          std::swap (pivot[i], pivot[max_row]);
          for (int c = 0; c < N; ++c)
            {
              std::swap (LU[i][c], LU[max_row][c]);
            }
        }
      for (int r = i + 1; r < N; ++r)
        {
          float factor = LU[r][i] / LU[i][i];
          LU[r][i] = factor;
          for (int c = i + 1; c < N; ++c)
            {
              LU[r][c] = LU[r][c] - factor * LU[i][c];
            }
        }
    }

  // Forward substitution (L * temp = b_pivoted).
  for (int i = 0; i < N; ++i)
    {
      temp[i] = src_b[pivot[i]];
      for (int j = 0; j < i; ++j)
        {
          temp[i] = temp[i] - LU[i][j] * temp[j];
        }
    }

  // Backward substitution (U * x = temp).
  for (int i = N - 1; i >= 0; --i)
    {
      dest_x[i] = temp[i];
      for (int j = i + 1; j < N; ++j)
        {
          dest_x[i] = dest_x[i] - LU[i][j] * dest_x[j];
        }
      dest_x[i] = dest_x[i] / LU[i][i];
    }

  return true;
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
// Create the per-core idle threads for cores 1..3 before releasing them.
int
os_main (int /*argc*/, char* /*argv*/[])
{
  using namespace os::rtos;

  this_thread::thread ().cpu_affinity (1u << 0);

  led::init ();
  timer_init ();

  g_barrier_lu.init (OS_NCPU, 0);
  g_barrier_step.init (OS_NCPU, 0);
  g_solver_done = false;

  write_str ("\n");
  write_str ("=============================================\n");
  write_str ("  " PORT_BANNER_LONG " - " PORT_BANNER_CPU "\n");
  write_str ("  micro-os-plus-iii  smp-mat-sdcard-test\n");
  write_str ("  parallel block linear solver, matrices on SD\n");
  write_fmt ("  N = %d, B = %d, cores = %d\n", N, B, OS_NCPU);
  write_str ("=============================================\n");

  if (N % B != 0)
    {
      write_fmt ("ERROR: N (%d) must be divisible by B (%d)!\n", N, B);
      hw_result::fail ();
      for (;;)
        {
          __asm__ volatile ("wfi");
        }
    }
  const int num_blocks = N / B;
  // NOTE: num_blocks need NOT be a multiple of OS_NCPU - the round-robin row
  // assignment ((i-(k+1)) % OS_NCPU == core_id) simply distributes the blocks
  // unevenly. With N=200, B=20 -> 10 blocks over OS_NCPU cores.
  if (num_blocks % OS_NCPU != 0)
    {
      write_fmt ("NOTE: %d blocks over %d cores (uneven round-robin split).\n",
                 num_blocks, OS_NCPU);
    }

  // =========================================================================
  // SD card + flatfs matrix store. Done on core 0 BEFORE the secondary cores
  // are released (single-core SD traffic, no SMP churn slowing the PIO).
  // =========================================================================
  write_str ("Initialising SD card (Arasan EMMC/SDHCI)...\n");
  if (!g_card.init ())
    {
      fatal ("SD card init failed");
    }
  write_fmt ("  SD ready: %u sectors (~%u MiB)\n", g_card.sector_count (),
             static_cast<unsigned> (g_card.capacity_bytes () >> 20));
#if defined(HW_BUILD)
  fatfshw::bind_card (g_card);
  if (!fatfshw::mount_volume ())
    {
      fatal ("FAT32 mount failed");
    }
  if (!fatfshw::ensure_tests_dir ())
    {
      fatal ("mkdir tests failed");
    }
  write_str ("  FAT32 /tests ready (boot files untouched).\n");
#else
  if (g_fs.format (g_card) != flatfs::FlatFs::Result::ok)
    {
      fatal ("flatfs format failed");
    }
  write_str ("  flatfs volume formatted.\n");
#endif

  write_fmt ("Generating diagonally dominant %d x %d matrix...\n", N, N);
  srand (42);
  for (int i = 0; i < N; ++i)
    {
      for (int j = 0; j < N; ++j)
        {
          A[i][j]
              = -5.0f + static_cast<float> (rand ())
                            / (static_cast<float> (RAND_MAX) / 10.0f);
        }
    }
  for (int i = 0; i < N; ++i)
    {
      float sum = 0.0f;
      for (int j = 0; j < N; ++j)
        {
          if (i != j)
            {
              sum = sum + std::abs (A[i][j]);
            }
        }
      A[i][i] = sum + 5.0f; // strictly diagonally dominant
      b[i] = 1.0f;
    }
  write_str ("  matrix generated.\n");

  // Store the generated system to the SD card.
  write_str ("Storing A and b on the SD card (A.mat, b.mat)...\n");
  if (!sd_store (kFileA, A, sizeof (A)))
    {
      fatal ("A.mat store failed");
    }
  write_fmt ("  stored A.mat (%u bytes)\n", static_cast<unsigned> (sizeof (A)));
  if (!sd_store (kFileB, b, sizeof (b)))
    {
      fatal ("b.mat store failed");
    }
  write_fmt ("  stored b.mat (%u bytes)\n", static_cast<unsigned> (sizeof (b)));

  // Load the solver working copy back from the card.
  write_str ("Loading A_block/b_block from the SD card (solver working set)...\n");
  if (!sd_load (kFileA, A_block, sizeof (A_block)))
    {
      fatal ("solver A load from SD failed");
    }
  if (!sd_load (kFileB, b_block, sizeof (b_block)))
    {
      fatal ("solver b load from SD failed");
    }
  write_str ("  solver working set loaded from SD.\n");

  // Preload the classical-solve copy too (again from the card).
  write_str ("Loading A/b for the classical solve from the SD card...\n");
  if (!sd_load (kFileA, A_classic, sizeof (A_classic))
      || !sd_load (kFileB, b_classic, sizeof (b_classic)))
    {
      fatal ("classical matrix load from SD failed");
    }
  write_str ("  classical working set loaded from SD.\n\n");

  // =========================================================================
  // Bring up SMP (cores 1-3), then do the RAM-only compute phases.
  // =========================================================================
  smp_install_boot_threads ();
  write_str ("core 0: releasing the secondary cores...\n");
  smp::start_secondary_cores ();
  {
    int waited = 0;
    while ((g_core_stage[1] < 3 || g_core_stage[2] < 3 || g_core_stage[3] < 3)
           && waited < 3000)
      {
        sysclock.sleep_for (50);
        waited += 50;
      }
    write_fmt ("core 0: SMP up (c1=%u c2=%u c3=%u).\n", g_core_stage[1],
               g_core_stage[2], g_core_stage[3]);
  }

  // ---- Thread Pool demo ----------------------------------------------------
  write_str ("Starting Thread Pool of 10 worker threads...\n");
  g_pool.start ();

  write_str ("Submitting 20 tasks to the Thread Pool...\n");
  for (int i = 0; i < 20; ++i)
    {
      g_pool.submit (pool_demo_task, reinterpret_cast<void*> (i));
    }

  while (g_pool_task_count < 20)
    {
      sysclock.sleep_for (10);
    }
  write_str ("Thread Pool demo finished. All 20 tasks executed successfully.\n\n");

  // ---- Launch the parallel solver threads ----------------------------------
  write_str ("Starting parallel " TEST_NCPU_STR "-core block elimination (N=200, B=20)...\n");
  for (int c = 0; c < OS_NCPU; ++c)
    {
      thread::attributes attr = thread::initializer;
      attr.th_stack_address = solver_stacks[c];
      attr.th_stack_size_bytes = sizeof (solver_stacks[c]);

      std::snprintf (solver_thread_names[c], 16, "solver_c%d", c);

      solver_threads[c]
          = new thread (solver_thread_names[c], solver_thread_func,
                        reinterpret_cast<void*> (c), attr);
      solver_threads[c]->cpu_affinity (1u << c);
    }

  bool led_state = false;
  while (!g_solver_done)
    {
      sysclock.sleep_for (100);
      led_state = !led_state;
      if (led_state)
        {
          led::on ();
        }
      else
        {
          led::off ();
        }
    }
  led::off ();
  write_str ("Block elimination finished.\n");

  // ---- Classical solve (ground truth, from the SD-preloaded copy) ----------
  write_str ("Solving full system using classical elimination...\n");
  std::uint32_t classic_start = timer_us32 ();
  solve_classical (A_classic, b_classic, x_classic);
  std::uint32_t classic_end = timer_us32 ();
  write_str ("Classical elimination finished.\n\n");

  // ---- Performance + comparison --------------------------------------------
  std::uint32_t block_us = g_block_end_us - g_block_start_us;
  std::uint32_t classic_us = classic_end - classic_start;

  write_str ("+---------------------------------------------------+\n");
  write_str ("| COMPARING BLOCK ELIMINATION VS CLASSICAL SOLVER  |\n");
  write_str ("+---------------------------------------------------+\n");
  float max_diff = 0.0f;
  float sum_diff = 0.0f;
  for (int i = 0; i < N; ++i)
    {
      const float diff = std::abs (x_block[i] - x_classic[i]);
      sum_diff = sum_diff + diff;
      if (diff > max_diff)
        {
          max_diff = diff;
        }
    }

  // Residuals ||A x - b||_inf against the original A/b (still in RAM).
  float res_block = 0.0f;
  float res_classic = 0.0f;
  for (int i = 0; i < N; ++i)
    {
      float sb = 0.0f;
      float sc = 0.0f;
      for (int j = 0; j < N; ++j)
        {
          sb = sb + A[i][j] * x_block[j];
          sc = sc + A[i][j] * x_classic[j];
        }
      const float rb = std::abs (b[i] - sb);
      const float rc = std::abs (b[i] - sc);
      if (rb > res_block)
        {
          res_block = rb;
        }
      if (rc > res_classic)
        {
          res_classic = rc;
        }
    }

  // Print a representative sample (not all 500 rows).
  const int step = N / 40 + 1;
  for (int i = 0; i < N; i += step)
    {
      const float diff = std::abs (x_block[i] - x_classic[i]);
      write_str ("x[");
      write_fmt ("%d", i);
      write_str ("] Block = ");
      print_float (x_block[i]);
      write_str (" | Classic = ");
      print_float (x_classic[i]);
      write_str (" | Abs Diff = ");
      print_float (diff);
      write_str ("\n");
    }
  write_str ("-----------------------------------------------------\n");
  write_str ("Total Absolute Difference (Sum): ");
  print_float (sum_diff);
  write_str ("\n");
  write_str ("Max Absolute Difference:        ");
  print_float (max_diff);
  write_str ("\n");
  write_str ("Residual ||A*x_block - b||_inf: ");
  print_float (res_block);
  write_str ("\n");
  write_str ("Residual ||A*x_classic - b||_inf: ");
  print_float (res_classic);
  write_str ("\n");
  write_str ("-----------------------------------------------------\n");
  write_str ("Performance Metrics:\n");
  write_str ("  Parallel Block Solver: ");
  print_time_us (static_cast<double> (block_us));
  write_str ("\n");
  write_str ("  Classical Solver:      ");
  print_time_us (static_cast<double> (classic_us));
  write_str ("\n");
  write_str ("  Speedup Ratio:         ");
  print_ratio (classic_us > 0
                   ? static_cast<double> (classic_us)
                         / static_cast<double> (block_us)
                   : 0.0);
  write_str ("x\n");
  write_str ("+---------------------------------------------------+\n");

  // ---- Write both solutions back to the SD card and verify the round trip --
  static float x_chk[N];
  bool sd_ok = true;
  write_str ("Storing solutions on the SD card (x_block.mat, xclassic.mat)...\n");
  sd_ok = sd_store (kFileXb, x_block, sizeof (x_block)) && sd_ok;
  sd_ok = sd_store (kFileXc, x_classic, sizeof (x_classic)) && sd_ok;
#if defined(HW_BUILD)
  if (sd_ok && fatfshw::load_file (kFileXb, x_chk, sizeof (x_chk)))
    {
      sd_ok = (std::memcmp (x_chk, x_block, sizeof (x_block)) == 0);
    }
  else
    {
      sd_ok = false;
    }
#else
  if (sd_ok && g_fs.read_file (kFileXb, x_chk, sizeof (x_chk), nullptr)
          == flatfs::FlatFs::Result::ok)
    {
      sd_ok = (std::memcmp (x_chk, x_block, sizeof (x_block)) == 0);
    }
  else
    {
      sd_ok = false;
    }
#endif
  write_fmt ("  solution round-trip to/from SD: %s\n",
             sd_ok ? "OK" : "FAILED");

#if defined(HW_BUILD)
  write_str ("\ntests/ contents:\n");
  char listing[512];
  fatfshw::list_tests_dir (listing, sizeof (listing));
  write_str (listing);
#endif

  // Tolerances (float32, N=500, diagonally dominant well-conditioned system).
  const float tol_diff = 0.05f;
  const float tol_res = 0.05f;
  bool pass = (max_diff < tol_diff) && (res_block < tol_res)
              && (res_classic < tol_res) && sd_ok;
  write_fmt ("RESULT: %s\n",
             pass ? "PASS (block == classical within tolerance, SD round-trip OK)"
                  : "FAIL");
  write_str ("---------------------------------------------\n");
  if (pass)
    {
      hw_result::ok ();
    }
  else
    {
    hw_result::fail ();
    }

  for (;;)
    {
      sysclock.sleep_for (200);
      led_toggle ();
    }
  return 0;
}

// ----------------------------------------------------------------------------
// µOS++ startup scaffolding (same pattern as smp_test0..4)
// ----------------------------------------------------------------------------
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
    led::init ();
    timer_init ();
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
    scheduler::initialize ();

    static thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof (main_stack);
    static thread main_thread {
      "main", reinterpret_cast<thread::func_t> (custom_main_trampoline),
      nullptr, attr
    };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}
