#include "nat_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace nat::engine {

ThreadSessionTable::ThreadSessionTable(std::size_t initial_capacity,
                                       std::size_t expected_active) noexcept
    : session_count_{0},
      total_translations_{0} {
  const std::size_t sizing_records =
      expected_active != 0 ? expected_active : initial_capacity;
  const std::size_t nbuckets = std::max<std::size_t>(1024, sizing_records / 2);
  const std::size_t bihash_mem =
      std::max<std::size_t>(32 << 20, sizing_records * 64);
  in2out_bihash_ = nat_bihash_create("worker_in2out", static_cast<uint32_t>(nbuckets), bihash_mem);
  out2in_bihash_ = nat_bihash_create("worker_out2in", static_cast<uint32_t>(nbuckets), bihash_mem);

  const std::size_t total_capacity = initial_capacity + 4096;
  sessions_.resize(total_capacity);
  lru_nodes_.resize(total_capacity, LruNode{kInvalidIndex, kInvalidIndex});
  port_to_session_.resize(kMaxPublicIps * 65536, kInvalidIndex);

  if (sessions_.data() == nullptr || lru_nodes_.data() == nullptr ||
      port_to_session_.data() == nullptr) {
    return;
  }

  public_ips_[0] = public_ip_;
  public_ip_count_ = 1;
  lru_head_.fill(kInvalidIndex);
  lru_tail_.fill(kInvalidIndex);
  free_indices_.reserve(total_capacity);
  retire_queue_.reserve(256);

  for (std::size_t i{total_capacity}; i > 0; --i) {
    free_indices_.push_back(static_cast<std::uint32_t>(i - 1));
  }
}

ThreadSessionTable::~ThreadSessionTable() noexcept {
  if (in2out_bihash_) {
    nat_bihash_destroy(in2out_bihash_);
    in2out_bihash_ = nullptr;
  }
  if (out2in_bihash_) {
    nat_bihash_destroy(out2in_bihash_);
    out2in_bihash_ = nullptr;
  }
}

ThreadSessionTable::ThreadSessionTable(ThreadSessionTable&& other) noexcept
    : in2out_bihash_{other.in2out_bihash_},
      out2in_bihash_{other.out2in_bihash_},
      sessions_{std::move(other.sessions_)},
      lru_nodes_{std::move(other.lru_nodes_)},
      port_to_session_{std::move(other.port_to_session_)},
      fast_state_{std::move(other.fast_state_)},
      hot_cache_{std::move(other.hot_cache_)},
      hot_mask_{other.hot_mask_},
      free_indices_{std::move(other.free_indices_)},
      retire_queue_{std::move(other.retire_queue_)},
      lru_head_(other.lru_head_),
      lru_tail_(other.lru_tail_),
      subscribers_{other.subscribers_},
      pba_block_bitmap_{other.pba_block_bitmap_},
      public_ips_{other.public_ips_},
      public_ip_count_{other.public_ip_count_},
      session_count_{other.session_count_},
      total_translations_{other.total_translations_},
      telemetry_{other.telemetry_},
      timeouts_{other.timeouts_} {
  other.in2out_bihash_ = nullptr;
  other.out2in_bihash_ = nullptr;
  other.lru_head_.fill(kInvalidIndex);
  other.lru_tail_.fill(kInvalidIndex);
  other.session_count_ = 0;
  other.total_translations_ = 0;
}

ThreadSessionTable& ThreadSessionTable::operator=(ThreadSessionTable&& other) noexcept {
  if (this != &other) {
    if (in2out_bihash_) nat_bihash_destroy(in2out_bihash_);
    if (out2in_bihash_) nat_bihash_destroy(out2in_bihash_);

    in2out_bihash_ = other.in2out_bihash_;
    out2in_bihash_ = other.out2in_bihash_;
    sessions_ = std::move(other.sessions_);
    lru_nodes_ = std::move(other.lru_nodes_);
    port_to_session_ = std::move(other.port_to_session_);
    fast_state_ = std::move(other.fast_state_);
    hot_cache_ = std::move(other.hot_cache_);
    hot_mask_ = other.hot_mask_;
    free_indices_ = std::move(other.free_indices_);
    retire_queue_ = std::move(other.retire_queue_);
    lru_head_ = other.lru_head_;
    lru_tail_ = other.lru_tail_;
    subscribers_ = other.subscribers_;
    pba_block_bitmap_ = other.pba_block_bitmap_;
    public_ips_ = other.public_ips_;
    public_ip_count_ = other.public_ip_count_;
    session_count_ = other.session_count_;
    total_translations_ = other.total_translations_;
    telemetry_ = other.telemetry_;
    timeouts_ = other.timeouts_;

    other.in2out_bihash_ = nullptr;
    other.out2in_bihash_ = nullptr;
    other.lru_head_.fill(kInvalidIndex);
    other.lru_tail_.fill(kInvalidIndex);
    other.session_count_ = 0;
    other.total_translations_ = 0;
  }
  return *this;
}

