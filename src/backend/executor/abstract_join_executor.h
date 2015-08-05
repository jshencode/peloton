//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// abstract_join_executor.h
//
// Identification: src/backend/executor/abstract_join_executor.h
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


#pragma once

#include "backend/catalog/schema.h"
#include "backend/executor/abstract_executor.h"
#include "backend/planner/project_info.h"

#include <vector>

namespace peloton {
namespace executor {

class AbstractJoinExecutor : public AbstractExecutor {
  AbstractJoinExecutor(const AbstractJoinExecutor &) = delete;
  AbstractJoinExecutor &operator=(const AbstractJoinExecutor &) = delete;

public:
  explicit AbstractJoinExecutor(planner::AbstractPlanNode *node,
                                ExecutorContext *executor_context);

protected:
  bool DInit();

  bool DExecute() = 0;

protected:
  //===--------------------------------------------------------------------===//
  // Helper
  //===--------------------------------------------------------------------===//
  std::vector<LogicalTile::ColumnInfo>
  BuildSchema(std::vector<LogicalTile::ColumnInfo> left,
              std::vector<LogicalTile::ColumnInfo> right);

  //===--------------------------------------------------------------------===//
  // Executor State
  //===--------------------------------------------------------------------===//

  /** @brief Result of nested loop join. */
  std::vector<LogicalTile *> result;

  /** @brief Starting left table scan. */
  bool left_scan_start = false;

  //===--------------------------------------------------------------------===//
  // Plan Info
  //===--------------------------------------------------------------------===//

  /** @brief Join predicate. */
  const expression::AbstractExpression *predicate_ = nullptr;

  /** @brief Projection info */
  const planner::ProjectInfo *proj_info_ = nullptr;
};

} // namespace executor
} // namespace peloton
