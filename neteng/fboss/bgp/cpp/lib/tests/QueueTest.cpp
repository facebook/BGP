/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <folly/Function.h>
#include <folly/Random.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/Queue.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/Utils.h"

using namespace facebook::nettools::bgplib;
using namespace folly::fibers;
using namespace std::chrono_literals;

namespace {
// Time duration to account for jitter of timer firing
constexpr auto kTimerGracePeriod = std::chrono::milliseconds(20);
} // namespace

/*
 * The fixture provides fiber manager and evb for the tests
 */
class QueueFixture : public ::testing::Test {
 public:
  QueueFixture() = default;
  ~QueueFixture() override = default;

  void SetUp() override {
    manager_ = std::make_unique<FiberManager>(
        std::make_unique<EventBaseLoopController>());

    static_cast<EventBaseLoopController&>(manager_->loopController())
        .attachEventBase(evb_);
  }

  void TearDown() override {}

  std::unique_ptr<FiberManager> manager_;
  folly::EventBase evb_;
};

/*
 * Queue: single producer, single consumers; trying
 * bounded and unbounded queues, bounded in range 1-8
 */
TEST_F(QueueFixture, QueuePutGet) {
  for (int i = 0; i < 8; i++) {
    manager_->addTask([i]() mutable {
      RWQueue<std::string> queue(i);
      auto reader = queue.getReader();
      auto writer = queue.getWriter();
      addTask([writer = std::move(writer)]() mutable {
        for (int j = 0; j < 16; j++) {
          XLOGF(DBG4, "Putting {}", j);
          writer.put(fmt::format("test-{}", j));
        }
        XLOG(DBG4, "Done putting value");
      });

      addTask([reader = std::move(reader)]() mutable {
        for (int j = 0; j < 16; j++) {
          auto val = reader.get();
          ASSERT_TRUE(val);
          XLOGF(DBG4, "Got {}", *val);
          EXPECT_EQ(fmt::format("test-{}", j), *val);
        }
        XLOG(DBG4, "Done getting value");
      });
    });
  }

  evb_.loop();
}

/*
 * Multiple writers, single consumer, unbounded
 */
TEST_F(QueueFixture, QueueMultipleWritersSingleConsumer) {
  const int kNumWriters = 1024;
  const int kNumMessages = 256;

  manager_->addTask([]() mutable {
    RWQueue<std::string> queue;

    // writers
    for (int i = 0; i < kNumWriters; i++) {
      auto writer = queue.getWriter();
      addTask([writer = std::move(writer), i]() mutable {
        for (int j = 0; j < kNumMessages; j++) {
          XLOGF(DBG4, "Putting {}:{}", i, j);
          writer.put(fmt::format("test-{}-{}", i, j));
        }
        XLOG(DBG4, "Done putting value");
      });
    }

    // read all and make sure we received everything
    auto reader = queue.getReader();
    addTask([reader = std::move(reader)]() mutable {
      std::set<std::string> values;
      for (int i = 0; i < kNumWriters * kNumMessages; i++) {
        auto val = reader.get();
        ASSERT_TRUE(val);
        XLOGF(DBG4, "Got {}", *val);
        values.insert(*val);
      }

      XLOG(DBG4, "Done getting values");

      for (int i = 0; i < kNumWriters; i++) {
        for (int j = 0; j < kNumMessages; j++) {
          auto str = fmt::format("test-{}-{}", i, j);
          EXPECT_TRUE(values.contains(str));
        }
      }
    });
  });

  evb_.loop();
}

/*
 * Multiple writers, single consumer, unbounded; We create
 * N writer threads, and single reader thread. This is to
 * valide Queue will not croak with multiple threads.
 */