bool ThreadSessionTable::MakeRoomForCreates(std::size_t count,
                                            std::uint64_t now_ns) noexcept {
  if (count == 0) return true;
  if (now_ns == 0) now_ns = current_timestamp_ns();

  // 1. Reclaim EXPIRED sessions only
  while (free_indices_.size() + retire_queue_.size() < count ||
         (max_sessions_ != 0 && session_count_ + count > max_sessions_)) {
    const std::uint32_t victim = LruFindExpiredHead(now_ns);
    if (victim == kInvalidIndex) break;
    LruRemove(victim);
    sessions_[victim].flags |= kSessionFlagRetiring;
    MarkFastRetiring(victim);
    retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
    ++telemetry_.expired_reclaims;
  }

  if (!retire_queue_.empty()) {
    ReclaimBatch(std::min<std::size_t>(
        retire_queue_.size(),
        std::max<std::size_t>(count * 2, kDefaultReclaimBatchSize)));
  }

  // 2. Only if force_evict_on_overflow_ is enabled, allow evicting active sessions
  if (force_evict_on_overflow_) {
    if (max_sessions_ != 0 && session_count_ + count > max_sessions_) {
      std::size_t need_evict = session_count_ + count - max_sessions_;
      while (need_evict > 0) {
        const std::uint32_t victim = LruPopOldestHead();
        if (victim == kInvalidIndex) break;
        sessions_[victim].flags |= kSessionFlagRetiring;
        MarkFastRetiring(victim);
        retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
        ++telemetry_.lru_evictions;
        --need_evict;
      }
    }

    while (free_indices_.size() + retire_queue_.size() < count) {
      const std::uint32_t victim = LruPopOldestHead();
      if (victim == kInvalidIndex) break;
      sessions_[victim].flags |= kSessionFlagRetiring;
      MarkFastRetiring(victim);
      retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
      ++telemetry_.lru_evictions;
    }

    if (!retire_queue_.empty()) {
      ReclaimBatch(std::min<std::size_t>(
          retire_queue_.size(),
          std::max<std::size_t>(count * 2, kDefaultReclaimBatchSize)));
    }
  }

  if (max_sessions_ != 0 && session_count_ + count > max_sessions_) {
    return false;
  }
  return free_indices_.size() >= count;
}

void ThreadSessionTable::CreateSessionsBatch(PendingCreate* pending, std::size_t count,
                                             std::uint64_t now_ns) noexcept {
  if (pending == nullptr || count == 0) return;
  if (now_ns == 0) now_ns = current_timestamp_ns();

  (void)MakeRoomForCreates(count, now_ns);
  const std::uint32_t public_ip = PublicIp();

  for (std::size_t i = 0; i < count; ++i) {
    auto port = AllocatePortForUser(pending[i].in_key.src_ip, public_ip);
    if (!port.has_value()) {
      ++telemetry_.create_failures;
      continue;
    }
    pending[i].port = port->port;
    pending[i].public_ip = port->public_ip;
    pending[i].port_allocated = true;
  }

  for (std::size_t i = 0; i < count; ++i) {
    if (!pending[i].port_allocated) continue;
    const auto& in_key = pending[i].in_key;
    const nat_bihash_kv_t in_kv = FiveTupleToKv(in_key);
    const std::uint64_t in_hash = nat_bihash_hash(&in_kv);
    pending[i].in_hash = in_hash;
    nat_bihash_prefetch_bucket_hash(in2out_bihash_, in_hash);
    nat_bihash_prefetch_data_hash(in2out_bihash_, in_hash);
    const nat_bihash_kv_t out_kv = FiveTupleToKv(FiveTuple{
        .src_ip{pending[i].public_ip},
        .dst_ip{in_key.dst_ip},
        .src_port{pending[i].port},
        .dst_port{in_key.dst_port},
        .protocol{in_key.protocol},
    });
    const std::uint64_t out_hash = nat_bihash_hash(&out_kv);
    pending[i].out_hash = out_hash;
    nat_bihash_prefetch_bucket_hash(out2in_bihash_, out_hash);
    nat_bihash_prefetch_data_hash(out2in_bihash_, out_hash);
  }

  for (std::size_t i = 0; i < count; ++i) {
    if (!pending[i].port_allocated) continue;
    if (free_indices_.empty()) [[unlikely]] {
      ReleasePortForUser(pending[i].in_key.src_ip, pending[i].public_ip,
                         pending[i].port);
      pending[i].port_allocated = false;
      ++telemetry_.create_failures;
      continue;
    }

    const std::uint32_t session_idx = free_indices_.back();
    free_indices_.pop_back();
    const std::uint16_t new_gen =
        static_cast<std::uint16_t>(sessions_[session_idx].generation + 1);
    const FiveTuple& in_key = pending[i].in_key;
    const FiveTuple out_key{
        .src_ip{pending[i].public_ip},
        .dst_ip{in_key.dst_ip},
        .src_port{pending[i].port},
        .dst_port{in_key.dst_port},
        .protocol{in_key.protocol},
    };

    sessions_[session_idx] = NatSession{
        .inside = in_key,
        .outside = out_key,
        .last_active_ts = now_ns,
        .total_packets = 0,
        .total_bytes = 0,
        .flags = kSessionFlagActive,
        .generation = new_gen,
        .last_lru_move_sec = static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL),
    };

    nat_bihash_kv_t in_kv = FiveTupleToKv(
        in_key, EncodeIn2OutValue(session_idx, pending[i].public_ip, pending[i].port));
    if (nat_bihash_add_del_with_hash(in2out_bihash_, &in_kv, pending[i].in_hash, 1) != 0) [[unlikely]] {
      sessions_[session_idx] = NatSession{};
      sessions_[session_idx].generation = new_gen;
      free_indices_.push_back(session_idx);
      ReleasePortForUser(in_key.src_ip, pending[i].public_ip, pending[i].port);
      pending[i].port_allocated = false;
      ++telemetry_.create_failures;
      ++telemetry_.bihash_insert_failures;
      continue;
    }

    nat_bihash_kv_t out_kv = FiveTupleToKv(out_key, session_idx);
    if (nat_bihash_add_del_with_hash(out2in_bihash_, &out_kv, pending[i].out_hash, 1) != 0) [[unlikely]] {
      nat_bihash_add_del_with_hash(in2out_bihash_, &in_kv, pending[i].in_hash, 0);
      sessions_[session_idx] = NatSession{};
      sessions_[session_idx].generation = new_gen;
      free_indices_.push_back(session_idx);
      ReleasePortForUser(in_key.src_ip, pending[i].public_ip, pending[i].port);
      pending[i].port_allocated = false;
      ++telemetry_.create_failures;
      ++telemetry_.bihash_insert_failures;
      continue;
    }

    port_to_session_[PortSlot(PublicIpIndex(pending[i].public_ip),
                              pending[i].port)] = session_idx;
    LruPushTail(session_idx, ClassOfProtocol(in_key.protocol, false));
    MarkFastActive(session_idx, static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL));
    HotInsert(in_key, HashKey(in_key), session_idx);
    ++session_count_;
    ++telemetry_.new_sessions_created;
    if (session_count_ > telemetry_.active_sessions_high_watermark) {
      telemetry_.active_sessions_high_watermark = session_count_;
    }
    ++total_translations_;
    pending[i].session_idx = session_idx;
    pending[i].session = &sessions_[session_idx];
  }
}

