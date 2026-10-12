#include "serin_link/sl2_rxq.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
  sl2_rxq_t queue;
  sl2_rxq_init(&queue);
  std::atomic<bool> start{false};
  constexpr uint32_t count = 20000;
  const uint8_t src[6]{2, 1, 2, 3, 4, 5}, dst[6]{2, 5, 4, 3, 2, 1};
  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t data[SL2_RXQ_FRAME];
      memset(data, i % 251, sizeof data);
      memcpy(data, &i, sizeof i);
      while (!sl2_rxq_push(&queue, src, dst, data, sizeof data)) std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    start.store(true, std::memory_order_release);
    for (uint32_t i = 0; i < count; ++i) {
      sl2_rxq_frame_t frame;
      while (!sl2_rxq_pop(&queue, &frame)) std::this_thread::yield();
      assert(frame.len == SL2_RXQ_FRAME);
      assert(memcmp(frame.src, src, 6) == 0 && memcmp(frame.dst, dst, 6) == 0);
      uint32_t sequence;
      memcpy(&sequence, frame.data, sizeof sequence);
      assert(sequence == i);
      for (unsigned j = sizeof sequence; j < frame.len; ++j) assert(frame.data[j] == i % 251);
      // Exercise the diagnostic reader concurrently with overflow accounting.
      (void) sl2_rxq_dropped(&queue);
    }
  });
  producer.join();
  consumer.join();
  puts("RX queue concurrent payload integrity: PASS");
}