TEST(QueueTest, MultipleThreadWritersSingleConsumer) {
  RWQueue<std::string> queue;

  const int kNumWriters = 256;
  const int kNumMessages = 256;

  folly::EventBase evb;
  auto& manager_ = folly::fibers::getFiberManager(evb);

  // read all and make sure we received everything
  auto reader = queue.getReader();
  manager_.addTask([reader = std::move(reader)]() mutable {
    std::set<std::string> values;
    for (int i = 0; i < kNumWriters * kNumMessages; i++) {
      auto val = reader.get();
      ASSERT_TRUE(val);
      XLOGF(DBG4, "Got {}", *val);
      values.insert(*val);
    }

    XLOG(DBG4, "Done getting values");

    for (int i = 0; i < kNumWriters; i++) {
      for (int j = 0; j < kNumMessages; j++) {
        auto str = fmt::format("test-{}-{}", i, j);
        EXPECT_TRUE(values.contains(str));
      }
    }
  });

  std::vector<std::thread> writers;
  for (int i = 0; i < kNumWriters; i++) {
    std::thread t([i, wqueue = queue.getWriter()]() mutable {
      folly::EventBase localEvb;
      auto& localManager_ = folly::fibers::getFiberManager(localEvb);

      localManager_.addTask([i, w = std::move(wqueue)]() mutable {
        for (int j = 0; j < kNumMessages; j++) {
          XLOGF(DBG4, "Putting {}:{}", i, j);
          w.put(fmt::format("test-{}-{}", i, j));
        }
        XLOGF(DBG4, "Thread {} done putting value", i);
      });
      localEvb.loop();
    });
    writers.emplace_back(std::move(t));
  }

  evb.loop();

  for (auto& t : writers) {
    t.join();
  }
}

/*
 * Single writer overflows the queue
 */
TEST_F(QueueFixture, QueueOverflow) {
  RWQueue<int> queue(2);
  manager_->addTask([w = queue.getWriter()]() mutable {
    w.put(1);
    w.put(2);
    w.put(3);
  });

  manager_->addTask([r = queue.getReader()]() mutable {
    // wait till the other dude clogs the queue
    while (!r.full()) {
      fiberSleepFor(0ms);
    }
    auto r1 = r.get();
    EXPECT_EQ(1, *r1);
    auto r2 = r.get();
    EXPECT_EQ(2, *r2);
    auto r3 = r.get();
    EXPECT_EQ(3, *r3);
  });

  evb_.loop();
}

/*
 * Block multiple readers on queue, wake them up
 */
TEST_F(QueueFixture, WakeUpReaders) {
  RWQueue<int> queue(1);
  int readerCount{0};
  manager_->addTask([&readerCount, r = queue.getReader()]() mutable {
    readerCount++;
    r.get();
    SUCCEED();
  });

  manager_->addTask([&readerCount, r = queue.getReader()]() mutable {
    readerCount++;
    r.get();
    SUCCEED();
  });

  manager_->addTask([&readerCount, r = queue.getReader()]() mutable {
    readerCount++;
    r.get();
    SUCCEED();
  });

  manager_->addTask([&readerCount, q = queue.share()]() mutable {
    // wait till the other dude clogs the queue
    while (readerCount < 3) {
      fiberSleepFor(0ms);
    }
    q.put(1);
    q.put(2);
    q.put(3);
  });

  evb_.loop();
  EXPECT_TRUE(queue.empty());
}

// block two writers on the queue, close queue
TEST_F(QueueFixture, QueueCloseWithPublishers) {
  // single-element queue
  RWQueue<int> queue(1);

  // this put must succeed
  manager_->addTask([w = queue.getWriter()]() mutable {
    auto v = w.put(1);
    EXPECT_TRUE(v);
  });

  int writerCount = 0;
  manager_->addTask([&writerCount, w = queue.getWriter()]() mutable {
    // wait for full queue so we owould be blocked
    while (!w.full()) {
      fiberSleepFor(0ms);
    }
    writerCount++;
    auto v = w.put(1);
    EXPECT_FALSE(v);
  });

  manager_->addTask([&writerCount, w = queue.getWriter()]() mutable {
    while (!w.full()) {
      fiberSleepFor(0ms);
    }
    writerCount++;
    auto v = w.put(1);
    EXPECT_FALSE(v);
  });

  manager_->addTask([&writerCount, q = queue.share()]() mutable {
    // wait till we have both writers blocked
    while (writerCount < 2) {
      fiberSleepFor(0ms);
    }
    q.close();
  });

  evb_.loop();
  EXPECT_TRUE(queue.empty());
}

/*
 * Block bunch of readers on queue, then close it
 */