NatSession* ThreadSessionTable::CreateSessionWithOutcome(
    const LookupOutcome& outcome, const FiveTuple& in_key, const FiveTuple& out_key) noexcept {
  const std::uint64_t now_ns = current_timestamp_ns();

  // 0. Proactively evict expired sessions from LRU head into retire_queue_
  while (retire_queue_.size() < kDefaultReclaimBatchSize * 2) {
    const std::uint32_t victim = LruFindExpiredHead(now_ns);
    if (victim == kInvalidIndex) {
      break;
    }
    LruRemove(victim);
    sessions_[victim].flags |= kSessionFlagRetiring;
    MarkFastRetiring(victim);
    retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
    ++telemetry_.expired_reclaims;
  }

  if (!retire_queue_.empty() &&
      (retire_queue_.size() >= kDefaultReclaimBatchSize || free_indices_.empty() ||
       (max_sessions_ != 0 && session_count_ >= max_sessions_))) {
    ReclaimBatch(std::min<std::size_t>(retire_queue_.size(), kDefaultReclaimBatchSize * 2));
  }

  // 1. Enforce configured per-worker active-session limit
  if (max_sessions_ != 0 && session_count_ >= max_sessions_) {
    if (force_evict_on_overflow_) {
      constexpr std::size_t kMaxEvictBatch{64};
      for (std::size_t i{0}; i < kMaxEvictBatch; ++i) {
        const std::uint32_t victim = LruPopOldestHead();
        if (victim == kInvalidIndex) break;
        sessions_[victim].flags |= kSessionFlagRetiring;
        MarkFastRetiring(victim);
        retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
        ++telemetry_.lru_evictions;
      }
      if (!retire_queue_.empty()) {
        ReclaimBatch(
            std::min<std::size_t>(retire_queue_.size(), kDefaultReclaimBatchSize * 2));
      }
    } else {
      // Do NOT evict active sessions! Reject create -> Redirect to slow path
      ++telemetry_.create_failures;
      return nullptr;
    }
  }

  // 2. If freelist is still empty, only force evict if configured
  if (free_indices_.empty()) {
    if (force_evict_on_overflow_ && session_count_ > 0) {
      const std::uint32_t victim_idx = LruPopOldestHead();
      if (victim_idx != kInvalidIndex) {
        DeleteSessionByIdx(victim_idx);
        ++telemetry_.lru_evictions;
      }
    }
  }

  if (free_indices_.empty()) [[unlikely]] {
    ++telemetry_.create_failures;
    return nullptr;
  }

  const std::uint32_t session_idx = free_indices_.back();
  free_indices_.pop_back();

  const std::uint16_t new_gen = static_cast<std::uint16_t>(sessions_[session_idx].generation + 1);
  sessions_[session_idx] = NatSession{
      .inside = in_key,
      .outside = out_key,
      .last_active_ts = now_ns,
      .total_packets = 0,
      .total_bytes = 0,
      .flags = kSessionFlagActive,
      .generation = new_gen,
      .last_lru_move_sec = static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL),
  };

  nat_bihash_kv_t in_kv = outcome.kv;
  in_kv.value = EncodeIn2OutValue(session_idx, out_key.src_ip, out_key.src_port);
  const std::uint64_t in_hash = nat_bihash_hash(&in_kv);
  int rc_in = nat_bihash_add_del_with_hash(in2out_bihash_, &in_kv, in_hash, 1 /* is_add */);
  if (rc_in != 0) [[unlikely]] {
    sessions_[session_idx] = NatSession{};
    sessions_[session_idx].generation = new_gen;
    free_indices_.push_back(session_idx);
    ++telemetry_.create_failures;
    ++telemetry_.bihash_insert_failures;
    return nullptr;
  }

  nat_bihash_kv_t out_kv = FiveTupleToKv(out_key, session_idx);
  const std::uint64_t out_hash = nat_bihash_hash(&out_kv);
  int rc_out = nat_bihash_add_del_with_hash(out2in_bihash_, &out_kv, out_hash, 1 /* is_add */);
  if (rc_out != 0) [[unlikely]] {
    nat_bihash_add_del_with_hash(in2out_bihash_, &in_kv, in_hash, 0 /* is_del */);
    sessions_[session_idx] = NatSession{};
    sessions_[session_idx].generation = new_gen;
    free_indices_.push_back(session_idx);
    ++telemetry_.create_failures;
    ++telemetry_.bihash_insert_failures;
    return nullptr;
  }

  port_to_session_[PortSlot(PublicIpIndex(out_key.src_ip), out_key.src_port)] =
      session_idx;
  LruPushTail(session_idx, ClassOfProtocol(in_key.protocol, false));
  MarkFastActive(session_idx, static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL));
  HotInsert(in_key, HashKey(in_key), session_idx);
  ++session_count_;
  ++telemetry_.new_sessions_created;
  if (session_count_ > telemetry_.active_sessions_high_watermark) {
    telemetry_.active_sessions_high_watermark = session_count_;
  }
  ++total_translations_;
  return &sessions_[session_idx];
}



