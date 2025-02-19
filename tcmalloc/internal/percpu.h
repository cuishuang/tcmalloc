// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_INTERNAL_PERCPU_H_
#define TCMALLOC_INTERNAL_PERCPU_H_

#define TCMALLOC_PERCPU_TCMALLOC_FIXED_SLAB_SHIFT 18

// TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM defines whether or not we have an
// implementation for the target OS and architecture.
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM 1
#else
#define TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM 0
#endif

#define TCMALLOC_PERCPU_RSEQ_VERSION 0x0
#define TCMALLOC_PERCPU_RSEQ_FLAGS 0x0
#if defined(__x86_64__)
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0x53053053
#elif defined(__ppc__)
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0x0FE5000B
#elif defined(__aarch64__)
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0xd428bc00
#else
// Rather than error, allow us to build, but with an invalid signature.
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0x0
#endif

// The constants above this line must be macros since they are shared with the
// RSEQ assembly sources.
#ifndef __ASSEMBLER__

#ifdef __linux__
#include <sched.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/per_thread_tls.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/atomic_danger.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linux_syscall_support.h"
#include "tcmalloc/internal/logging.h"

// TCMALLOC_PERCPU_USE_RSEQ defines whether TCMalloc support for RSEQ on the
// target architecture exists. We currently only provide RSEQ for 64-bit x86,
// Arm binaries.
#if !defined(TCMALLOC_PERCPU_USE_RSEQ)
#if (ABSL_PER_THREAD_TLS == 1) && (TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM == 1)
#define TCMALLOC_PERCPU_USE_RSEQ 1
#else
#define TCMALLOC_PERCPU_USE_RSEQ 0
#endif
#endif  // !defined(TCMALLOC_PERCPU_USE_RSEQ)

