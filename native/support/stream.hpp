#pragma once

#include "../runtime.hpp"

namespace sdc {

enum class StreamOperation { Read, Write, Copy, Triad };

inline std::size_t stream_array_count(StreamOperation operation) {
  switch (operation) {
    case StreamOperation::Read:
    case StreamOperation::Write: return 1;
    case StreamOperation::Copy: return 2;
    case StreamOperation::Triad: return 3;
  }
  return 1;
}

inline std::string stream_name(StreamOperation operation) {
  switch (operation) {
    case StreamOperation::Read: return "read";
    case StreamOperation::Write: return "write";
    case StreamOperation::Copy: return "copy";
    case StreamOperation::Triad: return "triad";
  }
  return "unknown";
}

inline double stream_bytes_per_element(StreamOperation operation) {
  switch (operation) {
    case StreamOperation::Read:
    case StreamOperation::Write: return sizeof(std::uint64_t);
    case StreamOperation::Copy: return 2.0 * sizeof(std::uint64_t);
    case StreamOperation::Triad: return 3.0 * sizeof(std::uint64_t);
  }
  return 0.0;
}

struct StreamResult {
  double gigabytes_per_second = 0.0;
  double elapsed_seconds = 0.0;
  std::uint64_t passes = 0;
};

inline std::uint64_t stream_read_vector_assembly(const std::uint64_t *data,
                                          std::size_t elements) {
  const auto *cursor = reinterpret_cast<const std::byte *>(data);
  const auto *end = cursor + elements * sizeof(std::uint64_t);
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2")) {
    const std::size_t vector_bytes = static_cast<std::size_t>(end - cursor) / 256U * 256U;
    const auto *vector_end = cursor + vector_bytes;
    if (cursor != vector_end) {
      asm volatile(
          "1:\n\t"
          "vmovdqu   0(%[cursor]), %%ymm0\n\t"
          "vmovdqu  32(%[cursor]), %%ymm1\n\t"
          "vmovdqu  64(%[cursor]), %%ymm2\n\t"
          "vmovdqu  96(%[cursor]), %%ymm3\n\t"
          "vmovdqu 128(%[cursor]), %%ymm4\n\t"
          "vmovdqu 160(%[cursor]), %%ymm5\n\t"
          "vmovdqu 192(%[cursor]), %%ymm6\n\t"
          "vmovdqu 224(%[cursor]), %%ymm7\n\t"
          "addq $256, %[cursor]\n\t"
          "cmpq %[vector_end], %[cursor]\n\t"
          "jb 1b\n\t"
          "vzeroupper\n\t"
          : [cursor] "+r"(cursor)
          : [vector_end] "r"(vector_end)
          : "cc", "memory", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5",
            "ymm6", "ymm7");
    }
  } else {
    const std::size_t vector_bytes = static_cast<std::size_t>(end - cursor) / 128U * 128U;
    const auto *vector_end = cursor + vector_bytes;
    if (cursor != vector_end) {
      asm volatile(
          "1:\n\t"
          "movdqu   0(%[cursor]), %%xmm0\n\t"
          "movdqu  16(%[cursor]), %%xmm1\n\t"
          "movdqu  32(%[cursor]), %%xmm2\n\t"
          "movdqu  48(%[cursor]), %%xmm3\n\t"
          "movdqu  64(%[cursor]), %%xmm4\n\t"
          "movdqu  80(%[cursor]), %%xmm5\n\t"
          "movdqu  96(%[cursor]), %%xmm6\n\t"
          "movdqu 112(%[cursor]), %%xmm7\n\t"
          "addq $128, %[cursor]\n\t"
          "cmpq %[vector_end], %[cursor]\n\t"
          "jb 1b\n\t"
          : [cursor] "+r"(cursor)
          : [vector_end] "r"(vector_end)
          : "cc", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
            "xmm6", "xmm7");
    }
  }
#elif defined(__aarch64__)
  const std::size_t vector_bytes = static_cast<std::size_t>(end - cursor) / 128U * 128U;
  const auto *vector_end = cursor + vector_bytes;
  if (cursor != vector_end) {
    asm volatile(
        "1:\n\t"
        "ldp q0, q1, [%[cursor], #0]\n\t"
        "ldp q2, q3, [%[cursor], #32]\n\t"
        "ldp q4, q5, [%[cursor], #64]\n\t"
        "ldp q6, q7, [%[cursor], #96]\n\t"
        "add %[cursor], %[cursor], #128\n\t"
        "cmp %[cursor], %[vector_end]\n\t"
        "b.lo 1b\n\t"
        : [cursor] "+r"(cursor)
        : [vector_end] "r"(vector_end)
        : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
  }
#endif
  std::uint64_t sink = static_cast<std::uint64_t>(elements);
  while (cursor < end) {
    std::uint64_t value = 0;
#if defined(__x86_64__)
    asm volatile("movq (%1), %0" : "=r"(value) : "r"(cursor) : "memory");
#elif defined(__aarch64__)
    asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(cursor) : "memory");
#else
    std::memcpy(&value, cursor, sizeof(value));
#endif
    sink ^= value;
    cursor += sizeof(value);
  }
  return sink;
}