NatSession* ThreadSessionTable::CreateSession(const FiveTuple& in_key,
                                              const FiveTuple& out_key) noexcept {
  auto outcome = LookupIn2OutOutcome(in_key);
  if (outcome.result == LookupResult::Hit && outcome.session) {
    outcome.session->outside = out_key;
    if (!lazy_updates_) {
      outcome.session->last_active_ts = current_timestamp_ns();
      outcome.session->total_packets++;
    }
    return outcome.session;
  }
  auto* s = CreateSessionWithOutcome(outcome, in_key, out_key);
  if (s && !lazy_updates_) {
    s->total_packets = 1;
  }
  return s;
}

bool ThreadSessionTable::DeleteSessionByIdx(std::uint32_t session_idx) noexcept {
  if (session_idx >= sessions_.size()) return false;
  auto& session = sessions_[session_idx];
  if (session.inside.src_ip == 0 && session.inside.dst_ip == 0) return false;

  const auto in_kv = FiveTupleToKv(session.inside, session_idx);
  nat_bihash_add_del(in2out_bihash_, &in_kv, 0 /* is_del */);

  const auto out_kv = FiveTupleToKv(session.outside, session_idx);
  nat_bihash_add_del(out2in_bihash_, &out_kv, 0 /* is_del */);

  ReleasePortForUser(session.inside.src_ip, session.outside.src_ip,
                     session.outside.src_port);
  LruRemove(session_idx);

  const std::uint16_t next_gen = static_cast<std::uint16_t>(session.generation + 1);
  session = NatSession{};
  session.generation = next_gen;
  MarkFastFree(session_idx);
  free_indices_.push_back(session_idx);
  if (session_count_ > 0) --session_count_;
  return true;
}

bool ThreadSessionTable::DeleteSession(const FiveTuple& in_key) noexcept {
  const auto kv = FiveTupleToKv(in_key);
  nat_bihash_kv_t res{};
  if (nat_bihash_search(in2out_bihash_, &kv, &res) != 0) {
    return false;
  }
  const auto session_idx = DecodeSessionIndex(res.value);
  return DeleteSessionByIdx(session_idx);
}

bool ThreadSessionTable::DeleteSessionByIdxIfCurrent(
    std::uint32_t session_idx) noexcept {
  if (session_idx >= sessions_.size()) return false;
  auto& session = sessions_[session_idx];
  if (session.inside.src_ip == 0 && session.inside.dst_ip == 0) return false;

  const auto in_kv = FiveTupleToKv(session.inside, session_idx);
  nat_bihash_kv_t res_in{};
  if (nat_bihash_search(in2out_bihash_, &in_kv, &res_in) == 0 &&
      static_cast<std::uint32_t>(res_in.value) == session_idx) {
    nat_bihash_add_del(in2out_bihash_, &in_kv, 0 /* is_del */);
  }

  const auto out_kv = FiveTupleToKv(session.outside, session_idx);
  nat_bihash_kv_t res_out{};
  if (nat_bihash_search(out2in_bihash_, &out_kv, &res_out) == 0 &&
      static_cast<std::uint32_t>(res_out.value) == session_idx) {
    nat_bihash_add_del(out2in_bihash_, &out_kv, 0 /* is_del */);
  }

  ReleasePortForUser(session.inside.src_ip, session.outside.src_ip,
                     session.outside.src_port);
  LruRemove(session_idx);

  const std::uint16_t next_gen = static_cast<std::uint16_t>(session.generation + 1);
  session = NatSession{};
  session.generation = next_gen;
  MarkFastFree(session_idx);
  free_indices_.push_back(session_idx);
  if (session_count_ > 0) --session_count_;
  return true;
}

void ThreadSessionTable::LruTouch(std::uint32_t session_idx) noexcept {
  const LruClass new_class = ClassOfSession(sessions_[session_idx]);
  const auto stored = static_cast<LruClass>(
      (sessions_[session_idx].flags & kSessionFlagLruMask) >> kSessionFlagLruShift);

  if (new_class != stored) {
    LruRemove(session_idx);
    LruPushTail(session_idx, new_class);
    return;
  }

  const auto index = static_cast<std::size_t>(new_class);
  if (session_idx == lru_tail_[index]) return;
  LruRemove(session_idx);
  LruPushTail(session_idx, new_class);
}

void ThreadSessionTable::LruPushTail(std::uint32_t session_idx, LruClass cls) noexcept {
  const auto index = static_cast<std::size_t>(cls);
  if (lru_tail_[index] == kInvalidIndex) {
    lru_head_[index] = lru_tail_[index] = session_idx;
    lru_nodes_[session_idx] = LruNode{kInvalidIndex, kInvalidIndex};
  } else {
    lru_nodes_[session_idx] = LruNode{lru_tail_[index], kInvalidIndex};
    lru_nodes_[lru_tail_[index]].next_idx = session_idx;
    lru_tail_[index] = session_idx;
  }
  SetLruClass(sessions_[session_idx], cls);
}

void ThreadSessionTable::LruRemove(std::uint32_t session_idx) noexcept {
  if (session_idx >= lru_nodes_.size()) return;
  const auto index = static_cast<std::size_t>(
      (sessions_[session_idx].flags & kSessionFlagLruMask) >> kSessionFlagLruShift);
  const auto prev = lru_nodes_[session_idx].prev_idx;
  const auto next = lru_nodes_[session_idx].next_idx;

  if (prev != kInvalidIndex) {
    lru_nodes_[prev].next_idx = next;
  } else if (lru_head_[index] == session_idx) {
    lru_head_[index] = next;
  }

  if (next != kInvalidIndex) {
    lru_nodes_[next].prev_idx = prev;
  } else if (lru_tail_[index] == session_idx) {
    lru_tail_[index] = prev;
  }

  lru_nodes_[session_idx] = LruNode{kInvalidIndex, kInvalidIndex};
}