// TCMALLOC_PERCPU_USE_RSEQ_VCPU defines whether TCMalloc support for RSEQ
// virtual CPU IDs is available on the target architecture.
#if TCMALLOC_PERCPU_USE_RSEQ && (defined(__x86_64__) || defined(__aarch64__))
#define TCMALLOC_PERCPU_USE_RSEQ_VCPU 1
#else
#define TCMALLOC_PERCPU_USE_RSEQ_VCPU 0
#endif  // !defined(TCMALLOC_PERCPU_USE_RSEQ_VCPU)

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {

inline constexpr int kRseqUnregister = 1;

// Internal state used for tracking initialization of RseqCpuId()
inline constexpr int kCpuIdUnsupported = -2;
inline constexpr int kCpuIdUninitialized = -1;
inline constexpr int kCpuIdInitialized = 0;

#if TCMALLOC_PERCPU_USE_RSEQ
extern "C" ABSL_PER_THREAD_TLS_KEYWORD volatile kernel_rseq __rseq_abi;

static inline int RseqCpuId() { return __rseq_abi.cpu_id; }

static inline int VirtualRseqCpuId(const size_t virtual_cpu_id_offset) {
#if TCMALLOC_PERCPU_USE_RSEQ_VCPU
  ASSERT(virtual_cpu_id_offset == offsetof(kernel_rseq, cpu_id) ||
         virtual_cpu_id_offset == offsetof(kernel_rseq, vcpu_id));
  return *reinterpret_cast<short*>(reinterpret_cast<uintptr_t>(&__rseq_abi) +
                                   virtual_cpu_id_offset);
#else
  ASSERT(virtual_cpu_id_offset == offsetof(kernel_rseq, cpu_id));
  return RseqCpuId();
#endif  // TCMALLOC_PERCPU_USE_RSEQ_VCPU
}
#else  // !TCMALLOC_PERCPU_USE_RSEQ
static inline int RseqCpuId() { return kCpuIdUnsupported; }

static inline int VirtualRseqCpuId(const size_t virtual_cpu_id_offset) {
  return kCpuIdUnsupported;
}
#endif

typedef int (*OverflowHandler)(int cpu, size_t size_class, void* item,
                               void* arg);
typedef void* (*UnderflowHandler)(int cpu, size_t size_class, void* arg);

// Functions below are implemented in the architecture-specific percpu_rseq_*.S
// files.
extern "C" {
int TcmallocSlab_Internal_PerCpuCmpxchg64(int target_cpu, intptr_t* p,
                                          intptr_t old_val, intptr_t new_val);

// Push a batch for a slab which the Shift equal to
// TCMALLOC_PERCPU_TCMALLOC_FIXED_SLAB_SHIFT
size_t TcmallocSlab_Internal_PushBatch_FixedShift(void* ptr, size_t size_class,
                                                  void** batch, size_t len);

// Pop a batch for a slab which the Shift equal to
// TCMALLOC_PERCPU_TCMALLOC_FIXED_SLAB_SHIFT
size_t TcmallocSlab_Internal_PopBatch_FixedShift(void* ptr, size_t size_class,
                                                 void** batch, size_t len);

#if TCMALLOC_PERCPU_USE_RSEQ_VCPU
int TcmallocSlab_Internal_PerCpuCmpxchg64_VCPU(int target_cpu, intptr_t* p,
                                               intptr_t old_val,
                                               intptr_t new_val);
size_t TcmallocSlab_Internal_PushBatch_FixedShift_VCPU(void* ptr,
                                                       size_t size_class,
                                                       void** batch,
                                                       size_t len);
size_t TcmallocSlab_Internal_PopBatch_FixedShift_VCPU(void* ptr,
                                                      size_t size_class,
                                                      void** batch, size_t len);
#endif  // TCMALLOC_PERCPU_USE_RSEQ_VCPU
}

// NOTE:  We skirt the usual naming convention slightly above using "_" to
// increase the visibility of functions embedded into the root-namespace (by
// virtue of C linkage) in the supported case.

// Return whether we are using flat virtual CPUs.
bool UsingFlatVirtualCpus();

inline int GetCurrentCpuUnsafe() {
  // Use the rseq mechanism.
  return RseqCpuId();
}

inline int GetCurrentCpu() {
  // We can't use the unsafe version unless we have the appropriate version of
  // the rseq extension. This also allows us a convenient escape hatch if the
  // kernel changes the way it uses special-purpose registers for CPU IDs.
  int cpu = GetCurrentCpuUnsafe();

  // We open-code the check for fast-cpu availability since we do not want to
  // force initialization in the first-call case.  This so done so that we can
  // use this in places where it may not always be safe to initialize and so
  // that it may serve in the future as a proxy for callers such as
  // CPULogicalId() without introducing an implicit dependence on the fast-path
  // extensions. Initialization is also simply unneeded on some platforms.
  if (ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized)) {
    return cpu;
  }

#ifdef TCMALLOC_HAVE_SCHED_GETCPU
  cpu = sched_getcpu();
  ASSERT(cpu >= 0);
#endif  // TCMALLOC_HAVE_SCHED_GETCPU

  return cpu;
}

inline int GetCurrentVirtualCpuUnsafe(const size_t virtual_cpu_id_offset) {
  return VirtualRseqCpuId(virtual_cpu_id_offset);
}

inline int GetCurrentVirtualCpu(const size_t virtual_cpu_id_offset) {
  // We can't use the unsafe version unless we have the appropriate version of
  // the rseq extension. This also allows us a convenient escape hatch if the
  // kernel changes the way it uses special-purpose registers for CPU IDs.
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset);

  // We open-code the check for fast-cpu availability since we do not want to
  // force initialization in the first-call case.  This so done so that we can
  // use this in places where it may not always be safe to initialize and so
  // that it may serve in the future as a proxy for callers such as
  // CPULogicalId() without introducing an implicit dependence on the fast-path
  // extensions. Initialization is also simply unneeded on some platforms.
  if (ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized)) {
    return cpu;
  }

  // Do not return a physical CPU ID when we expect a virtual CPU ID.
  CHECK_CONDITION(virtual_cpu_id_offset != offsetof(kernel_rseq, vcpu_id));