TEST_F(QueueFixture, QueueCloseWithConsumers) {
  RWQueue<int> queue(1);
  int readerCount{0};
  manager_->addTask([&readerCount, r = queue.getReader()]() mutable {
    readerCount++;
    auto v = r.get();
    EXPECT_FALSE(v);
  });

  manager_->addTask([&readerCount, r = queue.getReader()]() mutable {
    readerCount++;
    auto v = r.get();
    EXPECT_FALSE(v);
  });

  manager_->addTask([&readerCount, r = queue.getReader()]() mutable {
    readerCount++;
    auto v = r.get();
    EXPECT_FALSE(v);
  });

  manager_->addTask([&readerCount, q = queue.share()]() mutable {
    // wait till the other dude clogs the queue
    while (readerCount < 3) {
      fiberSleepFor(0ms);
    }
    q.close();
    EXPECT_FALSE(q.put(1));
    EXPECT_TRUE(q.empty());
  });

  evb_.loop();
  EXPECT_TRUE(queue.empty());
}

/*
 * Single writer, multiple consumers
 */
TEST_F(QueueFixture, QueueSingleWriterMultipleConsumers) {
  std::set<std::string> receivedValues;

  manager_->addTask([&receivedValues]() mutable {
    RWQueue<std::string> queue;
    // writer
    auto writer = queue.getWriter();
    addTask([writer = std::move(writer)]() mutable {
      for (int j = 0; j < 256; j++) {
        XLOGF(DBG4, "Putting {}", j);
        writer.put(fmt::format("test-{}", j));
      }
      XLOG(DBG4, "Done putting values");
    });

    // start and wait for all readers
    std::vector<folly::Function<void()>> consumers;
    for (int i = 0; i < 16; i++) {
      auto reader = queue.getReader();
      auto consumer =
          [reader = std::move(reader), &receivedValues, i]() mutable {
            XLOGF(DBG4, "Reader {} starting", i);
            for (int j = 0; j < 16; j++) {
              // reader is wrapped
              auto val = reader.get();
              ASSERT_TRUE(val);
              XLOGF(DBG4, "Reader {} got {}", i, *val);
              receivedValues.insert(*val);
            }
            XLOGF(DBG4, "Reader {} done getting values", i);
          };
      consumers.emplace_back(std::move(consumer));
    }

    collectAll(consumers.begin(), consumers.end());

    for (int i = 0; i < 256; i++) {
      auto str = fmt::format("test-{}", i);
      EXPECT_TRUE(receivedValues.contains(str)) << str;
    }
  });

  evb_.loop();
}

/*
 * Multiple writers, multiple consumers, bounded/unbounded
 */
TEST_F(QueueFixture, QueueMultipleWritersMultipleConsumers) {
  for (int k = 0; k < 8; k++) {
    // all of received values
    auto receivedValues = std::make_shared<std::set<std::string>>();
    manager_->addTask([k, receivedValues]() mutable {
      RWQueue<std::string> queue(k);
      std::vector<folly::Function<void()>> workers;

      // schedule writers
      for (int i = 0; i < 16; i++) {
        auto writer = queue.getWriter();
        workers.emplace_back([writer = std::move(writer), i]() mutable {
          for (int j = 0; j < 16; j++) {
            XLOGF(DBG4, "Writer {} putting {}", i, j);
            writer.put(fmt::format("test-{}-{}", i, j));
          }
          XLOGF(DBG4, "Writer {} done putting values", i);
        });
      }

      // schedule readers
      for (int i = 0; i < 16; i++) {
        workers.emplace_back(
            [reader = queue.getReader(), receivedValues, i]() mutable {
              for (int j = 0; j < 16; j++) {
                auto val = reader.get();
                ASSERT_TRUE(val);
                XLOGF(DBG4, "Reader {} got {}", i, *val);
                receivedValues->insert(*val);
              }
              XLOGF(DBG4, "Reader {} done getting values", i);
            });
      }

      XLOGF(DBG4, "Collecting {} worker", workers.size());
      collectAll(workers.begin(), workers.end());
      XLOG(DBG4, "Done collecting!");

      for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
          auto str = fmt::format("test-{}-{}", i, j);
          EXPECT_TRUE(receivedValues->contains(str)) << str;
        }
      }
    });
  }

  evb_.loop();
}

/*
 * Demonstrate merge operation. Notice that this assumes there
 * only one publisher for each merged queue
 */