std::uint32_t ThreadSessionTable::LruPopOldestHead() noexcept {
  std::uint32_t victim = kInvalidIndex;
  std::uint64_t oldest = ~std::uint64_t{0};
  for (std::size_t cls = 0; cls < kLruClassCount; ++cls) {
    const std::uint32_t head = lru_head_[cls];
    if (head == kInvalidIndex) continue;
    if (sessions_[head].last_active_ts < oldest) {
      oldest = sessions_[head].last_active_ts;
      victim = head;
    }
  }
  if (victim != kInvalidIndex) {
    LruRemove(victim);
  }
  return victim;
}

std::uint32_t ThreadSessionTable::LruFindExpiredHead(std::uint64_t now_ns) noexcept {
  std::uint32_t victim = kInvalidIndex;
  std::uint64_t oldest = ~std::uint64_t{0};
  for (std::size_t cls = 0; cls < kLruClassCount; ++cls) {
    const std::uint32_t head = lru_head_[cls];
    if (head == kInvalidIndex) continue;
    if (!IsExpired(sessions_[head], now_ns)) continue;
    if (sessions_[head].last_active_ts < oldest) {
      oldest = sessions_[head].last_active_ts;
      victim = head;
    }
  }
  return victim;
}

void ThreadSessionTable::ReclaimBatch(std::size_t batch_size) noexcept {
  const std::size_t count = std::min(batch_size, retire_queue_.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto entry = retire_queue_[i];
    if (entry.session_idx >= sessions_.size()) continue;
    auto& s = sessions_[entry.session_idx];
    if (s.generation != entry.generation || (s.flags & kSessionFlagRetiring) == 0) {
      continue;
    }
    __builtin_prefetch(&s, 1, 3);
    const nat_bihash_kv_t in_kv = FiveTupleToKv(s.inside);
    const std::uint64_t in_hash = nat_bihash_hash(&in_kv);
    nat_bihash_prefetch_bucket_hash(in2out_bihash_, in_hash);
    const nat_bihash_kv_t out_kv = FiveTupleToKv(s.outside);
    const std::uint64_t out_hash = nat_bihash_hash(&out_kv);
    nat_bihash_prefetch_bucket_hash(out2in_bihash_, out_hash);
  }

  for (std::size_t i = 0; i < count; ++i) {
    const auto entry = retire_queue_[i];
    if (entry.session_idx < sessions_.size()) {
      auto& s = sessions_[entry.session_idx];
      if (s.generation == entry.generation && (s.flags & kSessionFlagRetiring)) {
        DeleteSessionByIdx(entry.session_idx);
      }
    }
  }
  if (count == retire_queue_.size()) {
    retire_queue_.clear();
  } else {
    retire_queue_.erase(retire_queue_.begin(), retire_queue_.begin() + static_cast<std::ptrdiff_t>(count));
  }
}

void ThreadSessionTable::ProcessTick(std::uint64_t now_ns) noexcept {
  if (now_ns == 0) {
    now_ns = current_timestamp_ns();
  }

  std::size_t budget{kDefaultReclaimBatchSize};
  while (budget-- > 0) {
    const std::uint32_t victim = LruFindExpiredHead(now_ns);
    if (victim == kInvalidIndex) {
      break;
    }
    LruRemove(victim);
    sessions_[victim].flags |= kSessionFlagRetiring;
    MarkFastRetiring(victim);
    retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
    ++telemetry_.expired_reclaims;
  }

  if (!retire_queue_.empty()) {
    ReclaimBatch(std::min<std::size_t>(retire_queue_.size(), kDefaultReclaimBatchSize * 2));
  }
}

namespace {
[[nodiscard]] constexpr std::uint16_t BlockBaseForPort(std::uint16_t port,
                                                       std::uint16_t block_size) noexcept {
  if (port < PBA_MIN_PORT || block_size == 0) return 0;
  const std::uint32_t offset = static_cast<std::uint32_t>(port - PBA_MIN_PORT);
  return static_cast<std::uint16_t>(
      PBA_MIN_PORT + (offset / static_cast<std::uint32_t>(block_size)) * block_size);
}
}  // namespace

void ThreadSessionTable::ExportSessions(nat_session_export_cb_t cb, void* ctx) const noexcept {
  if (cb == nullptr || sessions_.data() == nullptr) return;
  for (std::size_t i{0}; i < sessions_.size(); ++i) {
    const NatSession& session = sessions_[i];
    if ((session.flags & kSessionFlagActive) == 0) continue;
    if (session.flags & kSessionFlagRetiring) continue;

    nat_session_record_t record{};
    record.last_active_ts = session.last_active_ts;
    record.thread_index = thread_index_;
    record.inside_src_ip = session.inside.src_ip;
    record.inside_dst_ip = session.inside.dst_ip;
    record.inside_src_port = session.inside.src_port;
    record.inside_dst_port = session.inside.dst_port;
    record.outside_src_ip = session.outside.src_ip;
    record.outside_dst_ip = session.outside.dst_ip;
    record.outside_src_port = session.outside.src_port;
    record.outside_dst_port = session.outside.dst_port;
    record.block_size = block_size_;
    record.block_base_port = BlockBaseForPort(session.outside.src_port, block_size_);
    record.flags = session.flags;
    record.generation = session.generation;
    record.protocol = session.inside.protocol;
    cb(ctx, &record);
  }
}