inline StreamResult run_stream(StreamOperation operation, const std::vector<int> &cpus,
                        std::size_t bytes_per_array, int duration_ms,
                        int memory_init_cpu = -1) {
  const std::size_t elements = std::max<std::size_t>(cpus.size() * 1024,
                                                      bytes_per_array / sizeof(std::uint64_t));
  MappedBuffer a(elements * sizeof(std::uint64_t));
  std::unique_ptr<MappedBuffer> b;
  std::unique_ptr<MappedBuffer> c;
  if (operation == StreamOperation::Copy || operation == StreamOperation::Triad) {
    b = std::make_unique<MappedBuffer>(elements * sizeof(std::uint64_t));
  }
  if (operation == StreamOperation::Triad) {
    c = std::make_unique<MappedBuffer>(elements * sizeof(std::uint64_t));
  }
  auto *a_ptr = a.as<std::uint64_t>();
  auto *b_ptr = b ? b->as<std::uint64_t>() : nullptr;
  auto *c_ptr = c ? c->as<std::uint64_t>() : nullptr;
  if (memory_init_cpu >= 0) {
    pin_to_cpu(memory_init_cpu);
    for (std::size_t index = 0; index < elements; ++index) {
      a_ptr[index] = 1;
      if (b_ptr != nullptr) b_ptr[index] = 2;
      if (c_ptr != nullptr) c_ptr[index] = 3;
    }
  }

  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::vector<std::uint64_t> passes(cpus.size(), 0);
  std::vector<std::uint64_t> sums(cpus.size(), 0);
  std::vector<std::thread> workers;
  workers.reserve(cpus.size());
  for (std::size_t worker = 0; worker < cpus.size(); ++worker) {
    workers.emplace_back([&, worker] {
      pin_to_cpu(cpus[worker]);
      const std::size_t begin = elements * worker / cpus.size();
      const std::size_t end = elements * (worker + 1) / cpus.size();
      if (memory_init_cpu < 0) {
        for (std::size_t index = begin; index < end; ++index) {
          a_ptr[index] = 1;
          if (b_ptr != nullptr) b_ptr[index] = 2;
          if (c_ptr != nullptr) c_ptr[index] = 3;
        }
      }
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        cpu_relax();
      }
      std::uint64_t sum = 0;
      std::uint64_t count = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        switch (operation) {
          case StreamOperation::Read:
            sum ^= stream_read_vector_assembly(a_ptr + begin, end - begin);
            break;
          case StreamOperation::Write:
            for (std::size_t index = begin; index < end; ++index) a_ptr[index] = sum + 1;
            break;
          case StreamOperation::Copy:
            for (std::size_t index = begin; index < end; ++index) a_ptr[index] = b_ptr[index];
            break;
          case StreamOperation::Triad:
            for (std::size_t index = begin; index < end; ++index)
              a_ptr[index] = b_ptr[index] + 3 * c_ptr[index];
            break;
        }
        // Make each completed pass observable without turning individual accesses volatile.
        // This preserves vectorization while preventing dead-store/pass collapsing.
        asm volatile("" : : "r"(a_ptr) : "memory");
        ++count;
      }
      passes[worker] = count;
      sums[worker] = sum;
    });
  }
  while (ready.load(std::memory_order_acquire) != static_cast<int>(cpus.size())) {
    std::this_thread::yield();
  }
  const auto begin = Clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  stop.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();
  const auto end = Clock::now();
  const double elapsed = seconds_between(begin, end);
  const std::uint64_t total_passes = std::accumulate(passes.begin(), passes.end(), 0ULL);
  double touched_elements = 0.0;
  for (std::size_t worker = 0; worker < cpus.size(); ++worker) {
    const std::size_t begin_index = elements * worker / cpus.size();
    const std::size_t end_index = elements * (worker + 1) / cpus.size();
    touched_elements += static_cast<double>(end_index - begin_index) *
                        static_cast<double>(passes[worker]);
  }
  global_sink = global_sink ^ std::accumulate(sums.begin(), sums.end(), 0ULL);
  return {touched_elements * stream_bytes_per_element(operation) / elapsed / 1e9,
          elapsed, total_passes};
}

inline std::vector<std::size_t> thread_counts(std::size_t maximum) {
  std::vector<std::size_t> values{1};
  for (std::size_t count = 2; count < maximum; count *= 2) values.push_back(count);
  if (values.back() != maximum) values.push_back(maximum);
  return values;
}


}  // namespace sdc