TEST_F(QueueFixture, QueueMerge) {
  /*
   * need to keep it in main scope, so that wrapper task
   * does not call the d-tor. You can optionally pass the
   * shared-ptr to the fiber that still refer to these queues
   */
  std::vector<RWQueue<std::pair<int, int>>> queues;
  manager_->addTask([&queues]() mutable {
    for (int i = 0; i < 64; i++) {
      RWQueue<std::pair<int, int>> queue;
      queues.push_back(queue.share());
      addTask([queue = std::move(queue), i]() mutable {
        for (int j = 0; j < 64; j++) {
          queue.put({i, j});
        }
        queue.putNull();
      });
    }

    addTask([&queues]() mutable {
      std::set<std::pair<int, int>> values;
      RWQueue<std::pair<int, int>> output;
      auto merged = output.getWriter();
      mergeQueues(queues.begin(), queues.end(), merged);

      while (true) {
        auto val = output.get();
        if (!val) {
          break;
        }
        XLOGF(DBG4, "{} : {}", val->first, val->second);
        values.emplace(*val);
      }

      for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j++) {
          EXPECT_TRUE(values.contains(std::make_pair(i, j)));
        }
      }
    });
  });
  evb_.loop();
}

/*
 * Wait on data or timeout using queue merge
 */
TEST_F(QueueFixture, DataOrTimeout) {
  struct TimeoutEvent {};
  using CombinedType = std::variant<int, TimeoutEvent>;
  using namespace std::chrono;

  struct CombinedVisitor {
    CombinedVisitor(
        std::function<void(int const&)> fnInt,
        std::function<void(TimeoutEvent const&)> fnTimeout)
        : fnInt_(std::move(fnInt)), fnTimeout_(std::move(fnTimeout)) {}

    void operator()(const int& val) {
      fnInt_(val);
    }
    void operator()(const TimeoutEvent& val) {
      fnTimeout_(val);
    }

    const std::function<void(int const&)> fnInt_;
    const std::function<void(TimeoutEvent const&)> fnTimeout_;
  };

  manager_->addTask([this]() {
    for (int i = 0; i < 16; i++) {
      /*
       * this actor will produce timeout events
       */
      RWQueue<CombinedType> timeoutQueue;
      auto timeoutQueueW = timeoutQueue.getWriter();
      addTask([timeoutQueueW = std::move(timeoutQueueW)]() mutable {
        for (int j = 0; j < 64; j++) {
          fiberSleepFor(milliseconds(10));
          timeoutQueueW.put(TimeoutEvent{});
        }
        timeoutQueueW.putNull();
      });

      RWQueue<CombinedType> dataQueue;
      auto dataQueueR = dataQueue.getReader();
      auto timeoutQueueR = timeoutQueue.getReader();

      /*
       * This is used to merge data and timeouts
       */
      std::vector<RQueue<CombinedType>> inputQueues;
      inputQueues.emplace_back(std::move(dataQueueR));
      inputQueues.emplace_back(std::move(timeoutQueueR));

      /*
       * The merging actor and queue
       */
      RWQueue<CombinedType> mergedQueue;
      addTask([inputQueues = std::move(inputQueues), &mergedQueue]() mutable {
        mergeQueues(inputQueues.begin(), inputQueues.end(), mergedQueue);
      });

      std::set<int> allValues;

      /*
       * The visitor collects values from the merged queue and stores
       * all of the numeric ones in the set
       */
      auto visitor = CombinedVisitor(
          [&i, &allValues](int const& val) {
            i++;
            XLOGF(DBG4, "Got value {}", val);
            allValues.insert(val);
          },
          [](TimeoutEvent const&) { XLOG(DBG4, "Got timeout!"); });

      /*
       * this actor consumes data and timeout events
       */
      auto consumer =
          manager_->addTaskFuture([&visitor, &mergedQueue]() mutable {
            while (true) {
              auto val = mergedQueue.get();
              if (!val) {
                break;
              }
              std::visit(visitor, *val);
            }
          });

      /*
       * This actor produces data
       */
      auto dataQueueW = dataQueue.getWriter();
      addTask([dataQueueW = std::move(dataQueueW)]() mutable {
        for (int j = 0; j < 64; j++) {
          fiberSleepFor(
              std::chrono::milliseconds(folly::Random::rand32() % 10));
          dataQueueW.put(std::move(j));
        }
        dataQueueW.putNull();
      });

      std::move(consumer).get();
      for (int j = 0; j < 64; j++) {
        EXPECT_TRUE(allValues.contains(j));
      }

    } // for
  });

  evb_.loop();
}

/*
 * Merging queues at compile time
 */
