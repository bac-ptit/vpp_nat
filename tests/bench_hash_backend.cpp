#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <x86intrin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "bihash_wrapper.h"

namespace {

constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

struct Key {
  std::uint64_t a{0};
  std::uint64_t b{0};
  std::uint64_t hash{0};
};

struct alignas(16) FlatSlot {
  std::uint64_t hash{0};
  std::uint32_t index{kEmpty};
};

class FlatHash {
 public:
  explicit FlatHash(std::size_t capacity) : mask_{capacity - 1}, slots_(capacity) {}

  bool Insert(std::uint64_t hash, std::uint32_t index) {
    std::size_t pos = hash & mask_;
    for (std::size_t probe = 0; probe <= mask_; ++probe) {
      FlatSlot& slot = slots_[pos];
      if (slot.index == kEmpty) {
        slot.hash = hash;
        slot.index = index;
        ++size_;
        return true;
      }
      pos = (pos + 1) & mask_;
    }
    return false;
  }

  std::uint32_t Find(std::uint64_t hash) const {
    std::size_t pos = hash & mask_;
    for (std::size_t probe = 0; probe <= mask_; ++probe) {
      const FlatSlot& slot = slots_[pos];
      if (slot.index == kEmpty) return kEmpty;
      if (slot.hash == hash) return slot.index;
      pos = (pos + 1) & mask_;
    }
    return kEmpty;
  }

  bool Erase(std::uint64_t hash) {
    std::size_t hole = hash & mask_;
    for (std::size_t probe = 0; probe <= mask_; ++probe) {
      if (slots_[hole].index == kEmpty) return false;
      if (slots_[hole].hash == hash) break;
      hole = (hole + 1) & mask_;
    }
    if (slots_[hole].index == kEmpty) return false;

    std::size_t next = (hole + 1) & mask_;
    while (slots_[next].index != kEmpty) {
      const std::size_t ideal = slots_[next].hash & mask_;
      bool move = false;
      if (hole <= next) {
        move = ideal > hole && ideal <= next;
      } else {
        move = ideal > hole || ideal <= next;
      }
      if (!move) {
        slots_[hole] = slots_[next];
        hole = next;
      }
      next = (next + 1) & mask_;
    }
    slots_[hole].index = kEmpty;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t Bytes() const { return slots_.size() * sizeof(FlatSlot); }

 private:
  std::size_t mask_;
  std::vector<FlatSlot> slots_;
  std::size_t size_{0};
};

void PinThread(std::size_t worker, std::size_t workers) {
  cpu_set_t set;
  CPU_ZERO(&set);
  const int cpu = workers <= 8
                      ? static_cast<int>(worker * 2)
                      : (worker < 8 ? static_cast<int>(worker * 2)
                                    : static_cast<int>((worker - 8) * 2 + 1));
  CPU_SET(cpu, &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  (void)setpriority(PRIO_PROCESS, 0, -20);
}

std::uint64_t NextRandom(std::uint64_t& state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = (argc > 1) ? argv[1] : "flat";
  const std::size_t workers = (argc > 2) ? std::strtoul(argv[2], nullptr, 10) : 16;
  const std::uint32_t duration = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 5;
  const std::size_t active = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 72'000;
  const std::size_t capacity = 131'072;
  const bool use_flat = std::strcmp(mode, "flat") == 0;

  std::vector<std::vector<Key>> keys(workers, std::vector<Key>(active));
  std::vector<FlatHash> flat;
  std::vector<nat_bihash_16_8_t*> bihash;
  if (use_flat) {
    flat.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) flat.emplace_back(capacity);
  } else {
    bihash.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      bihash.push_back(nat_bihash_create("bench_hash", capacity, 128 << 20));
      if (bihash.back() == nullptr) {
        std::fprintf(stderr, "bihash create failed\n");
        return 1;
      }
    }
  }