bool ThreadSessionTable::ReservePortForUser(std::uint32_t user_ip, std::uint32_t public_ip,
                                            std::uint16_t base_port, std::uint16_t block_size,
                                            std::uint16_t port) noexcept {
  if (block_size != 64) return false;
  if (port < base_port || port >= static_cast<std::uint32_t>(base_port) + block_size) {
    return false;
  }

  std::uint32_t sub_idx = user_ip & kSubscriberMask;
  std::size_t probes = 0;
  std::size_t first_tombstone = kSubscribersPerWorker;
  PbaSubscriber* sub = nullptr;

  while (probes < kSubscribersPerWorker) {
    auto& candidate = subscribers_[sub_idx];
    if (candidate.state == SubscriberState::kEmpty) break;
    if (candidate.state == SubscriberState::kTombstone) {
      if (first_tombstone == kSubscribersPerWorker) first_tombstone = sub_idx;
    } else if (candidate.user_ip == user_ip) {
      sub = &candidate;
      break;
    }
    sub_idx = (sub_idx + 1) & kSubscriberMask;
    ++probes;
  }

  if (sub == nullptr) {
    if (probes >= kSubscribersPerWorker) {
      if (first_tombstone == kSubscribersPerWorker) return false;
      sub_idx = static_cast<std::uint32_t>(first_tombstone);
    }
    sub = &subscribers_[sub_idx];
    *sub = PbaSubscriber{};
    sub->user_ip = user_ip;
    sub->state = SubscriberState::kOccupied;
  }

  const std::size_t ip_idx = PublicIpIndex(public_ip);
  for (std::uint8_t b{0}; b < sub->num_blocks; ++b) {
    if (sub->blocks[b].base_port == base_port &&
        (sub->blocks[b].pad % public_ip_count_) == ip_idx) {
      const std::uint32_t bit = static_cast<std::uint32_t>(port - base_port);
      sub->blocks[b].bitmap |= (1ULL << bit);
      sub->blocks[b].last_used_ts = current_timestamp_ns();
      return true;
    }
  }

  if (sub->num_blocks >= max_blocks_per_user_) return false;
  if (base_port < PBA_MIN_PORT) return false;
  const std::size_t local_block =
      static_cast<std::size_t>(base_port - PBA_MIN_PORT) / 64;
  if (local_block >= kBlocksPerPublicIp) return false;
  const std::size_t block_idx = ip_idx * kBlocksPerPublicIp + local_block;
  if (block_idx >= kTotalBlocks) return false;
  const std::uint64_t bit = 1ULL << (block_idx % 64);
  if (pba_block_bitmap_[block_idx / 64] & bit) return false;

  pba_block_bitmap_[block_idx / 64] |= bit;
  auto& block = sub->blocks[sub->num_blocks++];
  pba_block_init(&block, base_port, block_size);
  block.pad = static_cast<std::uint32_t>(ip_idx);
  block.bitmap |= (1ULL << static_cast<std::uint32_t>(port - base_port));
  block.last_used_ts = current_timestamp_ns();
  ++telemetry_.pba_blocks_allocated;
  return true;
}