TEST_F(QueueFixture, queueMergeStatic) {
  struct TimeoutEvent {};
  using CombinedType = std::variant<int, TimeoutEvent>;
  using namespace std::chrono;

  struct CombinedVisitor {
    CombinedVisitor(
        std::function<void(int const&)> fnInt,
        std::function<void(TimeoutEvent const&)> fnTimeout)
        : fnInt_(std::move(fnInt)), fnTimeout_(std::move(fnTimeout)) {}

    void operator()(const int& val) {
      fnInt_(val);
    }
    void operator()(const TimeoutEvent& val) {
      fnTimeout_(val);
    }

    const std::function<void(int const&)> fnInt_;
    const std::function<void(TimeoutEvent const&)> fnTimeout_;
  };

  manager_->addTask([this]() {
    RWQueue<int> dataQueue;
    RWQueue<TimeoutEvent> timeoutQueue;
    // this is the queue where we put merged data
    RWQueue<CombinedType> outputQueue;

    /*
     * this actor will produce timeout events
     */
    auto timeoutQueueW = timeoutQueue.getWriter();
    addTask([timeoutQueueW = std::move(timeoutQueueW)]() mutable {
      for (int i = 0; i < 64; i++) {
        fiberSleepFor(milliseconds(10));
        timeoutQueueW.put(TimeoutEvent{});
      }
      timeoutQueueW.putNull();
    });

    std::set<int> allValues;

    /*
     * The visitor collects values from the merged queue and stores
     * all of the numeric ones in the set
     */
    auto visitor = CombinedVisitor(
        [&allValues](int const& val) {
          XLOGF(DBG4, "Got value {}", val);
          allValues.insert(val);
        },
        [](TimeoutEvent const&) { XLOG(DBG4, "Got timeout!"); });

    /*
     * this actor consumes data and timeout events
     */
    auto outputQueueR = outputQueue.getReader();
    auto consumer = manager_->addTaskFuture(
        [&visitor, outputQueueR = std::move(outputQueueR)]() mutable {
          while (true) {
            auto val = outputQueueR.get();
            if (!val) {
              break;
            }
            std::visit(visitor, *val);
          }
        });

    /*
     * This actor produces data
     */
    auto dataQueueW = dataQueue.getWriter();
    addTask([&allValues, dataQueueW = std::move(dataQueueW)]() mutable {
      for (int i = 0; i < 64; i++) {
        fiberSleepFor(milliseconds(folly::Random::rand32() % 10));
        dataQueueW.put(std::move(i));
      }
      dataQueueW.putNull();
    });

    /*
     * This worker merges queue
     */
    addTask([&outputQueue, &dataQueue, &timeoutQueue]() mutable {
      mergeQueuesStatic(
          outputQueue, dataQueue.getReader(), timeoutQueue.getReader());
    });

    // wait for the consumer to finish
    std::move(consumer).get();
    for (int j = 0; j < 64; j++) {
      EXPECT_TRUE(allValues.contains(j));
    }
  });
  evb_.loop();
}

/*
 * Test the basic timer logic
 */
TEST_F(QueueFixture, BasicTimerTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] { timer.run(); });

  manager_->addTask([&timer] {
    auto start = std::chrono::steady_clock::now();
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_TRUE(msg);
    auto finish = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
    EXPECT_LE(elapsed, std::chrono::milliseconds(50) + kTimerGracePeriod);
  });

  evb_.loop();
}

/*
 * Test timer stop logic
 */
TEST_F(QueueFixture, TimerStopTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] { timer.run(); });

  manager_->addTask([&timer] {
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_FALSE(msg);
  });

  manager_->addTask([&timer] {
    fiberSleepFor(std::chrono::milliseconds(20));
    timer.stop();
  });

  evb_.loop();
}

/*
 * Test timer run exists before stop
 */
TEST_F(QueueFixture, TimerStopAfterRunComplete) {
  struct Timeout {};
  bool runFiberDone = false;
  bool stopDone = false;
  Timer<Timeout> timer{std::chrono::milliseconds(20)};

  manager_->addTask([&] {
    timer.run();
    runFiberDone = true;
  });

  manager_->addTask([&] {
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_TRUE(msg);
  });

  manager_->addTask([&] {
    fiberSleepFor(std::chrono::milliseconds(50));
    timer.stop();
    stopDone = true;
  });

  evb_.loop();
  ASSERT_TRUE(runFiberDone);
  ASSERT_TRUE(stopDone);
}

/*
 * Test ticker with multiple call to stop() when run is in progress.
 */
