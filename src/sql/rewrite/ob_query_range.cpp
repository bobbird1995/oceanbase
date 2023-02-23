/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_REWRITE
#include "lib/timezone/ob_time_convert.h"
#include "lib/container/ob_array_serialization.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/rewrite/ob_query_range.h"
#include "sql/engine/expr/ob_expr_result_type_util.h"
#include "sql/engine/expr/ob_expr_like.h"
#include "common/ob_smart_call.h"
#include "sql/optimizer/ob_optimizer_util.h"

//if cnd is true get full range key part which is always true
//else, get empty key part which is always false

#define GET_ALWAYS_TRUE_OR_FALSE(cnd, out_key_part) \
    do { \
      if (OB_SUCC(ret)) { \
        query_range_ctx_->cur_expr_is_precise_ = false; \
        if (OB_ISNULL(table_graph_.key_part_head_)) { \
          ret = OB_ERR_NULL_VALUE; \
          LOG_WARN("Can not find key_part");  \
        } else if (cnd) { \
          if (OB_FAIL(alloc_full_key_part(out_key_part))) { \
            LOG_WARN("alloc_full_key_part failed", K(ret)); \
          } else { \
            out_key_part->id_ = table_graph_.key_part_head_->id_;  \
            out_key_part->pos_ = table_graph_.key_part_head_->pos_;  \
          } \
        } else { \
          if (OB_FAIL(alloc_empty_key_part(out_key_part))) { \
            LOG_WARN("alloc_empty_key_part failed", K(ret)); \
          } else if (OB_ISNULL(out_key_part)) { \
            ret = OB_ALLOCATE_MEMORY_FAILED; \
            LOG_ERROR("out_key_part is null.", K(ret)); \
          } else { \
            out_key_part->id_ = table_graph_.key_part_head_->id_;  \
            out_key_part->pos_ = table_graph_.key_part_head_->pos_;  \
          } \
        } \
      } \
    } while(0)

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{
ObQueryRange::ObQueryRange()
    : state_(NEED_INIT),
      column_count_(0),
      contain_row_(false),
      inner_allocator_(ObModIds::OB_SQL_QUERY_RANGE),
      allocator_(inner_allocator_),
      query_range_ctx_(NULL),
      key_part_store_(allocator_),
      range_exprs_(allocator_),
      has_exec_param_(true),
      is_equal_and_(false),
      equal_offs_(allocator_),
      expr_final_infos_(allocator_)
{
}

ObQueryRange::ObQueryRange(ObIAllocator &alloc)
    : state_(NEED_INIT),
      column_count_(0),
      contain_row_(false),
      inner_allocator_(ObModIds::OB_SQL_QUERY_RANGE),
      allocator_(alloc),
      query_range_ctx_(NULL),
      key_part_store_(allocator_),
      range_exprs_(allocator_),
      has_exec_param_(true),
      is_equal_and_(false),
      equal_offs_(allocator_),
      expr_final_infos_(allocator_)
{
}

ObQueryRange::~ObQueryRange()
{
  reset();
}

ObQueryRange &ObQueryRange::operator=(const ObQueryRange &other)
{
  if (this != &other) {
    reset();
    deep_copy(other);
  }
  return *this;
}

void ObQueryRange::reset()
{
  DLIST_FOREACH_NORET(node, key_part_store_.get_obj_list()) {
    if (node != NULL && node->get_obj() != NULL) {
      node->get_obj()->~ObKeyPart();
    }
  }
  key_part_store_.destroy();
  query_range_ctx_ = NULL;
  state_ = NEED_INIT;
  column_count_ = 0;
  contain_row_ = false;
  table_graph_.reset();
  range_exprs_.reset();
  inner_allocator_.reset();
  has_exec_param_ = true;
  is_equal_and_ = false;
  equal_offs_.reset();
  expr_final_infos_.reset();
}

int ObQueryRange::init_query_range_ctx(ObIAllocator &allocator,
                                       const ColumnIArray &range_columns,
                                       ObExecContext *exec_ctx,
                                       ExprConstrantArray *expr_constraints,
                                       const ParamsIArray *params,
                                       const bool phy_rowid_for_table_loc,
                                       const bool ignore_calc_failure)
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  uint64_t table_id = OB_INVALID_ID;

  if (range_columns.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("range column array is empty");
  } else if (OB_ISNULL(ptr = allocator.alloc(sizeof(ObQueryRangeCtx)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc query range context failed");
  } else {
    query_range_ctx_ = new(ptr) ObQueryRangeCtx(exec_ctx, expr_constraints, params);
    query_range_ctx_->phy_rowid_for_table_loc_ = phy_rowid_for_table_loc;
    query_range_ctx_->ignore_calc_failure_ = ignore_calc_failure;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < range_columns.count(); ++i) {
    const ColumnItem &col = range_columns.at(i);
    if (OB_UNLIKELY(col.is_invalid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column item is invalid", K_(col.expr));
    } else {
      ObKeyPartId key_part_id(col.table_id_, col.column_id_);
      const ObExprResType *expr_res_type = col.get_column_type();
      void *ptr = NULL;
      if (OB_ISNULL(expr_res_type)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr result type is null", K(ret));
      } else if (OB_ISNULL(ptr = allocator.alloc(sizeof(ObKeyPartPos)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate memeory for ObKeyPartPos", K(ret));
      } else {
        ObExprResType tmp_expr_type = *expr_res_type;
        if (tmp_expr_type.is_lob_locator()) {
          tmp_expr_type.set_type(ObLongTextType);
        }
        ObKeyPartPos *key_part_pos = new(ptr) ObKeyPartPos(i, tmp_expr_type);
        table_id = (i > 0 ? table_id : col.table_id_);

        if (OB_UNLIKELY(table_id != col.table_id_)) { // 所有 range column 的 table_id 需要相同
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("range columns must have the same table id", K(table_id), K_(col.table_id));
        } else if (OB_FAIL(key_part_pos->set_enum_set_values(allocator_, col.expr_->get_enum_set_values()))) {
          LOG_WARN("fail to set values", K(key_part_pos), K(ret));
        } else if (OB_FAIL(query_range_ctx_->key_part_map_.set_refactored(key_part_id, key_part_pos))) {
          LOG_WARN("set key part map failed", K(ret), K(key_part_id));
        } else if (OB_FAIL(query_range_ctx_->key_part_pos_array_.push_back(key_part_pos))) {
          LOG_WARN("failed to push back key part pos", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    // Add the default range of the index and remember the count of the rowkeys.
    // Just to handle the full range case
    // E.g.
    //       select * from t where true;
    ObKeyPart *full_key_part = NULL;
    if (OB_FAIL(alloc_full_key_part(full_key_part))) {
      LOG_WARN("alloc_full_key_part failed", K(ret));
    } else if (OB_ISNULL(full_key_part)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("full_key_part is null.", K(ret));
    } else {
      full_key_part->id_ = ObKeyPartId(table_id, OB_INVALID_ID);
      full_key_part->pos_ = ObKeyPartPos(allocator_, -1);
      table_graph_.key_part_head_ = full_key_part;
      column_count_ = range_columns.count();
    }
  }
  if (OB_SUCC(ret)) {
    if (query_range_ctx_->final_expr_map_.create(20, ObModIds::OB_QUERY_RANGE_CTX)) {
      LOG_WARN("failed to create final expr map", K(ret));
    }
  }
  if (OB_SUCCESS != ret && NULL != query_range_ctx_) {
    destroy_query_range_ctx(allocator);
  }
  return ret;
}

void ObQueryRange::destroy_query_range_ctx(ObIAllocator &ctx_allocator)
{
  if (NULL != query_range_ctx_) {
    for (int64_t i = 0; i < query_range_ctx_->key_part_pos_array_.count(); ++i) {
      if (NULL != query_range_ctx_->key_part_pos_array_.at(i)) {
        query_range_ctx_->key_part_pos_array_.at(i)->~ObKeyPartPos();
        ctx_allocator.free(query_range_ctx_->key_part_pos_array_.at(i));
      }
    }
    query_range_ctx_->~ObQueryRangeCtx();
    ctx_allocator.free(query_range_ctx_);
    query_range_ctx_ = NULL;
  }
}

int ObQueryRange::preliminary_extract_query_range(const ColumnIArray &range_columns,
                                                  const ObRawExpr *expr_root,
                                                  const ObDataTypeCastParams &dtc_params,
                                                  ObExecContext *exec_ctx,
                                                  ExprConstrantArray *expr_constraints /* = NULL */,
                                                  const ParamsIArray *params /* = NULL */)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator ctx_allocator(ObModIds::OB_QUERY_RANGE_CTX);
  if (OB_FAIL(init_query_range_ctx(ctx_allocator, range_columns, exec_ctx, expr_constraints, params))) {
    LOG_WARN("init query range context failed", K(ret));
  } else if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("query_range_ctx_ is not inited.", K(ret));
  } else {
    query_range_ctx_->need_final_extact_ = false;
    ObKeyPart *root = NULL;
    if (OB_UNLIKELY(NULL == expr_root)) {
      //(MIN, MAX), whole range
      GET_ALWAYS_TRUE_OR_FALSE(true, root);
    } else {
      if (OB_FAIL(preliminary_extract(expr_root, root, dtc_params, T_OP_IN == expr_root->get_expr_type()))) {
        LOG_WARN("gen table range failed", K(ret));
      } else if (query_range_ctx_->cur_expr_is_precise_ && root != NULL) {
        // for simple in_expr
        int64_t max_pos = -1;
        bool is_strict_equal = true;
        if (OB_FAIL(is_strict_equal_graph(root, 0, max_pos, is_strict_equal))) {
          LOG_WARN("is strict equal graph failed", K(ret));
        } else if (is_strict_equal) {
          ObRangeExprItem expr_item;
          expr_item.cur_expr_ = expr_root;
          for (const ObKeyPart *cur_and = root; OB_SUCC(ret) && cur_and != NULL; cur_and = cur_and->and_next_) {
            if (OB_FAIL(expr_item.cur_pos_.push_back(cur_and->pos_.offset_))) {
              LOG_WARN("push back pos offset failed", K(ret));
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(query_range_ctx_->precise_range_exprs_.push_back(expr_item))) {
            LOG_WARN("store precise range exprs failed", K(ret));
          }
        } else if (NULL == root->and_next_ && is_general_graph(*root)) {
          //因为optimizer去filter只能够在顶端去，而且去filter的目标是合取范式
          //标准的合取范式例如(c1>1 or c1<0) and c2=1这样的表达式在resolver必须拆分成多个表达式
          //如果没有拆分，这样的表达式也不能被去掉，因为去表达式每次都是一个完整的表达式
          //这个表达式包含了多个键，要么全部去掉，要么都不去掉，显然全部去掉是不对的
          ObRangeExprItem expr_item;
          expr_item.cur_expr_ = expr_root;
          if (OB_FAIL(expr_item.cur_pos_.push_back(root->pos_.offset_))) {
            LOG_WARN("push back pos offset failed", K(ret));
          } else if (OB_FAIL(query_range_ctx_->precise_range_exprs_.push_back(expr_item))) {
            LOG_WARN("store precise range exprs failed", K(ret));
          }
        }
      }
    }
    if (OB_SUCC(ret) && NULL != root) {
      if (OB_FAIL(refine_large_range_graph(root))) {
        LOG_WARN("failed to refine large range graph", K(ret));
      } else if (OB_FAIL(remove_useless_range_graph(root))) {
        LOG_WARN("failed to remove useless range", K(ret));
      } else if (OB_FAIL(generate_expr_final_info())) {
        LOG_WARN("failed to generate final exprs");
      } else {
        SQL_REWRITE_LOG(DEBUG, "root key part", K(*root));
        int64_t max_pos = -1;
        table_graph_.key_part_head_ = root;
        table_graph_.is_standard_range_ = is_standard_graph(root);
        OZ(is_strict_equal_graph(root, 0, max_pos, table_graph_.is_equal_range_));
        OZ(check_graph_type());
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (query_range_ctx_->need_final_extact_) {
      state_ = NEED_PREPARE_PARAMS;
    } else {
      state_ = CAN_READ;
    }
  }
  destroy_query_range_ctx(ctx_allocator);
  return ret;
}

int ObQueryRange::preliminary_extract_query_range(const ColumnIArray &range_columns,
                                                  const ExprIArray &root_exprs,
                                                  const ObDataTypeCastParams &dtc_params,
                                                  ObExecContext *exec_ctx,
                                                  ExprConstrantArray *expr_constraints /* = NULL */,
                                                  const ParamsIArray *params /* = NULL */,
                                                  const bool phy_rowid_for_table_loc /* = false*/,
                                                  const bool ignore_calc_failure /* = true*/)
{
  int ret = OB_SUCCESS;
  ObKeyPartList and_ranges;
  ObKeyPart *temp_result = NULL;

  SQL_REWRITE_LOG(DEBUG, "preliminary extract", K(range_columns), K(root_exprs));
  ObArenaAllocator ctx_allocator(ObModIds::OB_QUERY_RANGE_CTX);
  if (OB_FAIL(init_query_range_ctx(ctx_allocator, range_columns, exec_ctx,
                                   expr_constraints, params, phy_rowid_for_table_loc,
                                   ignore_calc_failure))) {
    LOG_WARN("init query range context failed", K(ret));
  } else if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("query_range_ctx_ is not inited.", K(ret));
  } else {
    has_exec_param_ = false;
    if (OB_LIKELY(root_exprs.count() > 0)) {
      for (int64_t j = 0; OB_SUCC(ret) && j < root_exprs.count(); ++j) {
        if (NULL == root_exprs.at(j)) {
          // continue
        } else if (OB_FAIL(preliminary_extract(root_exprs.at(j), temp_result,
            dtc_params, T_OP_IN == root_exprs.at(j)->get_expr_type()))) {
          LOG_WARN("Generate table range failed", K(ret));
        } else if (NULL == temp_result) {
          // ignore the condition from which we can not extract key part range
        } else {
          if (root_exprs.at(j)->has_flag(CNT_DYNAMIC_PARAM)) {
            has_exec_param_ = true;
          }
          if (!and_ranges.add_last(temp_result)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("Add key part range failed", K(ret));
          } else if (query_range_ctx_->cur_expr_is_precise_ && temp_result != NULL) {
            if (is_strict_in_graph(temp_result)) {
              ObRangeExprItem expr_item;
              expr_item.cur_expr_ = root_exprs.at(j);
              for (const ObKeyPart *cur_and = temp_result; OB_SUCC(ret) && cur_and != NULL; cur_and = cur_and->and_next_) {
                if (OB_FAIL(expr_item.cur_pos_.push_back(cur_and->pos_.offset_))) {
                  LOG_WARN("push back pos offset failed", K(ret));
                }
              }
              if (OB_SUCC(ret) && OB_FAIL(query_range_ctx_->precise_range_exprs_.push_back(expr_item))) {
                LOG_WARN("store precise range exprs failed", K(ret));
              }
            } else if (NULL == temp_result->and_next_ && is_general_graph(*temp_result)) {
              //因为optimizer去filter只能够在顶端去，而且去filter的目标是合取范式
              //标准的合取范式例如(c1>1 or c1<0) and c2=1这样的表达式在resolver必须拆分成多个表达式
              //如果没有拆分，这样的表达式也不能被去掉，因为去表达式每次都是一个完整的表达式
              //这个表达式包含了多个键，要么全部去掉，要么都不去掉，显然全部去掉是不对的
              ObRangeExprItem expr_item;
              expr_item.cur_expr_ = root_exprs.at(j);
              if (OB_FAIL(expr_item.cur_pos_.push_back(temp_result->pos_.offset_))) {
                LOG_WARN("push back pos offset failed", K(ret));
              } else if (OB_FAIL(query_range_ctx_->precise_range_exprs_.push_back(expr_item))) {
                LOG_WARN("store precise range exprs failed", K(ret));
              }
            }
          }
        }
      } // for each where condition
    } else {
      GET_ALWAYS_TRUE_OR_FALSE(true, temp_result);
      if (OB_SUCC(ret)) {
        if (!and_ranges.add_last(temp_result)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("add key part range failed", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(and_range_graph(and_ranges, temp_result))) {
      LOG_WARN("And query range failed", K(ret));
    } else if (NULL == temp_result) {
      // no range left
    } else if (OB_FAIL(refine_large_range_graph(temp_result))) {
      LOG_WARN("failed to refine large range graph", K(ret));
    } else if (OB_FAIL(remove_useless_range_graph(temp_result))) {
      LOG_WARN("failed to remove useless range", K(ret));
    } else if (OB_FAIL(generate_expr_final_info())) {
      LOG_WARN("failed to generate final exprs");
    } else {
      int64_t max_pos = -1;
      table_graph_.key_part_head_ = temp_result;
      table_graph_.is_standard_range_ = is_standard_graph(temp_result);
      if (OB_FAIL(is_strict_equal_graph(temp_result,
              0,
              max_pos,
              table_graph_.is_equal_range_))) {
        LOG_WARN("is strict equal graph failed", K(ret));
      } else if (OB_FAIL(check_graph_type())) {
        LOG_WARN("check graph type failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (query_range_ctx_->need_final_extact_) {
      state_ = NEED_PREPARE_PARAMS;
    } else {
      state_ = CAN_READ;
    }
  }
  destroy_query_range_ctx(ctx_allocator);
  return ret;
}

int ObQueryRange::remove_useless_range_graph(ObKeyPart *key_part)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObKeyPart*, 8> key_parts;
  ObSEArray<ObKeyPart*, 8> next_key_parts;
  if (OB_ISNULL(key_part)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("keypart is null", K(ret), K(key_part));
  } else if (OB_FAIL(key_parts.push_back(key_part))) {
    LOG_WARN("failed to push back", K(ret));
  } else {
    while (OB_SUCC(ret) && !key_parts.empty()) {
      if (OB_FAIL(remove_useless_range_graph(key_parts, next_key_parts))) {
        LOG_WARN("failed to remove useless range graph", K(ret));
      } else if (OB_FAIL(key_parts.assign(next_key_parts))) {
        LOG_WARN("failed to assign array", K(ret), K(next_key_parts));
      }
    }
  }
  return ret;
}

int ObQueryRange::remove_useless_range_graph(const ObIArray<ObKeyPart*> &key_parts,
                                             ObIArray<ObKeyPart*> &next_key_parts)
{
  int ret = OB_SUCCESS;
  next_key_parts.reuse();
  ObKeyPart *cur = NULL;
  ObKeyPart *and_next = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < key_parts.count(); ++i) {
    cur = key_parts.at(i);
    while (OB_SUCC(ret) && NULL != cur) {
      if (NULL == (and_next = cur->and_next_)) {
        cur = cur->or_next_;
      } else if (is_and_next_useless(cur)) {
        while (NULL != cur && and_next == cur->and_next_) {
          cur->and_next_ = NULL;
          cur = cur->or_next_;
        }
      } else if (OB_FAIL(next_key_parts.push_back(and_next))) {
        LOG_WARN("failed to push back", K(ret));
      } else {
        while (NULL != cur && and_next == cur->and_next_) {
          cur = cur->or_next_;
        }
      }
    }
  }
  return ret;
}

bool ObQueryRange::is_and_next_useless(ObKeyPart *key_part)
{
  bool is_useless = false;
  ObKeyPart *and_next = NULL;
  if (NULL == key_part || NULL == (and_next = key_part->and_next_)) {
    /* do nothing */
  } else if (1 + key_part->pos_.offset_ == and_next->pos_.offset_) {
    /* do nothing */
  } else {
    is_useless = true;
    for (ObKeyPart *cur = key_part; is_useless && NULL != cur
                                    && and_next == cur->and_next_; cur = cur->or_next_) {
      is_useless = cur->is_equal_condition();
    }
  }
  return is_useless;
}

// if the range size is large then RANGE_MAX_SIZE, remove some ranges according to pos_.offset_
int ObQueryRange::refine_large_range_graph(ObKeyPart *&key_part)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObKeyPart*, 8> pre_key_parts;
  ObSEArray<ObKeyPart*, 8> key_parts;
  ObSEArray<uint64_t, 8> or_count;
  ObSEArray<ObKeyPart*, 8> next_key_parts;
  ObSEArray<uint64_t, 8> next_or_count;
  uint64_t cur_range_size = 1;
  bool need_refine = false;
  if (OB_ISNULL(key_part)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("keypart is null", K(ret), K(key_part));
  } else if (OB_FAIL(key_parts.push_back(key_part)) ||
             OB_FAIL(next_key_parts.push_back(key_part))) {
    LOG_WARN("failed to push back", K(ret));
  } else if (OB_FAIL(or_count.push_back(1))) {
    LOG_WARN("failed to push back", K(ret));
  }
  while (OB_SUCC(ret) && !next_key_parts.empty() && !need_refine) {
    if (OB_FAIL(compute_range_size(key_parts, or_count, next_key_parts, next_or_count,
                                   cur_range_size))) {
      LOG_WARN("failed to compute range size", K(ret));
    } else if (cur_range_size > MAX_RANGE_SIZE) {
      need_refine = true;
    } else if (OB_FAIL(pre_key_parts.assign(key_parts))) {
      LOG_WARN("failed to assign array", K(ret), K(key_parts));
    } else if (OB_FAIL(key_parts.assign(next_key_parts))) {
      LOG_WARN("failed to assign array", K(ret), K(next_key_parts));
    } else if (OB_FAIL(or_count.assign(next_or_count))) {
      LOG_WARN("failed to assign array", K(ret), K(next_or_count));
    } else { /* do nothing */ }
  }
  if (OB_SUCC(ret) && need_refine) {
    if (pre_key_parts.empty()) {
      // first or_next_ list size is large than RANGE_MAX_SIZE, create a full key part
      ObKeyPart *new_key = NULL;
      if (OB_FAIL(alloc_full_key_part(new_key))) {
        LOG_WARN("alloc full key part failed", K(ret));
      } else if (OB_ISNULL(new_key)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("keypart is null");
      } else if (OB_FAIL(remove_precise_range_expr(0))) {
        LOG_WARN("remove precise range expr failed", K(ret));
      } else {
        new_key->id_ = key_part->id_;
        key_part = new_key;
        LOG_TRACE("refine lagre query range with full key", K(cur_range_size));
      }
    } else if (OB_ISNULL(pre_key_parts.at(0))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected input", K(ret), K(pre_key_parts));
    } else if (OB_FAIL(remove_precise_range_expr(pre_key_parts.at(0)->pos_.offset_ + 1))) {
      LOG_WARN("remove precise range expr failed", K(ret));
    } else {
      // remove key part after pre key parts
      LOG_TRACE("refine lagre query range remove some key parts",  K(cur_range_size),
                                                            K(pre_key_parts.at(0)->pos_.offset_));
      ObKeyPart *cur = NULL;;
      ObKeyPart *and_next = NULL;
      for (int64_t i = 0; OB_SUCC(ret) && i < pre_key_parts.count(); ++i) {
        cur = pre_key_parts.at(i);
        while (NULL != cur) {
          if (NULL == cur->and_next_) {
            cur = cur->or_next_;
          } else {
            and_next = cur->and_next_;
            while (NULL != cur && cur->and_next_ == and_next) {
              cur->and_next_ = NULL;
              cur = cur->or_next_;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObQueryRange::compute_range_size(const ObIArray<ObKeyPart*> &key_parts,
                                     const ObIArray<uint64_t> &or_count,
                                     ObIArray<ObKeyPart*> &next_key_parts,
                                     ObIArray<uint64_t> &next_or_count,
                                     uint64_t &range_size)
{
  int ret = OB_SUCCESS;
  next_key_parts.reuse();
  next_or_count.reuse();
  ObKeyPart *pre = NULL;
  ObKeyPart *cur = NULL;
  uint64_t count = 0;
  if (OB_UNLIKELY(key_parts.empty()) || OB_UNLIKELY(key_parts.count() != or_count.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected input", K(ret), K(key_parts.count()), K(or_count.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < key_parts.count(); ++i) {
    if (OB_ISNULL(pre = key_parts.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret), K(i), K(key_parts.at(i)));
    } else {
      range_size -= or_count.at(i);
      cur = pre->or_next_;
      count = 1;
    }
    while (OB_SUCC(ret) && NULL != pre) {
      if (NULL != cur && cur->and_next_ == pre->and_next_) {
        cur = cur->or_next_;
        ++count;
      } else if (NULL != pre->and_next_ &&
                 OB_FAIL(next_key_parts.push_back(pre->and_next_))) {
        LOG_WARN("failed to push back", K(ret));
      } else if (NULL != pre->and_next_ &&
                 OB_FAIL(next_or_count.push_back(or_count.at(i) * count))) {
        LOG_WARN("failed to push back", K(ret));
      } else {
        range_size += or_count.at(i) * count;
        pre = cur;
        cur = NULL != cur ? cur->or_next_ : NULL;
        count = 1;
      }
    }
  }
  return ret;
}

int ObQueryRange::is_at_most_one_row(bool &is_one_row) const
{
  int ret = OB_SUCCESS;
  is_one_row = true;
  if (NULL == table_graph_.key_part_head_) {
    is_one_row = false;
  } else if (OB_FAIL(check_is_at_most_one_row(*table_graph_.key_part_head_, 0,
                                              column_count_, is_one_row))) {
    LOG_WARN("failed to check is get", K(ret));
  }
  return ret;
}

int ObQueryRange::check_is_at_most_one_row(ObKeyPart &key_part,
                                           const int64_t depth,
                                           const int64_t column_count,
                                           bool &bret) const
{
  int ret = OB_SUCCESS;
  if (key_part.pos_.offset_ != depth
      || !key_part.is_equal_condition()
      || NULL != key_part.or_next_) {
    bret = false;
  } else if (NULL != key_part.and_next_) {
    ret = SMART_CALL(check_is_at_most_one_row(*key_part.and_next_,
                                              depth + 1, column_count, bret));
  } else if (depth < column_count - 1) {
    bret = false;
  }
  return ret;
}

int ObQueryRange::is_get(bool &is_range_get) const
{
  return is_get(column_count_, is_range_get);
}

int ObQueryRange::is_get(int64_t column_count,
                         bool &is_range_get) const
{
  int ret = OB_SUCCESS;
  is_range_get = true;
  if (table_graph_.is_precise_get_) {
    // return true
  } else if (NULL == table_graph_.key_part_head_) {
    is_range_get = false;
  } else if (OB_FAIL(check_is_get(*table_graph_.key_part_head_, 0, column_count, is_range_get))) {
    LOG_WARN("failed to check is get", K(ret));
  }
  return ret;
}

int ObQueryRange::check_is_get(ObKeyPart &key_part,
                               const int64_t depth,
                               const int64_t column_count,
                               bool &bret) const
{
  int ret = OB_SUCCESS;
  if (key_part.pos_.offset_ != depth || !key_part.is_equal_condition()) {
    bret = false;
  } else {
    if (NULL != key_part.and_next_) {
      ret = SMART_CALL(check_is_get(*key_part.and_next_, depth + 1, column_count, bret));
    } else if (depth < column_count - 1) {
      bret = false;
    }
    if (OB_SUCC(ret) && bret && NULL != key_part.or_next_) {
      ret = SMART_CALL(check_is_get(*key_part.or_next_, depth, column_count, bret));
    }
  }
  return ret;
}

int ObQueryRange::check_graph_type()
{
  int ret = OB_SUCCESS;
  table_graph_.is_precise_get_ = true;
  if (OB_ISNULL(query_range_ctx_) || OB_ISNULL(table_graph_.key_part_head_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("query isn't init", K_(query_range_ctx), K_(table_graph_.key_part_head));
  }
  if (OB_SUCC(ret)) {
    int64_t max_pos = -1;
    int64_t depth = -1;
    int64_t column_count = column_count_;
    bool is_terminated = false;
    bool is_phy_rowid_key_part = false;
    for (ObKeyPart *cur = table_graph_.key_part_head_; !is_terminated && NULL != cur; cur = cur->and_next_) {
      if (cur->pos_.offset_ != (++depth)) {
        table_graph_.is_precise_get_ = false;
        max_pos = depth;
        is_terminated = true;
      } else if (NULL != cur->or_next_ || NULL != cur->item_next_) {
        table_graph_.is_precise_get_ = false;
      } else if (cur->is_like_key()) {
        table_graph_.is_precise_get_ = false;
      } else if (!cur->is_equal_condition()) {
        table_graph_.is_precise_get_ = false;
      } else {
        is_phy_rowid_key_part = cur->is_phy_rowid_key_part();
      }
      if (OB_SUCC(ret) && !is_terminated) {
        if (is_strict_in_graph(cur)) {
          // do nothing
        } else if (!is_general_graph(*cur)) {
          max_pos = cur->pos_.offset_ + 1;
          is_terminated = true;
        } else if (has_scan_key(*cur)) {
          max_pos = cur->pos_.offset_ + 1;
          is_terminated = true;
        }
      }
    }
    if (OB_SUCC(ret)) {
      max_pos = is_terminated ? max_pos : depth + 1;
      if (OB_FAIL(remove_precise_range_expr(max_pos))) {
        LOG_WARN("remove precise range expr failed", K(ret));
      } else if (table_graph_.is_precise_get_ && depth != column_count - 1 && !is_phy_rowid_key_part) {
        table_graph_.is_precise_get_ = false;
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(range_exprs_.init(query_range_ctx_->precise_range_exprs_.count()))) {
    LOG_WARN("init range exprs failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < query_range_ctx_->precise_range_exprs_.count(); ++i) {
    const ObRawExpr *cur_expr = query_range_ctx_->precise_range_exprs_.at(i).cur_expr_;
    if (NULL != cur_expr) {
      if (OB_FAIL(range_exprs_.push_back(const_cast<ObRawExpr*>(cur_expr)))) {
        LOG_WARN("push back precise range expr failed", K(ret));
      }
    }
  }
  return ret;
}

//这个函数用来判断待抽取的表达式的column type和表达式的比较类型是否兼容，
//然后来判断该表达式到底是应该进行精确抽取还是放大成(min, max)
bool ObQueryRange::can_be_extract_range(ObItemType cmp_type,
                                        const ObExprResType &col_type,
                                        const ObExprCalcType &calc_type,
                                        ObObjType data_type,
                                        bool &always_true)
{
  bool bret = true;
  always_true = true;
  //决定一个表达式能否运用我们的抽取规则的前提是进行抽取后的集合范围有没有比表达式表达的值域范围更小
  //对于一个表达式col compare const，比较类型calc_type决定了一个集合A(A中的元素是calc_type)满足关系，
  //而query range需要确定一个集合B(集合B中的元素都是column_type)被A包含，
  //这样才能让query range抽取的column value都满足这个表达式
  //而集合B是query range通过calc_type和column_type以及表达式抽取规则确定的一个集合
  //抽取的规则无论是类型的转换，还是字符集的转换，都只能做到一对一，例如int->varchar, 123被转换成'123'，不会是'0123'
  //字符集UTF8MB4_GENERAL_CI'A'->UTF8MB4_BIN'A'而不会是UTF8MB4_BIN'a'
  //因此要满足col compare const这个表达式，如果column_type和calc_type不兼容，那么需要涉及到类型转换
  //能完整表达compare关系的表达式为:f1(col, calc_type) compare f2(const, calc_type)
  //f1表示将col的值从column_type映射到calc_type，因此要讨论集合A和集合B的关系，就是讨论f1的映射关系
  //影响集合范围的因素可能是类型和字符集，而字符集只有在字符串类型才有意义
  //第一种情况：
  //如果column_type和calc_type的类型相同并且字符集也相同(如果为字符串类型)，那么f1是一一映射关系
  //也就是集合A=集合B
  //第二种情况：
  //如果column_type是大小写敏感的字符集，而calc_type是大小写不敏感的字符集，那么f1就是一个一对多的关系
  //!f1就是一个多对一的关系，general_ci 'A'-> bin'a' or bin'A'那么通过query range规则推导出来的B包含于A，不满足假设，
  //因此这种情况query range的结果是全集(min, max)
  //第三种情况:
  //如果column_type是字符串类型，而calc_type是非字符串类型，由于两者的排序规则和转换规则不一,
  //f1是多对一的关系，例如'0123'->123, '123'->123，那么!f1是一对多的关系，也不满足假设，所以这种情况下query range结果也是全集(min, max)
  //第四种情况：
  //如果column_type是数值类型，而calc_type是字符串类型，通过第三种情况可知，数值类型到字符串类型的映射f1是
  //一对多的关系,那么!f1的关系是多对一的关系，而query range的抽取规则是一一映射的关系，任何一个属于集合A的元素
  //a都能唯一确定出一个值b在集合B中使表达式成立，因此第四种情况也是能够通过抽取规则抽取的
  //其它情况下的f1也都是一一映射关系，集合A=集合B，因此也能够使用抽取规则
  if (T_OP_NSEQ == cmp_type && ObNullType == data_type) {
    bret = true;
  }
  if (bret && T_OP_NSEQ != cmp_type && ObNullType == data_type) {
    //pk cmp null
    bret = false;
    //视作恒false处理
    always_true = false;
  }
  if (bret && T_OP_LIKE == cmp_type) {
    //只对string like string的形式进行抽取
    if ((! col_type.is_string_or_lob_locator_type())
      || (! calc_type.is_string_or_lob_locator_type())) {
      bret = false;
      //不能进行规则抽取，将表达式视为恒true处理
      always_true = true;
    }
  }
  if (bret) {
    bool is_cast_monotonic = false;
    int ret = OB_SUCCESS;
    //由于cast对于某些时间类型的某些值域有特殊处理，导致A cast B，不一定可逆，
    //一个表达式能够抽取，需要双向都满足cast单调
    if (OB_FAIL(ObObjCaster::is_cast_monotonic(col_type.get_type(),
                                                 calc_type.get_type(),
                                                 is_cast_monotonic))) {
      LOG_WARN("check is cast monotonic failed", K(ret));
    } else if (!is_cast_monotonic) {
      bret = false;
      always_true = true;
    } else if (OB_FAIL(ObObjCaster::is_cast_monotonic(calc_type.get_type(),
                                                        col_type.get_type(),
                                                        is_cast_monotonic))) {
      LOG_WARN("check is cast monotonic failed", K(ret));
    } else if (!is_cast_monotonic) {
      bret = false;
      always_true = true;
    } else if (calc_type.is_string_or_lob_locator_type()
               && col_type.is_string_or_lob_locator_type()) {
      if (T_OP_LIKE == cmp_type && col_type.is_nstring()) {
        bret = true;
      } else if (col_type.get_collation_type() != calc_type.get_collation_type()) {
        bret = false;
        always_true = true;
      }
    } else if (calc_type.is_json() && col_type.is_json()) {
      bret = false;
      always_true = true;
    }
  }
  return bret;
}

int ObQueryRange::get_const_key_part(const ObRawExpr *l_expr,
                                     const ObRawExpr *r_expr,
                                     const ObRawExpr *escape_expr,
                                     ObItemType cmp_type,
                                     const ObExprResType &result_type,
                                     ObKeyPart *&out_key_part,
                                     const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(l_expr) || OB_ISNULL(r_expr) || (OB_ISNULL(escape_expr) && T_OP_LIKE == cmp_type)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", K(ret), KP(l_expr), KP(r_expr));
  } else if (l_expr->cnt_param_expr() || r_expr->cnt_param_expr()) {
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
  } else {
    ObObj l_val;
    ObObj r_val;
    bool l_valid = false;
    bool r_valid = false;
    const ObExprCalcType &calc_type = result_type.get_calc_meta();
    ObCollationType cmp_cs_type = calc_type.get_collation_type();
    // '?' is const too, if " '?' cmp const ", we seem it as true now
    if (OB_FAIL(get_calculable_expr_val(l_expr, l_val, l_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (OB_FAIL(get_calculable_expr_val(r_expr, r_val, r_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (!l_valid || !r_valid) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (l_val.is_null() || r_val.is_null()) {
      if (l_val.is_null() && r_val.is_null() && T_OP_NSEQ == cmp_type) {
        GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
      } else {
        GET_ALWAYS_TRUE_OR_FALSE(false, out_key_part);
      }
    } else if (cmp_type >= T_OP_EQ && cmp_type <= T_OP_NE) {
      ObObjType compare_type = ObMaxType;
      int64_t eq_cmp = 0;
      ObCastMode cast_mode = CM_WARN_ON_FAIL;
      ObCastCtx cast_ctx(&allocator_, &dtc_params, cast_mode, cmp_cs_type);
      if (OB_FAIL(ObExprResultTypeUtil::get_relational_cmp_type(compare_type,
                                                                l_val.get_type(),
                                                                r_val.get_type()))) {
        LOG_WARN("get compare type failed", K(ret));
      } else if (OB_FAIL(ObRelationalExprOperator::compare_nullsafe(eq_cmp, l_val, r_val,
                                                                    cast_ctx, compare_type, cmp_cs_type))) {
        LOG_WARN("compare obj failed", K(ret));
      } else if (T_OP_EQ == cmp_type || T_OP_NSEQ == cmp_type) {
        GET_ALWAYS_TRUE_OR_FALSE(0 == eq_cmp, out_key_part);
      } else if (T_OP_LE == cmp_type) {
        GET_ALWAYS_TRUE_OR_FALSE(eq_cmp <= 0, out_key_part);
      } else if (T_OP_LT == cmp_type) {
        GET_ALWAYS_TRUE_OR_FALSE(eq_cmp < 0, out_key_part);
      } else if (T_OP_GE == cmp_type) {
        GET_ALWAYS_TRUE_OR_FALSE(eq_cmp >= 0, out_key_part);
      } else if (T_OP_GT == cmp_type) {
        GET_ALWAYS_TRUE_OR_FALSE(eq_cmp > 0, out_key_part);
      } else {
        GET_ALWAYS_TRUE_OR_FALSE(0 != eq_cmp, out_key_part);
      }
    } else if (T_OP_LIKE == cmp_type) {
      if (!escape_expr->is_const_expr()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("escape_expr must be const expr", K(ret));
      } else {
        if (OB_FAIL(get_like_const_range(l_expr,
                                         r_expr,
                                         escape_expr,
                                         cmp_cs_type,
                                         out_key_part,
                                         dtc_params))) {
          LOG_WARN("get like const range failed", K(ret));
        }

      }
    } else {
      //do nothing
    }
  }
  return ret;
}

// create table t1(c1 int primary key);
// table get:  select * from t1 where rowid = '*AAEPAQEAwAEAAAA=';#(int)(1)
// table scan: select * from t1 where rowid = '*AAEPAQEAwAEAAAAPAQEAwAEAAAA=';#(int,int)(1,1)
int ObQueryRange::get_rowid_key_part(const ObRawExpr *l_expr,
                                     const ObRawExpr *r_expr,
                                     const ObRawExpr *escape_expr,
                                     ObItemType cmp_type,
                                     const ObExprResType &result_type,
                                     ObKeyPart *&out_key_part,
                                     const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(l_expr) || OB_ISNULL(r_expr) || (OB_ISNULL(escape_expr) && T_OP_LIKE == cmp_type)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", K(ret), KP(l_expr), KP(r_expr), KP(cmp_type));
  } else {
    ObSEArray<const ObColumnRefRawExpr *, 4> pk_column_items;
    ObSEArray<ObObj, 4> pk_vals;
    const ObRawExpr *const_expr = NULL;
    ObObj const_val;
    bool is_valid = false;
    const ObExprCalcType &calc_type = result_type.get_calc_meta();
    ObItemType c_type = cmp_type;
    bool is_physical_rowid = false;
    uint64_t table_id = common::OB_INVALID_ID;
    uint64_t part_column_id = common::OB_INVALID_ID;

    const ObRawExpr *calc_urowid_expr = NULL;
    if (OB_UNLIKELY(r_expr->has_flag(IS_ROWID))) {
      const_expr = l_expr;
      c_type = (T_OP_LE == cmp_type ? T_OP_GE : (T_OP_GE == cmp_type ? T_OP_LE :
                                                 (T_OP_LT == cmp_type ? T_OP_GT : (T_OP_GT == cmp_type ? T_OP_LT : cmp_type))));
      calc_urowid_expr = r_expr;
    } else if (l_expr->has_flag(IS_ROWID)) {
      const_expr = r_expr;
      c_type = cmp_type;
      calc_urowid_expr = l_expr;
    }
    if (const_expr->cnt_param_expr()) {
      query_range_ctx_->need_final_extact_ = true;
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(get_extract_rowid_range_infos(calc_urowid_expr,
                                                pk_column_items,
                                                is_physical_rowid,
                                                table_id,
                                                part_column_id))) {
        LOG_WARN("failed to get extract rowid range infos");
      } else if (OB_FAIL(get_calculable_expr_val(const_expr, const_val, is_valid, false))) {
        LOG_WARN("failed to get calculable expr val", K(ret));
      } else if (!is_valid) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("rowid expr should not calc failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(query_range_ctx_->params_) &&
        !const_expr->has_flag(CNT_DYNAMIC_PARAM)) {
      ObObj val = const_val;
      if (val.is_urowid()) {
        uint64_t pk_cnt;
        ObArray<ObObj> pk_vals;
        const ObURowIDData &urowid_data = val.get_urowid();
        if (is_physical_rowid && !urowid_data.is_physical_rowid()) {
          ret = OB_INVALID_ROWID;
          LOG_WARN("get invalid rowid", K(ret), K(urowid_data), K(is_physical_rowid));
        } else if (OB_FAIL(urowid_data.get_pk_vals(pk_vals))) {
          LOG_WARN("get pk values failed", K(ret));
        } else {
          pk_cnt = urowid_data.get_real_pk_count(pk_vals);
          if (OB_UNLIKELY(pk_cnt != pk_column_items.count())) {
            ret = OB_INVALID_ROWID;
            LOG_WARN("invalid rowid, table rowkey cnt and encoded row cnt mismatch", K(ret),
                                                             K(pk_cnt), K(pk_column_items.count()));
          } else {
            for (int i = 0; OB_SUCC(ret) && i < pk_cnt; i++) {
              if (!pk_vals.at(i).is_null() &&
                  !ObSQLUtils::is_same_type_for_compare(pk_vals.at(i).meta_,
                                                        pk_column_items.at(i)->get_result_type())) {
                ret = OB_INVALID_ROWID;
                LOG_WARN("invalid rowid, table rowkey type and encoded type mismatch", K(ret),
                            K(pk_vals.at(i).meta_), K(pk_column_items.at(i)->get_result_type()));
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObKeyPart *tmp_key_part = NULL;
      ObKeyPartList key_part_list;
      ObObj val;
      if (!const_expr->cnt_param_expr()) {
        val = const_val;
      } else {
        if (OB_FAIL(get_final_expr_val(const_expr, val))) {
          LOG_WARN("failed to get final expr idx", K(ret));
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < pk_column_items.count(); ++i) {
        const ObColumnRefRawExpr *column_item = pk_column_items.at(i);
        ObKeyPartId key_part_id(column_item->get_table_id(), column_item->get_column_id());
        ObKeyPartPos *key_part_pos = nullptr;
        bool b_is_key_part = false;
        bool always_true = true;
        tmp_key_part = NULL;
        if (OB_FAIL(is_key_part(key_part_id, key_part_pos, b_is_key_part))) {
          LOG_WARN("is_key_part failed", K(ret));
        } else if (!b_is_key_part) {
          if (is_physical_rowid &&
              query_range_ctx_->phy_rowid_for_table_loc_ &&
              table_id != common::OB_INVALID_ID &&
              part_column_id != common::OB_INVALID_ID) {
            key_part_id.table_id_ = table_id;
            key_part_id.column_id_ = part_column_id;
          }
          if (OB_FAIL(is_key_part(key_part_id, key_part_pos, b_is_key_part))) {
            LOG_WARN("is_key_part failed", K(ret));
          } else if (!b_is_key_part) {
            GET_ALWAYS_TRUE_OR_FALSE(true, tmp_key_part);
          }
        }
        if (OB_FAIL(ret) || !b_is_key_part) {
        } else if (OB_ISNULL(key_part_pos)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get null key part pos", K(ret));
        } else if (OB_ISNULL((tmp_key_part = create_new_key_part()))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_ERROR("alloc memory failed", K(ret));
        } else {
          ObObj tmp_val = val;
          tmp_key_part->rowid_column_idx_ = i;
          tmp_key_part->id_ = key_part_id;
          tmp_key_part->pos_ = *key_part_pos;
          tmp_key_part->null_safe_ = false;
          tmp_key_part->is_phy_rowid_key_part_ = is_physical_rowid;
          //if current expr can be extracted to range, just store the expr
          if (c_type != T_OP_LIKE) {
            bool is_inconsistent_rowid = false;
            if (tmp_val.is_urowid()) {
              if (OB_FAIL(get_result_value_with_rowid(*tmp_key_part,
                                                      tmp_val,
                                                      *query_range_ctx_->exec_ctx_,
                                                      is_inconsistent_rowid))) {
                LOG_WARN("failed to get result value", K(ret));
              } else if (is_inconsistent_rowid) {
                GET_ALWAYS_TRUE_OR_FALSE(false, tmp_key_part);
              }
            }
            if (OB_FAIL(ret) || is_inconsistent_rowid) {
            } else if (OB_FAIL(get_normal_cmp_keypart(c_type, tmp_val, *tmp_key_part))) {
              LOG_WARN("get normal cmp keypart failed", K(ret));
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(add_and_item(key_part_list, tmp_key_part))) {
            LOG_WARN("Add basic query key part failed", K(ret));
          } else if (pk_column_items.count() - 1 == i) {
            if (OB_FAIL(and_range_graph(key_part_list, out_key_part))) {
              LOG_WARN("and basic query key part failed", K(ret));
            }
          }
        }
      }
    }
    LOG_TRACE("succeed to get rowid key part", KPC(out_key_part));
  }
  return ret;
}

int ObQueryRange::get_extract_rowid_range_infos(const ObRawExpr *calc_urowid_expr,
                                                ObIArray<const ObColumnRefRawExpr*> &pk_columns,
                                                bool &is_physical_rowid,
                                                uint64_t &table_id,
                                                uint64_t &part_column_id)
{
  int ret = OB_SUCCESS;
  is_physical_rowid = false;
  table_id = common::OB_INVALID_ID;
  part_column_id = common::OB_INVALID_ID;
  if (OB_ISNULL(calc_urowid_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(calc_urowid_expr));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < calc_urowid_expr->get_param_count(); ++i) {
      if (calc_urowid_expr->get_param_expr(i)->has_flag(IS_COLUMN)) {
        const ObColumnRefRawExpr * col_expr =
                       static_cast<const ObColumnRefRawExpr *>(calc_urowid_expr->get_param_expr(i));
        table_id = col_expr->get_table_id();
        // pk_vals may store generated col which is partition key but not primary key
        if (!col_expr->is_rowkey_column()) {
          /*do nothing*/
        } else if (OB_FAIL(pk_columns.push_back(col_expr))) {
          LOG_WARN("push back pk_column item failed", K(ret));
        } else {/*do nothing*/}
      } else if (calc_urowid_expr->get_param_expr(i)->get_expr_type() == T_FUN_SYS_CALC_TABLET_ID) {
        ObSEArray<ObRawExpr *, 4> column_exprs;
        if (OB_FAIL(ObRawExprUtils::extract_column_exprs(calc_urowid_expr->get_param_expr(i),
                                                         column_exprs))) {
          LOG_WARN("get column exprs error", K(ret));
        } else {
          is_physical_rowid = true;
          bool find_it = false;
          for (int64_t i = 0; OB_SUCC(ret) && !find_it && i < column_exprs.count(); ++i) {
            ObColumnRefRawExpr *col_expr = NULL;
            if (OB_ISNULL(col_expr = static_cast<ObColumnRefRawExpr*>(column_exprs.at(i)))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("get unexpected null", K(ret), K(col_expr));
            } else {
              ObKeyPartId id(col_expr->get_table_id(), col_expr->get_column_id());
              ObKeyPartPos *pos = nullptr;
              int map_ret = query_range_ctx_->key_part_map_.get_refactored(id, pos);
              if (OB_SUCCESS == map_ret) {
                find_it = true;
                table_id = col_expr->get_table_id();
                part_column_id = col_expr->get_column_id();
              } else if (OB_HASH_NOT_EXIST != map_ret) {
                ret = map_ret;
                LOG_WARN("get part map failed", K(ret));
              } else {/*do nothing*/}
            }
          }
        }
      } else {/*do nothing*/}
    }
    LOG_TRACE("get extract rowid range infos", K(is_physical_rowid), K(table_id), K(part_column_id),
                                               K(pk_columns), KPC(calc_urowid_expr));
  }
  return ret;
}

int ObQueryRange::get_column_key_part(const ObRawExpr *l_expr,
                                      const ObRawExpr *r_expr,
                                      const ObRawExpr *escape_expr,
                                      ObItemType cmp_type,
                                      const ObExprResType &result_type,
                                      ObKeyPart *&out_key_part,
                                      const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(l_expr) || OB_ISNULL(r_expr) || (OB_ISNULL(escape_expr) && T_OP_LIKE == cmp_type)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", K(ret), KP(l_expr), KP(r_expr), KP(cmp_type));
  } else {
    const ObColumnRefRawExpr *column_item = NULL;
    const ObRawExpr *const_expr = NULL;
    const ObExprCalcType &calc_type = result_type.get_calc_meta();
    ObItemType c_type = cmp_type;
    ObObj const_val;
    bool is_valid = true;
    if (OB_UNLIKELY(r_expr->has_flag(IS_COLUMN))) {
      column_item = static_cast<const ObColumnRefRawExpr *>(r_expr);
      const_expr = l_expr;
      c_type = (T_OP_LE == cmp_type ? T_OP_GE : (T_OP_GE == cmp_type ? T_OP_LE :
                                                 (T_OP_LT == cmp_type ? T_OP_GT : (T_OP_GT == cmp_type ? T_OP_LT : cmp_type))));
    } else if (l_expr->has_flag(IS_COLUMN)) {
      column_item = static_cast<const ObColumnRefRawExpr *>(l_expr);
      const_expr = r_expr;
      c_type = cmp_type;
    }
    if (const_expr->cnt_param_expr()) {
      query_range_ctx_->need_final_extact_ = true;
    }
    ObKeyPartId key_part_id(column_item->get_table_id(), column_item->get_column_id());
    ObKeyPartPos *key_part_pos = nullptr;
    bool b_is_key_part = false;
    bool always_true = true;
    if (OB_FAIL(is_key_part(key_part_id, key_part_pos, b_is_key_part))) {
      LOG_WARN("is_key_part failed", K(ret));
    } else if (!b_is_key_part) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (OB_UNLIKELY(!const_expr->is_const_expr())) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (OB_ISNULL(key_part_pos)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null key part pos", K(ret));
    } else if (!can_be_extract_range(cmp_type, key_part_pos->column_type_, calc_type,
                                     const_expr->get_result_type().get_type(), always_true)) {
      GET_ALWAYS_TRUE_OR_FALSE(always_true, out_key_part);
    } else if (OB_FAIL(get_calculable_expr_val(const_expr, const_val, is_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (!is_valid) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (OB_ISNULL((out_key_part = create_new_key_part()))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc memory failed", K(ret));
    } else {
      ObObj val;
      out_key_part->id_ = key_part_id;
      out_key_part->pos_ = *key_part_pos;
      out_key_part->null_safe_ = (T_OP_NSEQ == c_type);
      if (!const_expr->cnt_param_expr()
          || (!const_expr->has_flag(CNT_DYNAMIC_PARAM)
              && T_OP_LIKE == c_type
              && NULL != query_range_ctx_->params_)) {
        val = const_val;
      } else {
        if (OB_FAIL(get_final_expr_val(const_expr, val))) {
          LOG_WARN("failed to get final expr idx", K(ret));
        }
      }
      //if current expr can be extracted to range, just store the expr
      if (OB_SUCC(ret)) {
        if (c_type != T_OP_LIKE) {
          if (OB_FAIL(get_normal_cmp_keypart(c_type, val, *out_key_part))) {
            LOG_WARN("get normal cmp keypart failed", K(ret));
          }
        } else {

          ObObj escape_val;
          is_valid = false;
          if (OB_FAIL(get_calculable_expr_val(escape_expr, escape_val, is_valid))) {
            LOG_WARN("failed to get calculable expr val", K(ret));
          } else if (!is_valid) {
            GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
          } else if (const_expr->cnt_param_expr() || escape_expr->cnt_param_expr()) {
            if (OB_FAIL(out_key_part->create_like_key())) {
              LOG_WARN("create like key part failed", K(ret));
            } else if (OB_FAIL(get_final_expr_val(const_expr, out_key_part->like_keypart_->pattern_))) {
              LOG_WARN("failed to get final expr idx", K(ret));
            } else if (OB_FAIL(get_final_expr_val(escape_expr, out_key_part->like_keypart_->escape_))) {
              LOG_WARN("failed to get final expr idx", K(ret));
            } else if (escape_expr->cnt_param_expr()) {
              query_range_ctx_->need_final_extact_ = true;
            } else {
              // do nothing
            }
            if (OB_SUCC(ret) &&
                (NULL != query_range_ctx_->params_) &&
                !const_expr->has_flag(CNT_DYNAMIC_PARAM) &&
                !escape_expr->has_flag(CNT_DYNAMIC_PARAM)) {
              char escape_ch = 0x00;
              if (escape_val.is_null()) {
                escape_ch = '\\';
              } else {
                escape_ch = (escape_val.get_string_len()==0)?0x00:*(escape_val.get_string_ptr());
              }

              if (OB_FAIL(ret)) {
                // do nothing
              } else if(val.is_null()) {
                query_range_ctx_->cur_expr_is_precise_ = false;
              } else if(OB_FAIL(ObQueryRange::is_precise_like_range(val,
                                              escape_ch,
                                              query_range_ctx_->cur_expr_is_precise_))) {
                LOG_WARN("failed to jugde whether is precise", K(ret));
              } else if (OB_FAIL(add_precise_constraint(const_expr,
                                                        query_range_ctx_->cur_expr_is_precise_))) {
                LOG_WARN("failed to add precise constraint", K(ret));
              }
            }
          } else {
            if (OB_FAIL(get_like_range(val, escape_val, *out_key_part, dtc_params))) {
              LOG_WARN("get like range failed", K(ret));
            }
          }
          if (OB_SUCC(ret) && is_oracle_mode()) {
            // NChar like Nchar, Char like Char is not precise due to padding blank characters
            ObObjType column_type = key_part_pos->column_type_.get_type();
            ObObjType const_type = const_expr->get_result_type().get_type();
            if ((ObCharType == column_type && ObCharType == const_type) ||
                (ObNCharType == column_type && ObNCharType == const_type)) {
              query_range_ctx_->cur_expr_is_precise_ = false;
            }
          }
        }
        if (OB_SUCC(ret) && out_key_part->is_normal_key() && !out_key_part->is_question_mark()) {
          if (OB_FAIL(out_key_part->cast_value_type(dtc_params, contain_row_))) {
            LOG_WARN("cast keypart value type failed", K(ret));
          } else {
            // do nothing
          }
        }
        if (OB_SUCC(ret) && key_part_pos->column_type_.is_string_type() && calc_type.is_string_type()) {
          if (CS_TYPE_UTF8MB4_GENERAL_CI == key_part_pos->column_type_.get_collation_type()
              && CS_TYPE_UTF8MB4_GENERAL_CI != calc_type.get_collation_type()) {
            //这种情况下，要把字符集统一成目标列的字符集，使用general ci比较，由于calc type的字符集是general bin
            //由于general bin转换成general ci，存在多对一，所以得到的range可能会放大，不是一个精确的range
            query_range_ctx_->cur_expr_is_precise_ = false;
          }
        }
        if (OB_SUCC(ret) && is_oracle_mode() && out_key_part->is_normal_key() && NULL != const_expr) {
          // c1 char(5), c2 varchar(5) 对于值'abc', c1 = 'abc  ', c2 = 'abc'
          // oracle模式下'abc  ' != 'abc', 但是 c1 = cast('abc' as varchar2(5))
          // 会抽出(abc ; abc)这个range, 因为存储层存储字符串是不存padding的空格的, 所以这个range会导致'abc  '
          // 被select出来, 因此这个range不是一个精确的range
          ObObjType column_type = key_part_pos->column_type_.get_type();
          ObObjType const_type = const_expr->get_result_type().get_type();
          if ((ObCharType == column_type && ObVarcharType == const_type) ||
              (ObNCharType == column_type && ObNVarchar2Type == const_type)) {
            query_range_ctx_->cur_expr_is_precise_ = false;
          }
        }
      }
    }
  }
  return ret;
}

int ObQueryRange::get_normal_cmp_keypart(ObItemType cmp_type,
                                         const ObObj &val,
                                         ObKeyPart &out_keypart) const
{
  int ret = OB_SUCCESS;
  bool always_false = false;
  //精确的range expr
  if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("query range context is null");
  } else if (OB_FAIL(out_keypart.create_normal_key())) {
    LOG_WARN("create normal key failed", K(ret));
  } else if (OB_ISNULL(out_keypart.normal_keypart_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("normal keypart is null");
  } else if (val.is_null() && T_OP_NSEQ != cmp_type) {
    always_false = true;
  } else if (T_OP_EQ == cmp_type || T_OP_NSEQ == cmp_type) {
    out_keypart.normal_keypart_->include_start_ = true;
    out_keypart.normal_keypart_->include_end_ = true;
    // 特殊处理 MySQL 模式下 `c1 is NULL` 这种表达式.
    // https://dev.mysql.com/doc/refman/8.0/en/comparison-operators.html#operator_is-null
    if (lib::is_mysql_mode()
        && (out_keypart.pos_.column_type_.is_datetime() || out_keypart.pos_.column_type_.is_date())
        && out_keypart.pos_.column_type_.is_not_null_for_read()
        && T_OP_NSEQ == cmp_type
        && val.is_null()) {
      // datetime 和 date 没有自己申请的内存, 浅考 ok.
      // TODO wuyuming.wym: this branch can be removed after resolving c_date_not_null is null
      // as c_date_not_null = ZERO_DATE
      ObObj tmp_val;
      tmp_val.set_meta_type(out_keypart.pos_.column_type_);
      if (out_keypart.pos_.column_type_.is_datetime()) {
        tmp_val.set_datetime(ObTimeConverter::ZERO_DATETIME);
      } else if (out_keypart.pos_.column_type_.is_date()) {
        tmp_val.set_date(ObTimeConverter::ZERO_DATE);
      }
      out_keypart.normal_keypart_->start_ = tmp_val;
      out_keypart.normal_keypart_->end_ = tmp_val;
    } else {
      // normal action
      out_keypart.normal_keypart_->start_ = val;
      out_keypart.normal_keypart_->end_ = val;
    }
  } else if (T_OP_LE == cmp_type || T_OP_LT == cmp_type) {
    //index order in storage is, Null is greater than min, less than the value of any meaningful
    //c1 < val doesn't contain Null -> (NULL, val)
    if (lib::is_oracle_mode()) {
      // Oracle 存储层使用 NULL Last
      out_keypart.normal_keypart_->start_.set_min_value();
    } else {
      out_keypart.normal_keypart_->start_.set_null();
    }
    out_keypart.normal_keypart_->end_ = val;
    out_keypart.normal_keypart_->include_start_ = false;
    out_keypart.normal_keypart_->include_end_ = (T_OP_LE == cmp_type);
  } else if (T_OP_GE == cmp_type || T_OP_GT == cmp_type) {
    out_keypart.normal_keypart_->start_ = val;
    if (lib::is_oracle_mode()) {
      // Oracle 存储层使用 NULL Last
      out_keypart.normal_keypart_->end_.set_null();
    } else {
      out_keypart.normal_keypart_->end_.set_max_value();
    }
    out_keypart.normal_keypart_->include_start_ = (T_OP_GE == cmp_type);
    out_keypart.normal_keypart_->include_end_ = false;
  }
  if (OB_SUCC(ret)) {
    query_range_ctx_->cur_expr_is_precise_ = true;
    if (!always_false) {
      out_keypart.normal_keypart_->always_false_ = false;
      out_keypart.normal_keypart_->always_true_ = false;
    } else {
      out_keypart.normal_keypart_->start_.set_max_value();
      out_keypart.normal_keypart_->end_.set_min_value();
      out_keypart.normal_keypart_->include_start_ = false;
      out_keypart.normal_keypart_->include_end_ = false;
      out_keypart.normal_keypart_->always_false_ = true;
      out_keypart.normal_keypart_->always_true_ = false;
    }
  }
  return ret;
}

int ObQueryRange::get_row_key_part(const ObRawExpr *l_expr,
                                   const ObRawExpr *r_expr,
                                   ObItemType cmp_type,
                                   const ObExprResType &result_type,
                                   ObKeyPart *&out_key_part,
                                   const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(l_expr) || OB_ISNULL(r_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", KP(l_expr), KP(r_expr), K_(query_range_ctx));
  } else {
    bool row_is_precise = true;
    if (T_OP_EQ != cmp_type) {
      contain_row_ = true;
      row_is_precise = false;
    }
    ObKeyPartList key_part_list;
    ObKeyPart *row_tail = out_key_part;
    // resolver makes sure the syntax right, so we don't concern whether the numbers of row are equal
    const ObOpRawExpr *l_row = static_cast<const ObOpRawExpr *>(l_expr);
    const ObOpRawExpr *r_row = static_cast<const ObOpRawExpr *>(r_expr);
    int64_t num = 0;
    num = l_row->get_param_count() <= r_row->get_param_count()
        ? l_row->get_param_count()
        : r_row->get_param_count();

    ObItemType c_type = T_INVALID;
    switch (cmp_type) {
      case T_OP_LT:
      case T_OP_LE:
        c_type = T_OP_LE;
        break;
      case T_OP_GT:
      case T_OP_GE:
        c_type = T_OP_GE;
        break;
      default:
        //其它的compare type,不做改变传递下去，在向量中，T_OP_EQ和T_OP_NSEQ的处理逻辑跟普通条件一样，
        //T_OP_NE抽取range没有意义，不能改变compare type类型，让下层能够判断并忽略这样的row compare
        //子查询的compare type也将原来的compare type传递下去，让下层判断接口能够忽略子查询的compare表达式
        c_type = cmp_type;
        break;
    }
    bool b_flag = false;
    ObArenaAllocator alloc;
    ObExprResType res_type(alloc);
    ObKeyPart *tmp_key_part = NULL;
    for (int i = 0; OB_SUCC(ret) && !b_flag && i < num; ++i) {
      res_type.set_calc_meta(result_type.get_row_calc_cmp_types().at(i));
      tmp_key_part = NULL;
      if (OB_FAIL(check_null_param_compare_in_row(l_row->get_param_expr(i),
                                                  r_row->get_param_expr(i),
                                                  tmp_key_part))) {
        LOG_WARN("failed to check null param compare in row", K(ret));
      } else if (tmp_key_part == NULL &&
                 OB_FAIL(get_basic_query_range(l_row->get_param_expr(i),
                                               r_row->get_param_expr(i),
                                               NULL,
                                               i < num - 1 ? c_type : cmp_type,
                                               res_type,
                                               tmp_key_part,
                                               dtc_params))) {
        LOG_WARN("Get basic query key part failed", K(ret), K(*l_row), K(*r_row), K(c_type));
      } else if (T_OP_ROW == l_row->get_param_expr(i)->get_expr_type()
                 || T_OP_ROW == r_row->get_param_expr(i)->get_expr_type()) {
        // ((a,b),(c,d)) = (((1,2),(2,3)),((1,2),(2,3)))
        row_is_precise = false;
      } else if (T_OP_EQ == cmp_type || T_OP_NSEQ == cmp_type) {
        row_is_precise = (row_is_precise && query_range_ctx_->cur_expr_is_precise_);
        if (OB_FAIL(add_and_item(key_part_list, tmp_key_part))) {
          LOG_WARN("Add basic query key part failed", K(ret));
        } else if (num - 1 == i) {
          if (OB_FAIL(and_range_graph(key_part_list, out_key_part))) {
            LOG_WARN("and basic query key part failed", K(ret));
          } else {
            b_flag = true;
          }
        }
      } else if (OB_FAIL(add_row_item(row_tail, tmp_key_part))) {
        LOG_WARN("Add basic query key part failed", K(ret));
      } else {
        if (NULL == out_key_part) {
          out_key_part = tmp_key_part;
        }
        if (NULL == tmp_key_part || tmp_key_part->is_always_false()) {
          // when find false key part, then no need to do next
          // E.g. (10, c1) > (5, 0)
          if (NULL == out_key_part) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("out_key_part is null", K(ret));
          }
          b_flag = true; // break
        } else {
          row_tail = tmp_key_part;
        }
      }
    }
    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = row_is_precise;
    }
  }
  return ret;
}

// Get range from basic compare expression, like 'col >= 30', 'row(c1, c2) > row(1, 2)'
//  if this compare expression is not kinds of that we can use,
//  return alway true key part, because it may be in OR expression
//  E.g.
//  case 1:
//         key1 > 0 and (key2 < 5 or not_key3 >0)
//         currently, we get range from 'not_key3 >0', if we do not generate  always true key part for it,
//         the result of (key2 < 5 or not_key3 >0) will be 'key2 belongs (min, 5)'
//  case 2:
//         key1 > 0 and (key2 < 5 or key1+key2 >0)
//  case 3:
//         key1 > 0 and (key2 < 5 or func(key1) >0)

int ObQueryRange::get_basic_query_range(const ObRawExpr *l_expr,
                                        const ObRawExpr *r_expr,
                                        const ObRawExpr *escape_expr,
                                        ObItemType cmp_type,
                                        const ObExprResType &result_type,
                                        ObKeyPart *&out_key_part,
                                        const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  out_key_part = NULL;
  if (OB_ISNULL(query_range_ctx_)
      || OB_ISNULL(l_expr)
      || OB_ISNULL(r_expr)
      || (OB_ISNULL(escape_expr) && T_OP_LIKE == cmp_type)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Wrong input params to get basic query range",
             K(ret), KP(query_range_ctx_), KP(l_expr), KP(r_expr), KP(escape_expr), K(cmp_type));
  } else if ((T_OP_ROW == l_expr->get_expr_type() && T_OP_ROW != r_expr->get_expr_type())
             || (T_OP_ROW != l_expr->get_expr_type() && T_OP_ROW == r_expr->get_expr_type())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Row must compare to row", K(ret));
  } else {
    //在进行单值抽取的时候，先将cur_expr_is_precise_初始化为false
    query_range_ctx_->cur_expr_is_precise_ = false;
  }
  if (OB_FAIL(ret)) {
    //do nothing
  } else if (T_OP_LIKE == cmp_type && !escape_expr->is_const_expr()) {
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
  } else if (IS_BASIC_CMP_OP(cmp_type)) {
    if (T_OP_ROW != l_expr->get_expr_type()) {// 1. unary compare
      if (OB_FAIL(ObOptimizerUtil::get_expr_without_lossless_cast(l_expr, l_expr))) {
        LOG_WARN("failed to get expr without lossless cast", K(ret));
      } else if (OB_FAIL(ObOptimizerUtil::get_expr_without_lossless_cast(r_expr, r_expr))) {
        LOG_WARN("failed to get expr without lossless cast", K(ret));
      } else if (l_expr->is_const_expr() && r_expr->is_const_expr()) { //const
        if (OB_FAIL(get_const_key_part(l_expr, r_expr, escape_expr, cmp_type,
                                      result_type, out_key_part, dtc_params))) {
          LOG_WARN("get const key part failed.", K(ret));
        }
      } else if ((l_expr->has_flag(IS_COLUMN) && r_expr->is_const_expr())
                || (l_expr->is_const_expr() && r_expr->has_flag(IS_COLUMN) && T_OP_LIKE != cmp_type)) {
        if (OB_FAIL(get_column_key_part(l_expr, r_expr, escape_expr, cmp_type,
                                        result_type, out_key_part, dtc_params))) {//column
          LOG_WARN("get column key part failed.", K(ret));
        }
      } else if ((l_expr->has_flag(IS_ROWID) && r_expr->is_const_expr())
                || (r_expr->has_flag(IS_ROWID) && l_expr->is_const_expr() && T_OP_LIKE != cmp_type)) {
        if (OB_FAIL(get_rowid_key_part(l_expr, r_expr, escape_expr, cmp_type,
                                        result_type, out_key_part, dtc_params))) {//rowid
          LOG_WARN("get rowid key part failed.", K(ret));
        }
      } else if (l_expr->has_flag(IS_COLUMN) && r_expr->has_flag(IS_COLUMN)) {
        GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
      } else {
        GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
      }
    } else if (OB_FAIL(get_row_key_part(l_expr, r_expr, cmp_type, result_type,
                                        out_key_part, dtc_params))){// 2. row compare
      LOG_WARN("get row key part failed.", K(ret));
    }
  } else {
    // we can not extract range from this type, return all
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
  }
  return ret;
}

int ObQueryRange::get_like_const_range(const ObRawExpr *text_expr,
                                       const ObRawExpr *pattern_expr,
                                       const ObRawExpr *escape_expr,
                                       ObCollationType cmp_cs_type,
                                       ObKeyPart *&out_key_part,
                                       const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(text_expr) || OB_ISNULL(pattern_expr) ||  OB_ISNULL(escape_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(ret), K(text_expr), K(pattern_expr), K(escape_expr));
  } else {
    ObObj text;
    ObObj pattern;
    ObObj escape;
    ObString escape_str;
    ObString text_str;
    ObString pattern_str;
    bool text_valid = false;
    bool pattern_valid = false;
    bool escape_valid = false;
    if (OB_FAIL(get_calculable_expr_val(text_expr, text, text_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (OB_FAIL(get_calculable_expr_val(pattern_expr, pattern, pattern_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (OB_FAIL(get_calculable_expr_val(escape_expr, escape, escape_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (!text_valid || !pattern_valid || !escape_valid) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (escape.is_null()) {
      escape_str.assign_ptr("\\", 1);
    } else if (ObVarcharType != escape.get_type()) {
      ObObj tmp_obj = escape;
      tmp_obj.set_scale(escape_expr->get_result_type().get_scale());
      ObCastCtx cast_ctx(&allocator_, &dtc_params, CM_WARN_ON_FAIL, cmp_cs_type);
      EXPR_GET_VARCHAR_V2(escape, escape_str);
    } else {
      escape_str = escape.get_varchar();
    }
    if (OB_SUCC(ret)) {
      if (escape_str.empty()) { // escape ''
        escape_str.assign_ptr("\\", 1);
      }
      if (ObVarcharType != text.get_type()) {
        ObObj tmp_obj = text;
        tmp_obj.set_scale(text_expr->get_result_type().get_scale()); // 1.0 like 1
        ObCastCtx cast_ctx(&allocator_, &dtc_params, CM_WARN_ON_FAIL, cmp_cs_type);
        EXPR_GET_VARCHAR_V2(tmp_obj, text_str);
      } else {
        text_str = text.get_varchar();
      }
    }
    if (OB_SUCC(ret)) {
      if (ObVarcharType != pattern.get_type()) {
        ObObj tmp_obj = pattern;
        tmp_obj.set_scale(pattern_expr->get_result_type().get_scale()); // 1 like 1.0
        ObCastCtx cast_ctx(&allocator_, &dtc_params, CM_WARN_ON_FAIL, cmp_cs_type);
        EXPR_GET_VARCHAR_V2(tmp_obj, pattern_str);
      } else {
        pattern_str = pattern.get_varchar();
      }
    }
    if (OB_SUCC(ret)) {
      ObObj result;
      bool is_true = false;
      if (OB_FAIL(ObExprLike::calc_with_non_instr_mode(result,
                                                       cmp_cs_type,
                                                       escape.get_collation_type(),
                                                       text_str,
                                                       pattern_str,
                                                       escape_str))) { // no optimization.
        LOG_WARN("calc like func failed", K(ret));
      } else if (OB_FAIL(ObObjEvaluator::is_true(result, is_true))) {
        LOG_WARN("failed to call is_true", K(ret));
      } else if (is_true) {
        GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
      } else {
        GET_ALWAYS_TRUE_OR_FALSE(false, out_key_part);
      }
    }
  }
  return ret;
}

//  Add row item to the end of the row list
//  1. if item is always true, do not do next
//      there are two kinds of cases:
//      1) real true, row(k1, 2, k2)>row(1, 1, 1), "2>1" is true.
//          Under this kind of case, in fact we can ignore the second item and do next
//      2) not real true, row(k1, k2, k2)>=(1, k1, 4), "k2>=k1" is run-time defined,
//          we can not know during parser, so true is returned.
//          under this case, we can not ignore it. (k1=1 and k2=3) satisfied this condtion,
//          but not satisfied row(k1, k2)>=(1, 4)
//      we can not distinguish them, so do not do next.
//  2. if item is always false, no need to do next
//  3. if key part pos is not larger than exists', ignore it. because the range already be (min, max)
//      E.g.
//          a. rowkey(c1, c2, c3), condition: row(c1, c3, c2) > row(1, 2, 3).
//              while compare, row need item in order,  when considering c3, key part c2 must have range,
//              so (min, max) already used for c2.
//          b. (c1, c1, c2) > (const1, const2, const3) ==> (c1, c2) > (const1, const3)
//
//  NB: 1 and 2 are ensured by caller

int ObQueryRange::add_row_item(ObKeyPart *&row_tail, ObKeyPart *key_part)
{
  int ret = OB_SUCCESS;
  if (NULL != key_part) {
    if (NULL == row_tail) {
      row_tail = key_part;
    } else if (key_part->is_always_true()) {
      // ignore
    } else if (key_part->is_always_false()) {
      row_tail->and_next_ = key_part;
      key_part->and_next_ = NULL;
    } else {
      if (NULL == row_tail->and_next_ && row_tail->pos_.offset_ < key_part->pos_.offset_) {
        row_tail->and_next_ = key_part;
        key_part->and_next_ = NULL;
      } else {
        // find key part id no less than it
        // ignore
      }
    }
  } else {
    // do nothing
  }
  return ret;
}

// Add and item to array

int ObQueryRange::add_and_item(ObKeyPartList &and_storage, ObKeyPart *key_part)
{
  int ret = OB_SUCCESS;
  if (NULL != key_part) {
    if (key_part->is_always_true()) {
      // everything and true is itself
      // if other item exists, ignore this key_part
      if (and_storage.get_size() <= 0) {
        if (!and_storage.add_last(key_part)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add and key part graph failed", K(ret));
        }
      }
    } else if (key_part->is_always_false()) {
      // everything and false is false
      if (1 == and_storage.get_size() && NULL != and_storage.get_first()
          && and_storage.get_first()->is_always_false()) {
        // already false, ignore add action
      } else {
        and_storage.clear();
        if (!and_storage.add_last(key_part)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add and key part graph failed", K(ret));
        }
      }
    } else { // normal case
      if (1 == and_storage.get_size() && NULL != and_storage.get_first()
          && and_storage.get_first()->is_always_false()) {
        // already false, ignore add action
      } else {
        if (1 == and_storage.get_size() && NULL != and_storage.get_first()
            && and_storage.get_first()->is_always_true()) {
          and_storage.clear();
        }
        if (!and_storage.increasing_add(key_part)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add and key part graph failed", K(ret));
        }
      }
    }
  } else {
    // do nothing
  }
  return ret;
}

// Add or item to array

int ObQueryRange::add_or_item(ObKeyPartList &or_storage, ObKeyPart *key_part)
{
  int ret = OB_SUCCESS;
  if (NULL != key_part) {
    if (key_part->is_always_false()) {
      // everything or false is itself
      // if other item exists, ignore this key_part
      if (or_storage.get_size() <= 0) {
        if (!or_storage.add_last(key_part)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add or key part graph failed", K(ret));
        }
      }
    } else if (key_part->is_always_true()) {
      // everything or true is true
      if (1 == or_storage.get_size() && NULL != or_storage.get_first()
          && or_storage.get_first()->is_always_true()) {
        // already true, ignore add action
      } else {
        or_storage.clear();
        if (!or_storage.add_last(key_part)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add or key part graph failed", K(ret));
        }
      }
    } else { // normal case
      if (1 == or_storage.get_size() && NULL != or_storage.get_first()
          && or_storage.get_first()->is_always_true()) {
        // already true, ignore add action
      } else {
        if (1 == or_storage.get_size()
            && NULL != or_storage.get_first() && or_storage.get_first()->is_always_false()) {
          or_storage.clear();
        }
        if (!or_storage.add_last(key_part)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add or key part graph failed", K(ret));
        }
      }
    }
  } else {
    // do nothing
  }
  return ret;
}

int ObQueryRange::pre_extract_basic_cmp(const ObRawExpr *node,
                                        ObKeyPart *&out_key_part,
                                        const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("node is null.", K(node));
  } else {
    const ObRawExpr *escape_expr = NULL;
    const ObOpRawExpr *multi_expr = static_cast<const ObOpRawExpr *>(node);
    if (T_OP_LIKE != node->get_expr_type()) {
      if (2 != multi_expr->get_param_count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("multi_expr must has 2 arguments", K(ret));
      }
    } else {
      if (3 != multi_expr->get_param_count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("multi_expr must has 3 arguments", K(ret));
      } else {
        escape_expr = multi_expr->get_param_expr(2);
      }
    }
    if (OB_SUCC(ret)) {
      const ObRawExpr* right_expr = multi_expr->get_param_expr(1);
      if (lib::is_oracle_mode() && T_OP_ROW == multi_expr->get_param_expr(0)->get_expr_type()) {
        if (T_OP_ROW == multi_expr->get_param_expr(1)->get_expr_type() &&
            1 == multi_expr->get_param_expr(1)->get_param_count() &&
            T_OP_ROW == multi_expr->get_param_expr(1)->get_param_expr(0)->get_expr_type()) {
          right_expr = multi_expr->get_param_expr(1)->get_param_expr(0);
        }
      }
      //因为我们只处理某些特殊的表达式，对于一些复杂表达式即使是精确的，也不对其做优化，所以先将flag初始化为false
      if (OB_FAIL(get_basic_query_range(multi_expr->get_param_expr(0),
                                        right_expr,
                                        escape_expr,
                                        node->get_expr_type(),
                                        node->get_result_type(),
                                        out_key_part,
                                        dtc_params))) {
        LOG_WARN("Get basic query key part failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_ne_op(const ObOpRawExpr *t_expr,
                                    ObKeyPart *&out_key_part,
                                    const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(t_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(t_expr), K_(query_range_ctx));
  } else if (2 != t_expr->get_param_count()) {//trip op expr
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t_expr must has 2 arguments", K(ret));
  } else {
    bool is_precise = true;
    const ObRawExpr *l_expr = t_expr->get_param_expr(0);
    const ObRawExpr *r_expr = t_expr->get_param_expr(1);
    ObKeyPartList key_part_list;
    if (lib::is_oracle_mode()
      && T_OP_ROW == l_expr->get_expr_type() && T_OP_ROW == r_expr->get_expr_type()) {
      if (1 == r_expr->get_param_count()
          && T_OP_ROW == r_expr->get_param_expr(0)->get_expr_type()) {
        r_expr = r_expr->get_param_expr(0);
      }
    }
    for (int i = 0; OB_SUCC(ret) && i < 2; ++i) {
      query_range_ctx_->cur_expr_is_precise_ = false;
      ObKeyPart *tmp = NULL;
      if (OB_FAIL(get_basic_query_range(l_expr,
                                        r_expr,
                                        NULL,
                                        i == 0 ? T_OP_LT : T_OP_GT,
                                        t_expr->get_result_type(),
                                        tmp,
                                        dtc_params))) {
        LOG_WARN("Get basic query range failed", K(ret));
      } else if (OB_FAIL(add_or_item(key_part_list, tmp))) {
        LOG_WARN("push back failed", K(ret));
      } else {
        // A != B 表达式被拆分成了两个表达式, A < B OR A > B
        // 要保证每个表达式都是精确的，整个表达式才是精确的
        is_precise = (is_precise && query_range_ctx_->cur_expr_is_precise_);
      }
    }
    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = is_precise;
      //not need params when preliminary extract
      if (OB_FAIL(or_range_graph(key_part_list, NULL, out_key_part, dtc_params))) {
        LOG_WARN("or range graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_is_op(const ObOpRawExpr *b_expr,
                                    ObKeyPart *&out_key_part,
                                    const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(b_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(b_expr), K_(query_range_ctx));
  } else if (ObNullType == b_expr->get_param_expr(1)->get_result_type().get_type()) {
    //pk is null will be extracted
    if (3 != b_expr->get_param_count()) {//binary op expr
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("b_expr must has 3 arguments", K(ret));
    } else if (OB_FAIL(get_basic_query_range(b_expr->get_param_expr(0),
                                             b_expr->get_param_expr(1),
                                             NULL,
                                             T_OP_NSEQ,
                                             b_expr->get_result_type(),
                                             out_key_part,
                                             dtc_params))) {
      LOG_WARN("Get basic query key part failed", K(ret));
    }
  } else {
    query_range_ctx_->cur_expr_is_precise_ = false;
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
  }
  return ret;
}

int ObQueryRange::pre_extract_btw_op(const ObOpRawExpr *t_expr,
                                     ObKeyPart *&out_key_part,
                                     const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(t_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(t_expr), K_(query_range_ctx));
  } else if (3 != t_expr->get_param_count()) {//trip op expr
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t_expr must has 3 arguments", K(ret));
  } else {
    bool btw_op_is_precise = true;
    const ObRawExpr *l_expr = t_expr->get_param_expr(0);
    ObKeyPartList key_part_list;
    for (int i = 0; OB_SUCC(ret) && i < 2; ++i) {
      const ObRawExpr *r_expr = t_expr->get_param_expr(i + 1);
      ObKeyPart *tmp = NULL;
      if (OB_FAIL(get_basic_query_range(l_expr,
                                        r_expr,
                                        NULL,
                                        i == 0 ? T_OP_GE : T_OP_LE,
                                        t_expr->get_result_type(),
                                        tmp,
                                        dtc_params))) {
        LOG_WARN("Get basic query range failed", K(ret));
      } else if (OB_FAIL(add_and_item(key_part_list, tmp))) {
        LOG_WARN("push back failed", K(ret));
      } else {
        //BETWEEN...AND...表达式被拆分成了两个表达式
        //要保证每个表达式都是精确的，整个表达式才是精确的
        btw_op_is_precise = (btw_op_is_precise && query_range_ctx_->cur_expr_is_precise_);
      }
    }
    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = btw_op_is_precise;
      if (OB_FAIL(and_range_graph(key_part_list, out_key_part))) {
        LOG_WARN("and range graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_not_btw_op(const ObOpRawExpr *t_expr,
                                         ObKeyPart *&out_key_part,
                                         const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(t_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(t_expr), K_(query_range_ctx));
  } else if (3 != t_expr->get_param_count()) {//trip op expr
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t_expr must has 3 arguments", K(ret));
  } else {
    bool not_btw_op_is_precise = true;
    const ObRawExpr *l_expr = t_expr->get_param_expr(0);
    ObKeyPartList key_part_list;
    for (int i = 0; OB_SUCC(ret) && i < 2; ++i) {
      query_range_ctx_->cur_expr_is_precise_ = false;
      const ObRawExpr *r_expr = t_expr->get_param_expr(i + 1);
      ObKeyPart *tmp = NULL;
      if (OB_FAIL(get_basic_query_range(l_expr,
                                        r_expr,
                                        NULL,
                                        i == 0 ? T_OP_LT : T_OP_GT,
                                        t_expr->get_result_type(),
                                        tmp,
                                        dtc_params))) {
        LOG_WARN("Get basic query range failed", K(ret));
      } else if (OB_FAIL(add_or_item(key_part_list, tmp))) {
        LOG_WARN("push back failed", K(ret));
      } else {
        //NOT BETWEEN...AND...表达式被拆分成了两个表达式
        //要保证每个表达式都是精确的，整个表达式才是精确的
        not_btw_op_is_precise = (not_btw_op_is_precise && query_range_ctx_->cur_expr_is_precise_);
      }
    }
    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = not_btw_op_is_precise;
      //not need params when preliminary extract
      if (OB_FAIL(or_range_graph(key_part_list, NULL, out_key_part, dtc_params))) {
        LOG_WARN("or range graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_single_in_op(const ObOpRawExpr *b_expr,
                                           ObKeyPart *&out_key_part,
                                           const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  const ObOpRawExpr *r_expr = NULL;
  if (OB_ISNULL(b_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr or query_range_ctx is null. ", K(b_expr), K_(query_range_ctx));
  } else if (2 != b_expr->get_param_count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t_expr must be 3 argument", K(ret));
  } else if (T_OP_ROW != b_expr->get_param_expr(1)->get_expr_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expect row_expr in in_expr", K(ret));
  } else if (OB_ISNULL(r_expr = static_cast<const ObOpRawExpr *>(b_expr->get_param_expr(1)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("r_expr is null.", K(ret));
  } else if (r_expr->get_param_count() > MAX_RANGE_SIZE) {
    // do not extract range over MAX_RANGE_SIZE
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    query_range_ctx_->cur_expr_is_precise_ = false;
  } else {
    ObArenaAllocator alloc;
    bool cur_in_is_precise = true;
    ObKeyPart *tmp_tail = NULL;
    ObKeyPart *find_false = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < r_expr->get_param_count(); i++) {
      ObKeyPart *tmp = NULL;
      ObExprResType res_type(alloc);
      if (OB_FAIL(get_in_expr_res_type(b_expr, i, res_type))) {
        LOG_WARN("get in expr element result type failed", K(ret), K(i));
      } else if (OB_FAIL(get_basic_query_range(b_expr->get_param_expr(0),
                                                r_expr->get_param_expr(i),
                                                NULL,
                                                T_OP_EQ,
                                                res_type,
                                                tmp,
                                                dtc_params))) {
        LOG_WARN("Get basic query range failed", K(ret));
      } else if (OB_ISNULL(tmp) || NULL != tmp->or_next_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tmp is null or tmp->or_next is not null", K(ret), K(tmp));
      } else if (tmp->is_always_true()) { // find true , out_key_part -> true, ignore other
        out_key_part = tmp;
        cur_in_is_precise = (cur_in_is_precise && query_range_ctx_->cur_expr_is_precise_);
        break;
      } else if (tmp->is_always_false()) { // find false
        find_false = tmp;
      } else if (NULL == tmp_tail) {
        tmp_tail = tmp;
        out_key_part = tmp;
      } else {
        tmp_tail->or_next_ = tmp;
        tmp_tail = tmp;
      }
      if (OB_SUCC(ret)) {
        cur_in_is_precise = (cur_in_is_precise && query_range_ctx_->cur_expr_is_precise_);
      }
    }
    if (OB_SUCC(ret)) {
      if (NULL != find_false && NULL == out_key_part) {
        out_key_part = find_false;
      }
      query_range_ctx_->cur_expr_is_precise_ = cur_in_is_precise;
      ObKeyPartList key_part_list;
      if (OB_FAIL(split_or(out_key_part, key_part_list))) {
        LOG_WARN("split temp_result to or_list failed", K(ret));
      } else if (OB_FAIL(or_range_graph(key_part_list, NULL, out_key_part, dtc_params))) {
        LOG_WARN("or range graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_in_op(const ObOpRawExpr *b_expr,
                                    ObKeyPart *&out_key_part,
                                    const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  // treat IN operation as 'left_param = right_item_1 or ... or left_param = right_item_n'
  const ObOpRawExpr *r_expr = NULL;
  if (OB_ISNULL(b_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(b_expr), K_(query_range_ctx));
  } else if (2 != b_expr->get_param_count()) {//binary op expr
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t_expr must has 3 arguments", K(ret));
  } else if (OB_ISNULL(r_expr = static_cast<const ObOpRawExpr *>(b_expr->get_param_expr(1)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("r_expr is null.", K(ret));
  } else if (r_expr->get_param_count() > MAX_RANGE_SIZE) {
    // do not extract range over MAX_RANGE_SIZE
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    query_range_ctx_->cur_expr_is_precise_ = false;
  } else {
    ObKeyPartList key_part_list;
    ObArenaAllocator alloc;
    bool cur_in_is_precise = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < r_expr->get_param_count(); i++) {
      ObKeyPart *tmp = NULL;
      ObExprResType res_type(alloc);
      if (OB_FAIL(get_in_expr_res_type(b_expr, i, res_type))) {
        LOG_WARN("get in expr element result type failed", K(ret), K(i));
      } else if (OB_FAIL(get_basic_query_range(b_expr->get_param_expr(0),
                                                r_expr->get_param_expr(i),
                                                NULL,
                                                T_OP_EQ,
                                                res_type,
                                                tmp,
                                                dtc_params))) {
        LOG_WARN("Get basic query range failed", K(ret));
      } else if (OB_FAIL(add_or_item(key_part_list, tmp))) {
        LOG_WARN("push back failed", K(ret));
      } else {
        cur_in_is_precise = (cur_in_is_precise && query_range_ctx_->cur_expr_is_precise_);
      }
    }
    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = cur_in_is_precise;
      if (OB_FAIL(or_range_graph(key_part_list, NULL, out_key_part, dtc_params))) {
        LOG_WARN("or range graph failed", K(ret));
      }
    }
  }
  return ret;
}

// treat L NOT IN (R1, ... , Rn) operation as '(L < R1 or L > R1) and ... and (L < Rn or L > Rn)'
int ObQueryRange::pre_extract_not_in_op(const ObOpRawExpr *b_expr,
                                        ObKeyPart *&out_key_part,
                                        const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  const ObRawExpr *l_expr = NULL;
  const ObOpRawExpr *r_expr = NULL;
  if (OB_ISNULL(b_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(b_expr), K_(query_range_ctx));
  } else if (2 != b_expr->get_param_count()) {//binary op expr
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t_expr must has 3 arguments", K(ret));
  } else if (OB_ISNULL(l_expr = b_expr->get_param_expr(0)) ||
             OB_ISNULL(r_expr = static_cast<const ObOpRawExpr *>(b_expr->get_param_expr(1)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("r_expr is null.", K(ret));
  } else if (r_expr->get_param_count() > MAX_NOT_IN_SIZE) {
    // do not extract range over MAX_NOT_IN_SIZE
    GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    query_range_ctx_->cur_expr_is_precise_ = false;
  } else {
    bool cur_expr_is_precise = true;
    ObKeyPartList key_part_list;
    ObArenaAllocator alloc;
    for (int64_t i = 0; OB_SUCC(ret) && i < r_expr->get_param_count(); ++i) {
      ObKeyPart *tmp = NULL;
      ObExprResType res_type(alloc);
      ObKeyPartList or_array;
      bool cur_or_is_precise = true;
      if (OB_FAIL(get_in_expr_res_type(b_expr, i, res_type))) {
        LOG_WARN("get in expr element result type failed", K(ret), K(i));
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < 2; ++j) {
        query_range_ctx_->cur_expr_is_precise_ = false;
        if (OB_FAIL(get_basic_query_range(l_expr,
                                          r_expr->get_param_expr(i),
                                          NULL,
                                          j == 0 ? T_OP_LT : T_OP_GT,
                                          res_type,
                                          tmp,
                                          dtc_params))) {
          LOG_WARN("Get basic query range failed", K(ret));
        } else if (OB_FAIL(add_or_item(or_array, tmp))) {
          LOG_WARN("push back failed", K(ret));
        } else {
          cur_or_is_precise = (cur_or_is_precise && query_range_ctx_->cur_expr_is_precise_);
        }
      }
      if (OB_SUCC(ret)) {
        query_range_ctx_->cur_expr_is_precise_ = cur_or_is_precise;
        if (OB_FAIL(or_range_graph(or_array, NULL, tmp, dtc_params))) {
          LOG_WARN("or range graph failed", K(ret));
        } else if (OB_FAIL(add_and_item(key_part_list, tmp))) {
          LOG_WARN("push back failed", K(ret));
        } else {
          cur_expr_is_precise = (cur_expr_is_precise && query_range_ctx_->cur_expr_is_precise_);
        }
      }
    }

    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = cur_expr_is_precise;
      if (OB_FAIL(and_range_graph(key_part_list, out_key_part))) {
        LOG_WARN("and range graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_and_or_op(const ObOpRawExpr *m_expr,
                                        ObKeyPart *&out_key_part,
                                        const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(m_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(m_expr), K_(query_range_ctx));
  } else {
    bool cur_expr_is_precise = true;
    ObKeyPartList key_part_list;
    for (int64_t i = 0; OB_SUCC(ret) && i < m_expr->get_param_count(); ++i) {
      ObKeyPart *tmp = NULL;
      query_range_ctx_->cur_expr_is_precise_ = false;
      if (OB_FAIL(preliminary_extract(m_expr->get_param_expr(i), tmp, dtc_params))) {
        LOG_WARN("preliminary_extract failed", K(ret));
      } else if (T_OP_AND == m_expr->get_expr_type()) {
        if (OB_FAIL(add_and_item(key_part_list, tmp))) {
          LOG_WARN("push back failed", K(ret));
        }
      } else { //T_OP_OR
        if (OB_FAIL(add_or_item(key_part_list, tmp))) {
          LOG_WARN("push back failed", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        cur_expr_is_precise = (cur_expr_is_precise && query_range_ctx_->cur_expr_is_precise_);
      }
    }
    // for
    if (OB_SUCC(ret)) {
      query_range_ctx_->cur_expr_is_precise_ = cur_expr_is_precise;
      if (T_OP_AND == m_expr->get_expr_type()) {
        if (OB_FAIL(and_range_graph(key_part_list, out_key_part))) {
          LOG_WARN("and range graph failed", K(ret));
        }
      } else {
        if (OB_FAIL(or_range_graph(key_part_list, NULL, out_key_part, dtc_params))) {
          LOG_WARN("or range graph failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObQueryRange::pre_extract_const_op(const ObRawExpr *c_expr,
                                       ObKeyPart *&out_key_part)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(c_expr) || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expr is null.", K(c_expr), K_(query_range_ctx));
  } else {
    ObObj val;
    bool is_valid = false;
    bool b_val = false;
    query_range_ctx_->cur_expr_is_precise_ = false;
    if (c_expr->cnt_param_expr()) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (OB_FAIL(get_calculable_expr_val(c_expr, val, is_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (!is_valid) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (OB_FAIL(ObObjEvaluator::is_true(val, b_val))) {
      LOG_WARN("get bool value failed.", K(ret));
    } else {
      GET_ALWAYS_TRUE_OR_FALSE(b_val, out_key_part);
    }
  }
  return ret;
}

//  For each index, preliminary extract query range,
//  the result may contain prepared '?' expression.
//  If prepared '?' expression exists, final extract action is needed
int ObQueryRange::preliminary_extract(const ObRawExpr *node,
                                      ObKeyPart *&out_key_part,
                                      const ObDataTypeCastParams &dtc_params,
                                      const bool is_single_in)
{
  int ret = OB_SUCCESS;
  out_key_part = NULL;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("argument is not inited", K(ret), KP(node), KP(query_range_ctx_));
  } else if(NULL == node) {
    // do nothing
  } else if (node->is_const_expr()) {
    if(OB_FAIL(pre_extract_const_op(node, out_key_part))) {
      LOG_WARN("extract is_op failed", K(ret));
    }
  } else {
    const ObOpRawExpr *b_expr = static_cast<const ObOpRawExpr *>(node);
    if (IS_BASIC_CMP_OP(node->get_expr_type())) {
      if (OB_FAIL(pre_extract_basic_cmp(node, out_key_part, dtc_params))) {
        LOG_WARN("extract basic cmp failed", K(ret));
      }
    } else if (T_OP_NE == node->get_expr_type()) {
      if (OB_FAIL(pre_extract_ne_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract ne_op failed", K(ret));
      }
    } else if (T_OP_IS == node->get_expr_type()) {
      if (OB_FAIL(pre_extract_is_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract is_op failed", K(ret));
      }
    } else if (T_OP_BTW == node->get_expr_type()) {
      if (OB_FAIL(pre_extract_btw_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract btw_op failed", K(ret));
      }
    } else if (T_OP_NOT_BTW == node->get_expr_type()) {
      if (OB_FAIL(pre_extract_not_btw_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract not_btw failed", K(ret));
      }
    } else if (T_OP_IN  == node->get_expr_type()) {
      if (is_single_in) {
        if (OB_FAIL(pre_extract_single_in_op(b_expr, out_key_part, dtc_params))) {
          LOG_WARN("extract single in_op failed", K(ret));
        }
      } else if (OB_FAIL(pre_extract_in_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract in_op failed", K(ret));
      }
    } else if (T_OP_NOT_IN  == node->get_expr_type()) {
      if (OB_FAIL(pre_extract_not_in_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract in_op failed", K(ret));
      }
    } else if (T_OP_AND == node->get_expr_type() || T_OP_OR == node->get_expr_type()) {
      if (OB_FAIL(pre_extract_and_or_op(b_expr, out_key_part, dtc_params))) {
        LOG_WARN("extract and_or failed", K(ret));
      }
    } else {
      query_range_ctx_->cur_expr_is_precise_ = false;
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    }
  }
  return ret;
}

int ObQueryRange::check_null_param_compare_in_row(const ObRawExpr *l_expr,
                                                  const ObRawExpr *r_expr,
                                                  ObKeyPart *&out_key_part)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(l_expr) || OB_ISNULL(r_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), KP(l_expr), KP(r_expr));
  } else if (((l_expr->has_flag(IS_COLUMN) || l_expr->has_flag(IS_ROWID)) && r_expr->is_const_expr()) ||
             (l_expr->is_const_expr() && (r_expr->has_flag(IS_COLUMN) || r_expr->has_flag(IS_ROWID)))) {
    const ObRawExpr *const_expr = l_expr->is_const_expr() ? l_expr : r_expr;
    ObObj const_val;
    bool is_valid = false;
    if (const_expr->has_flag(CNT_DYNAMIC_PARAM)) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    } else if (OB_FAIL(get_calculable_expr_val(const_expr, const_val, is_valid))) {
      LOG_WARN("failed to get calculable expr val", K(ret));
    } else if (!is_valid) {
      //do nothing
    } else if (const_val.is_null()) {
      GET_ALWAYS_TRUE_OR_FALSE(true, out_key_part);
    }
  } else {/*do nothing*/}
  return ret;
}

#undef GET_ALWAYS_TRUE_OR_FALSE

int ObQueryRange::get_in_expr_res_type(const ObRawExpr *in_expr, int64_t val_idx,
                                       ObExprResType &res_type) const
{
  int ret = OB_SUCCESS;
  if (NULL == in_expr) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("in_expr is null.", K(ret));
  } else {
    const ObRawExpr *l_expr = in_expr->get_param_expr(0);
    if (NULL == l_expr) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("l_expr is null.", K(ret));
    } else {
      int64_t row_dimension = (T_OP_ROW == l_expr->get_expr_type()) ? l_expr->get_param_count() : 1;
      const ObIArray<ObExprCalcType> &calc_types = in_expr->get_result_type().get_row_calc_cmp_types();
      if (T_OP_ROW != l_expr->get_expr_type()) {
        res_type.set_calc_meta(calc_types.at(val_idx));
      } else if (OB_FAIL(res_type.init_row_dimension(row_dimension))) {
        LOG_WARN("fail to init row dimension", K(ret), K(row_dimension));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < row_dimension; ++i) {
          ret = res_type.get_row_calc_cmp_types().push_back(calc_types.at(val_idx * row_dimension + i));
        }
      }
    }
  }
  return ret;
}

int ObQueryRange::is_key_part(const ObKeyPartId &id, ObKeyPartPos *&pos, bool &is_key_part)
{
  int ret = OB_SUCCESS;
  if (NULL == query_range_ctx_) {
    ret = OB_NOT_INIT;
    LOG_WARN("query_range_ctx_ is not inited.", K(ret));
  } else {
    is_key_part = false;
    int map_ret = query_range_ctx_->key_part_map_.get_refactored(id, pos);
    if (OB_SUCCESS == map_ret) {
      is_key_part = true;
      SQL_REWRITE_LOG(DEBUG, "id pair is  key part", K_(id.table_id), K_(id.column_id));
    } else {
      if ( OB_HASH_NOT_EXIST != map_ret) {
        ret = map_ret;
        LOG_WARN("get kay_part_id from hash map failed",
                        K(ret), K_(id.table_id), K_(id.column_id));
      } else {
        is_key_part = false;
        SQL_REWRITE_LOG(DEBUG, "id pair is not key part", K_(id.table_id), K_(id.column_id));
      }
    }
  }
  return ret;
}

// split the head key part to general term or-array.
// each gt has its own and_next_, so no need to deep copy it.

int ObQueryRange::split_general_or(ObKeyPart *graph, ObKeyPartList &or_storage)
{
  int ret = OB_SUCCESS;
  or_storage.clear();
  if (NULL != graph) {
    ObKeyPart *cur_gt = graph;
    while (NULL != cur_gt && OB_SUCC(ret)) {
      ObKeyPart *or_next_gt = cur_gt->cut_general_or_next();
      if (!or_storage.add_last(cur_gt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Split graph to or array failed", K(ret));
      } else {
        cur_gt = or_next_gt;
      }
    }
  } else {
    // do nothing
  }
  return ret;
}


// split the head key part to or-list
// several or key parts may share and_next_, so deep copy is needed.

int ObQueryRange::split_or(ObKeyPart *graph, ObKeyPartList &or_list)
{
  int ret = OB_SUCCESS;
  if (NULL != graph) {
    ObKeyPart *cur = (graph);
    ObKeyPart *prev_and_next = NULL;
    while (OB_SUCC(ret) && NULL != cur) {
      ObKeyPart *or_next = cur->or_next_;
      if (cur->and_next_ != prev_and_next) {
        prev_and_next = cur->and_next_;
      } else {
        if (NULL != prev_and_next) {
          ObKeyPart *new_and_next = NULL;
          if (OB_FAIL(deep_copy_range_graph(cur->and_next_, new_and_next))) {
            LOG_WARN("Copy range graph failed", K(ret));
          }
          cur->and_next_ = new_and_next;
        }
      }
      if (OB_SUCC(ret)) {
        cur->or_next_ = NULL;
        if (!(or_list.add_last(cur))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Split graph to or array failed", K(ret));
        } else {
          cur = or_next;
        }
      }
    }
  }
  return ret;
}

//int ObQueryRange::deal_not_align_keypart(ObKeyPart *l_cur,
//                                         ObKeyPart *r_cur,
//                                         const int64_t &s_offset,
//                                         const int64_t &e_offset,
//                                         ObKeyPart *&rest1)
//{
//  int ret = OB_SUCCESS;
//  ObKeyPart *tail = NULL;
//  while (OB_SUCC(ret)
//         && ) {
//    ObKeyPart *new_key_part = NULL;
//    if (OB_FAIL(alloc_full_key_part(new_key_part))) {
//      //warn
//    } else if (NULL != l_cur && NULL != r_cur) {
//      if (l_cur->pos_.offset_ < r_cur->pos_.offset_) {
//        if (s_need_continue) {
//          new_key_part->set_normal_start(l_cur);
//        }
//        if (e_need_continue) {
//          new_key_part->set_normal_end(l_cur);
//        }
//        if (OB_FAIL(link_item(new_key_part, l_cur))) {
//          //warn
//        } else if (NULL != l_cur->or_next_) {
//          l_cur = NULL;
//        } else {
//          l_cur = l_cur->and_next_;
//        }
//      } else if (l_cur->pos_.offset_ > r_cur->pos_.offset_) {
//        if (s_need_continue) {
//          new_key_part->set_normal_start(r_cur);
//        }
//        if (e_need_continue) {
//          new_key_part->set_normal_end(r_cur);
//        }
//        if (OB_FAIL(link_item(new_key_part, r_cur))) {
//          //warn
//        } else if (NULL != r_cur->or_next_) {
//          r_cur = NULL;
//        } else {
//          r_cur = r_cur->and_next_;
//        }
//      }
//    } else {
//      if (NULL == l_cur) {
//        new_key_part->set_normal_start(r_cur);
//        new_key_part->set_normal_end(r_cur);
//        if (OB_FAIL(link_item(new_key_part, r_cur))) {
//          //warn
//        } else if (NULL != r_cur->or_next_) {
//          r_cur = NULL;
//        } else {
//          r_cur = r_cur->and_next_;
//        }
//      } else if (NULL == r_cur) {
//        new_key_part->set_normal_start(l_cur);
//        new_key_part->set_normal_end(l_cur);
//        if (OB_FAIL(link_item(new_key_part, l_cur))) {
//          //warn
//        } else if (NULL != l_cur->or_next_) {
//          l_cur = NULL;
//        } else {
//          l_cur = l_cur->and_next_;
//        }
//      }
//    }
//    if (OB_SUCC(ret) && NULL != new_key_part) {
//      if (NULL == tail) {
//        tail = new_key_part;
//        rest1 = tail;
//      } else {
//        tail->and_next_ = new_key_part;
//        tail = new_key_part;
//      }
//    }
//  }
//  return ret;
//}
//int ObQueryRange::link_item(ObKeyPart *new_key_part, ObKeyPart *cur)
//{
//  int ret = OB_SUCCESS;
//  ObKeyPart *item = cur ? cur->item_next_ : NULL;
//  while (OB_SUCC(ret) && NULL != item) {
//    ObKeyPart *new_item = NULL;
//    if (OB_ISNULL(new_item = deep_copy_key_part(item))) {
//      ret = OB_ERR_UNEXPECTED;
//      LOG_WARN("Cope item key part failed", K(ret));
//    } else {
//      new_item->item_next_ = new_key_part->item_next_;
//      new_key_part->item_next_ = new_item;
//      item = item->item_next_;
//    }
//  }
//  return ret;
//}
//
// For row action, we need to know where the new start_ and the new end_
// come from, start_border_type/end_border_type will return the new
//  edges source, then we can treat row as integrity
//
//  E.g.
//       row(k1, k2) > (3, 2) and row(k1, k2) > (1, 4)
//       when find the intersection of k1, we need to know the result3 is comes
//       from the first const row(3,2), then 2 is the new start of k2.
//       ==> row(k1, k2) > (3, 2),  not row(k1, k2) > (3, 4)

//对于当前范围为等值的时候，取下一个来进行边界判断，例如(a, b)>(1, 2) and (a, b) > (1, 3),
//通过b来判断。
//若出现不对齐的情况，例如(a, c) > (1, 3) and (a, b, c) > (1, 2, 3)
//选取有值那一方为边界，这里选右边
//用两个bool变量来区分起始边界是否需要通过下一个列值来判断。
//对于属于等值条件，但是实际值不相等设为恒false.比如(a,b)=(1, 1) and (a, b)=(1, 2)
//对于非第一列范围无交集的情况，设为恒false.
//比如(a, b) > (1, 2) and (a, b) <  (2, 3) and (a, b) > (1, 4) and (a, b) < (2, 5)

int ObQueryRange::intersect_border_from(const ObKeyPart *l_key_part,
                                        const ObKeyPart *r_key_part,
                                        ObRowBorderType &start_border_type,
                                        ObRowBorderType &end_border_type,
                                        bool &is_always_false)
{
  int ret = OB_SUCCESS;
  bool start_identified = true;
  bool end_identified = true;
  bool s_need_continue = false;
  bool e_need_continue = false;
  start_border_type = OB_FROM_NONE;
  end_border_type = OB_FROM_NONE;
  bool left_is_equal = false;
  if (OB_ISNULL(l_key_part) || OB_ISNULL(r_key_part)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", K(l_key_part), K(r_key_part));
  } else if (OB_UNLIKELY(!l_key_part->is_normal_key()) || OB_UNLIKELY(!r_key_part->is_normal_key())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("keypart isn't normal key", K(*l_key_part), K(*r_key_part));
  } else if (l_key_part->pos_.offset_ < r_key_part->pos_.offset_) {
    start_border_type = OB_FROM_LEFT;
    end_border_type = OB_FROM_LEFT;
  } else if (l_key_part->pos_.offset_ > r_key_part->pos_.offset_) {
    start_border_type = OB_FROM_RIGHT;
    end_border_type = OB_FROM_RIGHT;
  } else if (true == (left_is_equal = l_key_part->is_equal_condition())
             || r_key_part->is_equal_condition()) {
    if (l_key_part->is_question_mark() || r_key_part->is_question_mark()) {
      s_need_continue = true;
      e_need_continue = true;
    } else if (l_key_part->has_intersect(r_key_part)) {
      // incase here is last
      if (NULL == l_key_part->and_next_) {
        start_border_type = left_is_equal ? OB_FROM_LEFT : OB_FROM_RIGHT;
        end_border_type = start_border_type;
      } else {
        s_need_continue = true;
        e_need_continue = true;
      }
    } else {
      is_always_false = true;
    }
  } else {
    ObObj *s1 = &l_key_part->normal_keypart_->start_;
    ObObj *e1 = &l_key_part->normal_keypart_->end_;
    ObObj *s2 = &r_key_part->normal_keypart_->start_;
    ObObj *e2 = &r_key_part->normal_keypart_->end_;
    if (OB_ISNULL(s1) || OB_ISNULL(e1) || OB_ISNULL(s2) || OB_ISNULL(e2)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("s1,e1,s2,e2 can not be null", K(ret), KP(s1), KP(e1), KP(s2), KP(e2));
    } else if (l_key_part->is_question_mark() || r_key_part->is_question_mark()) {
      if (!is_min_range_value(*s1) && !is_min_range_value(*s2)) {
        // both have none-min start value
        start_border_type = OB_FROM_NONE;
      } else if (is_min_range_value(*s1) && !is_min_range_value(*s2)) {
        // only r_key_part has start value
        start_border_type = OB_FROM_RIGHT;
      } else if (!is_min_range_value(*s1) && is_min_range_value(*s2)) {
        // only l_key_part has start value
        start_border_type = OB_FROM_LEFT;
      } else { // both have min start value
        start_identified = false;
      }
      if (!is_max_range_value(*e1) && !is_max_range_value(*e2)) {
        end_border_type = OB_FROM_NONE;
      } else if (is_max_range_value(*e1) && !is_max_range_value(*e2)) {
        end_border_type = OB_FROM_RIGHT;
      } else if (!is_max_range_value(*e1) && is_max_range_value(*e2)) {
        end_border_type = OB_FROM_LEFT;
      } else {
        end_identified = false;
      }
    } else {
      // has changeless value
      if (l_key_part->id_ != r_key_part->id_ || l_key_part->pos_ != r_key_part->pos_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("l_id is not equal to r_id", K(ret));
      } else if (l_key_part->has_intersect(r_key_part)) {
        if (is_min_range_value(*s1) && is_min_range_value(*s2)) {
          start_identified = false;
        } else {
          int cmp = s1->compare(*s2);
          if (cmp > 0) {
            start_border_type = OB_FROM_LEFT;
          } else if (cmp < 0) {
            start_border_type = OB_FROM_RIGHT;
          } else { // equal
            if (NULL == l_key_part->and_next_) {
              start_border_type = OB_FROM_LEFT; // lucky left
            }
            s_need_continue = true;
          }
        }
        if (is_max_range_value(*e1) && is_max_range_value(*e2)) {
          end_identified = false;
        } else {
          int cmp = e1->compare(*e2);
          if (cmp > 0) {
            end_border_type = OB_FROM_RIGHT;
          } else if (cmp < 0) {
            end_border_type = OB_FROM_LEFT;
          } else { // euqal
            if (NULL == l_key_part->and_next_) {
              end_border_type = OB_FROM_LEFT; // lucky left
            }
            e_need_continue = true;
          }
        }
      } else {
        is_always_false = true;
      }
    }
    if (!start_identified || !end_identified) {
      if (!start_identified && !end_identified) {
        start_border_type = OB_FROM_LEFT; // lucky left
        end_border_type = OB_FROM_LEFT; // lucky left
      } else if (start_identified) {
        end_border_type = start_border_type;
        e_need_continue = s_need_continue;
      } else if (end_identified) {
        start_border_type = end_border_type;
        s_need_continue = e_need_continue;
      } else {
        // do nothing
      }
    }
  }
  if (OB_SUCC(ret)
      && !is_always_false
      && NULL != l_key_part->and_next_
      && NULL != r_key_part->and_next_
      && (s_need_continue || e_need_continue)) {
    ObRowBorderType tmp_start_border = OB_FROM_NONE;
    ObRowBorderType tmp_end_border = OB_FROM_NONE;
    if (OB_FAIL(SMART_CALL(intersect_border_from(l_key_part->and_next_, r_key_part->and_next_,
        tmp_start_border, tmp_end_border, is_always_false)))) {
      LOG_WARN("invalid argument.", K(ret), K(l_key_part), K(r_key_part));
    } else if (s_need_continue) {
      start_border_type = tmp_start_border;
      if (e_need_continue) {
        end_border_type = tmp_end_border;
      }
    } else {}
  }
  return ret;
}

bool ObQueryRange::is_max_range_value(const ObObj &obj) const
{
  return lib::is_oracle_mode() ? (obj.is_max_value() || obj.is_null()) : obj.is_max_value();
}

bool ObQueryRange::is_min_range_value(const ObObj &obj) const
{
  return lib::is_oracle_mode() ? (obj.is_min_value()) : (obj.is_min_value() || obj.is_null());
}


//After wen known where the edges of the row come from,
//set the new edges to the result.
//if or_next_ list is not NULL, cut it.

int ObQueryRange::set_partial_row_border(
    ObKeyPart *l_gt,
    ObKeyPart *r_gt,
    ObRowBorderType start_border_type,
    ObRowBorderType end_border_type,
    ObKeyPart  *&result)
{
  int ret = OB_SUCCESS;
  ObKeyPart *l_cur = (NULL != l_gt && NULL == l_gt->or_next_) ? l_gt : NULL;
  ObKeyPart *r_cur = (NULL != r_gt && NULL == r_gt->or_next_) ? r_gt : NULL;
  ObKeyPart *prev_key_part = NULL;
  result = NULL;
  if (ObQueryRange::OB_FROM_NONE != start_border_type
      || ObQueryRange::OB_FROM_NONE != end_border_type) {
    bool b_flag = false;
    while (OB_SUCC(ret) && !b_flag && (NULL != l_cur || NULL != r_cur)) {
      ObKeyPart *new_key_part = NULL;
      if (start_border_type != end_border_type
          && NULL != l_cur
          && NULL != r_cur
          && l_cur->is_question_mark() != r_cur->is_question_mark()) {
        // we can express such case: start is unknown value, but end is known value
        b_flag = true;
      } else if (((ObQueryRange::OB_FROM_LEFT == start_border_type || ObQueryRange::OB_FROM_LEFT == end_border_type) && OB_ISNULL(l_cur))
                 || ((ObQueryRange::OB_FROM_RIGHT == start_border_type || ObQueryRange::OB_FROM_RIGHT == end_border_type)
                     && OB_ISNULL(r_cur))) {
        //target row的取值达到被取值row的末尾
        b_flag = true;
      } else if (OB_FAIL(alloc_full_key_part(new_key_part))) {
        LOG_WARN("Get key part failed", K(ret));
        b_flag = true;
      } else if (OB_ISNULL(new_key_part) || OB_UNLIKELY(!new_key_part->is_normal_key())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("new_key_part is null.");
      } else {
        new_key_part->normal_keypart_->always_true_ = false;
        if (ObQueryRange::OB_FROM_LEFT == start_border_type && l_cur) {
          new_key_part->set_normal_start(l_cur);
        } else if (ObQueryRange::OB_FROM_RIGHT == start_border_type && NULL != r_cur) {
          new_key_part->set_normal_start(r_cur);
        } else {
          // do nothing
        }
        if (ObQueryRange::OB_FROM_LEFT == end_border_type && l_cur) {
          new_key_part->set_normal_end(l_cur);
        } else if (ObQueryRange::OB_FROM_RIGHT == end_border_type && NULL != r_cur) {
          new_key_part->set_normal_end(r_cur);
        } else {
          // do nothing
        }
        ObKeyPart *item = NULL != l_cur ? l_cur->item_next_ : NULL;
        while (OB_SUCC(ret) && NULL != item) {
          ObKeyPart *new_item = NULL;
          if (OB_ISNULL(new_item = deep_copy_key_part(item))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("Cope item key part failed", K(ret));
          } else {
            new_item->item_next_ = new_key_part->item_next_;
            new_key_part->item_next_ = new_item;
            item = item->item_next_;
          }
        }
        item = NULL != r_cur ? r_cur->item_next_ : NULL;
        while (OB_SUCC(ret) && NULL != item) {
          ObKeyPart *new_item = NULL;
          if (OB_ISNULL(new_item = deep_copy_key_part(item))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("Cope item key part failed", K(ret));
          } else {
            new_item->item_next_ = new_key_part->item_next_;
            new_key_part->item_next_ = new_item;
            item = item->item_next_;
          }
        }
      }
      if (OB_SUCC(ret) && !b_flag) {
        if (NULL != prev_key_part) {
          prev_key_part->and_next_ = new_key_part;
        } else {
          result = new_key_part;
        }
        prev_key_part = new_key_part;
        // if and_next_ does not have or-item, then do next
        if (NULL != l_cur && NULL != l_cur->and_next_ && NULL == l_cur->and_next_->or_next_) {
          l_cur = l_cur->and_next_;
        } else {
          l_cur = NULL;
        }
        if (NULL != r_cur && NULL != r_cur->and_next_ && NULL == r_cur->and_next_->or_next_) {
          r_cur = r_cur->and_next_;
        } else {
          r_cur = NULL;
        }
      }
    }
  }
  return ret;
}

//  do and of general term (gt) when row exists.
//  if you need, find the meaning of gt from comments of do_gt_and().
//
//  ROW(k1(s1, e1), k2(s2, e2)) and ROW(k1(s3, e4), k2(s3, e4))
//         k1(s1, e1): means the first key part in row between s1 and e1.
//  because the and operands is row, we can not AND each key part separtely.
//  so the result is:
//  ROW(k1(ns1, ne1), k2(ns2, ne2)), if two row values has intersection.
//         if s1>s3, ns1 = s1 and ns2 = s2 else ns1 = s3 and ns2 = s4;
//         if e1<e2, ne1 = e1 and ne2 = e2 else ne1 = e3 and ne2 = e4;
//         if start/end value can not compare, k1 is returned only
//             e.g. row(k1, k2) > (?1, ?2) and row(k1, k2) > (?3, ?4)

int ObQueryRange::do_row_gt_and(ObKeyPart *l_gt, ObKeyPart *r_gt, ObKeyPart  *&res_gt)
{
  int ret = OB_SUCCESS;
  if (NULL == l_gt && NULL == r_gt) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Wrong argument", K(ret));
  } else if (NULL == l_gt) {
    res_gt = r_gt;
  } else if (NULL == r_gt) {
    res_gt = l_gt;
  } else if (l_gt->pos_.offset_ < r_gt->pos_.offset_) {
    res_gt = l_gt; //两个向量做and，右边向量前缀缺失，直接用左边向量的结果
  } else if (r_gt->pos_.offset_ < l_gt->pos_.offset_) {
    res_gt = r_gt; //两个向量做and，左边向量前缀缺失，直接用右边向量的结果
  } else {
    ObKeyPart *l_gt_next = l_gt->and_next_;
    ObKeyPart *r_gt_next = r_gt->and_next_;
    res_gt = NULL;
    bool always_true = false;
    ObKeyPart *find_false = NULL;
    ObKeyPart *tail = NULL;
    ObKeyPart *l_cur = NULL;
    ObKeyPart *r_cur = NULL;
    for (l_cur = l_gt; OB_SUCC(ret) && !always_true && NULL != l_cur; l_cur = l_cur->or_next_) {
      for (r_cur = r_gt; OB_SUCC(ret) && !always_true && NULL != r_cur; r_cur = r_cur->or_next_) {
        ObKeyPart *result = NULL;
        ObKeyPart *rest = NULL;
        if (l_cur->is_like_key() && r_cur->is_like_key()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("l_cur and r_cur are both like key", K(ret), K(*l_cur), K(*r_cur));
        } else if (l_cur->is_like_key()) {
          result = r_cur;
        } else if (r_cur->is_like_key()) {
          result = l_cur;
        } else if (!l_cur->is_normal_key() || !r_cur->is_normal_key()
                   || l_cur->is_always_true() || l_cur->is_always_false()
                   || r_cur->is_always_true() || r_cur->is_always_false()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("l_cur and r_cur are not always true or false.", K(*l_cur), K(*r_cur));
        } else {
          ObKeyPart *new_l_cur = NULL;
          ObKeyPart *new_r_cur = NULL;
          if (OB_FAIL(deep_copy_key_part_and_items(l_cur, new_l_cur))) {
            LOG_WARN("Light copy key part and items failed", K(ret));
          } else if(OB_FAIL(deep_copy_key_part_and_items(r_cur, new_r_cur))) {
            LOG_WARN("Right copy key part and items failed", K(ret));
          } else if (OB_FAIL(do_key_part_node_and(new_l_cur, new_r_cur, result))) {  // do AND of each key part node only
            LOG_WARN("Do key part node intersection failed", K(ret));
          } else if(NULL == result) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("result is null.", K(ret));
          } else if (result->is_always_true()) {
            always_true = true;
            res_gt = result;
          } else if (result->is_always_false()) {
            // ignore
            find_false = result;
          } else {
            // set other value of row
            ObRowBorderType s_border = OB_FROM_NONE;
            ObRowBorderType e_border = OB_FROM_NONE;
            bool is_always_false = false;
            if (OB_FAIL(intersect_border_from(l_cur, r_cur,
                s_border, e_border, is_always_false))) {
              LOG_WARN("Find row border failed", K(ret));
            } else if (is_always_false) {
              result->normal_keypart_->always_false_ = true;
              result->normal_keypart_->always_true_ = true;
              result->normal_keypart_->start_.set_max_value();
              result->normal_keypart_->end_.set_min_value();
              find_false = result;
            } else if (OB_FAIL(set_partial_row_border(l_gt_next, r_gt_next,
                s_border, e_border, rest))) {
              LOG_WARN("Set row border failed", K(ret));
            } else if (OB_ISNULL(rest)) {
              //如果做向量条件融合，导致条件被丢弃掉，说明抽取有放大，需要清除掉精确抽取的filter标记，不能去除对应的filter
              if (OB_FAIL(remove_precise_range_expr(result->pos_.offset_ + 1))) {
                LOG_WARN("remove precise range expr failed", K(ret));
              }
            }
          }
        }

        if (OB_SUCC(ret)) {
          result->link_gt(rest);
          // link to the or_next_ list
          if (NULL != tail) {
            tail->or_next_ = result;
          } else {
            res_gt = result;
          }
          tail = result;
        }
      }
    }
    if (OB_SUCC(ret) && NULL == res_gt) {
      if (NULL == find_false) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("find_false is null.", K(ret));
      } else {
        res_gt = find_false;
      }
    }
  }
  return ret;
}


//  AND single key part (itself) and items in item_next_ list
//  Each item in item_next_ list must be unkown item untill physical plan open
//
//  E.g.
//       (A1 and ?1 and ?2 and ... and ?m) and (A2 and ?I and ?II and ... and ?n)
//       ==>
//       (A12 and ?1 and ?2 and ... and ?m and ?I and ?II and ... and ?n)
//       A12 is the result of (A1 intersect A2)

int ObQueryRange::do_key_part_node_and(
    ObKeyPart *l_key_part,
    ObKeyPart *r_key_part,
    ObKeyPart *&res_key_part)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(l_key_part) || OB_ISNULL(r_key_part)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Wrong argument", K(ret));
  } else {
    if (l_key_part->is_always_true() || r_key_part->is_always_true()) {
      res_key_part = r_key_part;
    } else if (l_key_part->is_always_false() || r_key_part->is_always_false()) {
      res_key_part = l_key_part;
    } else {
      res_key_part = NULL;
      l_key_part->and_next_ = NULL;
      l_key_part->or_next_ = NULL;
      r_key_part->and_next_ = NULL;
      r_key_part->or_next_ = NULL;
      ObKeyPart *l_items = l_key_part;
      ObKeyPart *r_items = r_key_part;
      if (!l_key_part->is_question_mark() && !r_key_part->is_question_mark()) {
        l_items = l_key_part->item_next_;
        r_items = r_key_part->item_next_;
        if (OB_FAIL(l_key_part->intersect(r_key_part, contain_row_))) {
          LOG_WARN("Do key part node intersection failed", K(ret));
        } else {
          res_key_part = l_key_part;
        }
      } else if (!l_key_part->is_question_mark()) {
        res_key_part = l_key_part;
        l_items = l_key_part->item_next_;
      } else if (!r_key_part->is_question_mark()) {
        res_key_part = r_key_part;
        r_items = r_key_part->item_next_;
      }

      // link all unkown-value items
      if (OB_SUCC(ret)) {
        if (NULL != l_items) {
          if (NULL != res_key_part) {
            res_key_part->item_next_ = l_items;
          } else {
            res_key_part = l_items;
          }
        }
        if (NULL != r_items) {
          if (OB_ISNULL(res_key_part)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("res_key_part is null.", K(ret));
          } else {
            ObKeyPart *tail = res_key_part;
            // find the item_next_ list tail
            while (NULL != tail->item_next_) {
              tail = tail->item_next_;
            }
            tail->item_next_ = r_items;
          }
        }
      }
    }
  }
  return ret;
}

//  Just get the key part node itself and items in its item_next_ list

int ObQueryRange::deep_copy_key_part_and_items(
    const ObKeyPart *src_key_part,
    ObKeyPart  *&dest_key_part)
{
  int ret = OB_SUCCESS;
  const ObKeyPart *tmp_key_part = src_key_part;
  ObKeyPart *prev_key_part = NULL;
  while (OB_SUCC(ret) && NULL != tmp_key_part) {
    ObKeyPart *new_key_part = NULL;
    if (OB_ISNULL(new_key_part = create_new_key_part())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc ObKeyPart failed", K(ret));
    } else if (OB_FAIL(new_key_part->deep_node_copy(*tmp_key_part))) {
      LOG_WARN("Copy key part node failed", K(ret));
    } else {
      if (NULL != prev_key_part) {
        prev_key_part->item_next_ = new_key_part;
      } else {
        dest_key_part = new_key_part;
      }
      prev_key_part = new_key_part;
      tmp_key_part = tmp_key_part->item_next_;
    }
  }
  return ret;
}

//  Just and the general item
//       each key part in the general item or_next_ list has same and_next_.
//       each key part in the general item or_next_ list may have item_next_,
//       any key part linked by item_next_ is run-time value,
//       which can not know at this time
//
//  l_gt/r_gt is a group
//  l_gt/r_gt must satisfy following features:
//       1. the key parts must have same key offset
//       2. all key part in same or_next_ list have same and_next_.
//
//  E.g.
//       (A1 or A2 or ... or Am) and (AI or AII or ... or An)
//       ==>
//       (A1I or A1II or ... or A1n or A2I or A2II or ... or A2n or ...... or AmI or AmII or ... or A1mn)
//       Aij = (Ai from (A1 or A2 or ... or Am) intersect Aj from (AI or AII or ... or An))

int ObQueryRange::do_gt_and(ObKeyPart *l_gt, ObKeyPart *r_gt, ObKeyPart *&res_gt)
{
  int ret = OB_SUCCESS;
  if (NULL == l_gt && NULL == r_gt) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Wrong argument", K(ret));
  } else if (NULL == l_gt) {
    res_gt = r_gt;
  } else if (NULL == r_gt) {
    res_gt = l_gt;
  } else {
    res_gt = NULL;
    if (OB_UNLIKELY(l_gt->pos_.offset_ != r_gt->pos_.offset_)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("Wrong argument", K(ret), K(l_gt->pos_.offset_), K(r_gt->pos_.offset_));
    } else {
      ObKeyPart *find_false = NULL;
      ObKeyPart *tail = NULL;
      ObKeyPart *l_cur = NULL;
      ObKeyPart *r_cur = NULL;
      for (l_cur = l_gt; OB_SUCC(ret) && NULL != l_cur; l_cur = l_cur->or_next_) {
        bool find_true = false;
        for (r_cur = r_gt; OB_SUCC(ret) && !find_true && NULL != r_cur; r_cur = r_cur->or_next_) {
          ObKeyPart *result = NULL;
          ObKeyPart *new_l_cur = NULL;
          ObKeyPart *new_r_cur = NULL;
          if (OB_FAIL(deep_copy_key_part_and_items(l_cur, new_l_cur))) {
            LOG_WARN("Light copy key part and items failed", K(ret));
          } else if (OB_FAIL(deep_copy_key_part_and_items(r_cur, new_r_cur))) {
            LOG_WARN("right copy key part and items failed", K(ret));
          } else if (OB_FAIL(do_key_part_node_and(new_l_cur, new_r_cur, result))) { // do AND of each key part node only
            LOG_WARN("Do key part node intersection failed", K(ret));
          } else if (NULL == result) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("result is null.", K(ret));
          } else if (result->is_always_true()) {
            res_gt = result;
            find_true = true;
          } else if (result->is_always_false()) {
            // ignore
            find_false = result;
          } else {
            if (NULL == res_gt) {
              res_gt = result;
            }
            // link to the or_next_ list
            if (NULL != tail) {
              tail->or_next_ = result;
            }
            tail = result;
          }
        }
      }
      if (OB_SUCC(ret)) {
        // all false
        if (NULL == res_gt) {
          if (NULL == find_false) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("find_false is NULL.", K(ret));
          } else {
            res_gt = find_false;
          }
        }
      }
    }
  }
  return ret;
}

//  do and operation of each graph in l_array and r_array by two path method,
//  then treat the results as or relation
//
//  we do the and operation recursively:
//  1. if left id < right id, result = left-current-key and RECUSIVE_AND(left-rest-keys, right-keys) ;
//  2. if left id > right id, result = right-current-key and RECUSIVE_AND(left-keys, right-rest-keys) ;
//  3. if left id == right id, result = INTERSECT(left-current-key, right-current-key) and RECUSIVE_AND(left-rest-keys, right-rest-keys) ;

int ObQueryRange::and_single_gt_head_graphs(
    ObKeyPartList &l_array,
    ObKeyPartList &r_array,
    ObKeyPartList &res_array)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (l_array.get_size() <= 0 || r_array.get_size() <= 0 || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("And operand can not be empty",
             K(ret), K(l_array.get_size()), K(r_array.get_size()));
  } else {
    res_array.clear();
    ObKeyPart *find_false = NULL;
    bool always_true = false;
    for (ObKeyPart *l = l_array.get_first();
         OB_SUCC(ret) && !always_true && l != l_array.get_header() && NULL != l;
         l = l->get_next()) {
      ObKeyPart *tmp_result = NULL;
      for (ObKeyPart *r = r_array.get_first();
           OB_SUCC(ret) && r != r_array.get_header() && NULL != r;
           r = r->get_next()) {
        ObKeyPart *l_cur_gt = NULL;
        ObKeyPart *r_cur_gt = NULL;
        if (OB_FAIL(deep_copy_range_graph(l, l_cur_gt))) {
          LOG_WARN("Left deep copy range graph failed", K(ret));
        } else if (OB_FAIL(deep_copy_range_graph(r, r_cur_gt))) {
          LOG_WARN("Right deep copy range graph failed", K(ret));
        } else if (NULL == l_cur_gt || NULL == r_cur_gt) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("key_part is null.", K(ret));
        } else {
          ObKeyPart *l_and_next = l_cur_gt->and_next_;
          ObKeyPart *r_and_next = r_cur_gt->and_next_;
          ObKeyPart *rest_result = NULL;
          // false and everything is false
          if (l_cur_gt->is_always_false() || r_cur_gt->is_always_false()) {
            tmp_result = l_cur_gt->is_always_false() ? l_cur_gt : r_cur_gt;
            tmp_result->and_next_ = NULL;
          } else if (r_cur_gt->is_always_true()) {
            tmp_result = l_cur_gt;
            // tmp_result->and_next_ = NULL;
          } else if (l_cur_gt->is_always_true()) {
            tmp_result = r_cur_gt;
            // tmp_result->and_next_ = NULL;
          } else if (contain_row_) {
            if (OB_FAIL(do_row_gt_and(l_cur_gt, r_cur_gt, tmp_result))) {
              LOG_WARN("AND row failed", K(ret));
            } else if (query_range_ctx_ != NULL) {
              //经过向量的合并后，为了正确性考虑，都保守不再去filter
              //清空精确条件的记录
              query_range_ctx_->precise_range_exprs_.reset();
            }
          } else { // normal case
            // 1. do and of the first general item
            if (l_cur_gt->pos_.offset_ < r_cur_gt->pos_.offset_) {
              tmp_result = l_cur_gt;
              r_and_next = r_cur_gt;
            } else if (r_cur_gt->pos_.offset_  < l_cur_gt->pos_.offset_) {
              tmp_result = r_cur_gt;
              l_and_next = l_cur_gt;
            } else { //r_cur_gt->id_ == l_cur_gt->id_
              if (OB_FAIL(do_gt_and(l_cur_gt, r_cur_gt, tmp_result))) {
                LOG_WARN("Do AND of gerneral term failed", K(ret));
              }
            }
            if (OB_SUCCESS != ret ) {
              // do nothing
            } else if (OB_ISNULL(tmp_result)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("tmp_result is null.", K(ret));
            } else {
              tmp_result->and_next_ = NULL;

              // 2. recursive do rest part keys
              if (tmp_result->is_always_false() || tmp_result->is_always_true()) {
                //no need to do rest part
              } else {
                ObKeyPartList and_storage;
                if (NULL == l_and_next) {
                  rest_result = r_and_next;
                } else if (NULL == r_and_next) {
                  rest_result = l_and_next;
                } else if (!and_storage.add_last(l_and_next) || !and_storage.add_last(r_and_next)) {
                  ret = OB_ERR_UNEXPECTED;
                  LOG_WARN("Add ObKeyPart to list failed", K(ret));
                } else if (OB_FAIL(and_range_graph(and_storage, rest_result))) {
                  LOG_WARN("And range graph failed", K(ret));
                } else {
                  // do nothing
                }

                // 3. AND head and rest
                if (OB_SUCC(ret)) {
                  // not contain row, if rest result is false, then whole result is false
                  if (NULL != rest_result && rest_result->is_always_false()) {
                    if (!contain_row_) {
                      tmp_result = rest_result;
                    }
                  } else if (NULL != rest_result && rest_result->is_always_true()) {
                    // no need to link rest part
                  } else {
                    tmp_result->link_gt(rest_result);
                  }
                }
              }
            }
          }
        }

        // 4. add current result to result array
        if (OB_SUCC(ret)) {
          // and the result to result array
          if (OB_ISNULL(tmp_result)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("tmp_result is null.", K(ret));
          } else {
            if (tmp_result->is_always_false()) {
              // ignore false
              if (!find_false) {
                find_false = tmp_result;
              }
            } else if (tmp_result->is_always_true()) {
              // no need to do any more
              always_true = true;
              res_array.clear();
              if (!res_array.add_last(tmp_result)) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("res_array added tmp_result failed.", K(ret));
              }
            } else {
              if (!res_array.add_last(tmp_result)) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("res_array added tmp_result failed.", K(ret));
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && res_array.get_size() <= 0) {
      // all false ranges
      if (OB_ISNULL(find_false)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("find_false is null.", K(ret));
      } else if (!res_array.add_last(find_false)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("res_array added find_false failed.", K(ret));
      }
    }
  }
  return ret;
}

//  And all query range graph
//  the initial ranges must come from add_and_item (), that ensure only 1 real-true graph in array
//  and no part false in graph

int ObQueryRange::and_range_graph(ObKeyPartList &ranges, ObKeyPart  *&out_key_part)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (ranges.get_size() <= 0 || OB_ISNULL(query_range_ctx_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("AND array can not be empty",
             K(ret), KP(query_range_ctx_), K(ranges.get_size()));
  } else if (1 == ranges.get_size()) {
    out_key_part = ranges.get_first();
    ranges.remove(out_key_part);
  } else {

    //each graph may have many or_next_
    //    we split them to a single and list, then and each of two,
    //    at last do or of all the result
    //E.g.
    //    l_graph: ((A1 and B1) or (A2 and B2)) and other_cnd1
    //                 AND
    //    r_graph: ((A3 and B3) or (A4 and B4)) and other_cnd2
    //    Equal to:
    //                 ((A1 and B1) and (A3 and B3)) and (other_cnd1 and other_cnd2)
    //                 OR
    //                 ((A1 and B1) and (A4 and B4)) and (other_cnd1 and other_cnd2)
    //                 OR
    //                 ((A2 and B2) and (A3 and B3)) and (other_cnd1 and other_cnd2)
    //                 OR
    //                 ((A2 and B2) and (A4 and B4)) and (other_cnd1 and other_cnd2)

    ObKeyPartList res_storage1;
    ObKeyPartList res_storage2;
    ObKeyPart *cur = ranges.get_first();
    if (OB_ISNULL(cur)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur is null.", K(ret));
    } else {
      ObKeyPart *cur_next = cur->get_next();
      ranges.remove_first();
      if (OB_ISNULL(cur_next)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("cur_next is null.", K(ret));
      } else if (cur->is_always_true() || cur->is_always_false()) {
        cur->and_next_ = NULL;
        cur->or_next_ = NULL;
        if (!res_storage1.add_last(cur)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("res_storage1 added cur failed.", K(ret));
        }
      } else if (OB_FAIL(split_general_or(cur, res_storage1))) {
        LOG_WARN("split general or key part failed", K(ret));
      } else {
        // do nothing
      }
    }
    ObKeyPartList *l_array = &res_storage1;
    ObKeyPartList *res_array = &res_storage2;
    int i = 0;
    while (OB_SUCC(ret) && ranges.get_size() > 0) {
      ObKeyPart *other = ranges.get_first();
      ranges.remove_first();
      ++i;
      if (i % 2) {
        l_array = &res_storage1;
        res_array = &res_storage2;
      } else {
        l_array = &res_storage2;
        res_array = &res_storage1;
      }
      ObKeyPartList r_array;
      if (OB_ISNULL(other) || OB_ISNULL(l_array) || OB_ISNULL(l_array->get_first())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("other is null.", K(ret));
      } else if (other->is_always_true() || other->is_always_false()) {
        if (!r_array.add_last(other)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("r_array added other failed", K(ret));
        }
      } else if (OB_FAIL(split_general_or(other, r_array))) {
        LOG_WARN("split general or key part failed", K(ret));
      } else {
        // do nothing
      }
      if (OB_FAIL(ret)) {
        LOG_WARN("Split graph failed", K(ret));
      } else if (OB_FAIL(SMART_CALL(and_single_gt_head_graphs(*l_array, r_array, *res_array)))) {
        LOG_WARN("And single general term head graphs failed", K(ret));
      } else if (OB_ISNULL(res_array) || OB_ISNULL(ranges.get_first())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("res_array or ranges.get_first() is null.", K(ret));
      } else { }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(link_or_graphs(*res_array, out_key_part))) {
        //LOG_WARN("And single general term head graphs failed res_array=%s arr_size=%ld",
        //          ret, to_cstring(*res_array), res_array->count());
        LOG_WARN("And single general term head graphs failed",
                 K(ret), K(res_array->get_size()));
      }
    }
  }
  return ret;
}

// Link all graphs of OR relation

int ObQueryRange::link_or_graphs(ObKeyPartList &storage, ObKeyPart  *&out_key_part)
{
  int ret = OB_SUCCESS;
  ObKeyPart *last_gt_tail = NULL;
  if (OB_UNLIKELY(storage.get_size() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Or storage is empty", K(ret), "query range", to_cstring(*this));
  } else {
    ObKeyPart *first_key_part = storage.get_first();
    bool b_flag  = false;
    while (OB_SUCC(ret) && !b_flag && storage.get_size() > 0) {
      ObKeyPart *cur = storage.get_first();
      storage.remove_first();
      if (NULL != cur) {
        if (cur == first_key_part) {
          out_key_part = first_key_part;
          last_gt_tail = first_key_part;
        } else if (cur->is_always_true()) {
          // return ture
          out_key_part = cur;
          b_flag = true;
        } else if (cur->is_always_false()) {
          // ignore false
          // if the first is always_false_, out_key_part has already pointed to it
        } else {
          // we have record the first always_false_,
          // replace it to the first following none-always_false_ key part
          if (NULL == out_key_part) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("out_key_part is null.", K(ret));
          } else if (out_key_part->is_always_false()) {
            out_key_part = cur;
          } else {
            if (NULL == last_gt_tail) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("last_gt_tail is null.", K(ret));
            } else {
              // link the key part to previous or_next_
              last_gt_tail->or_next_ = cur;
            }
          }
        }
        if (OB_SUCC(ret)) {
          // find the last item in gt
          for (last_gt_tail = cur;
               NULL != last_gt_tail && last_gt_tail->or_next_ != NULL;
               last_gt_tail = last_gt_tail->or_next_);
        }
      }
    }
  }
  return ret;
}

// Replace unknown value in item_next_ list,
// and intersect them.

int ObQueryRange::definite_key_part(ObKeyPart *&key_part, ObExecContext &exec_ctx, const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  if (NULL != key_part) {
    for (ObKeyPart *cur = key_part;
         OB_SUCC(ret) && NULL != cur;
         cur = cur->item_next_) {
      if (OB_FAIL(replace_unknown_value(cur, exec_ctx, dtc_params))) {
        LOG_WARN("Replace unknown value failed", K(ret));
      } else if (cur->is_always_false()) { // set key_part false
        key_part->normal_keypart_ = cur->normal_keypart_;
        key_part->key_type_ = T_NORMAL_KEY;
        // key_part = cur; cause bug -> https://aone.alibaba-inc.com/issue/9827308?spm=0.0.0.0.PlJXuW
        break;
      } else if (key_part != cur) {
        if (OB_FAIL(key_part->intersect(cur, contain_row_))) {
          LOG_WARN("Intersect key part failed", K(ret));
        } else if (key_part->is_always_false()) {
          break;
        }
      } else {
        // do nothing
      }
    }
    key_part->item_next_ = NULL;
  }
  return ret;
}

int ObQueryRange::union_single_equal_cond(ObKeyPartList &or_list,
                                          ObExecContext *exec_ctx,
                                          const ObDataTypeCastParams &dtc_params,
                                          ObKeyPart *cur1,
                                          ObKeyPart *cur2)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(cur1) || OB_ISNULL(cur2)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("cur1 or cur2 is null", K(ret), K(cur1), K(cur2));
  } else if (cur1->and_next_ && cur2->and_next_) {
    ObKeyPart *next_key_part = NULL;
    ObKeyPartList next_or_list;
    if (!next_or_list.add_last(cur1->and_next_) || !next_or_list.add_last(cur2->and_next_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Do merge Or graph failed", K(ret));
    } else if (OB_FAIL(or_range_graph(next_or_list, exec_ctx, next_key_part, dtc_params))) {
      LOG_WARN("Do merge Or graph failed", K(ret));
    } else {
      cur1->and_next_ = next_key_part;
    }
  } else {
    cur1->and_next_ = NULL;
    if (query_range_ctx_ != NULL && query_range_ctx_->cur_expr_is_precise_) {
      query_range_ctx_->cur_expr_is_precise_ = (NULL == cur1->and_next_ && NULL == cur2->and_next_);
    }
    if (OB_FAIL(remove_precise_range_expr(cur1->pos_.offset_ + 1))) {
      LOG_WARN("remove precise range expr failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    or_list.remove(cur2);
  }
  return ret;
}

// do two path and operation
int ObQueryRange::or_single_head_graphs(ObKeyPartList &or_list,
                                        ObExecContext *exec_ctx,
                                        const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (or_list.get_size() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("And array can not be empty", K(ret));
  } else {
    // 1. replace unknown value, and refresh the graph
    if (NEED_PREPARE_PARAMS == state_) {
      ObKeyPart *find_true = NULL;
      ObKeyPart *cur = or_list.get_first();
      if (OB_ISNULL(exec_ctx)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("exec_ctx is needed to extract question mark");
      } else if (OB_ISNULL(cur)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("cur is null.", K(ret));
      } else {}
      bool part_is_true = false;
      while (!part_is_true && OB_SUCC(ret) && cur != or_list.get_header()) {
        ObKeyPart *old_tmp = cur;
        ObKeyPart *new_tmp = cur;
        ObKeyPart *and_next = cur->and_next_;
        cur = cur->get_next();
        // replace undefinited value
        if (OB_FAIL(definite_key_part(new_tmp, *exec_ctx, dtc_params))) {
          LOG_WARN("Fill unknown value failed", K(ret));
        } else if (new_tmp != old_tmp) {
          old_tmp->replace_by(new_tmp);
        } else {
          // do nothing
        }
        if (OB_FAIL(ret)) {
          // do nothing
        } else if (OB_ISNULL(new_tmp) || OB_UNLIKELY(!new_tmp->is_normal_key())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("new_tmp is null.", K(new_tmp));
        } else {
        // check result keypart
          if (new_tmp->is_always_true()) {
            if (!find_true) {
              find_true = new_tmp;
              new_tmp->and_next_ = NULL;
            }
            or_list.remove(new_tmp);
            part_is_true = true;
          } else if (new_tmp->is_always_false()) {
            new_tmp->and_next_ = NULL;
            if (or_list.get_size() > 1) {
              or_list.remove(new_tmp);
            }
          } else {
            // handle the rest of the graph recursively
            if (NULL != and_next) {
              // recursively process following and key part
              ObKeyPartList sub_or_list;
              if (OB_FAIL(split_or(and_next, sub_or_list))) {
                LOG_WARN("Split OR failed", K(ret));
              } else if (OB_FAIL(or_range_graph(sub_or_list, exec_ctx, new_tmp->and_next_, dtc_params))) {
                LOG_WARN("Do OR of range graphs failed", K(ret));
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && NULL != find_true) {
        or_list.clear();
        if (!or_list.add_last(find_true)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Add true keypart graph failed", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && or_list.get_size() > 1) { // 2. do OR of the heads of range graphs
      ObKeyPart *cur1 = or_list.get_first();
      while (OB_SUCC(ret) && cur1 != or_list.get_last() && cur1 != or_list.get_header()) {
        bool has_union = false;
        // Union key part who have intersection from next
        ObKeyPart *cur2 = NULL;
        if (NULL == cur1 || NULL == cur1->get_next()) {//yeti2
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("keypart is null.", K(ret));
        } else {
          cur2 = cur1->get_next();
        }
        while (OB_SUCC(ret) && cur2 != or_list.get_header()) {
          ObKeyPart *cur2_next = cur2->get_next();
          if (cur1->normal_key_is_equal(cur2)) {
            if (OB_FAIL(union_single_equal_cond(or_list, exec_ctx, dtc_params, cur1, cur2))) {
              LOG_WARN("union single equal cond failed", K(ret));
            }
          } else if (cur1->can_union(cur2)) {
            has_union = true;
            int cmp = cur2->normal_keypart_->start_.compare(cur1->normal_keypart_->start_);
            if (cmp < 0) {
              cur1->normal_keypart_->start_ = cur2->normal_keypart_->start_;
              cur1->normal_keypart_->include_start_ = cur2->normal_keypart_->include_start_;
            } else if (0 == cmp) {
              cur1->normal_keypart_->include_start_ =
                  (cur1->normal_keypart_->include_start_ || cur2->normal_keypart_->include_start_);
            }
            cmp = cur2->normal_keypart_->end_.compare(cur1->normal_keypart_->end_);
            if (cmp > 0) {
              cur1->normal_keypart_->end_ = cur2->normal_keypart_->end_;
              cur1->normal_keypart_->include_end_ = cur2->normal_keypart_->include_end_;
            } else if (0 == cmp) {
              cur1->normal_keypart_->include_end_ =
                  (cur1->normal_keypart_->include_end_ || cur2->normal_keypart_->include_end_);
            }
            if (cur1->and_next_ && cur1->and_next_->equal_to(cur2->and_next_)) {
              // keep and_next_
            } else {
              cur1->and_next_ = NULL;
              if (query_range_ctx_ != NULL && query_range_ctx_->cur_expr_is_precise_) {
                query_range_ctx_->cur_expr_is_precise_ = (NULL == cur1->and_next_ && NULL == cur2->and_next_);
              }
              if (OB_FAIL(remove_precise_range_expr(cur1->pos_.offset_ + 1))) {
                LOG_WARN("remove precise range expr failed", K(ret));
              }
            }
            or_list.remove(cur2);
          }
          if (OB_SUCC(ret)) {
            cur2 = cur2_next;
          }
        }
        if (OB_SUCC(ret)) {
          if (!has_union) {
            cur1 = cur1->get_next();
          }
        }
      }
    }
  }
  return ret;
}

int ObQueryRange::definite_in_range_graph(ObExecContext &exec_ctx,
                                          ObKeyPart *&root,
                                          bool &has_scan_key,
                                          const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(THIS_WORKER.check_status())) {
    LOG_WARN("check status fail", K(ret));
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (OB_ISNULL(root)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("root key is null", K(root));
  } else if (OB_FAIL(definite_key_part(root, exec_ctx, dtc_params))) {
    LOG_WARN("definite key part failed", K(ret));
  } else if (OB_UNLIKELY(!root->is_normal_key())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("root key is invalid", K(root));
  } else {
    //如果graph中某个节点不是严格的等值条件，那么这个节点是一个scan key，需要做or合并
    //如果有恒false条件，也需要走到or去做去除处理
    if (!root->is_equal_condition()) {
      has_scan_key = true;
    }
    if (NULL != root->and_next_ && (NULL == root->or_next_ ||
                                    root->or_next_->and_next_ != root->and_next_)) {
      if (OB_FAIL(SMART_CALL(definite_in_range_graph(exec_ctx, root->and_next_,
                                                     has_scan_key, dtc_params)))) {
        LOG_WARN("definite and_next_ key part failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && NULL != root->or_next_) {
      if (OB_FAIL(SMART_CALL(definite_in_range_graph(exec_ctx, root->or_next_,
                                                     has_scan_key, dtc_params)))) {
        LOG_WARN("definit or_next_ key part failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::or_range_graph(ObKeyPartList &ranges,
                                 ObExecContext *exec_ctx,
                                 ObKeyPart *&out_key_part,
                                 const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (ranges.get_size() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("OR array can not be empty", K(ranges.get_size()), K_(query_range_ctx));
  } else {

    bool need_do_or = true;
    ObKeyPart *find_false = NULL;
    ObKeyPart *find_true = NULL;
    ObKeyPartList or_list;
    ObKeyPart *head_key_part = ranges.get_first();
    if (OB_ISNULL(head_key_part)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("head_key_part is null.", K(ret));
    } else {
      int64_t offset = head_key_part->pos_.offset_;
      ObKeyPart *cur = NULL;
      while (OB_SUCC(ret) && ranges.get_size() > 0) {
        cur = ranges.get_first();
        ranges.remove_first();
        if (NULL == cur) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("cur is null.", K(ret));
        } else if (cur->is_always_false() && NULL == cur->or_next_) {
          if (!find_false) {
            cur->and_next_ = NULL;
            find_false = cur;
          }
        } else if (cur->is_always_true() && NULL == cur->and_next_) {
          if (!find_true) {
            cur->or_next_ = NULL;
            find_true = cur;
          }
          need_do_or = false;
          break;
        } else if (offset != cur->pos_.offset_) {
          // E.g. (k1>0 and k2>0) or (k2<5) ==> (min, max)
          if (NULL == find_true) {
            if (OB_FAIL(alloc_full_key_part(find_true))) {
              LOG_WARN("Get full key part failed", K(ret));
            } else if (OB_ISNULL(find_true)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
              LOG_ERROR("find_true is null.", K(ret));
            } else {
              // just to remember the index id and column number
              find_true->id_ = offset < cur->pos_.offset_ ? head_key_part->id_ : cur->id_;
              find_true->pos_ = offset < cur->pos_.offset_ ? head_key_part->pos_ : cur->pos_;
              if (query_range_ctx_ != NULL) {
                query_range_ctx_->cur_expr_is_precise_ = false; //在or关系中这种情况被放大了
                //当前key以及后面的表达式范围都没有意义，所以remove对应的表达式
                if (OB_FAIL(remove_precise_range_expr(find_true->pos_.offset_))) {
                  LOG_WARN("remove precise range expr failed", K(ret));
                }
              }
            }
          }
          need_do_or = false;
          break;
        } else if (OB_FAIL(split_or(cur, or_list))) {
          LOG_WARN("Split OR graph failed", K(ret));
        } else {
          // do nothing
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (find_true) {
        out_key_part = find_true;
      } else if (or_list.get_size() <= 0) {
        // all false
        if (OB_ISNULL(find_false)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("find_false is null.", K(ret));
        } else {
          out_key_part = find_false;
        }
      } else if (!need_do_or) {
        if (OB_ISNULL(find_true)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("find_true is null.", K(ret));
        } else {
          out_key_part = find_true;
        }
      } else if (OB_FAIL(SMART_CALL(or_single_head_graphs(or_list, exec_ctx, dtc_params)))) {
        LOG_WARN("Or single head graphs failed", K(ret));
      } else if (OB_FAIL(link_or_graphs(or_list, out_key_part))) {
        LOG_WARN("Or single head graphs failed", K(ret));
      } else {
        // do nothing
      }
    }
  }
  return ret;
}

int ObQueryRange::get_param_value(ObObj &val, const ParamsIArray &params) const
{
  int ret = OB_SUCCESS;
  int64_t param_idx = OB_INVALID_ID;

  if (val.is_unknown()) {
    if (OB_FAIL(val.get_unknown(param_idx))) {
      LOG_WARN("get question mark value failed", K(ret), K(val));
    } else if (param_idx < 0 || param_idx >= params.count()) {
      // do nothing for exec param
    } else {
      val = params.at(param_idx);
      if (val.is_nop_value()) {
        ret = OB_ERR_NOP_VALUE;
      }
    }
  }
  return ret;
}

int ObQueryRange::get_result_value(ObObj &val, ObExecContext &exec_ctx, ObIAllocator *allocator) const
{
  int ret = OB_SUCCESS;
  if (val.is_unknown()) {
    int64_t expr_idx = OB_INVALID_ID;
    ObPhysicalPlanCtx *phy_ctx = NULL;
    ObTempExpr *temp_expr = NULL;
    ObNewRow tmp_row;
    ObObj result;
    ObIAllocator &res_allocator = allocator != NULL ? *allocator : allocator_;
    if (OB_ISNULL(phy_ctx = exec_ctx.get_physical_plan_ctx())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null", K(ret));
    } else if (OB_FAIL(val.get_unknown(expr_idx))) {
      LOG_WARN("failed to get question mark value", K(ret), K(val));
    } else if (expr_idx < 0 || expr_idx >= expr_final_infos_.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid expr idx", K(expr_idx), K(ret));
    } else if (expr_final_infos_.at(expr_idx).cnt_exec_param_ &&
               phy_ctx->get_param_store().count() <= phy_ctx->get_original_param_cnt()) {
      // do nothing for exec param
    } else if (expr_final_infos_.at(expr_idx).is_param_) {
      int64_t param_idx = expr_final_infos_.at(expr_idx).param_idx_;
      if (OB_UNLIKELY(param_idx < 0 || param_idx >= phy_ctx->get_param_store().count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid param idx", K(param_idx), K(ret));
      } else {
        val = phy_ctx->get_param_store().at(param_idx);
        if (val.is_nop_value()) {
          ret = OB_ERR_NOP_VALUE;
        }
      }
    } else if (OB_ISNULL(temp_expr = expr_final_infos_.at(expr_idx).temp_expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null temp expr", K(expr_idx), K(ret));
    } else if (OB_FAIL(temp_expr->eval(exec_ctx, tmp_row, result))) {
      LOG_WARN("failed to eval temp expr", K(ret));
    } else if (result.is_nop_value()) {
      ret = OB_ERR_NOP_VALUE;
    } else if (OB_FAIL(ob_write_obj(res_allocator, result, val))) {
      LOG_WARN("failed to write obj", K(result), K(ret));
    }
  }
  return ret;
}

int ObQueryRange::get_result_value_with_rowid(const ObKeyPart &key_part,
                                              ObObj &val,
                                              ObExecContext &exec_ctx,
                                              bool &is_inconsistent_rowid,
                                              ObIAllocator *allocator /* =NULL */ ) const
{
  int ret = OB_SUCCESS;
  int64_t param_idx = OB_INVALID_ID;
  is_inconsistent_rowid = false;

  if (OB_FAIL(get_result_value(val, exec_ctx, allocator))) {
    LOG_WARN("get param value failed", K(ret));
  } else if (!val.is_unknown() && key_part.is_rowid_key_part()) {
    if (val.is_urowid()) {
      uint64_t pk_cnt;
      ObArray<ObObj> pk_vals;
      const ObURowIDData &urowid_data = val.get_urowid();
      if ((urowid_data.is_physical_rowid() && key_part.is_logical_rowid_key_part()) ||
          (!urowid_data.is_physical_rowid() && key_part.is_phy_rowid_key_part())) {
        is_inconsistent_rowid = true;
        val.set_null();
        LOG_TRACE("Occur inconsistent rowid", K(urowid_data), K(key_part.is_rowid_key_part()));
      } else if (urowid_data.is_physical_rowid()) {//not convert, will convert in table scan.
        //do nothing
      } else if (OB_FAIL(urowid_data.get_pk_vals(pk_vals))) {
        LOG_WARN("failed to get pk values", K(ret));
      } else if ((pk_cnt = urowid_data.get_real_pk_count(pk_vals)) <= key_part.rowid_column_idx_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid key_part", K(ret), K(pk_cnt), K(key_part.rowid_column_idx_));
      } else {
        val = pk_vals.at(key_part.rowid_column_idx_);
      }
    }
  }
  return ret;
}

OB_INLINE int ObQueryRange::gen_simple_get_range(const ObKeyPart &root,
                                                 ObIAllocator &allocator,
                                                 ObExecContext &exec_ctx,
                                                 ObQueryRangeArray &ranges,
                                                 ObGetMethodArray &get_methods,
                                                 const ObDataTypeCastParams &dtc_params) const
{
  int ret = OB_SUCCESS;
  ObObj *start = nullptr;
  ObObj *end = nullptr;
  ObNewRange *range = nullptr;
  bool always_false = false;
  bool always_true = false;
  size_t rowkey_size = sizeof(ObObj) * column_count_ * 2;
  size_t range_size = sizeof(ObNewRange) + rowkey_size;
  void *range_buffer = nullptr;
  bool contain_phy_rowid_key = false;
  if (OB_ISNULL(range_buffer = allocator.alloc(range_size))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory for ObNewRange failed", K(ret));
  } else {
    range = new(range_buffer) ObNewRange();
    start = reinterpret_cast<ObObj*>(static_cast<char*>(range_buffer) + sizeof(ObNewRange));
    end = start + column_count_;
    //init obj
    for (int64_t i = 0; i < column_count_; ++i) {
      (start + i)->set_min_value();
      (end + i)->set_max_value();
    }
  }
  const ObKeyPart *cur = &root;
  bool b_flag = false;
  for (int64_t i = 0; OB_SUCC(ret) && NULL != cur && !b_flag && i < column_count_; ++i) {
    contain_phy_rowid_key = cur->is_phy_rowid_key_part();
    if (OB_UNLIKELY(cur->normal_keypart_->always_false_)) {
      always_false = true;
    } else {
      ObObj *cur_val = start + i;
      new (cur_val) ObObj(cur->normal_keypart_->start_);
      bool is_inconsistent_rowid = false;
      if (OB_FAIL(get_result_value_with_rowid(*cur, *cur_val, exec_ctx, is_inconsistent_rowid, &allocator))) {
        LOG_WARN("get end param value failed", K(ret));
      } else if (is_inconsistent_rowid) {
        always_false = true;
      } else if (OB_UNLIKELY(cur_val->is_unknown())) {
        //下推的？
        always_true = true;
      } else if (OB_LIKELY(ObSQLUtils::is_same_type_for_compare(cur_val->get_meta(),
                                                      cur->pos_.column_type_.get_obj_meta()))) {
        cur_val->set_collation_type(cur->pos_.column_type_.get_collation_type());
        //copy end
        new(end + i) ObObj(*cur_val);
      } else if (OB_LIKELY(cur_val->get_meta().get_type() == ObURowIDType)) {
        new(end + i) ObObj(*cur_val);
      } else if (OB_LIKELY(!cur_val->is_overflow_integer(cur->pos_.column_type_.get_type()))) {
        //fast cast with integer value
        cur_val->set_meta_type(cur->pos_.column_type_);
        new(end + i) ObObj(*cur_val);
      } else if (OB_FAIL(cold_cast_cur_node(cur, allocator, dtc_params, *cur_val, always_false))) {
        LOG_WARN("cold fill cur node failed", K(ret));
      } else if (OB_LIKELY(!always_false)) {
        new(end + i) ObObj(*cur_val);
      }
    }
    if (OB_SUCC(ret) && always_false) {
      //set whole range max to min
      for (int64_t j = 0; j < column_count_; ++j) {
        (start + j)->set_max_value();
        (end + j)->set_min_value();
      }
      b_flag = true;
    }
    if (OB_SUCC(ret) && always_true) {
      //set whole range max to min
      for (int64_t j = 0; j < column_count_; ++j) {
        (start + j)->set_min_value();
        (end + j)->set_max_value();
      }
      b_flag = true;
    }

    if (OB_SUCC(ret) && !b_flag) {
      cur = cur->and_next_;
    }
  }
  if (OB_SUCC(ret)) {
    range->table_id_ = root.id_.table_id_;
    range->start_key_.assign(start, column_count_);
    range->end_key_.assign(end, column_count_);
    if (!always_false && !always_true) {
      range->border_flag_.set_inclusive_start();
      range->border_flag_.set_inclusive_end();
    } else {
      range->border_flag_.unset_inclusive_start();
      range->border_flag_.unset_inclusive_end();
    }
    range->is_physical_rowid_range_ = contain_phy_rowid_key;
    if (OB_FAIL(ranges.push_back(range))) {
      LOG_WARN("push back range to array failed", K(ret));
    } else if (OB_FAIL(get_methods.push_back(!always_false))) {
      LOG_WARN("push back get method failed", K(ret));
    }
  }
  return ret;
}

OB_NOINLINE int ObQueryRange::cold_cast_cur_node(const ObKeyPart *cur,
                                                 ObIAllocator &allocator,
                                                 const ObDataTypeCastParams &dtc_params,
                                                 ObObj &cur_val,
                                                 bool &always_false) const
{
  int ret = OB_SUCCESS;
  const ObObj *dest_val = NULL;
  if (OB_UNLIKELY(cur_val.is_null()) && !cur->is_rowid_key_part() && !cur->null_safe_) {
    always_false = true;
  } else if (!cur_val.is_min_value() && !cur_val.is_max_value()) {
    ObCastCtx cast_ctx(&allocator,
                       &dtc_params,
                       CM_WARN_ON_FAIL,
                       cur->pos_.column_type_.get_collation_type());
    ObExpectType expect_type;
    expect_type.set_type(cur->pos_.column_type_.get_type());
    expect_type.set_collation_type(cur->pos_.column_type_.get_collation_type());
    expect_type.set_type_infos(&cur->pos_.get_enum_set_values());
    EXPR_CAST_OBJ_V2(expect_type, cur_val, dest_val);
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_ISNULL(dest_val)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cast failed.", K(ret));
    } else { // 下面这个比较是目的是检查上面的cast有没有丢失数值的精度
      int64_t cmp = 0;
      ObObjType cmp_type = ObMaxType;
      if (OB_FAIL(ObExprResultTypeUtil::get_relational_cmp_type(cmp_type,
                                                                cur_val.get_type(),
                                                                dest_val->get_type()))) {
        LOG_WARN("get compare type failed", K(ret));
      } else if (OB_FAIL(ObRelationalExprOperator::compare_nullsafe(cmp, cur_val, *dest_val,
                                                                    cast_ctx, cmp_type,
                                                                    cur->pos_.column_type_.get_collation_type()))) {
        LOG_WARN("compare obj value failed", K(ret));
      } else if (0 == cmp) {
        cur_val = *dest_val;
        cur_val.set_collation_type(cur->pos_.column_type_.get_collation_type());
      } else {
        //always false
        always_false = true;
      }
    }
  }
  return ret;
}

//generate always true range or always false range.
int ObQueryRange::generate_true_or_false_range(const ObKeyPart *cur,
                                               ObIAllocator &allocator,
                                               ObNewRange *&range) const
{
  int ret = OB_SUCCESS;
  ObObj *start = NULL;
  ObObj *end = NULL;
  if (OB_ISNULL(cur)) {
    ret = OB_NOT_INIT;
    LOG_WARN("cur is null", K(ret));
  } else if (OB_ISNULL(start = static_cast<ObObj *>(allocator.alloc(sizeof(ObObj) * column_count_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for start_obj failed", K(ret));
  } else if(OB_ISNULL(end = static_cast<ObObj *>(allocator.alloc(sizeof(ObObj) * column_count_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for end_obj failed", K(ret));
  } else {
    new(start) ObObj();
    new(end) ObObj();
    if (cur->is_always_false()) {
      start[0].set_max_value();
      end[0].set_min_value();
    } else { //  always true or whole range
      start[0].set_min_value();
      end[0].set_max_value();
    }
    for (int i = 1 ; i < column_count_ ; i++) {
      new(start + i) ObObj();
      new(end + i) ObObj();
      start[i] = start[0];
      end[i] = end[0];
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(range =
        static_cast<ObNewRange *>(allocator.alloc(sizeof(ObNewRange))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc memory failed", K(ret));
    } else {
      new(range) ObNewRange();
      range->table_id_ = cur->id_.table_id_;
      range->border_flag_.unset_inclusive_start();
      range->border_flag_.unset_inclusive_end();
      range->start_key_.assign(start, column_count_);
      range->end_key_.assign(end, column_count_);
    }
  }
  return ret;
}

// copy existing key parts to range, and fill in the missing key part
int ObQueryRange::generate_single_range(ObSearchState &search_state,
                                        int64_t column_num,
                                        uint64_t table_id,
                                        ObNewRange *&range,
                                        bool &is_get_range) const
{
  int ret = OB_SUCCESS;
  ObObj *start = NULL;
  ObObj *end = NULL;
  if (column_num <= 0 || search_state.max_exist_index_ < 0
      || !search_state.start_ || !search_state.end_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Wrong argument", K(ret));
  } else if (OB_ISNULL(start = static_cast<ObObj *>(search_state.allocator_.alloc(sizeof(ObObj) * column_num)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for start_obj failed", K(ret));
  } else if(OB_ISNULL(end = static_cast<ObObj *>(search_state.allocator_.alloc(sizeof(ObObj) * column_num)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for end_obj failed", K(ret));
  } else {
    int64_t max_pred_index = search_state.max_exist_index_;
    column_num = search_state.is_phy_rowid_range_ ? 1 : column_num;//physcial rowid range just use only one column
    for (int i = 0; OB_SUCC(ret) && i < column_num; i++) {
      new(start + i) ObObj();
      new(end + i) ObObj();
      if (i < max_pred_index) {
        // exist key part, deep copy it
        if (OB_FAIL(ob_write_obj(search_state.allocator_, *(search_state.start_ + i), *(start + i)))) {
          LOG_WARN("deep copy start obj failed", K(ret), K(i));
        } else if (OB_FAIL(ob_write_obj(search_state.allocator_, *(search_state.end_ + i), *(end + i)))) {
          LOG_WARN("deep copy end obj failed", K(ret), K(i));
        }
      } else {
        // fill in the missing key part as (min, max)

        // If the start key is included in the range, then we should set
        // min to the rest of the index keys; otherwise, we should set
        // max to the rest of the index keys in order to skip any values
        // in (.. start_key, min:max, min:max ).
        //
        // For example:
        // sql> create table t1(c1 int, c2 int, c3 int, primary key(c1, c2, c3));
        // sql> select * from t1 where c1 > 1 and c1 < 7;
        //
        // The range we get should be (1, max, max) to (7, min, min)
        // and if the condition become c1 >= 1 and c1<= 7, we should get
        // (1, min, min) to (7, max, max) instead.
        //
        // This should be done always with the exception of start_key being
        // min or max, in which case, we should set min to the rest all the
        // time.
        // Same logic applies to the end key as well.
        if (max_pred_index > 0
            && !(search_state.start_[max_pred_index - 1]).is_min_value()
            && search_state.last_include_start_ == false) {
          start[i].set_max_value();
        } else {
          start[i].set_min_value();
        }

        // See above
        if (max_pred_index > 0
            && !(search_state.end_[max_pred_index - 1]).is_max_value()
            && search_state.last_include_end_ == false) {
          end[i].set_min_value();
        } else {
          end[i].set_max_value();
        }
      }
    }
    if (OB_SUCC(ret)) {
      range = static_cast<ObNewRange *>(search_state.allocator_.alloc(sizeof(ObNewRange)));
      if (NULL == range) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc memory failed", K(ret));
      } else {
        new(range) ObNewRange();
        range->table_id_ = table_id;
        range->is_physical_rowid_range_ = search_state.is_phy_rowid_range_;
      }
    }
    if (OB_SUCC(ret)) {
      if (search_state.max_exist_index_ == column_num && search_state.last_include_start_) {
        range->border_flag_.set_inclusive_start();
      } else {
        range->border_flag_.unset_inclusive_start();
      }
      if (search_state.max_exist_index_ == column_num && search_state.last_include_end_) {
        range->border_flag_.set_inclusive_end();
      } else {
        range->border_flag_.unset_inclusive_end();
      }
      range->start_key_.assign(start, column_num);
      range->end_key_.assign(end, column_num);
      is_get_range = (range->start_key_ == range->end_key_)
                     && range->border_flag_.inclusive_start()
                     && range->border_flag_.inclusive_end();
    }
  }
  return ret;
}

int ObQueryRange::store_range(ObNewRange *range,
                              bool is_get_range,
                              ObSearchState &search_state,
                              ObQueryRangeArray &ranges,
                              ObGetMethodArray &get_methods)
{
  int ret = OB_SUCCESS;
  bool is_duplicate = false;
  if (search_state.is_equal_range_) {
    ObRangeWrapper range_wrapper;
    range_wrapper.range_ = range;
    if (OB_HASH_NOT_EXIST == (ret = search_state.range_set_.exist_refactored(range_wrapper))) {
      is_duplicate = false;
      if (OB_FAIL(search_state.range_set_.set_refactored(range_wrapper))) {
        LOG_WARN("set range to range set failed", K(ret));
      } else {
        ret = OB_SUCCESS;
      }
    } else if (OB_HASH_EXIST == ret) {
      is_duplicate = true;
      ret = OB_SUCCESS;
    }
  } else {
    //对于非in表达式，前面已经做过or运算，不会存在重复的range，不需要去重
    is_duplicate = false;
  }
  if (OB_SUCC(ret) && !is_duplicate) {
    if (OB_FAIL(ranges.push_back(range))) {
      LOG_WARN("push back range failed", K(ret));
    } else if (OB_FAIL(get_methods.push_back(is_get_range))) {
      LOG_WARN("push back get_method failed", K(ret));
    }
  }
  return ret;
}

int ObQueryRange::and_first_search(ObSearchState &search_state,
                                   ObKeyPart *cur,
                                   ObQueryRangeArray &ranges,
                                   ObGetMethodArray &get_methods,
                                   const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_UNLIKELY(THIS_WORKER.check_status())) {
    LOG_WARN("check status fail", K(ret));
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (OB_ISNULL(search_state.start_) || OB_ISNULL(search_state.end_)
      || OB_ISNULL(search_state.include_start_) || OB_ISNULL(search_state.include_end_)
      || OB_ISNULL(cur) || OB_UNLIKELY(!cur->is_normal_key())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K_(search_state.start), K_(search_state.end), K(cur));
  } else {
    int copy_depth = search_state.depth_;
    bool copy_produce_range = search_state.produce_range_;

    // 1. generate current key part range, fill missing part as (min, max)
    int i = search_state.depth_;
    if (!cur->is_always_true() && !cur->is_always_false()) {
      for (; cur->pos_.offset_ != -1 /* default */ && i < cur->pos_.offset_; i++) {
        if (lib::is_oracle_mode()) {
          // Oracle 存储层使用 NULL Last
          search_state.start_[i].set_min_value();
          search_state.end_[i].set_null();
        } else {
          search_state.start_[i].set_null();
          search_state.end_[i].set_max_value();
        }
        search_state.include_start_[i] = false;
        search_state.include_end_[i] = false;
      }
    }
    if (search_state.max_exist_index_ >= search_state.depth_ + 1) {
      // get the larger scope
      if (search_state.start_[i] > cur->normal_keypart_->start_) {
        search_state.start_[i] = cur->normal_keypart_->start_;
        search_state.include_start_[i] = cur->normal_keypart_->include_start_;
      }
      if (search_state.end_[i] < cur->normal_keypart_->end_) {
        search_state.end_[i] = cur->normal_keypart_->end_;
        search_state.include_end_[i] = cur->normal_keypart_->include_end_;
      }
    } else {
      search_state.start_[i] = cur->normal_keypart_->start_;
      search_state.end_[i] = cur->normal_keypart_->end_;
      search_state.max_exist_index_ = search_state.depth_ + 1;
      search_state.include_start_[i] = cur->normal_keypart_->include_start_;
      search_state.include_end_[i] = cur->normal_keypart_->include_end_;
    }

    // 2. process next and key part
    if (NULL != cur->and_next_ && cur->pos_.offset_ >= 0
        && cur->pos_.offset_ + 1 == cur->and_next_->pos_.offset_) {
      // current key part is not equal value
      // include_start_/include_end_ is ignored
      if (cur->normal_keypart_->start_ != cur->normal_keypart_->end_) {
        search_state.produce_range_ = false;
      }
      search_state.depth_++;
      if (OB_FAIL(SMART_CALL(and_first_search(search_state,
                                              cur->and_next_,
                                              ranges,
                                              get_methods,
                                              dtc_params)))) {
      } else {
        search_state.depth_ = copy_depth;
      }
    }

    // 3. to check if need to  output
    //copy_produec_range的作用是控制range能不能够输出，不是所有递归到最后都能输出
    //例如：a>1 and a<=2 and ((b>1 and b < 2) or (b > 4, and b < 5))
    //这个例子不能抽成两段range，只能抽成一段range
    //因为如果抽成两段range->(1, max;2, 2) or (1, max;2, 5)这两段区间是有重叠的
    //如果前缀是一个范围的时候，后缀的范围只能用来确定边界值，所以应该只记录后缀所有区间的总的起始边界和结束边界
    //这个range应该是(1, max; 2, 5)
    if (OB_SUCC(ret)) {
      // several case need to output:
      // that previous key parts are all equal value is necessary,
      // 1. current key part is not equal value;
      // 2. current key part is equal value and and_next_ is NULL,
      // 3. current key part is equal value and and_next_ is not NULL, but consequent key does not exist.
      if (copy_produce_range
          && (NULL == cur->and_next_
              || cur->normal_keypart_->start_ != cur->normal_keypart_->end_
              || cur->pos_.offset_ + 1 != cur->and_next_->pos_.offset_)) {
        ObNewRange *range = NULL;
        bool is_get_range = false;
        search_state.last_include_start_ = true;
        search_state.last_include_end_ = true;
        search_state.is_phy_rowid_range_ = cur->is_phy_rowid_key_part();
        if (OB_FAIL(search_state.tailor_final_range(column_count_))) {
          LOG_WARN("tailor final range failed", K(ret));
        } else if (OB_FAIL(generate_single_range(search_state, column_count_, cur->id_.table_id_, range, is_get_range))) {
          LOG_WARN("Get single range failed", K(ret));
        } else if (OB_FAIL(store_range(range, is_get_range, search_state, ranges, get_methods))) {
          LOG_WARN("store range failed", K(ret));
        } else {
          // reset search_state
          search_state.depth_ = copy_depth;
          search_state.max_exist_index_ = 0;
          search_state.produce_range_ = copy_produce_range;
          search_state.is_phy_rowid_range_ = false;
        }
      }
    }

    // 4. has or item
    if (OB_SUCC(ret) && NULL != cur->or_next_) {
      cur = cur->or_next_;
      if (OB_FAIL(SMART_CALL(and_first_search(search_state, cur, ranges, get_methods, dtc_params)))) {
        LOG_WARN("and_first_search failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::get_tablet_ranges(common::ObIAllocator &allocator,
                                    ObExecContext &exec_ctx,
                                    ObQueryRangeArray &ranges,
                                    bool &all_single_value_ranges,
                                    const ObDataTypeCastParams &dtc_params) const
{
  int ret = OB_SUCCESS;
  ObGetMethodArray get_methods;
  if (OB_LIKELY(!need_deep_copy())) {
    if (OB_FAIL(get_tablet_ranges(allocator, exec_ctx, ranges, get_methods, dtc_params))) {
      LOG_WARN("get tablet ranges without deep copy failed", K(ret));
    }
  } else {
    //need to deep copy
    ObQueryRange tmp_query_range(allocator);
    if (OB_FAIL(tmp_query_range.deep_copy(*this, true))) {
      LOG_WARN("deep copy query range failed", K(ret));
    } else if (OB_FAIL(tmp_query_range.final_extract_query_range(exec_ctx, dtc_params))) {
      LOG_WARN("final extract query range failed", K(ret));
    } else if (OB_FAIL(tmp_query_range.get_tablet_ranges(ranges, get_methods, dtc_params))) {
      LOG_WARN("get tablet range with deep copy failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    int64_t N = get_methods.count();
    all_single_value_ranges = true;
    for (int64_t i = 0; all_single_value_ranges && i < N; ++i) {
      if (!get_methods.at(i)) {
        all_single_value_ranges = false;
      }
    }
  }
  return ret;
}

int ObQueryRange::ObSearchState::tailor_final_range(int64_t column_count)
{
  int ret = OB_SUCCESS;
  bool skip_start = false;
  bool skip_end = false;
  if (OB_ISNULL(include_start_) || OB_ISNULL(include_end_) || OB_ISNULL(start_) || OB_ISNULL(end_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("search state is not init", K_(include_start), K_(include_end), K_(start), K_(end));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < max_exist_index_ && !is_empty_range_; ++i) {
    //确定最后的边界并裁剪掉一些无用的range节点
    if (!skip_start) {
      last_include_start_ = (last_include_start_ && include_start_[i]);
      if (!start_[i].is_min_value() && !include_start_[i]) {
        //左边界是有效值并且是开区间，表示这个有效值永远取不到，那么后缀的值也没有意义，取MAX
        for (int64_t j = i + 1; OB_SUCC(ret) && j < column_count; ++j) {
          start_[j].set_max_value();
        }
        last_include_start_ = false;
        skip_start = true;
      }
    }
    if (!skip_end) {
      last_include_end_ = (last_include_end_ && include_end_[i]);
      if (!end_[i].is_max_value() && !include_end_[i]) {
        //右边界是有效值并且是开区间，表示这个有效值永远取不到，所以后缀的值没有意义，取MIN
        for (int64_t j = i + 1; OB_SUCC(ret) && j < column_count; ++j) {
          end_[j].set_min_value();
        }
        last_include_end_ = false;
        skip_end = true;
      }
    }
    if (start_[i].is_min_value() && end_[i].is_max_value()) {
      //当前节点是(MIN, MAX)，后缀没有实际意义裁剪掉
      last_include_start_ = false;
      last_include_end_ = false;
      max_exist_index_ = i + 1;
      break;
    }
  }
  return ret;
}

// @notice 调用这个接口之前必须调用need_deep_copy()来判断是否可以不用拷贝就进行final extract
int ObQueryRange::get_tablet_ranges(ObIAllocator &allocator,
                                    ObExecContext &exec_ctx,
                                    ObQueryRangeArray &ranges,
                                    ObGetMethodArray &get_methods,
                                    const ObDataTypeCastParams &dtc_params) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(table_graph_.key_part_head_->pos_.offset_ < 0)) {
    ObNewRange *range = NULL;
    bool is_get_range = false;
    if (OB_FAIL(generate_true_or_false_range(table_graph_.key_part_head_, allocator, range))) {
     LOG_WARN("get true_or_false range failed", K(ret));
    } else if (OB_FAIL(ranges.push_back(range))) {
     LOG_WARN("push back range failed", K(ret));
    } else if (OB_FAIL(get_methods.push_back(is_get_range))) {
     LOG_WARN("push back get_method failed", K(ret));
    } else {}
  } else if (OB_LIKELY(table_graph_.is_precise_get_)) {
    if (OB_FAIL(gen_simple_get_range(*table_graph_.key_part_head_,
                                     allocator,
                                     exec_ctx,
                                     ranges,
                                     get_methods,
                                     dtc_params))) {
      LOG_WARN("gen simple get range failed", K(ret));
    }
  } else {
    OZ(gen_simple_scan_range(allocator, exec_ctx, ranges, get_methods, dtc_params));
  }
  return ret;
}

OB_NOINLINE int ObQueryRange::gen_simple_scan_range(ObIAllocator &allocator,
                                                    ObExecContext &exec_ctx,
                                                    ObQueryRangeArray &ranges,
                                                    ObGetMethodArray &get_methods,
                                                    const ObDataTypeCastParams &dtc_params) const
{
  int ret = OB_SUCCESS;
  ObSearchState search_state(allocator);
  void *start_ptr = NULL;
  void *end_ptr = NULL;

  if (OB_ISNULL(start_ptr = search_state.allocator_.alloc(sizeof(ObObj) * column_count_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for start_ptr failed", K(ret));
  } else if(OB_ISNULL(end_ptr = search_state.allocator_.alloc(sizeof(ObObj) * column_count_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for end_ptr failed", K(ret));
  } else if (OB_ISNULL(search_state.include_start_ = static_cast<bool*>(search_state.allocator_.alloc(sizeof(bool) * column_count_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for search state start failed", K(ret));
  } else if (OB_ISNULL(search_state.include_end_ = static_cast<bool*>(search_state.allocator_.alloc(sizeof(bool) * column_count_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc memory for search state end failed", K(ret));
  } else {
    search_state.start_ = new(start_ptr) ObObj[column_count_];
    search_state.end_ = new(end_ptr) ObObj[column_count_];
    search_state.max_exist_index_ = column_count_;
    search_state.last_include_start_ = true;
    search_state.last_include_end_ = true;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count_; ++i) {
    //将所有range都初始化成true
    search_state.start_[i].set_min_value();
    search_state.end_[i].set_max_value();
    search_state.include_start_[i] = false;
    search_state.include_end_[i] = false;
  }
  for (ObKeyPart *cur = table_graph_.key_part_head_;
       OB_SUCC(ret) && NULL != cur && !search_state.is_empty_range_;
       cur = cur->and_next_) {
    if (OB_FAIL(get_single_key_value(cur, exec_ctx, search_state, dtc_params))) {
      LOG_WARN("get single key value failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(search_state.tailor_final_range(column_count_))) {
    LOG_WARN("tailor final range failed", K(ret));
  }
  if (OB_SUCC(ret)) {
    ObNewRange *range = NULL;
    bool is_get_range = false;
    if (OB_FAIL(generate_single_range(search_state, column_count_,
                                      table_graph_.key_part_head_->id_.table_id_,
                                      range, is_get_range))) {
      LOG_WARN("generate single range failed", K(ret));
    } else if (OB_FAIL(ranges.push_back(range))) {
      LOG_WARN("push back range to array failed", K(ret));
    } else if (OB_FAIL(get_methods.push_back(is_get_range))) {
      LOG_WARN("push back get method to array failed", K(ret));
    }
  }
  return ret;
}

#define CAST_VALUE_TYPE(expect_type, column_type, start, include_start, end, include_end) \
if (OB_SUCC(ret) ) { \
  ObObj cast_obj; \
  const ObObj *dest_val = NULL; \
  if (!start.is_min_value() && !start.is_max_value() && !start.is_unknown() \
    && !ObSQLUtils::is_same_type_for_compare(start.get_meta(), column_type.get_obj_meta())) { \
    ObCastCtx cast_ctx(&allocator, &dtc_params, CM_WARN_ON_FAIL, expect_type.get_collation_type()); \
    ObObj &tmp_start = start; \
    EXPR_CAST_OBJ_V2(expect_type, tmp_start, dest_val); \
    if (OB_FAIL(ret)) { \
      LOG_WARN("cast obj to dest type failed", K(ret), K(start), K(expect_type)); \
    } else if (OB_ISNULL(dest_val)) { \
      ret = OB_ERR_UNEXPECTED; \
      LOG_WARN("dest_val is null.", K(ret)); \
    } else { /* 下面这个比较是目的是检查上面的cast有没有丢失数值的精度 */ \
      int64_t cmp = 0; \
      ObObjType cmp_type = ObMaxType; \
      if (OB_FAIL(ObExprResultTypeUtil::get_relational_cmp_type(cmp_type, start.get_type(), dest_val->get_type()))) { \
        LOG_WARN("get compare type failed", K(ret)); \
      } else if (OB_FAIL(ObRelationalExprOperator::compare_nullsafe(cmp, start, *dest_val, cast_ctx, \
                                                                    cmp_type, column_type.get_collation_type()))) { \
        LOG_WARN("compare obj value failed", K(ret)); \
      } else if (cmp < 0) { \
        /* 转换后精度发生变化，结果更大，需要将原来的开区间变为闭区间 */ \
        include_start = true; \
      } else if (cmp > 0) { \
        include_start = false; \
      } \
      start = *dest_val; \
    } \
  } \
  if (OB_SUCC(ret)) { \
    if (!end.is_min_value() && !end.is_max_value() && !end.is_unknown() \
      && !ObSQLUtils::is_same_type_for_compare(end.get_meta(), column_type.get_obj_meta())) { \
      ObCastCtx cast_ctx(&allocator, &dtc_params, CM_WARN_ON_FAIL, expect_type.get_collation_type()); \
        ObObj &tmp_end = end; \
        EXPR_CAST_OBJ_V2(expect_type, tmp_end, dest_val); \
      if (OB_FAIL(ret)) { \
        LOG_WARN("cast obj to dest type failed", K(ret), K(end), K(expect_type)); \
      } else { \
        int64_t cmp = 0; \
        ObObjType cmp_type = ObMaxType; \
        if (OB_FAIL(ObExprResultTypeUtil::get_relational_cmp_type(cmp_type, end.get_type(), dest_val->get_type()))) { \
          LOG_WARN("get compare type failed", K(ret)); \
        } else if (OB_FAIL(ObRelationalExprOperator::compare_nullsafe(cmp, end, *dest_val, cast_ctx, \
                                                                      cmp_type, column_type.get_collation_type()))) { \
          LOG_WARN("compare obj value failed", K(ret)); \
        } else if (cmp > 0) { \
          /* 转换后精度发生变化，结果变为更小，需要将原来的开区间变为闭区间 */ \
          include_end = true; \
        } else if (cmp < 0) { \
          include_end = false; \
        } \
        end = *dest_val; \
      } \
    } \
  } \
  if (OB_SUCC(ret)) { \
    start.set_collation_type(expect_type.get_collation_type()); \
    end.set_collation_type(expect_type.get_collation_type()); \
  } \
}

inline int ObQueryRange::get_single_key_value(const ObKeyPart *key,
                                              ObExecContext &exec_ctx,
                                              ObSearchState &search_state,
                                              const ObDataTypeCastParams &dtc_params) const
{
  int ret = OB_SUCCESS;
  for (const ObKeyPart *cur = key;
       OB_SUCC(ret) && NULL != cur && cur->is_normal_key() && !search_state.is_empty_range_;
       cur = cur->item_next_) {
    ObObj start = cur->normal_keypart_->start_;
    ObObj end = cur->normal_keypart_->end_;
    bool include_start = cur->normal_keypart_->include_start_;
    bool include_end = cur->normal_keypart_->include_end_;
    ObExpectType expect_type;
    expect_type.set_type(cur->pos_.column_type_.get_type());
    expect_type.set_collation_type(cur->pos_.column_type_.get_collation_type());
    expect_type.set_type_infos(&cur->pos_.get_enum_set_values());
    ObIAllocator &allocator = search_state.allocator_;
    if (cur->normal_keypart_->always_false_) {
      start.set_max_value();
      end.set_min_value();
      include_start = false;
      include_end = false;
    } else if (cur->normal_keypart_->always_true_) {
      start.set_min_value();
      end.set_max_value();
      include_start = false;
      include_end = false;
    } else {
      bool is_inconsistent_rowid = false;
      if (start.is_unknown()) {
        if (OB_FAIL(get_result_value_with_rowid(*cur, start, exec_ctx, is_inconsistent_rowid, &search_state.allocator_))) {
          LOG_WARN("get result value failed", K(ret));
        } else if (is_inconsistent_rowid) {
          if (key->is_phy_rowid_key_part()) {//phy rowid query get a logical rowid, can't parse.
            start.set_min_value();
          } else {//logical rowid query get a phy rowid, can't parse.
            start.set_max_value();
            end.set_min_value();
            include_start = false;
            include_end = false;
          }
        } else if (!cur->null_safe_ && start.is_null()) {
          start.set_max_value();
          end.set_min_value();
          include_start = false;
          include_end = false;
        } else if (start.is_unknown()) {
          //条件下推的？的range是[min, max]
          start.set_min_value();
          include_start = false;
          end.set_max_value();
          include_end = false;
        }
      }
      if (OB_SUCC(ret) && end.is_unknown()) {
        if (OB_FAIL(get_result_value_with_rowid(*cur, end, exec_ctx, is_inconsistent_rowid, &search_state.allocator_))) {
          LOG_WARN("get result value failed", K(ret));
        } else if (is_inconsistent_rowid) {
          if (key->is_phy_rowid_key_part()) {//phy rowid query get a logical rowid, can't parse.
            start.set_max_value();
            end.set_min_value();
            include_start = false;
            include_end = false;
          } else {//logical rowid query get a phy rowid, can't parse.
            end.set_max_value();
          }
        } else if (!cur->null_safe_ && end.is_null()) {
          start.set_max_value();
          end.set_min_value();
          include_start = false;
          include_end = false;
        } else if (end.is_unknown()) {
          //条件下推的？的range是[min, max]
          start.set_min_value();
          include_start = false;
          end.set_max_value();
          include_end = false;
        }
      }
    }
    //为了性能，减少函数跳转，所以用宏来代替
    if (OB_SUCC(ret) && cur->is_phy_rowid_key_part()) {
      //physical rowid range no need cast, it's will be transformed in table scan phase.
    } else {
      CAST_VALUE_TYPE(expect_type, cur->pos_.column_type_, start, include_start, end, include_end);
    }
    if (OB_SUCC(ret)) {
      search_state.depth_ = static_cast<int>(cur->pos_.offset_);
      if (search_state.is_phy_rowid_range_ != cur->is_phy_rowid_key_part()) {
        if (search_state.is_phy_rowid_range_) {
          //do nothing
        } else {//just only see the physical rowid key part.
          search_state.start_[search_state.depth_]= start;
          search_state.end_[search_state.depth_] = end;
          search_state.include_start_[search_state.depth_] = include_start;
          search_state.include_end_[search_state.depth_] = include_end;
          search_state.is_phy_rowid_range_ = true;
        }
      } else if (OB_FAIL(search_state.intersect(start, include_start, end, include_end))) {
        LOG_WARN("intersect current key part failed", K(ret));
      }
    }
  }
  return ret;
}
#undef CAST_VALUE_TYPE

OB_NOINLINE int ObQueryRange::get_tablet_ranges(ObQueryRangeArray &ranges,
                                                ObGetMethodArray &get_methods,
                                                const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  int64_t last_mem_usage = allocator_.total();
  int64_t query_range_mem_usage = 0;
  ObSearchState search_state(allocator_);

  ranges.reset();
  get_methods.reset();
  if (CAN_READ != state_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Can not get query range before final extraction", K(ret), K_(state));
  } else if (OB_ISNULL(table_graph_.key_part_head_) || OB_UNLIKELY(!table_graph_.key_part_head_->is_normal_key())) {
    ret = OB_NOT_INIT;
    LOG_WARN("table_graph_.key_part_head_ is not inited.", K(table_graph_.key_part_head_));
  } else if (table_graph_.is_equal_range_ && OB_FAIL(search_state.range_set_.create(RANGE_BUCKET_SIZE))) {
    LOG_WARN("create range set bucket failed", K(ret));
  } else if (table_graph_.key_part_head_->pos_.offset_ > 0) {
    SQL_REWRITE_LOG(DEBUG, "get table range from index failed, whole range will returned", K(ret));
    ObNewRange *range = NULL;
    bool is_get_range = false;
    if (OB_FAIL(generate_true_or_false_range(table_graph_.key_part_head_, allocator_, range))) {
      LOG_WARN("generate true_or_false range failed", K(ret));
    } else if (OB_FAIL(ranges.push_back(range))) {
      LOG_WARN("push back range failed", K(ret));
    } else if (OB_FAIL(get_methods.push_back(is_get_range))) {
      LOG_WARN("push back get_method failed", K(ret));
    } else {}
  } else {
    ret = OB_SUCCESS;
    search_state.depth_ = 0;
    search_state.max_exist_index_ = 0;
    search_state.last_include_start_ = false;
    search_state.last_include_end_ = false;
    search_state.produce_range_ = true;
    search_state.is_equal_range_ = table_graph_.is_equal_range_;
    search_state.start_ = static_cast<ObObj *>(search_state.allocator_.alloc(sizeof(ObObj) * column_count_));
    search_state.end_ = static_cast<ObObj *>(search_state.allocator_.alloc(sizeof(ObObj) * column_count_));
    search_state.include_start_ = static_cast<bool*>(search_state.allocator_.alloc(sizeof(bool) * column_count_));
    search_state.include_end_ = static_cast<bool*>(search_state.allocator_.alloc(sizeof(bool) * column_count_));
    if (OB_ISNULL(search_state.start_) || OB_ISNULL(search_state.end_)
        || OB_ISNULL(search_state.include_start_) || OB_ISNULL(search_state.include_end_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc memory failed", K(search_state.start_), K(search_state.end_),
               K_(search_state.include_start), K_(search_state.include_end), K_(column_count));
    } else {
      for (int i = 0; i < column_count_; ++i) {
        new(search_state.start_ + i) ObObj();
        new(search_state.end_ + i) ObObj();
        (search_state.start_ + i)->set_max_value();
        (search_state.end_ + i)->set_min_value();
        search_state.include_start_[i] = false;
        search_state.include_end_[i] = false;
      }
      if (OB_FAIL(and_first_search(search_state, table_graph_.key_part_head_, ranges,
                                                get_methods, dtc_params))) {
        LOG_WARN("and_first_search failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    query_range_mem_usage = allocator_.total() - last_mem_usage;
    LOG_TRACE("[SQL MEM USAGE] query range memory usage", K(query_range_mem_usage),
                                                          K(last_mem_usage));
    LOG_TRACE("get range success", K(ranges));
    if (table_graph_.is_equal_range_) {
      search_state.range_set_.destroy();
    }
  }
  return ret;
}

int ObQueryRange::alloc_empty_key_part(ObKeyPart  *&out_key_part)
{
  int ret = OB_SUCCESS;
  out_key_part = NULL;
  if (OB_ISNULL(out_key_part = create_new_key_part())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc ObKeyPart failed", K(ret));
  } else if (OB_FAIL(out_key_part->create_normal_key())) {
    LOG_WARN("create normal key failed", K(ret));
  } else {
    out_key_part->normal_keypart_->start_.set_max_value();
    out_key_part->normal_keypart_->end_.set_min_value();
    out_key_part->normal_keypart_->always_false_ = true;
    out_key_part->normal_keypart_->include_start_ = false;
    out_key_part->normal_keypart_->include_end_ = false;
  }
  return ret;
}

int ObQueryRange::alloc_full_key_part(ObKeyPart  *&out_key_part)
{
  int ret = OB_SUCCESS;
  out_key_part = NULL;
  if (OB_ISNULL(out_key_part = create_new_key_part())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("alloc ObKeyPart failed", K(ret));
  } else if (OB_FAIL(out_key_part->create_normal_key())) {
    LOG_WARN("create normal key failed", K(ret));
  } else {
    out_key_part->normal_keypart_->start_.set_min_value();
    out_key_part->normal_keypart_->end_.set_max_value();
    out_key_part->normal_keypart_->always_true_ = true;
    out_key_part->normal_keypart_->include_start_ = false;
    out_key_part->normal_keypart_->include_end_ = false;
  }
  return ret;
}

#define FINAL_EXTRACT(graph) \
  if (OB_SUCC(ret)) { \
    or_array.clear(); \
    if (!or_array.add_last(graph)) { \
      ret = OB_ERR_UNEXPECTED;  \
      LOG_WARN("Add query graph to list failed", K(ret));  \
    } else if (OB_FAIL(or_range_graph(or_array, &exec_ctx, graph, dtc_params))) { \
      LOG_WARN("Do OR of range graph failed", K(ret));  \
    } \
  }

OB_NOINLINE int ObQueryRange::final_extract_query_range(ObExecContext &exec_ctx,
                                                        const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  SQL_REWRITE_LOG(DEBUG, "final extract query range");
  if (state_ == NEED_PREPARE_PARAMS && NULL != table_graph_.key_part_head_) {
    ObKeyPartList or_array;
    // find all key part path and do OR option
    bool has_scan_key = false;
    if (table_graph_.is_equal_range_) {
      if (OB_FAIL(definite_in_range_graph(exec_ctx, table_graph_.key_part_head_,
                                                     has_scan_key, dtc_params))) {
        LOG_WARN("definite in range graph failed", K(ret));
      } else if (has_scan_key) {
        //包含范围性的节点，所以需要做or合并
        table_graph_.is_equal_range_ = false;
        FINAL_EXTRACT(table_graph_.key_part_head_);
      }
    } else {
        FINAL_EXTRACT(table_graph_.key_part_head_);
    }
    if (OB_SUCC(ret)) {
      state_ = CAN_READ;
    }
  }
  return ret;
}
#undef FINAL_EXTRACT

int ObQueryRange::replace_unknown_value(ObKeyPart *root, ObExecContext &exec_ctx, const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  bool is_inconsistent_rowid = false;
  if (OB_ISNULL(root)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("root=null ", K(ret));
  } else if (!root->is_like_key()) {
    if (root->normal_keypart_->start_.is_unknown()) {
      if (OB_FAIL(get_result_value_with_rowid(*root, root->normal_keypart_->start_, exec_ctx, is_inconsistent_rowid))) {
        LOG_WARN("get result value failed", K(ret));
      } else if (is_inconsistent_rowid) {
        if (root->is_phy_rowid_key_part()) {//phy rowid query get a logical rowid, can't parse.
          root->normal_keypart_->start_.set_min_value();
        } else {//logical rowid query get a phy rowid, can't parse.
          root->normal_keypart_->always_false_ = true;
        }
      } else if (!root->null_safe_ &&
                 !root->is_rowid_key_part() &&
                 root->normal_keypart_->start_.is_null()) {
        root->normal_keypart_->always_false_ = true;
      } else if (root->normal_keypart_->start_.is_unknown()) {
        //条件下推的？，range为[min, max]
        root->normal_keypart_->start_.set_min_value();
        root->normal_keypart_->include_start_ = false;
      }
    }
    if (OB_SUCC(ret) && root->normal_keypart_->end_.is_unknown()) {
      if (OB_FAIL(get_result_value_with_rowid(*root, root->normal_keypart_->end_, exec_ctx, is_inconsistent_rowid))) {
        LOG_WARN("get result value failed", K(ret));
      } else if (is_inconsistent_rowid) {
        if (root->is_phy_rowid_key_part()) {//phy rowid query get a logical rowid, can't parse.
          root->normal_keypart_->always_false_ = true;
        } else {//logical rowid query get a phy rowid, can't parse.
          root->normal_keypart_->end_.set_max_value();
        }
      } else if (!root->null_safe_ &&
                 !root->is_rowid_key_part() &&
                 root->normal_keypart_->end_.is_null()) {
        root->normal_keypart_->always_false_ = true;
      } else if (root->normal_keypart_->end_.is_unknown()) {
        //条件下推的？，range为[min, max]
        root->normal_keypart_->end_.set_max_value();
        root->normal_keypart_->include_end_ = false;
      }
    }
    if (OB_SUCC(ret) && root->normal_keypart_->always_false_) {
      root->normal_keypart_->start_.set_max_value();
      root->normal_keypart_->end_.set_min_value();
      root->normal_keypart_->include_start_ = false;
      root->normal_keypart_->include_end_ = false;
    }
  } else {
    if (OB_FAIL(get_result_value_with_rowid(*root, root->like_keypart_->pattern_, exec_ctx, is_inconsistent_rowid))) {
      LOG_WARN("get result value failed", K(ret));
    } else if (OB_FAIL(get_result_value_with_rowid(*root, root->like_keypart_->escape_, exec_ctx, is_inconsistent_rowid))) {
      LOG_WARN("get result value failed", K(ret));
    } else if (OB_FAIL(get_like_range(root->like_keypart_->pattern_,
                                      root->like_keypart_->escape_,
                                      *root,
                                      dtc_params))) {
      LOG_WARN("get like range failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (root->is_phy_rowid_key_part()) {
      ////physical rowid no need cast, it's will be transformed in table scan phase.
    } else if (OB_FAIL(root->cast_value_type(dtc_params, contain_row_))) {
      LOG_WARN("cast value type failed", K(ret));
    }
  }
  return ret;
}

int ObQueryRange::get_like_range(const ObObj &pattern,
                                 const ObObj &escape,
                                 ObKeyPart &out_key_part,
                                 const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  ObString pattern_str;
  ObString escape_str;
  ObObj start;
  ObObj end;
  void *min_str_buf = NULL;
  void *max_str_buf = NULL;
  int32_t col_len = out_key_part.pos_.column_type_.get_accuracy().get_length();
  ObCollationType cs_type = out_key_part.pos_.column_type_.get_collation_type();
  size_t min_str_len = 0;
  size_t max_str_len = 0;
  ObObj pattern_buf_obj;
  ObObj escape_buf_obj;
  const ObObj *pattern_val = NULL;
  // const ObObj *escape_val = NULL;
  //like expr抽取后转化成normal key
  if (OB_FAIL(out_key_part.create_normal_key())) {
    LOG_WARN("create normal key failed", K(ret));
  } else if (pattern.is_null()) {
    //a like null return empty range
    out_key_part.normal_keypart_->start_.set_max_value();
    out_key_part.normal_keypart_->end_.set_min_value();
    out_key_part.normal_keypart_->include_start_ = false;
    out_key_part.normal_keypart_->include_end_ = false;
    out_key_part.normal_keypart_->always_false_ = true;
    out_key_part.normal_keypart_->always_true_ = false;
  } else if (!pattern.is_string_type()
             || (!escape.is_string_type() && !escape.is_null())
             || col_len <= 0) {
    //1 like 1 return whole range
    out_key_part.normal_keypart_->start_.set_min_value();
    out_key_part.normal_keypart_->end_.set_max_value();
    out_key_part.normal_keypart_->include_start_ = false;
    out_key_part.normal_keypart_->include_end_ = false;
    out_key_part.normal_keypart_->always_false_ = false;
    out_key_part.normal_keypart_->always_true_ = true;
  } else if (OB_FAIL(cast_like_obj_if_needed(pattern, pattern_buf_obj, pattern_val,
                                             out_key_part, dtc_params))) {
    LOG_WARN("failed to cast like obj if needed", K(ret));
  } else if (OB_FAIL(pattern_val->get_string(pattern_str))) {
    LOG_WARN("get varchar failed", K(ret), K(pattern));
  } else {
    int64_t mbmaxlen = 1;
    ObString escape_val;
    if (escape.is_null()) {  //如果escape是null,则给默认的'\\'
      escape_str.assign_ptr("\\", 1);
    } else if (ObCharset::is_cs_nonascii(escape.get_collation_type())) {
      if (OB_FAIL(escape.get_string(escape_val))) {
        LOG_WARN("failed to get escape string", K(escape), K(ret));
      } else if (OB_FAIL(ObCharset::charset_convert(allocator_, escape_val,
                                     escape.get_collation_type(),
                                     CS_TYPE_UTF8MB4_GENERAL_CI, escape_str, true))) {
        LOG_WARN("failed to do charset convert", K(ret), K(escape_val));
      }
    } else if (OB_FAIL(escape.get_string(escape_str))) {
      LOG_WARN("failed to get escape string", K(escape), K(ret));
    } else if (escape_str.empty()) {
      escape_str.assign_ptr("\\", 1);
    } else { /* do nothing */ }
    if (OB_FAIL(ret)) {
      // do nothing;
    } else if (OB_FAIL(ObCharset::get_mbmaxlen_by_coll(cs_type, mbmaxlen))) {
      LOG_WARN("fail to get mbmaxlen", K(ret), K(cs_type), K(pattern), K(escape));
    } else if (OB_ISNULL(escape_str.ptr())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Escape str should not be NULL", K(ret));
    } else if (OB_UNLIKELY((lib::is_oracle_mode() && 1 != escape_str.length())
     || (!lib::is_oracle_mode() && 1 > escape_str.length()))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("failed to check escape length", K(escape_str), K(escape_str.length()));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ESCAPE");
    } else { }

    if (OB_SUCC(ret)) {
      //convert character counts to len in bytes
      col_len = static_cast<int32_t>(col_len * mbmaxlen);
      min_str_len = col_len;
      max_str_len = col_len;
      if (OB_ISNULL(min_str_buf = allocator_.alloc(min_str_len))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc memory failed", K(min_str_len));
      } else if (OB_ISNULL(max_str_buf = allocator_.alloc(max_str_len))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc memory failed", K(max_str_len));
      } else if (escape_str.length() > 1 || OB_FAIL(ObCharset::like_range(cs_type,
                                               pattern_str,
                                               *(escape_str.ptr()),
                                               static_cast<char*>(min_str_buf),
                                               &min_str_len,
                                               static_cast<char*>(max_str_buf),
                                               &max_str_len))) {
        //set whole range
        out_key_part.normal_keypart_->start_.set_min_value();
        out_key_part.normal_keypart_->end_.set_max_value();
        out_key_part.normal_keypart_->include_start_ = false;
        out_key_part.normal_keypart_->include_end_ = false;
        out_key_part.normal_keypart_->always_false_ = false;
        out_key_part.normal_keypart_->always_true_ = true;
        ret = OB_SUCCESS;
      } else {
        ObObj &start = out_key_part.normal_keypart_->start_;
        ObObj &end = out_key_part.normal_keypart_->end_;
        start.set_collation_type(out_key_part.pos_.column_type_.get_collation_type());
        start.set_string(out_key_part.pos_.column_type_.get_type(),
                         static_cast<char*>(min_str_buf), static_cast<int32_t>(min_str_len));
        end.set_collation_type(out_key_part.pos_.column_type_.get_collation_type());
        end.set_string(out_key_part.pos_.column_type_.get_type(),
                       static_cast<char*>(max_str_buf), static_cast<int32_t>(max_str_len));
        out_key_part.normal_keypart_->include_start_ = true;
        out_key_part.normal_keypart_->include_end_ = true;
        out_key_part.normal_keypart_->always_false_ = false;
        out_key_part.normal_keypart_->always_true_ = false;

        /// check if is precise
        if (NULL != query_range_ctx_) {
          query_range_ctx_->cur_expr_is_precise_ =
                                            ObQueryRange::check_like_range_precise(pattern_str,
                                                              static_cast<char *>(max_str_buf),
                                                             max_str_len, *(escape_str.ptr()));
        }
      }
      if (NULL != min_str_buf) {
        allocator_.free(min_str_buf);
        min_str_buf = NULL;
      }
      if (NULL != max_str_buf) {
        allocator_.free(max_str_buf);
        max_str_buf = NULL;
      }
    }
  }
  return ret;
}

// Generaal term: if one or more same key parts with OR realtion have same and_next_,
//                        we call them general term(GT)
// E.g.
//       A and B, A is GT
//       A and (B1 or B2) and C, (B1 or B2) is GT.
//       A and (((B1 or B2) and C1) or (B3 and c2)) and D,
//       (((B1 or B2) and C1) or (B3 and c2)) is not GT.
//
// Our query range graph must abide by following rules:
// 1. on key part is pointed to by more than one key parts, except they are GT.
// 2. a key part can only point to the first member of a GT.
//
// So, we do not generate following graph:
//       A--B1--C1--D
//             |
//             B2(points to D too)
// but generate
//       A--B1--C1--D1
//             |
//             B2--D2
//       (key part D1 is equal to D2, but has different storage)
// '--', means and_next_;
// '|', means or_next_;



// Out of usage, ObKeyPart is free by key_part_store_


// maybe use later

//void ObQueryRange::free_range_graph(ObKeyPart *&graph)
//{
//  ObKeyPart *next_gt = NULL;
//  for (ObKeyPart *cur_gt = graph; cur_gt != NULL; cur_gt = next_gt) {
//    next_gt = cur_gt->general_or_next();
//    free_range_graph(cur_gt->and_next_);
//    ObKeyPart *next_or = NULL;
//    for (ObKeyPart *cur_or = cur_gt;
//         cur_or != NULL && cur_or->and_next_ == cur_gt->and_next_;
//         cur_or = next_or) {
//      next_or = cur_or->or_next_;
//      ObKeyPart *next_item = NULL;
//      for (ObKeyPart *item = cur_or->item_next_; item != NULL; item = next_item) {
//        next_item = item->item_next_;
//        ObKeyPart::free(item);
//      }
//      ObKeyPart::free(cur_or);
//    }
//  }
//  graph = NULL;
//}

ObKeyPart *ObQueryRange::create_new_key_part()
{
  void *ptr = NULL;
  ObKeyPart *key_part = NULL;
  if (NULL != (ptr = allocator_.alloc(sizeof(ObKeyPart)))) {
    key_part = new(ptr) ObKeyPart(allocator_);
    if (OB_SUCCESS != key_part_store_.store_obj(key_part)) {
      key_part->~ObKeyPart();
      key_part = NULL;
      LOG_WARN("Store ObKeyPart failed");
    }
  }
  return key_part;
}

// Deep copy this key part node only, not include any item in XXX_next_ list

ObKeyPart *ObQueryRange::deep_copy_key_part(ObKeyPart *key_part)
{
  int ret = OB_SUCCESS;
  ObKeyPart *new_key_part = NULL;
  if (key_part) {
    if (OB_ISNULL(new_key_part = create_new_key_part())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("Get new key part failed", K(ret));
    } else if ((ret = new_key_part->deep_node_copy(*key_part)) != OB_SUCCESS) {
      LOG_WARN("Copy key part node failed", K(ret));
    } else {
      // do nothing
    }
  }
  if (OB_FAIL(ret)) {
    new_key_part = NULL;
  }
  return new_key_part;
}

int ObQueryRange::serialize_range_graph(const ObKeyPart *cur,
                                        const ObKeyPart *pre_and_next,
                                        char *buf,
                                        int64_t buf_len,
                                        int64_t &pos) const
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (OB_ISNULL(cur)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("cur is null.", K(ret));
  } else {
    bool has_and_next = cur->and_next_ ? true : false;
    bool has_or_next = cur->or_next_ ? true : false;
    bool encode_and_next = false;

    OB_UNIS_ENCODE(has_and_next);
    OB_UNIS_ENCODE(has_or_next);
    if (OB_SUCC(ret) && has_and_next) {
      encode_and_next = cur->and_next_ == pre_and_next ? false : true;
      OB_UNIS_ENCODE(encode_and_next);
      if (OB_SUCC(ret) && encode_and_next) {
        if (OB_FAIL(SMART_CALL(serialize_range_graph(cur->and_next_, NULL, buf, buf_len, pos)))) {
          LOG_WARN("serialize and_next_ child graph failed", K(ret));
        }
      }
    }
    if (OB_SUCC((ret)) && OB_FAIL(serialize_cur_keypart(*cur, buf, buf_len, pos))) {
      LOG_WARN("serialize current key part failed", K(ret));
    }
    if (OB_SUCC(ret) && has_or_next) {
      if (OB_FAIL(SMART_CALL(serialize_range_graph(cur->or_next_, cur->and_next_,
                                                   buf, buf_len, pos)))) {
        LOG_WARN("serialize or_next_ child graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::deserialize_range_graph(ObKeyPart *pre_key,
                                          ObKeyPart *&cur,
                                          const char *buf,
                                          int64_t data_len,
                                          int64_t &pos)
{
  int ret = OB_SUCCESS;
  bool has_and_next = false;
  bool has_or_next = false;
  bool encode_and_next = false;
  ObKeyPart *and_next = NULL;

  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else {
    OB_UNIS_DECODE(has_and_next);
    OB_UNIS_DECODE(has_or_next);
    if (OB_SUCC(ret) && has_and_next) {
      OB_UNIS_DECODE(encode_and_next);
      if (OB_SUCC(ret) && encode_and_next) {
        if (OB_FAIL(SMART_CALL(deserialize_range_graph(NULL, and_next, buf, data_len, pos)))) {
          LOG_WARN("deserialize and_next_ child graph failed", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(deserialize_cur_keypart(cur, buf, data_len, pos))) {
      LOG_WARN("deserialize current key part failed", K(ret));
    }
    //build and_next_ child grap and pre_key with current key part
    if (OB_SUCC(ret)) {
      if (encode_and_next) {
        cur->and_next_ = and_next;
      } else if (has_and_next) {
        if (OB_ISNULL(pre_key)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("pre_key is null.", K(ret));
        } else {
          cur->and_next_ = pre_key->and_next_;
        }
      } else {
        // do nothing
      }
      if (pre_key) {
        pre_key->or_next_ = cur;
      }
    }
    if (OB_SUCC(ret) && has_or_next) {
      if (OB_FAIL(SMART_CALL(deserialize_range_graph(cur, cur->or_next_, buf, data_len, pos)))) {
        LOG_WARN("deserialize or_next_ child graph failed", K(ret));
      }
    }
  }
  return ret;
}

int ObQueryRange::serialize_cur_keypart(const ObKeyPart &cur, char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  bool has_item_next = (cur.item_next_ != NULL);
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else {
    OB_UNIS_ENCODE(cur);
    OB_UNIS_ENCODE(has_item_next);
    if (OB_SUCC(ret) && has_item_next) {
      if (OB_FAIL(SMART_CALL(serialize_cur_keypart(*cur.item_next_, buf, buf_len, pos)))) {
        LOG_WARN("serialize cur keypart failed", K(ret));
      }
    }
  }
  return ret;
}

int64_t ObQueryRange::get_cur_keypart_serialize_size(const ObKeyPart &cur) const
{
  int64_t len = 0;
  bool has_item_next = (cur.item_next_ != NULL);
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else {
    OB_UNIS_ADD_LEN(cur);
    OB_UNIS_ADD_LEN(has_item_next);
    if (has_item_next) {
      len += get_cur_keypart_serialize_size(*cur.item_next_);
    }
  }
  return len;
}

int ObQueryRange::deserialize_cur_keypart(ObKeyPart *&cur, const char *buf, int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  bool has_item_next = false;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else {
    if (OB_ISNULL(cur = create_new_key_part())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("create new key part failed", K(ret));
    }
    OB_UNIS_DECODE(*cur);
    OB_UNIS_DECODE(has_item_next);
    if (OB_SUCC(ret) && has_item_next) {
      if (OB_FAIL(SMART_CALL(deserialize_cur_keypart(cur->item_next_, buf, data_len, pos)))) {
        LOG_WARN("deserialize item next failed", K(ret));
      }
    }
  }
  return ret;
}

int64_t ObQueryRange::get_range_graph_serialize_size(const ObKeyPart *cur,
                                                     const ObKeyPart *pre_and_next) const
{
  int64_t len = 0;
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (OB_ISNULL(cur)) {
    LOG_WARN("cur is null.");
  } else {
    bool has_and_next = cur->and_next_ ? true : false;
    bool has_or_next = cur->or_next_ ? true : false;
    bool encode_and_next = false;

    OB_UNIS_ADD_LEN(has_and_next);
    OB_UNIS_ADD_LEN(has_or_next);
    if (has_and_next) {
      encode_and_next = cur->and_next_ == pre_and_next ? false : true;
      OB_UNIS_ADD_LEN(encode_and_next);
      if (encode_and_next) {
        len += get_range_graph_serialize_size(cur->and_next_, NULL);
      }
    }
    len += get_cur_keypart_serialize_size(*cur);
    if (has_or_next) {
      len += get_range_graph_serialize_size(cur->or_next_, cur->and_next_);
    }
  }
  return len;
}

int ObQueryRange::serialize_expr_final_info(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  OB_UNIS_ENCODE(expr_final_infos_.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < expr_final_infos_.count(); ++i) {
    OB_UNIS_ENCODE(expr_final_infos_.at(i).flags_);
    if (expr_final_infos_.at(i).is_param_) {
      OB_UNIS_ENCODE(expr_final_infos_.at(i).param_idx_);
    } else if (expr_final_infos_.at(i).expr_exists_) {
      OB_UNIS_ENCODE(*expr_final_infos_.at(i).temp_expr_);
    }
  }
  return ret;
}

int ObQueryRange::deserialize_expr_final_info(const char *buf, int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t final_expr_count = 0;
  expr_final_infos_.reset();
  OB_UNIS_DECODE(final_expr_count);
  if (OB_SUCC(ret)) {
    if (OB_FAIL(expr_final_infos_.prepare_allocate(final_expr_count))) {
      LOG_WARN("failed to init array", K(ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < final_expr_count; ++i) {
    OB_UNIS_DECODE(expr_final_infos_.at(i).flags_);
    if (expr_final_infos_.at(i).is_param_) {
      OB_UNIS_DECODE(expr_final_infos_.at(i).param_idx_);
    } else if (expr_final_infos_.at(i).expr_exists_) {
      ObTempExpr *temp_expr = NULL;
      char *mem = static_cast<char *>(allocator_.alloc(sizeof(ObTempExpr)));
      if (OB_ISNULL(mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc temp expr failed", K(ret));
      } else {
        temp_expr = new(mem)ObTempExpr(allocator_);
      }
      OB_UNIS_DECODE(*temp_expr);
      if (OB_SUCC(ret)) {
        expr_final_infos_.at(i).temp_expr_ = temp_expr;
      }
    }
  }
  return ret;
}

int64_t ObQueryRange::get_expr_final_info_serialize_size() const
{
  int ret = OB_SUCCESS;
  int64_t len = 0;
  OB_UNIS_ADD_LEN(expr_final_infos_.count());
  for (int64_t i = 0; i < expr_final_infos_.count(); ++i) {
    OB_UNIS_ADD_LEN(expr_final_infos_.at(i).flags_);
    if (expr_final_infos_.at(i).is_param_) {
      OB_UNIS_ADD_LEN(expr_final_infos_.at(i).param_idx_);
    } else if (expr_final_infos_.at(i).expr_exists_) {
      OB_UNIS_ADD_LEN(*expr_final_infos_.at(i).temp_expr_);
    }
  }
  return len;
}

OB_SERIALIZE_MEMBER(ObQueryRange::ObEqualOff,
                    only_pos_, param_idx_, pos_off_, pos_type_, pos_value_);

OB_DEF_SERIALIZE(ObQueryRange)
{
  int ret = OB_SUCCESS;
  int64_t graph_count = (NULL != table_graph_.key_part_head_ ? 1 : 0);

  OB_UNIS_ENCODE(static_cast<int64_t>(state_));
  OB_UNIS_ENCODE(column_count_);
  OB_UNIS_ENCODE(graph_count);
  if (1 == graph_count) {
    if (OB_FAIL(serialize_range_graph(table_graph_.key_part_head_, NULL, buf, buf_len, pos))) {
      LOG_WARN("serialize range graph failed", K(ret));
    }
    OB_UNIS_ENCODE(table_graph_.is_precise_get_);
    OB_UNIS_ENCODE(table_graph_.is_equal_range_);
    OB_UNIS_ENCODE(table_graph_.is_standard_range_);
  }
  //新增对contain_row_序列化，放到最后面
  OB_UNIS_ENCODE(contain_row_);
  OB_UNIS_ENCODE(has_exec_param_);
  //新增 equal_query_range 序列化
  OB_UNIS_ENCODE(is_equal_and_);
  OB_UNIS_ENCODE(equal_offs_);
  if (OB_SUCC(ret)) {
    if (OB_FAIL(serialize_expr_final_info(buf, buf_len, pos))) {
      LOG_WARN("failed to serialize final exprs", K(ret));
    }
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObQueryRange)
{
  int64_t len = 0;
  int64_t graph_count = (NULL != table_graph_.key_part_head_ ? 1 : 0);

  OB_UNIS_ADD_LEN(static_cast<int64_t>(state_));
  OB_UNIS_ADD_LEN(column_count_);
  OB_UNIS_ADD_LEN(graph_count);
  if (1 == graph_count) {
    len += get_range_graph_serialize_size(table_graph_.key_part_head_, NULL);
    OB_UNIS_ADD_LEN(table_graph_.is_precise_get_);
    OB_UNIS_ADD_LEN(table_graph_.is_equal_range_);
    OB_UNIS_ADD_LEN(table_graph_.is_standard_range_);
  }
  //新增对contain_row_序列化，放到最后面
  OB_UNIS_ADD_LEN(contain_row_);
  OB_UNIS_ADD_LEN(has_exec_param_);
  //新增 equal_query_range 序列化
  OB_UNIS_ADD_LEN(is_equal_and_);
  OB_UNIS_ADD_LEN(equal_offs_);
  len += get_expr_final_info_serialize_size();
  return len;
}

OB_DEF_DESERIALIZE(ObQueryRange)
{
  int ret = OB_SUCCESS;
  int64_t state = 0;
  int64_t graph_count = 0;

  OB_UNIS_DECODE(state);
  OB_UNIS_DECODE(column_count_);
  OB_UNIS_DECODE(graph_count);
  if (1 == graph_count) {
    if (OB_FAIL(deserialize_range_graph(NULL, table_graph_.key_part_head_, buf, data_len, pos))) {
      LOG_WARN("deserialize range graph failed", K(ret));
    }
    OB_UNIS_DECODE(table_graph_.is_precise_get_);
    OB_UNIS_DECODE(table_graph_.is_equal_range_);
    OB_UNIS_DECODE(table_graph_.is_standard_range_);
  }
  OB_UNIS_DECODE(contain_row_);
  if (OB_SUCC(ret)) {
    state_ = static_cast<ObQueryRangeState>(state);
  }
  OB_UNIS_DECODE(has_exec_param_);
  // 新增 equal_query_range 反序列化
  OB_UNIS_DECODE(is_equal_and_);
  OB_UNIS_DECODE(equal_offs_);
  if (OB_SUCC(ret)) {
    if (OB_FAIL(deserialize_expr_final_info(buf, data_len, pos))) {
      LOG_WARN("failed to deserialize final exprs", K(ret));
    }
  }
  return ret;
}

// Deep copy range graph of one index
int ObQueryRange::deep_copy_range_graph(ObKeyPart *src, ObKeyPart  *&dest)
{
  int ret = OB_SUCCESS;
  ObKeyPart *prev_gt = NULL;
  for (ObKeyPart *cur_gt = src; OB_SUCC(ret) && NULL != cur_gt; cur_gt = cur_gt->general_or_next()) {
    ObKeyPart *and_next = NULL;
    ObKeyPart *new_key_part = NULL;
    ObKeyPart *prev_key_part = NULL;
    ObKeyPart *new_cur_gt_head = NULL;
    if (OB_FAIL(SMART_CALL(deep_copy_range_graph(cur_gt->and_next_, and_next)))) {
      LOG_WARN("Deep copy range graph failed", K(ret));
    } else {
      for (ObKeyPart *cur_or = cur_gt;
           OB_SUCC(ret) && NULL != cur_or && cur_or->and_next_ == cur_gt->and_next_;
           cur_or = cur_or->or_next_) {
        if (OB_FAIL(deep_copy_key_part_and_items(cur_or, new_key_part))) {
          LOG_WARN("Deep copy key part and items failed");
        } else if (cur_or == cur_gt) {
          new_cur_gt_head = new_key_part;
        } else {
          if (OB_ISNULL(prev_key_part)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("prev_key_part is null.", K(ret));
          } else {
            prev_key_part->or_next_ = new_key_part;
          }
        }
        if (OB_SUCC(ret)) {
          prev_key_part = new_key_part;
          if (OB_ISNULL(new_key_part)) { //yeti2
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("new_key_part is null.", K(ret));
          } else {
            new_key_part->and_next_ = and_next;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (NULL != prev_gt) {
        prev_gt->or_next_ = new_cur_gt_head;
      } else {
        dest = new_cur_gt_head;
      }
      prev_gt = new_key_part;
    }
  }
  if (OB_FAIL(ret)) {
    dest = NULL;
  }
  return ret;
}

int ObQueryRange::deep_copy_expr_final_info(const ObIArray<ExprFinalInfo> &final_infos)
{
  int ret = OB_SUCCESS;
  expr_final_infos_.reset();
  if (OB_FAIL(expr_final_infos_.prepare_allocate(final_infos.count()))) {
    LOG_WARN("failed to init array", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < final_infos.count(); i++) {
    ObTempExpr *temp_expr = NULL;
    expr_final_infos_.at(i).flags_ = final_infos.at(i).flags_;
    if (final_infos.at(i).is_param_) {
      expr_final_infos_.at(i).param_idx_ = final_infos.at(i).param_idx_;
    } else if (!final_infos.at(i).expr_exists_) {
      // do nothing
    } else if (OB_ISNULL(final_infos.at(i).temp_expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null expr", K(ret));
    } else if (OB_FAIL(final_infos.at(i).temp_expr_->deep_copy(allocator_, temp_expr))) {
      LOG_WARN("failed to deep copy temp expr failed", K(ret));
    } else {
      expr_final_infos_.at(i).temp_expr_ = temp_expr;
    }
  }
  return ret;
}

int ObQueryRange::shallow_copy_expr_final_info(const ObIArray<ExprFinalInfo> &final_infos)
{
  int ret = OB_SUCCESS;
  expr_final_infos_.reset();
  if (OB_FAIL(expr_final_infos_.prepare_allocate(final_infos.count()))) {
    LOG_WARN("failed to init array", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < final_infos.count(); i++) {
    expr_final_infos_.at(i).flags_ = final_infos.at(i).flags_;
    expr_final_infos_.at(i).param_idx_ = final_infos.at(i).param_idx_;
  }
  return ret;
}

// Deep copy whole query range

OB_NOINLINE int ObQueryRange::deep_copy(const ObQueryRange &other,
                                        const bool copy_for_final /* = false*/)
{
  int ret = OB_SUCCESS;
  const ObRangeGraph &other_graph = other.table_graph_;
  state_ = other.state_;
  contain_row_ = other.contain_row_;
  column_count_ = other.column_count_;
  has_exec_param_ = other.has_exec_param_;
  is_equal_and_ = other.is_equal_and_;
  if (OB_FAIL(range_exprs_.assign(other.range_exprs_))) {
    LOG_WARN("assign range exprs failed", K(ret));
  } else if (OB_FAIL(table_graph_.assign(other_graph))) {
    LOG_WARN("Deep copy range columns failed", K(ret));
  } else if (OB_FAIL(equal_offs_.assign(other.equal_offs_))) {
    LOG_WARN("Deep copy equal and offset failed", K(ret));
  } else if (OB_FAIL(deep_copy_range_graph(other_graph.key_part_head_, table_graph_.key_part_head_))) {
    LOG_WARN("Deep copy key part graph failed", K(ret));
  }
  if (OB_FAIL(ret)) {
  } else if (copy_for_final) {
    if (OB_FAIL(shallow_copy_expr_final_info(other.expr_final_infos_))) {
      LOG_WARN("Deep copy expr final info failed", K(ret));
    }
  } else {
    if (OB_FAIL(deep_copy_expr_final_info(other.expr_final_infos_))) {
      LOG_WARN("Deep copy expr final info failed", K(ret));
    }
  }
  return ret;
}

int ObQueryRange::all_single_value_ranges(bool &all_single_values, const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  all_single_values = false;
  ObQueryRangeArray ranges;
  ObGetMethodArray dummy;
  if (OB_FAIL(get_tablet_ranges(ranges, dummy, dtc_params))) {
    LOG_WARN("fail to get tablet ranges", K(ret));
  } else {
    if (ranges.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid ranges.", K(ret));
    } else {
      all_single_values = true;
      for (int64_t i = 0; OB_SUCC(ret) && all_single_values && i < ranges.count(); i++) {
        if (OB_ISNULL(ranges.at(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("the memver of ranges is null.", K(ret));
        } else if (!ranges.at(i)->border_flag_.inclusive_start()
                   || !ranges.at(i)->border_flag_.inclusive_end()
                   || ranges.at(i)->start_key_ != ranges.at(i)->end_key_) {
          all_single_values = false;
        } else {
          // do nothing
        }
      }
    }
  }
  return ret;
}

int ObQueryRange::is_min_to_max_range(bool &is_min_to_max_range, const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  is_min_to_max_range = false;
  ObQueryRangeArray ranges;
  ObGetMethodArray dummy;
  if (OB_FAIL(get_tablet_ranges(ranges, dummy, dtc_params))) {
    LOG_WARN("fail to get tablet ranges", K(ret));
  } else if (OB_ISNULL(ranges.at(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the memver of ranges is null.", K(ret));
  } else {
    if (1 == ranges.count()
        && ranges.at(0)->start_key_.is_min_row()
        && ranges.at(0)->end_key_.is_max_row()) {
      is_min_to_max_range = true;
    }
  }
  return ret;
}

DEF_TO_STRING(ObQueryRange)
{
  int64_t pos = 0;

  J_ARRAY_START();
  if (NULL != table_graph_.key_part_head_) {
    J_OBJ_START();
    J_KV(N_IN, table_graph_.is_equal_range_,
         N_IS_GET, table_graph_.is_precise_get_,
         N_IS_STANDARD, table_graph_.is_standard_range_);
    J_COMMA();
    J_NAME(N_RANGE_GRAPH);
    J_COLON();
    pos += range_graph_to_string(buf + pos, buf_len - pos, table_graph_.key_part_head_);
    J_OBJ_END();
  }
  J_ARRAY_END();
  return pos;
}

int64_t ObQueryRange::range_graph_to_string(
    char *buf,
    const int64_t buf_len,
    ObKeyPart *key_part) const
{
  int64_t pos = 0;
  bool is_stack_overflow = false;
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else {
    J_OBJ_START();
    if (NULL != key_part) {
      J_KV(N_KEY_PART_VAL, *key_part);
      if (key_part->item_next_) {
        J_COMMA();
        J_NAME(N_ITEM_KEY_PART);
        J_COLON();
        pos += range_graph_to_string(buf + pos, buf_len - pos, key_part->item_next_);
      }
      if (key_part->and_next_) {
        J_COMMA();
        J_NAME(N_AND_KEY_PART);
        J_COLON();
        pos += range_graph_to_string(buf + pos, buf_len - pos, key_part->and_next_);
      }
      if (key_part->or_next_) {
        J_COMMA();
        J_NAME(N_OR_KEY_PART);
        J_COLON();
        pos += range_graph_to_string(buf + pos, buf_len - pos, key_part->or_next_);
      }
    }
    J_OBJ_END();
  }
  return pos;
}

inline bool ObQueryRange::is_standard_graph(const ObKeyPart *root) const
{
  bool bret = true;
  if (contain_row_) {
    bret = false;
  } else {
    for (const ObKeyPart *cur = root; bret && NULL != cur; cur = cur->and_next_) {
      if (NULL != cur->or_next_ || cur->is_like_key()) {
        bret = false;
      } else {
        for (const ObKeyPart *item_next = cur->item_next_;
             bret && NULL != item_next;
             item_next = item_next->item_next_) {
          if (item_next->is_like_key()) {
            bret = false;
          }
        }
      }
    }
  }
  return bret;
}

// 判断是否可以免or
// 严格的等值node的graph
// 满足条件： KEY连续、对齐、等值条件
int ObQueryRange::is_strict_equal_graph(
    const ObKeyPart *node,
    const int64_t cur_pos,
    int64_t &max_pos,
    bool &is_strict_equal) const
{
  is_strict_equal = true;
  bool is_stack_overflow = false;
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to do stack overflow check", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack overflow", K(ret));
  } else if (NULL == node || node->pos_.offset_ != cur_pos) { // check 连续
    is_strict_equal = false;
  } else if (!node->is_equal_condition()) { // 等值
    is_strict_equal = false;
  } else {
    // and direction
    if (NULL != node->and_next_) {
      if (NULL == node->or_next_ || node->or_next_->and_next_ != node->and_next_) {
        OZ(SMART_CALL(is_strict_equal_graph(node->and_next_, cur_pos + 1,
                                            max_pos, is_strict_equal)));
      }
    } else { // check 对齐
      if (-1 == max_pos) {
        max_pos = cur_pos;
      } else if (cur_pos != max_pos) {
        is_strict_equal = false;
      }
    }
    //or direction
    if (is_strict_equal && OB_SUCC(ret) && NULL != node->or_next_) {
      OZ(SMART_CALL(is_strict_equal_graph(node->or_next_, cur_pos, max_pos, is_strict_equal)));
    }
  }
  return ret;
}

// graph是严格的range的条件是抽取的range中只能包含一个in表达式
// 可以设定起始键的位置start_pos，默认是0.

// pos不为0时用来判断是不是整齐的graph时(硬解析时判断需不需要做or操作),主键c1,c2,c3
// (c2,c3) in ((1,2),(2,3)) -> 是整齐的graph,pos从1开始

// 普通情况例如：主键是c1, c2, c3
// (c1, c2, c3) in ((1, 2, 3), (4, 5, 6))  TRUE
// (c1, c3) in ((1, 3), (4, 6)) 主键不连续, FALSE
// (c2,c3) in ((1,1),(2,2)) 不是主键前缀 ,  FALSE
// (c1, c2, c3) in ((1, 2, 3), (4, (select 5), 6)) FALSE，主键有个位置的值是子查询，(min, max)覆盖了后面的范围
bool ObQueryRange::is_strict_in_graph(const ObKeyPart *root, const int64_t start_pos) const
{
  bool bret = true;
  if (NULL == root) {
    bret = false;
  } else if (column_count_ < 1) {
    bret = false;
  }
  int64_t first_len = -1;
  for (const ObKeyPart *cur_or = root; bret && NULL != cur_or; cur_or = cur_or->or_next_) {
    const ObKeyPart *cur_and = cur_or;
    int64_t j = start_pos;
    for (j = start_pos; bret && j < column_count_ && NULL != cur_and; ++j) {
      if (cur_and->pos_.offset_ != j) {
        //key graph不是按键的位置连续排列
        bret = false;
      } else if (!cur_and->is_equal_condition()) {
        bret = false;
      } else if (start_pos != j && NULL != cur_and->or_next_) {
        // except the first item, others can't has or_next
        bret = false;
      } else {
        cur_and = cur_and->and_next_;
      }
    }
    if (bret) {
      if (first_len < 0) {
        first_len = j;
      } else if (j != first_len || NULL != cur_and) {
        bret = false;
      }
    }
  }
  return bret;
}

bool ObQueryRange::is_regular_in_graph(const ObKeyPart *root) const
{
  bool bret = true;
  if (NULL == root) {
    bret = false;
  } else {
    bret = is_strict_in_graph(root, root->pos_.offset_);
  }
  return bret;
}

int ObQueryRange::ObSearchState::intersect(const ObObj &start, bool include_start, const ObObj &end, bool include_end)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(start_) || OB_ISNULL(end_) || OB_ISNULL(include_start_) || OB_ISNULL(include_end_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K_(start), K_(end), K_(include_start), K_(include_end), K_(depth));
  } else if (depth_ < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid depth", K(ret), K_(depth));
    //depth_允许小于0，但是depth_小于0，节点的值只可能会恒true或者恒false or whole key_part，在上面已经处理了这3种条件
    //不应该走到这里来
  } else {
    ObObj &s1 = start_[depth_];
    ObObj &e1 = end_[depth_];
    int cmp = start.compare(end);
    if (cmp > 0 || (0 == cmp && (!include_start || !include_end)) //参数中的range范围是空集，所以结果为空集
        || !has_intersect(start, include_start, end, include_end)) {
      // 结果为空集
      for (int64_t i = 0; i < max_exist_index_; ++i) {
        //and表达式中有一个键为false，那么整个range都是false
        start_[i].set_max_value();
        end_[i].set_min_value();
      }
      last_include_start_ = false;
      last_include_end_ = false;
      is_empty_range_ = true;
    } else if (start.is_min_value() && end.is_max_value()) {
      //always true, true and A => the result's A, so ignore true
    } else {
      //取大
      cmp = s1.compare(start);
      if (cmp > 0) {
        // do nothing
      } else if (cmp < 0) {
        s1 = start;
        include_start_[depth_] = include_start;
      } else {
        include_start_[depth_] = (include_start_[depth_] && include_start);
      }

      // 取小
      cmp = e1.compare(end);
      if (cmp > 0) {
        e1 = end;
        include_end_[depth_] = include_end;
      } else if (cmp < 0) {
        // do nothing
      } else {
        include_end_[depth_] = (include_end_[depth_] && include_end);
      }
    }
  }
  return ret;
}

int ObQueryRange::remove_precise_range_expr(int64_t offset)
{
  int ret = OB_SUCCESS;
  if (query_range_ctx_ != NULL) {
    for (int64_t i = 0; OB_SUCC(ret) && i < query_range_ctx_->precise_range_exprs_.count(); ++i) {
      ObRangeExprItem &expr_item = query_range_ctx_->precise_range_exprs_.at(i);
      for (int64_t j = 0 ; j < expr_item.cur_pos_.count() ; ++j) {
        if (expr_item.cur_pos_.at(j) >= offset) {
          expr_item.cur_expr_ = NULL;
          break;
        }
      }
    }
  }
  return ret;
}

bool ObQueryRange::is_general_graph(const ObKeyPart &keypart) const
{
  bool bret = true;
  const ObKeyPart *cur_key = &keypart;
  const ObKeyPart *cur_and_next = cur_key->and_next_;
  for (const ObKeyPart *or_next = cur_key->or_next_; bret && or_next != NULL; or_next = or_next->or_next_) {
    if (or_next->and_next_ != cur_and_next) {
      bret = false;
    }
  }
  return bret;
}

bool ObQueryRange::has_scan_key(const ObKeyPart &keypart) const
{
  bool bret = false;
  for (const ObKeyPart *or_next = &keypart; !bret && or_next != NULL; or_next = or_next->or_next_) {
    if (!or_next->is_equal_condition()) {
      bret = true;
    }
  }
  return bret;
}

bool ObQueryRange::check_like_range_precise(const ObString &pattern_str,
                                            const char *max_str_buf,
                                            const size_t max_str_len,
                                            const char escape)
{
    const char *pattern_str_buf = pattern_str.ptr();
    bool end_with_percent = false;
    bool find_first_percent = false;
    bool is_precise = false;
    int64_t i = 0;
    int64_t j = 0;
    int64_t last_equal_idx = -1;
    while (i < pattern_str.length() && j < max_str_len) {
      // handle escape
      if (pattern_str_buf[i] == escape) {
        if (i == pattern_str.length() - 1){
          // if escape is last character in pattern, then we will use its origin meaning
          // e.g. c1 like 'aa%' escape '%', % in pattern means match any
        } else {
          ++i;
        }
      }

      if (pattern_str_buf[i] == max_str_buf[j]) {
        last_equal_idx = i;
        ++i;
        ++j;
      } else if (pattern_str_buf[i] == '%') {
        if (find_first_percent) {
          if (pattern_str_buf[i - 1] == '%') {
            end_with_percent = (pattern_str.length() == i + 1);
          } else {
            end_with_percent = false;
            break;
          }
        } else {
          find_first_percent = true;
          end_with_percent = (pattern_str.length() == i + 1);
        }
        ++i;
      } else {
        // 通配符'_'对于不同的字符集有不同的处理方式, 因此这里不处理'_'的情况, 如果遇到'_'直接认为不是一个精确匹配
        break;
      }
    }
    bool match_without_wildcard = (i == pattern_str.length() && j == max_str_len);
    // 以'%'结尾或pattern中不存在通配符，且上一个相等字符不是空格(即pattern中不含尾部空格)
    if ((end_with_percent || match_without_wildcard) &&
        (-1 == last_equal_idx || pattern_str_buf[last_equal_idx] != ' ')) {
      if (!is_oracle_mode() && match_without_wildcard) {
        // in mysql, all operater will ignore trailing spaces except like.
        // for example, 'abc  ' = 'abc' is true, but 'abc  ' like 'abc' is false
      } else {
        is_precise = true;
      }
    }
  return is_precise;
}

int ObQueryRange::cast_like_obj_if_needed(const ObObj &string_obj,
                                          ObObj &buf_obj,
                                          const ObObj *&obj_ptr,
                                          ObKeyPart &out_key_part,
                                          const ObDataTypeCastParams &dtc_params)
{
  int ret = OB_SUCCESS;
  obj_ptr = &string_obj;
  ObExprResType &col_res_type = out_key_part.pos_.column_type_;
  if (!ObSQLUtils::is_same_type_for_compare(string_obj.get_meta(), col_res_type.get_obj_meta())) {
    ObCastCtx cast_ctx(&allocator_, &dtc_params, CM_WARN_ON_FAIL, col_res_type.get_collation_type());
    ObExpectType expect_type;
    expect_type.set_type(col_res_type.get_type());
    expect_type.set_collation_type(col_res_type.get_collation_type());
    expect_type.set_type_infos(&out_key_part.pos_.get_enum_set_values());
    if (OB_FAIL(ObObjCaster::to_type(expect_type, cast_ctx, string_obj, buf_obj, obj_ptr))) {
      LOG_WARN("cast obj to dest type failed", K(ret), K(string_obj), K(col_res_type));
    }
  }
  return ret;
}

// Notes:
// Since Mysql and Oracle think 0x00 and null is different, they think null is equivalent to '\\'
// but 0x00 is just 0x00. Therefore, before use this interface, PLEASE MAKE SURE escape is not null.
int ObQueryRange::is_precise_like_range(const ObObjParam &pattern, char escape, bool &is_precise)
{
  int ret = OB_SUCCESS;

  ObCollationType cs_type = pattern.get_collation_type();
  int64_t mbmaxlen = 1;
  is_precise = false;
  if (pattern.is_string_type()) {
    ObString pattern_str = pattern.get_string();
    if (cs_type == CS_TYPE_INVALID || cs_type >= CS_TYPE_MAX) {
    }else if (OB_FAIL(ObCharset::get_mbmaxlen_by_coll(cs_type, mbmaxlen))) {
      LOG_WARN("fail to get mbmaxlen", K(ret), K(cs_type), K(escape));
    } else {
      ObArenaAllocator allocator;
      size_t col_len = pattern.get_string_len();
      col_len = static_cast<int32_t>(col_len * mbmaxlen);
      size_t min_str_len = col_len;
      size_t max_str_len = col_len;
      char *min_str_buf = NULL;
      char *max_str_buf = NULL;

      if (col_len == 0) {
        is_precise = true;
      } else if (OB_ISNULL(min_str_buf = (char *)allocator.alloc(min_str_len))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("no enough memory", K(ret), K(col_len), K(min_str_len));
      } else if (OB_ISNULL(max_str_buf = (char *)allocator.alloc(max_str_len))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("no enough memory", K(ret));
      } else if (OB_FAIL(ObCharset::like_range(cs_type, pattern_str, escape,
                                       min_str_buf, &min_str_len,
                                       max_str_buf, &max_str_len))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to retrive like range", K(ret));
      } else {
        is_precise = ObQueryRange::check_like_range_precise(pattern_str,
                                                            static_cast<char*>(max_str_buf),
                                                            max_str_len, escape);
      }
    }
  }

  return ret;
}

int ObQueryRange::get_calculable_expr_val(const ObRawExpr *expr,
                                          ObObj &val,
                                          bool &is_valid,
                                          const bool ignore_error/*default true*/)
{
  int ret = OB_SUCCESS;
  ParamStore dummy_params;
  const ParamStore *params = NULL;
  bool ignore_failure = false;
  if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret));
  } else if (expr->has_flag(CNT_DYNAMIC_PARAM)) {
    is_valid = true;
  } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(query_range_ctx_->exec_ctx_,
                                            expr,
                                            val,
                                            is_valid,
                                            allocator_,
                                            ignore_error && query_range_ctx_->ignore_calc_failure_,
                                            query_range_ctx_->expr_constraints_))) {
    LOG_WARN("failed to calc const or calculable expr", K(ret));
  }
  return ret;
}

int ObQueryRange::add_precise_constraint(const ObRawExpr *expr, bool is_precise)
{
  int ret = OB_SUCCESS;
  PreCalcExprExpectResult expect_result = is_precise ? PreCalcExprExpectResult::PRE_CALC_PRECISE :
                                              PreCalcExprExpectResult::PRE_CALC_NOT_PRECISE;
  ObExprConstraint cons(const_cast<ObRawExpr*>(expr), expect_result);
  if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret));
  } else if (NULL == query_range_ctx_->expr_constraints_) {
    // do nothing
  } else if (OB_FAIL(add_var_to_array_no_dup(*query_range_ctx_->expr_constraints_, cons))) {
    LOG_WARN("failed to add precise constraint");
  }
  return ret;
}

int ObQueryRange::get_final_expr_val(const ObRawExpr *expr, ObObj &val)
{
  int ret = OB_SUCCESS;
  int64_t idx = -1;
  const uint64_t key = reinterpret_cast<const uint64_t>(expr);
  if (OB_ISNULL(query_range_ctx_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("query range context is null");
  } else if (OB_SUCCESS != (ret = query_range_ctx_->final_expr_map_.get_refactored(key, idx))) {
    if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      idx = query_range_ctx_->final_expr_map_.size();
      if (OB_FAIL(query_range_ctx_->final_expr_map_.set_refactored(key, idx))) {
        LOG_WARN("failed to set final expr idx", K(ret));
      }
    } else {
      LOG_WARN("failed to get final expr idx", K(ret));
    }
  } else if (OB_UNLIKELY(idx >= query_range_ctx_->final_expr_map_.size() || idx < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("does not find pullup predicates", K(idx), K(ret));
  }
  if (OB_SUCC(ret)) {
    val.set_unknown(idx);
  }
  return ret;
}

int ObQueryRange::generate_expr_final_info()
{
  int ret = OB_SUCCESS;
  // todo sean.yyj: only cg the remaining final expr in table graph
  RowDesc row_desc;
  expr_final_infos_.reset();
  if (OB_ISNULL(query_range_ctx_) ||  OB_ISNULL(query_range_ctx_->exec_ctx_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("query range context is null");
  } else if (OB_FAIL(expr_final_infos_.prepare_allocate(query_range_ctx_->final_expr_map_.size()))) {
    LOG_WARN("init expr final info failed", K(ret));
  }
  for (auto iter = query_range_ctx_->final_expr_map_.begin();
       OB_SUCC(ret) && iter != query_range_ctx_->final_expr_map_.end(); ++iter) {
    const ObRawExpr *expr = reinterpret_cast<const ObRawExpr*>(iter->first);
    int64_t expr_idx = iter->second;
    ObTempExpr *temp_expr = NULL;
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr is null", K(ret));
    } else if (OB_UNLIKELY(expr_idx >= expr_final_infos_.count() || expr_idx < 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid expr idx", K(expr_idx), K(ret));
    } else if (T_QUESTIONMARK == expr->get_expr_type()) {
      const ObConstRawExpr *const_expr = static_cast<const ObConstRawExpr *>(expr);
      int64_t param_idx = OB_INVALID_ID;
      ObObj val = const_expr->get_value();
      if (OB_UNLIKELY(!val.is_unknown())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected value type", K(val), K(ret));
      } else {
        val.get_unknown(expr_final_infos_.at(expr_idx).param_idx_);
        expr_final_infos_.at(expr_idx).is_param_ = true;
        expr_final_infos_.at(expr_idx).cnt_exec_param_ = expr->has_flag(CNT_DYNAMIC_PARAM);
      }
    } else if (OB_FAIL(ObStaticEngineExprCG::gen_expr_with_row_desc(expr,
                                                    row_desc,
                                                    query_range_ctx_->exec_ctx_->get_allocator(),
                                                    query_range_ctx_->exec_ctx_->get_my_session(),
                                                    temp_expr))) {
      LOG_WARN("failed to generate expr with row desc", K(ret));
    } else {
      expr_final_infos_.at(expr_idx).temp_expr_ = temp_expr;
      expr_final_infos_.at(expr_idx).expr_exists_ = true;
      expr_final_infos_.at(expr_idx).cnt_exec_param_ = expr->has_flag(CNT_DYNAMIC_PARAM);
    }
  }
  return ret;
}

DEF_TO_STRING(ObQueryRange::ObRangeExprItem)
{
  int64_t pos = 0;
  J_OBJ_START();
  J_KV(K_(cur_expr), K_(cur_pos));
  J_OBJ_END();
  return pos;
}

}  // namespace sql
}  // namespace oceanbase
