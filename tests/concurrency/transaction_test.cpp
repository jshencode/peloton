//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// transaction_test.cpp
//
// Identification: tests/concurrency/transaction_test.cpp
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "harness.h"
#include "concurrency/transaction_tests_util.h"

namespace peloton {

namespace test {

//===--------------------------------------------------------------------===//
// Transaction Tests
//===--------------------------------------------------------------------===//

class TransactionTests : public PelotonTest {};

static std::vector<ConcurrencyType> TEST_TYPES = {CONCURRENCY_TYPE_OCC,
                                                  CONCURRENCY_TYPE_2PL};

void TransactionTest(concurrency::TransactionManager *txn_manager) {
  uint64_t thread_id = TestingHarness::GetInstance().GetThreadId();

  for (oid_t txn_itr = 1; txn_itr <= 50; txn_itr++) {
    txn_manager->BeginTransaction();
    if (thread_id % 2 == 0) {
      std::chrono::microseconds sleep_time(1);
      std::this_thread::sleep_for(sleep_time);
    }

    if (txn_itr % 25 != 0) {
      txn_manager->CommitTransaction();
    } else {
      txn_manager->AbortTransaction();
    }
  }
}

TEST_F(TransactionTests, TransactionTest) {
  for (auto test_type : TEST_TYPES) {
    concurrency::TransactionManagerFactory::Configure(test_type);
    auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();

    LaunchParallelTest(8, TransactionTest, &txn_manager);

    std::cout << "next Commit Id :: " << txn_manager.GetNextCommitId() << "\n";
  }
}

TEST_F(TransactionTests, SingleTransactionTest) {
  for (auto test_type : TEST_TYPES) {
    concurrency::TransactionManagerFactory::Configure(test_type);
    auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
    std::unique_ptr<storage::DataTable> table(
        TransactionTestsUtil::CreateTable());
    // read, read, read, read
    {
      TransactionScheduler scheduler(1, table.get(), &txn_manager);
      scheduler.Txn(0).Read(0);
      scheduler.Txn(0).Read(0);
      scheduler.Txn(0).Read(0);
      scheduler.Txn(0).Read(0);
      scheduler.Txn(0).Commit();

      scheduler.Run();

      EXPECT_EQ(RESULT_SUCCESS, scheduler.schedules[0].txn_result);
      EXPECT_EQ(0, scheduler.schedules[0].results[0]);
      EXPECT_EQ(0, scheduler.schedules[0].results[1]);
      EXPECT_EQ(0, scheduler.schedules[0].results[2]);
      EXPECT_EQ(0, scheduler.schedules[0].results[3]);
    }
   
  }
}

TEST_F(TransactionTests, AbortTest) {
  for (auto test_type : TEST_TYPES) {
    concurrency::TransactionManagerFactory::Configure(test_type);
    auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
    std::unique_ptr<storage::DataTable> table(
        TransactionTestsUtil::CreateTable());
    {
      TransactionScheduler scheduler(2, table.get(), &txn_manager);
      scheduler.Txn(0).Update(0, 100);
      scheduler.Txn(0).Abort();
      scheduler.Txn(1).Read(0);
      scheduler.Txn(1).Commit();

      scheduler.Run();

      EXPECT_EQ(RESULT_ABORTED, scheduler.schedules[0].txn_result);
      EXPECT_EQ(RESULT_SUCCESS, scheduler.schedules[1].txn_result);
      EXPECT_EQ(0, scheduler.schedules[1].results[0]);
    }

    {
      TransactionScheduler scheduler(2, table.get(), &txn_manager);
      scheduler.Txn(0).Insert(100, 0);
      scheduler.Txn(0).Abort();
      scheduler.Txn(1).Read(100);
      scheduler.Txn(1).Commit();

      scheduler.Run();
      EXPECT_EQ(RESULT_ABORTED, scheduler.schedules[0].txn_result);
      EXPECT_EQ(RESULT_SUCCESS, scheduler.schedules[1].txn_result);
      EXPECT_EQ(-1, scheduler.schedules[1].results[0]);
    }
  }
}

}  // End test namespace
}  // End peloton namespace