  for (std::size_t w = 0; w < workers; ++w) {
    for (std::size_t i = 0; i < active; ++i) {
      Key& key = keys[w][i];
      key.a = (static_cast<std::uint64_t>(w + 1) << 48) | i;
      key.b = 0x9E3779B97F4A7C15ULL ^ key.a;
      nat_bihash_kv_t kv{};
      kv.key[0] = key.a;
      kv.key[1] = key.b;
      key.hash = nat_bihash_hash(&kv);
      if (use_flat) {
        flat[w].Insert(key.hash, static_cast<std::uint32_t>(i));
      } else {
        kv.value = i;
        (void)nat_bihash_add_del(bihash[w], &kv, 1);
      }
    }
  }

  std::atomic<std::uint64_t> total_ops{0};
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds(duration);
  std::vector<std::thread> threads;
  threads.reserve(workers);

  for (std::size_t w = 0; w < workers; ++w) {
    threads.emplace_back([&, w]() {
      PinThread(w, workers);
      std::uint64_t rng = 0x123456789ABCDEF0ULL ^ (w * 0x9E3779B97F4A7C15ULL);
      std::uint64_t ops = 0;
      std::uint64_t next_id = active;
      while (true) {
        if ((ops & 0xFF) == 0 && std::chrono::steady_clock::now() >= deadline) break;
        const std::uint64_t roll = NextRandom(rng) % 100;
        if (roll < 15) {
          const std::size_t slot = NextRandom(rng) % active;
          Key& old_key = keys[w][slot];
          if (use_flat) {
            flat[w].Erase(old_key.hash);
          } else {
            nat_bihash_kv_t kv{};
            kv.key[0] = old_key.a;
            kv.key[1] = old_key.b;
            (void)nat_bihash_add_del(bihash[w], &kv, 0);
          }

          Key& new_key = keys[w][slot];
          new_key.a = (static_cast<std::uint64_t>(w + 1) << 48) | next_id++;
          new_key.b = 0x9E3779B97F4A7C15ULL ^ new_key.a;
          nat_bihash_kv_t kv{};
          kv.key[0] = new_key.a;
          kv.key[1] = new_key.b;
          new_key.hash = nat_bihash_hash(&kv);
          if (use_flat) {
            flat[w].Insert(new_key.hash, static_cast<std::uint32_t>(slot));
          } else {
            kv.value = slot;
            (void)nat_bihash_add_del(bihash[w], &kv, 1);
          }
          ops += 2;
        } else {
          const Key& key = keys[w][NextRandom(rng) % active];
          if (use_flat) {
            (void)flat[w].Find(key.hash);
          } else {
            nat_bihash_kv_t kv{};
            kv.key[0] = key.a;
            kv.key[1] = key.b;
            nat_bihash_kv_t result{};
            (void)nat_bihash_search_with_hash(bihash[w], &kv, key.hash, &result);
          }
          ++ops;
        }
      }
      total_ops.fetch_add(ops, std::memory_order_relaxed);
    });
  }
  for (auto& thread : threads) thread.join();
  const auto end = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(end - start).count();
  const std::uint64_t ops = total_ops.load();
  const double mops = static_cast<double>(ops) / elapsed / 1e6;
  const double ns_per_op = ops > 0 ? elapsed * 1e9 / static_cast<double>(ops) : 0.0;
  std::printf("mode=%s workers=%zu active/worker=%zu capacity=%zu ops=%llu mops=%.2f ns/op=%.2f\n",
              mode, workers, active, capacity,
              static_cast<unsigned long long>(ops), mops, ns_per_op);
  if (use_flat) {
    std::printf("flat_bytes/table=%zu total=%.2f MB\n", flat.front().Bytes(),
                static_cast<double>(flat.front().Bytes() * workers) / (1024.0 * 1024.0));
  } else {
    for (auto* table : bihash) nat_bihash_destroy(table);
  }
  return 0;
}
