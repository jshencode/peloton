//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// thread_pool_test.cpp
//
// Identification: test/common/thread_pool_test.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


#include "common/thread_pool.h"
#include "common/harness.h"

namespace peloton {
namespace test {

//===--------------------------------------------------------------------===//
// Thread Pool Test
//===--------------------------------------------------------------------===//

class ThreadPoolTests : public PelotonTest {};


TEST_F(ThreadPoolTests, BasicTest) {

  ThreadPool thread_pool;

  auto num_threads = thread_pool.GetNumThreads();
  LOG_INFO("Num threads : %lu", num_threads);

}

}  // End test namespace
}  // End peloton namespace