#ifdef TCMALLOC_HAVE_SCHED_GETCPU
  cpu = sched_getcpu();
  ASSERT(cpu >= 0);
#endif  // TCMALLOC_HAVE_SCHED_GETCPU

  return cpu;
}

bool InitFastPerCpu();

inline bool IsFast() {
  if (!TCMALLOC_PERCPU_USE_RSEQ) {
    return false;
  }

  int cpu = RseqCpuId();

  if (ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized)) {
    return true;
  } else if (ABSL_PREDICT_FALSE(cpu == kCpuIdUnsupported)) {
    return false;
  } else {
    // Sets 'cpu' for next time, and calls EnsureSlowModeInitialized if
    // necessary.
    return InitFastPerCpu();
  }
}

// As IsFast(), but if this thread isn't already initialized, will not
// attempt to do so.
inline bool IsFastNoInit() {
  if (!TCMALLOC_PERCPU_USE_RSEQ) {
    return false;
  }
  int cpu = RseqCpuId();
  return ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized);
}

// A barrier that prevents compiler reordering.
inline void CompilerBarrier() {
#if defined(__GNUC__)
  __asm__ __volatile__("" : : : "memory");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

// Internal tsan annotations, do not use externally.
// Required as tsan does not natively understand RSEQ.
#ifdef ABSL_HAVE_THREAD_SANITIZER
extern "C" {
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
}
#endif

// TSAN relies on seeing (and rewriting) memory accesses.  It can't
// get at the memory acccesses we make from RSEQ assembler sequences,
// which means it doesn't know about the semantics our sequences
// enforce.  So if we're under TSAN, add barrier annotations.
inline void TSANAcquire(void* p) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  __tsan_acquire(p);
#endif
}

inline void TSANAcquireBatch(void** batch, int n) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  for (int i = 0; i < n; i++) {
    __tsan_acquire(batch[i]);
  }
#endif
}

inline void TSANRelease(void* p) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  __tsan_release(p);
#endif
}

inline void TSANReleaseBatch(void** batch, int n) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  for (int i = 0; i < n; i++) {
    __tsan_release(batch[i]);
  }
#endif
}

inline void TSANMemoryBarrierOn(void* p) {
  TSANAcquire(p);
  TSANRelease(p);
}

// These methods may *only* be called if IsFast() has been called by the current
// thread (and it returned true).
inline int CompareAndSwapUnsafe(int target_cpu, std::atomic<intptr_t>* p,
                                intptr_t old_val, intptr_t new_val,
                                const size_t virtual_cpu_id_offset) {
  TSANMemoryBarrierOn(p);
#if TCMALLOC_PERCPU_USE_RSEQ
  switch (virtual_cpu_id_offset) {
    case offsetof(kernel_rseq, cpu_id):
      return TcmallocSlab_Internal_PerCpuCmpxchg64(
          target_cpu, tcmalloc_internal::atomic_danger::CastToIntegral(p),
          old_val, new_val);
#if TCMALLOC_PERCPU_USE_RSEQ_VCPU
    case offsetof(kernel_rseq, vcpu_id):
      return TcmallocSlab_Internal_PerCpuCmpxchg64_VCPU(
          target_cpu, tcmalloc_internal::atomic_danger::CastToIntegral(p),
          old_val, new_val);
#endif  // TCMALLOC_PERCPU_USE_RSEQ_VCPU
    default:
      __builtin_unreachable();
  }
#else   // !TCMALLOC_PERCPU_USE_RSEQ
  __builtin_unreachable();
#endif  // !TCMALLOC_PERCPU_USE_RSEQ
}

void FenceCpu(int cpu, const size_t virtual_cpu_id_offset);
void FenceAllCpus();

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // !__ASSEMBLER__
#endif  // TCMALLOC_INTERNAL_PERCPU_H_