bool ThreadSessionTable::ImportSession(const nat_session_record_t& record) noexcept {
  if (record.protocol != 6 && record.protocol != 17 && record.protocol != 1) return false;
  if (record.outside_src_port < PBA_MIN_PORT) return false;

  const FiveTuple in_key{
      .src_ip{record.inside_src_ip},
      .dst_ip{record.inside_dst_ip},
      .src_port{record.inside_src_port},
      .dst_port{record.inside_dst_port},
      .protocol{record.protocol},
  };
  const FiveTuple out_key{
      .src_ip{record.outside_src_ip},
      .dst_ip{record.outside_dst_ip},
      .src_port{record.outside_src_port},
      .dst_port{record.outside_dst_port},
      .protocol{record.protocol},
  };

  auto outcome = LookupIn2OutOutcome(in_key, 0, 0);
  if (outcome.result == LookupResult::Hit && outcome.session != nullptr) {
    outcome.session->outside = out_key;
    outcome.session->flags = record.flags;
    outcome.session->last_active_ts = record.last_active_ts;
    return true;
  }

  if (!ReservePortForUser(in_key.src_ip, out_key.src_ip, record.block_base_port,
                          record.block_size, out_key.src_port)) {
    return false;
  }
  if (free_indices_.empty()) return false;

  const std::uint32_t session_idx = free_indices_.back();
  free_indices_.pop_back();

  NatSession session{};
  session.inside = in_key;
  session.outside = out_key;
  session.last_active_ts = record.last_active_ts;
  session.flags = static_cast<std::uint16_t>(record.flags | kSessionFlagActive);
  session.flags = static_cast<std::uint16_t>(
      session.flags & static_cast<std::uint16_t>(~kSessionFlagRetiring));
  session.generation = record.generation;
  session.last_lru_move_sec =
      static_cast<std::uint32_t>(record.last_active_ts / 1'000'000'000ULL);
  sessions_[session_idx] = session;

  const auto in_kv = FiveTupleToKv(
      in_key, EncodeIn2OutValue(session_idx, out_key.src_ip, out_key.src_port));
  const std::uint64_t in_hash = nat_bihash_hash(&in_kv);
  if (nat_bihash_add_del_with_hash(in2out_bihash_, &in_kv, in_hash, 1) != 0) {
    sessions_[session_idx] = NatSession{};
    free_indices_.push_back(session_idx);
    ReleasePortForUser(in_key.src_ip, out_key.src_ip, out_key.src_port);
    return false;
  }
  const auto out_kv = FiveTupleToKv(out_key, session_idx);
  const std::uint64_t out_hash = nat_bihash_hash(&out_kv);
  if (nat_bihash_add_del_with_hash(out2in_bihash_, &out_kv, out_hash, 1) != 0) {
    nat_bihash_add_del_with_hash(in2out_bihash_, &in_kv, in_hash, 0);
    sessions_[session_idx] = NatSession{};
    free_indices_.push_back(session_idx);
    ReleasePortForUser(in_key.src_ip, out_key.src_ip, out_key.src_port);
    return false;
  }

  port_to_session_[PortSlot(PublicIpIndex(out_key.src_ip), out_key.src_port)] =
      session_idx;
  LruPushTail(session_idx,
              ClassOfProtocol(record.protocol,
                              (record.flags & kSessionFlagTcpEstablished) != 0));
  MarkFastActive(session_idx,
                 static_cast<std::uint32_t>(record.last_active_ts / 1'000'000'000ULL));
  HotInsert(in_key, HashKey(in_key), session_idx);
  ++session_count_;
  return true;
}

void ThreadSessionTable::FlushSessions() noexcept {
  if (sessions_.data() == nullptr) return;
  for (std::size_t i{0}; i < sessions_.size(); ++i) {
    if ((sessions_[i].flags & kSessionFlagActive) != 0) {
      DeleteSessionByIdx(static_cast<std::uint32_t>(i));
    }
  }
}

std::optional<PortAllocation> ThreadSessionTable::AllocatePortForUser(
    std::uint32_t user_ip, std::uint32_t public_ip) noexcept {
  (void)public_ip;
  std::uint32_t sub_idx = user_ip & kSubscriberMask;
  std::size_t probes = 0;
  std::size_t first_tombstone = kSubscribersPerWorker;
  PbaSubscriber* sub = nullptr;

  // Open-addressed lookup. Tombstones are skipped but remembered so the slot
  // can be reused after the probe sequence has been searched for an existing
  // subscriber.
  while (probes < kSubscribersPerWorker) {
    auto& candidate = subscribers_[sub_idx];
    if (candidate.state == SubscriberState::kEmpty) {
      break;
    }
    if (candidate.state == SubscriberState::kTombstone) {
      if (first_tombstone == kSubscribersPerWorker) {
        first_tombstone = sub_idx;
      }
    } else if (candidate.user_ip == user_ip) {
      sub = &candidate;
      break;
    }
    sub_idx = (sub_idx + 1) & kSubscriberMask;
    ++probes;
  }

  if (sub == nullptr) {
    if (probes >= kSubscribersPerWorker) {
      if (first_tombstone == kSubscribersPerWorker) [[unlikely]] {
        ++telemetry_.pba_port_alloc_failures;
        return std::nullopt;
      }
      sub_idx = static_cast<std::uint32_t>(first_tombstone);
    }
    auto& fresh = subscribers_[sub_idx];
    fresh = PbaSubscriber{};
    fresh.user_ip = user_ip;
    fresh.num_blocks = 0;
    fresh.state = SubscriberState::kOccupied;
    sub = &fresh;
  }

  auto block_ip = [&](const pba_block_t& block) noexcept -> std::uint32_t {
    return public_ips_[block.pad % public_ip_count_];
  };

  // 1. Try existing blocks across all public IPs.
  for (std::uint8_t b = 0; b < sub->num_blocks; ++b) {
    std::uint16_t port{0};
    if (pba_alloc_port(&sub->blocks[b], &port)) {
      sub->blocks[b].last_used_ts = current_timestamp_ns();
      return PortAllocation{port, block_ip(sub->blocks[b])};
    }
  }

  // 2. Allocate a new block from the per-IP block bitmap if within quota.
  auto try_alloc_block = [&]() noexcept -> std::optional<PortAllocation> {
    if (sub->num_blocks < max_blocks_per_user_) {
      for (std::size_t w = 0; w < pba_block_bitmap_.size(); ++w) {
        std::uint64_t free_bits = ~pba_block_bitmap_[w];
        if (w == pba_block_bitmap_.size() - 1) {
          constexpr std::size_t kRem = kTotalBlocks % 64;
          if constexpr (kRem > 0) {
            free_bits &= ((1ULL << kRem) - 1ULL);
          }
        }
        if (free_bits == 0) continue;
        const int bit = __builtin_ctzll(free_bits);
        const std::size_t block_idx = w * 64 + static_cast<std::size_t>(bit);
        if (block_idx >= kTotalBlocks) continue;
        const std::size_t ip_idx = block_idx / kBlocksPerPublicIp;
        const std::size_t local_block = block_idx % kBlocksPerPublicIp;
        pba_block_bitmap_[w] |= (1ULL << bit);
        const auto base_port =
            static_cast<std::uint16_t>(PBA_MIN_PORT + local_block * 64);
        auto& blk = sub->blocks[sub->num_blocks++];
        pba_block_init(&blk, base_port, block_size_);
        blk.pad = static_cast<std::uint32_t>(ip_idx);
        ++telemetry_.pba_blocks_allocated;

        std::uint16_t port{0};
        if (pba_alloc_port(&blk, &port)) {
          blk.last_used_ts = current_timestamp_ns();
          return PortAllocation{port, public_ips_[ip_idx]};
        }
      }
    }
    return std::nullopt;
  };

  if (auto port = try_alloc_block(); port.has_value()) {
    return port;
  }

  // 3. Fallback: reclaim expired sessions first to free blocks
  constexpr std::size_t kEvictBatch = 64;
  const std::uint64_t now_ns = current_timestamp_ns();
  for (std::size_t ev = 0; ev < kEvictBatch; ++ev) {
    const std::uint32_t victim = LruFindExpiredHead(now_ns);
    if (victim == kInvalidIndex) break;
    LruRemove(victim);
    sessions_[victim].flags |= kSessionFlagRetiring;
    MarkFastRetiring(victim);
    retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
    ++telemetry_.expired_reclaims;
  }
  if (!retire_queue_.empty()) {
    ReclaimBatch(std::min<std::size_t>(retire_queue_.size(), kDefaultReclaimBatchSize * 4));
  }

  if (force_evict_on_overflow_) {
    for (std::size_t ev = 0; ev < kEvictBatch; ++ev) {
      const std::uint32_t victim = LruPopOldestHead();
      if (victim == kInvalidIndex) break;
      sessions_[victim].flags |= kSessionFlagRetiring;
      MarkFastRetiring(victim);
      retire_queue_.push_back(RetireEntry{victim, sessions_[victim].generation});
      ++telemetry_.lru_evictions;
    }
    if (!retire_queue_.empty()) {
      ReclaimBatch(std::min<std::size_t>(retire_queue_.size(), kDefaultReclaimBatchSize * 4));
    }
  }

  // Releasing the last port of this subscriber may have tombstoned its slot.
  if (sub->state != SubscriberState::kOccupied) {
    *sub = PbaSubscriber{};
    sub->user_ip = user_ip;
    sub->state = SubscriberState::kOccupied;
  }

  for (std::uint8_t b = 0; b < sub->num_blocks; ++b) {
    std::uint16_t port{0};
    if (pba_alloc_port(&sub->blocks[b], &port)) {
      sub->blocks[b].last_used_ts = current_timestamp_ns();
      return PortAllocation{port, block_ip(sub->blocks[b])};
    }
  }
  if (auto port = try_alloc_block(); port.has_value()) {
    return port;
  }

  if (sub->num_blocks == 0) {
    *sub = PbaSubscriber{};
    sub->state = SubscriberState::kTombstone;
  }

  ++telemetry_.pba_port_alloc_failures;
  return std::nullopt;
}

bool ThreadSessionTable::ReleasePortForUserIp(std::uint32_t user_ip,
                                               std::uint32_t public_ip,
                                               std::uint16_t port) noexcept {
  const std::size_t ip_idx = PublicIpIndex(public_ip);
  std::uint32_t sub_idx = user_ip & kSubscriberMask;
  std::size_t probes = 0;
  PbaSubscriber* sub = nullptr;

  while (probes < kSubscribersPerWorker) {
    auto& candidate = subscribers_[sub_idx];
    if (candidate.state == SubscriberState::kEmpty) {
      return false;
    }
    if (candidate.state == SubscriberState::kOccupied && candidate.user_ip == user_ip) {
      sub = &candidate;
      break;
    }
    sub_idx = (sub_idx + 1) & kSubscriberMask;
    ++probes;
  }

  if (sub == nullptr) return false;

  for (std::uint8_t b = 0; b < sub->num_blocks; ++b) {
    if ((sub->blocks[b].pad % public_ip_count_) != ip_idx) continue;
    if (!pba_free_port(&sub->blocks[b], port)) continue;

    port_to_session_[PortSlot(ip_idx, port)] = kInvalidIndex;
    if (pba_is_empty(&sub->blocks[b])) {
      const std::uint16_t base = sub->blocks[b].base_port;
      if (base >= PBA_MIN_PORT) {
        const std::size_t local_block =
            static_cast<std::size_t>(base - PBA_MIN_PORT) / 64;
        const std::size_t block_idx = ip_idx * kBlocksPerPublicIp + local_block;
        if (block_idx < kTotalBlocks) {
          pba_block_bitmap_[block_idx / 64] &= ~(1ULL << (block_idx % 64));
          ++telemetry_.pba_blocks_freed;
        }
      }
      if (b + 1 < sub->num_blocks) {
        sub->blocks[b] = sub->blocks[sub->num_blocks - 1];
      }
      --sub->num_blocks;
      if (sub->num_blocks == 0) {
        *sub = PbaSubscriber{};
        sub->state = SubscriberState::kTombstone;
      }
    }
    return true;
  }
  return false;
}

void ThreadSessionTable::ReleasePortForUser(std::uint32_t user_ip,
                                             std::uint16_t port) noexcept {
  for (std::size_t i = 0; i < public_ip_count_; ++i) {
    if (ReleasePortForUserIp(user_ip, public_ips_[i], port)) return;
  }
}

void ThreadSessionTable::ReleasePortForUser(std::uint32_t user_ip,
                                             std::uint32_t public_ip,
                                             std::uint16_t port) noexcept {
  (void)ReleasePortForUserIp(user_ip, public_ip, port);
}

NatEngine::NatEngine(std::size_t num_threads, std::size_t sessions_per_worker,
                     std::size_t expected_active) noexcept {
  const auto threads{std::max<std::size_t>(num_threads, 1)};
  sessions_per_worker_ = sessions_per_worker;
  expected_active_ = expected_active;
  thread_tables_.reserve(threads);
  for (std::size_t i{0}; i < threads; ++i) {
    thread_tables_.emplace_back(sessions_per_worker, expected_active);
    thread_tables_.back().SetThreadIndex(static_cast<std::uint32_t>(i));
    thread_tables_.back().SetMaxSessions(sessions_per_worker);
  }
  SetPool(pool_start_ip_, pool_end_ip_);
}

std::uint64_t NatEngine::GetTotalSessions() const noexcept {
  return std::ranges::fold_left(
      thread_tables_, std::uint64_t{0},
      [](std::uint64_t sum, const ThreadSessionTable& table) { return sum + table.SessionCount(); });
}

std::uint64_t NatEngine::GetTotalTranslations() const noexcept {
  return std::ranges::fold_left(
      thread_tables_, std::uint64_t{0}, [](std::uint64_t sum, const ThreadSessionTable& table) {
        return sum + table.TotalTranslations();
      });
}

}  // namespace nat::engine