TEST_F(QueueFixture, TimerMultipleStopDuringRun) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(70)};
  manager_->addTask([&timer] {
    fiberSleepFor(std::chrono::milliseconds(10));
    timer.stop();
    fiberSleepFor(std::chrono::milliseconds(10));
    timer.stop();
  });

  manager_->addTask([&timer] {
    timer.run();
    SUCCEED();
  });

  manager_->addTask([&timer] {
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_FALSE(msg);
  });

  evb_.loop();
}
/*
 * Test timer race condition when stop is called before run
 */
TEST_F(QueueFixture, TimerWaitTillRunningTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] { timer.stop(); });

  manager_->addTask([&timer] {
    /*
     * delay before start to mimic race condition issue.
     * test stop is properly called after we wait till ticker is running
     */
    fiberSleepFor(std::chrono::milliseconds(20));
    timer.run();
    SUCCEED();
  });

  manager_->addTask([&timer] {
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_FALSE(msg);
  });

  evb_.loop();
}

/*
 * Test timer stop logic when run is never called
 */
TEST_F(QueueFixture, TimerStopWithoutRunTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] {
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_FALSE(msg);
  });

  manager_->addTask([&timer] {
    fiberSleepFor(std::chrono::milliseconds(20));
    timer.stop();
  });

  evb_.loop();
}

/*
 * Test timer reset logic
 */
TEST_F(QueueFixture, TimerResetTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] { timer.run(); });

  /*
   * the timer should expired in 50 ms, but the other fiber
   * resets it 30 ms later, so total wait time should be 80ms
   */
  manager_->addTask([&timer] {
    auto start = std::chrono::steady_clock::now();
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_TRUE(msg);
    auto finish = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    EXPECT_GE(elapsed, std::chrono::milliseconds(80));
    EXPECT_LE(elapsed, std::chrono::milliseconds(80) + kTimerGracePeriod);
  });

  manager_->addTask([&timer] {
    fiberSleepFor(std::chrono::milliseconds(30));
    timer.reset();
  });

  evb_.loop();
}

/*
 * Test timer reset to a new time logic
 */
TEST_F(QueueFixture, TimerResetToNewTimeTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] { timer.run(); });

  /*
   * the timer should expire in 50 ms, but the other fiber
   * resets it 30 ms later, to new time of 70 ms
   * so total wait time should be 100ms
   */
  manager_->addTask([&timer] {
    auto start = std::chrono::steady_clock::now();
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_TRUE(msg);
    auto finish = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    EXPECT_GE(elapsed, std::chrono::milliseconds(100));
    EXPECT_LE(elapsed, std::chrono::milliseconds(100) + kTimerGracePeriod);
  });

  manager_->addTask([&timer] {
    fiberSleepFor(std::chrono::milliseconds(30));
    timer.reset(std::chrono::milliseconds(70));
  });

  evb_.loop();
}

/*
 * Test timer reuse logic
 */
TEST_F(QueueFixture, TimerReuseTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(50)};

  manager_->addTask([&timer] { timer.run(); });

  /*
   * the timer expires in 50 ms, and the other fiber
   * reschedules it when it expires.
   * The total wait time should be 100ms.
   */
  manager_->addTask([&timer] {
    auto start = std::chrono::steady_clock::now();
    auto queue = timer.getQueue();
    auto msg = queue.get();
    ASSERT_TRUE(msg);

    addTask([&timer] { timer.run(); });

    msg = queue.get();
    ASSERT_TRUE(msg);

    auto finish = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    EXPECT_GE(elapsed, std::chrono::milliseconds(100));
    EXPECT_LE(elapsed, std::chrono::milliseconds(100) + kTimerGracePeriod);
  });

  evb_.loop();
}

/*
 * Test the basic timer with 0 sec logic
 */
TEST_F(QueueFixture, Timerwith0SecTest) {
  struct Timeout {};

  Timer<Timeout> timer{std::chrono::milliseconds(0)};

  manager_->addTask([&timer] { timer.run(); });

  manager_->addTask([&timer] {
    auto start = std::chrono::steady_clock::now();
    auto queue = timer.getQueue();
    ASSERT_TRUE(queue.get());
    auto finish = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    EXPECT_GE(elapsed, std::chrono::milliseconds(0));
    EXPECT_LE(elapsed, kTimerGracePeriod);
  });

  evb_.loop();
}
