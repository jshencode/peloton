//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// set_op_node.h
//
// Identification: src/backend/planner/set_op_node.h
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


#pragma once

#include "backend/common/types.h"
#include "backend/planner/abstract_plan_node.h"

namespace peloton {
namespace planner {

/**
 * @brief Plan node for set operation:
 * INTERSECT/INTERSECT ALL/EXPECT/EXCEPT ALL
 *
 * @warning UNION (ALL) is handled differently.
 * IMPORTANT: Both children must have the same physical schema.
 */
class SetOpNode : public AbstractPlanNode {
public:
  SetOpNode(const SetOpNode &) = delete;
  SetOpNode &operator=(const SetOpNode &) = delete;
  SetOpNode(const SetOpNode &&) = delete;
  SetOpNode &operator=(const SetOpNode &&) = delete;

  SetOpNode(SetOpType set_op) : set_op_(set_op) {}

  SetOpType GetSetOp() const { return set_op_; }

  inline PlanNodeType GetPlanNodeType() const { return PLAN_NODE_TYPE_SETOP; }

  inline std::string GetInfo() const { return "SetOp"; }

private:
  /** @brief Set Operation of this node */
  SetOpType set_op_;
};
}
}
