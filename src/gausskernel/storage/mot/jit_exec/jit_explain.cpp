/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * jit_explain.cpp
 *    Implementation of JIT explain plan.
 *
 * IDENTIFICATION
 *    src/gausskernel/storage/mot/jit_exec/jit_explain.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "global.h"
#include "jit_plan.h"
#include "jit_common.h"
#include "mot_engine.h"
#include "utilities.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"
#include "mot_internal.h"

namespace JitExec {
DECLARE_LOGGER(JitExplain, JitExec)

// Forward declarations
static void ExplainExpr(Query* query, JitPlan* plan, JitExpr* jitExpr);
static void ExplainPointQueryPlan(Query* query, JitPointQueryPlan* plan, bool isSubQuery = false);
static void ExplainRangeSelectPlan(Query* query, JitRangeSelectPlan* plan, bool isSubQuery = false);

static void ExplainConstExpr(JitConstExpr* expr)
{
    if (expr->_is_null) {  // special case of default parameters
        PrintDatum(MOT::LogLevel::LL_TRACE, expr->_const_type, PointerGetDatum(nullptr), true);
    } else {
        MOT_ASSERT(expr->_source_expr != nullptr);
        MOT_ASSERT(expr->_source_expr->type == T_Const);
        Const* constExpr = (Const*)expr->_source_expr;
        PrintDatum(MOT::LogLevel::LL_TRACE, constExpr->consttype, constExpr->constvalue, constExpr->constisnull);
    }
}

static void ExplainRealColumnName(const Query* query, int tableRefId, int columnId)
{
    MOT::TxnManager* currTxn = GetSafeTxn(__FUNCTION__);
    MOT_ASSERT(currTxn != nullptr);
    if (tableRefId <= list_length(query->rtable)) {  // varno index is 1-based
        RangeTblEntry* rte = (RangeTblEntry*)list_nth(query->rtable, tableRefId - 1);
        if (rte->rtekind == RTE_RELATION) {
            MOT::Table* table = currTxn->GetTableByExternalId(rte->relid);
            if (table != nullptr) {
                if (list_length(query->rtable) == 1) {
                    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "%s", table->GetFieldName(columnId));
                } else {
                    MOT_LOG_APPEND(
                        MOT::LogLevel::LL_TRACE, "%s.%s", table->GetTableName().c_str(), table->GetFieldName(columnId));
                }
                return;
            }
        } else if (rte->rtekind == RTE_JOIN) {
            Var* aliasVar = (Var*)list_nth(rte->joinaliasvars, columnId - 1);  // this is zero-based!
            tableRefId = aliasVar->varno;
            if (tableRefId <= list_length(query->rtable)) {  // tableRefId is 1-based
                rte = (RangeTblEntry*)list_nth(query->rtable, tableRefId - 1);
                MOT::Table* table = currTxn->GetTableByExternalId(rte->relid);
                if (table != nullptr) {
                    // take real column id and not column id from virtual join table
                    columnId = aliasVar->varattno;
                    MOT_LOG_APPEND(
                        MOT::LogLevel::LL_TRACE, "%s.%s", table->GetTableName().c_str(), table->GetFieldName(columnId));
                    return;
                }
            }
        }
    }
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "UnknownColumn");
}

static void ExplainVarExpr(Query* query, JitVarExpr* expr)
{
    MOT_ASSERT((expr->_source_expr->type == T_Var) || (expr->_source_expr->type == T_RelabelType));
    Var* varExpr = nullptr;
    if (expr->_source_expr->type == T_Var) {
        varExpr = (Var*)expr->_source_expr;
    } else if (expr->_source_expr->type == T_RelabelType) {
        RelabelType* relabelType = (RelabelType*)expr->_source_expr;
        Expr* subExpr = relabelType->arg;
        if (subExpr->type == T_Var) {
            varExpr = (Var*)subExpr;
        }
    }
    if (varExpr != nullptr) {
        int columnId = varExpr->varattno;
        int tableRefId = varExpr->varno;
        ExplainRealColumnName(query, tableRefId, columnId);
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "[var]");
    }
}

static void ExplainParamExpr(JitParamExpr* expr)
{
    MOT_ASSERT((expr->_source_expr->type == T_Param) || (expr->_source_expr->type == T_RelabelType));
    Param* paramExpr = nullptr;
    if (expr->_source_expr->type == T_Param) {
        paramExpr = (Param*)expr->_source_expr;
    } else if (expr->_source_expr->type == T_RelabelType) {
        RelabelType* relabelType = (RelabelType*)expr->_source_expr;
        Expr* subExpr = relabelType->arg;
        if (subExpr->type == T_Param) {
            paramExpr = (Param*)subExpr;
        }
    }
    if (paramExpr != nullptr) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "$%d", paramExpr->paramid);
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "[param]");
    }
}

static void ExplainPGFunction(Query* query, JitPlan* plan, Oid functionId, JitExpr** args, int argCount)
{
    // get PG function name
    char* functionName = get_func_name(functionId);
    if (functionName != nullptr) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "<%s %u>(", functionName, (unsigned)functionId);
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "<function %u>(", (unsigned)functionId);
    }
    pfree(functionName);

    for (int i = 0; i < argCount; ++i) {
        ExplainExpr(query, plan, args[i]);
        if ((i + 1) < argCount) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
    }
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
}

static void ExplainOpExpr(Query* query, JitPlan* plan, JitOpExpr* expr)
{
    ExplainPGFunction(query, plan, expr->_op_func_id, expr->_args, expr->_arg_count);
}

static void ExplainFuncExpr(Query* query, JitPlan* plan, JitFuncExpr* expr)
{
    ExplainPGFunction(query, plan, expr->_func_id, expr->_args, expr->_arg_count);
}

static void ExplainSubLinkExpr(Query* query, JitPlan* plan, JitSubLinkExpr* subLinkExpr)
{
    // currently we support only one sub-query (so we don't use sub-query plan index)
    if ((plan != nullptr) && (plan->_plan_type == JIT_PLAN_COMPOUND)) {
        JitCompoundPlan* compoundPlan = (JitCompoundPlan*)plan;
        SubLink* subLink = (SubLink*)subLinkExpr->_source_expr;
        Query* subQuery = (Query*)subLink->subselect;
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "[");
        JitPlan* subQueryPlan = compoundPlan->_sub_query_plans[subLinkExpr->_sub_query_index];
        if (subQueryPlan->_plan_type == JIT_PLAN_POINT_QUERY) {
            ExplainPointQueryPlan(subQuery, (JitPointQueryPlan*)subQueryPlan, true);
        } else if (subQueryPlan->_plan_type == JIT_PLAN_RANGE_SCAN) {
            ExplainRangeSelectPlan(subQuery, (JitRangeSelectPlan*)subQueryPlan, true);
        } else {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "sub-query");
        }
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "]");
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "[sub-query]");
    }
}

static void ExplainScalarArrayOpExpr(Query* query, JitPlan* plan, JitScalarArrayOpExpr* expr)
{
    ExplainExpr(query, plan, expr->m_scalar);
    if (expr->m_useOr) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " IN ANY ");
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " IN ALL ");
    }

    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "[");
    for (int i = 0; i < expr->m_arraySize; ++i) {
        ExplainExpr(query, plan, expr->m_arrayElements[i]);
        if ((i + 1) < expr->m_arraySize) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
    }
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "]");
}

static void ExplainExpr(Query* query, JitPlan* plan, JitExpr* expr)
{
    switch (expr->_expr_type) {
        case JIT_EXPR_TYPE_CONST:
            ExplainConstExpr((JitConstExpr*)expr);
            break;

        case JIT_EXPR_TYPE_VAR:
            ExplainVarExpr(query, (JitVarExpr*)expr);
            break;

        case JIT_EXPR_TYPE_PARAM:
            ExplainParamExpr((JitParamExpr*)expr);
            break;

        case JIT_EXPR_TYPE_OP:
            ExplainOpExpr(query, plan, (JitOpExpr*)expr);
            break;

        case JIT_EXPR_TYPE_FUNC:
            ExplainFuncExpr(query, plan, (JitFuncExpr*)expr);
            break;

        case JIT_EXPR_TYPE_SUBLINK:
            ExplainSubLinkExpr(query, plan, (JitSubLinkExpr*)expr);
            break;

        case JIT_EXPR_TYPE_SCALAR_ARRAY_OP:
            ExplainScalarArrayOpExpr(query, plan, (JitScalarArrayOpExpr*)expr);
            break;

        default:
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "[expr]");
            break;
    }
}

static const char* JitWhereOperatorClassToString(JitWhereOperatorClass opClass)
{
    switch (opClass) {
        case JIT_WOC_EQUALS:
            return "==";
        case JIT_WOC_LESS_THAN:
            return "<";
        case JIT_WOC_GREATER_THAN:
            return ">";
        case JIT_WOC_LESS_EQUALS:
            return "<=";
        case JIT_WOC_GREATER_EQUALS:
            return ">=";
        default:
            return "??";
    }
}

static void ExplainFilterArray(Query* query, JitPlan* plan, int indent, JitFilterArray* filterArray,
    bool isSubQuery = false, bool isOneTime = false)
{
    if (!isSubQuery) {
        if (isOneTime) {
            MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sONE-TIME FILTER ON (", indent, "");
        } else {
            MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sFILTER ON (", indent, "");
        }
    } else {
        if (isOneTime) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " FILTER ON (");
        } else {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " ONE-TIME FILTER ON (");
        }
    }
    for (int i = 0; i < filterArray->_filter_count; ++i) {
        ExplainExpr(query, plan, filterArray->_scan_filters[i]);
        if (i < (filterArray->_filter_count - 1)) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " AND ");
        }
    }
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    if (!isSubQuery) {
        MOT_LOG_END(MOT::LogLevel::LL_TRACE);
    }
}

static void ExplainExprArray(
    Query* query, JitPlan* plan, JitColumnExprArray* exprArray, const char* sep, bool forSelect = false)
{
    for (int i = 0; i < exprArray->_count; ++i) {
        if (!forSelect) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
                "%s = ",
                exprArray->_exprs[i]._table->GetFieldName(exprArray->_exprs[i]._table_column_id));
        }
        ExplainExpr(query, plan, exprArray->_exprs[i]._expr);
        if (i < (exprArray->_count - 1)) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, sep);
        }
    }
}

static void ExplainInsertExprArray(Query* query, JitPlan* plan, int indent, JitColumnExprArray* exprArray)
{
    MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sVALUES (", indent, "");
    ExplainExprArray(query, plan, exprArray, ", ");
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    MOT_LOG_END(MOT::LogLevel::LL_TRACE);
}

static void ExplainUpdateExprArray(Query* query, JitPlan* plan, int indent, JitColumnExprArray* exprArray)
{
    MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sSET (", indent, "");
    ExplainExprArray(query, plan, exprArray, ", ");
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    MOT_LOG_END(MOT::LogLevel::LL_TRACE);
}

static void ExplainSelectExprArray(Query* query, JitPlan* plan, JitSelectExprArray* exprArray)
{
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "(");
    for (int i = 0; i < exprArray->_count; ++i) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "%%%d = ", exprArray->_exprs[i]._tuple_column_id);
        ExplainExpr(query, plan, (JitExpr*)exprArray->_exprs[i]._expr);
        if (i < (exprArray->_count - 1)) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
    }
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
}

static void ExplainSearchExprArray(
    Query* query, JitPlan* plan, int indent, JitColumnExprArray* exprArray, bool isSubQuery)
{
    if (!isSubQuery) {
        MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sWHERE (", indent, "");
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " WHERE (");
    }
    ExplainExprArray(query, plan, exprArray, " AND ", false);
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    if (!isSubQuery) {
        MOT_LOG_END(MOT::LogLevel::LL_TRACE);
    }
}

static void ExplainInsertPlan(Query* query, JitInsertPlan* plan)
{
    MOT_LOG_TRACE("[Plan] INSERT into table %s", plan->_table->GetTableName().c_str());
    ExplainInsertExprArray(query, (JitPlan*)plan, 2, &plan->_insert_exprs);
}

static void ExplainUpdateQueryPlan(Query* query, JitUpdatePlan* plan, int indent)
{
    MOT_LOG_TRACE("%*sUPDATE table %s:", indent, "", plan->_query._table->GetTableName().c_str());
    ExplainUpdateExprArray(query, (JitPlan*)plan, indent + 2, &plan->_update_exprs);
}

static void ExplainDeleteQueryPlan(JitDeletePlan* plan, int indent)
{
    MOT_LOG_TRACE("%*sDELETE from table %s:", indent, "", plan->_query._table->GetTableName().c_str());
}

static void ExplainSelectQueryPlan(Query* query, JitSelectPlan* plan, int indent, bool isSubQuery)
{
    if (isSubQuery) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " SELECT from table %s:", plan->_query._table->GetTableName().c_str());
    } else {
        MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE,
            "%*sSELECT from table %s:",
            indent,
            "",
            plan->_query._table->GetTableName().c_str());
    }
    ExplainSelectExprArray(query, (JitPlan*)plan, &plan->_select_exprs);
    if (!isSubQuery) {
        MOT_LOG_END(MOT::LogLevel::LL_TRACE);
    }
}

static void ExplainPointQueryPlan(Query* query, JitPointQueryPlan* plan, bool isSubQuery /* = false */)
{
    if (!isSubQuery) {
        MOT_LOG_TRACE("[Plan] Point Query");
    }
    int indent = 0;
    if (plan->_query.m_oneTimeFilters._filter_count > 0) {
        ExplainFilterArray(query, (JitPlan*)plan, indent, &plan->_query.m_oneTimeFilters, isSubQuery, true);
        indent += 2;
    }
    if (plan->_query._filters._filter_count > 0) {
        ExplainFilterArray(query, (JitPlan*)plan, indent, &plan->_query._filters, isSubQuery);
        indent += 2;
    }
    switch (plan->_command_type) {
        case JIT_COMMAND_UPDATE:
            ExplainUpdateQueryPlan(query, (JitUpdatePlan*)plan, indent + 2);
            break;

        case JIT_COMMAND_DELETE:
            ExplainDeleteQueryPlan((JitDeletePlan*)plan, indent + 2);
            break;

        case JIT_COMMAND_SELECT:
            ExplainSelectQueryPlan(query, (JitSelectPlan*)plan, indent + 2, isSubQuery);
            break;

        default:
            MOT_LOG_TRACE("Invalid plan command type");
            return;
    }

    ExplainSearchExprArray(query, (JitPlan*)plan, indent + 4, &plan->_query._search_exprs, isSubQuery);
}

static void ExplainIndexExprArray(Query* query, JitPlan* plan, JitIndexScan* indexScan, int count)
{
    for (int i = 0; i < count; ++i) {
        if (indexScan->_search_exprs._exprs[i]._join_expr) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "JoinExpr(");
        }
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
            "%s = ",
            indexScan->_table->GetFieldName(indexScan->_search_exprs._exprs[i]._table_column_id));
        ExplainExpr(query, plan, indexScan->_search_exprs._exprs[i]._expr);
        if (indexScan->_search_exprs._exprs[i]._join_expr) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
        }
        if (i < (count - 1)) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " AND ");
        }
    }
}

static void ExplainIndexOpenExpr(
    Query* query, JitPlan* plan, JitIndexScan* indexScan, int index, JitWhereOperatorClass opClass)
{
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
        "%s",
        indexScan->_table->GetFieldName(indexScan->_search_exprs._exprs[index]._table_column_id));
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " %s ", JitWhereOperatorClassToString(opClass));
    ExplainExpr(query, plan, indexScan->_search_exprs._exprs[index]._expr);
}

static void ExplainIndexExprArray(Query* query, JitPlan* plan, JitIndexScan* indexScan)
{
    if ((indexScan->_scan_type == JIT_INDEX_SCAN_CLOSED) || (indexScan->_scan_type == JIT_INDEX_SCAN_POINT)) {
        ExplainIndexExprArray(query, plan, indexScan, indexScan->_search_exprs._count);
    } else if (indexScan->_scan_type == JIT_INDEX_SCAN_SEMI_OPEN) {
        if (indexScan->_search_exprs._count > 1) {
            ExplainIndexExprArray(query, plan, indexScan, indexScan->_search_exprs._count - 1);
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " AND ");
        }
        ExplainIndexOpenExpr(query, plan, indexScan, indexScan->_search_exprs._count - 1, indexScan->_last_dim_op1);
    } else if (indexScan->_scan_type == JIT_INDEX_SCAN_OPEN) {
        if (indexScan->_search_exprs._count > 2) {
            ExplainIndexExprArray(query, plan, indexScan, indexScan->_search_exprs._count - 2);
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " AND ");
        }
        ExplainIndexOpenExpr(query, plan, indexScan, indexScan->_search_exprs._count - 2, indexScan->_last_dim_op1);
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " AND ");
        ExplainIndexOpenExpr(query, plan, indexScan, indexScan->_search_exprs._count - 1, indexScan->_last_dim_op2);
    }
}

static const char* JitQuerySortOrderToString(JitQuerySortOrder sortOrder)
{
    switch (sortOrder) {
        case JIT_QUERY_SORT_ASCENDING:
            return "Ascending";
        case JIT_QUERY_SORT_DESCENDING:
            return "Descending";
        case JIT_QUERY_SORT_INVALID:
        default:
            return "Invalid-Sort-Order";
    }
}

static const char* JitIndexScanTypeToString(JitIndexScanType scanType)
{
    switch (scanType) {
        case JIT_INDEX_SCAN_CLOSED:
            return "Closed-Scan";
        case JIT_INDEX_SCAN_OPEN:
            return "Open-Scan";
        case JIT_INDEX_SCAN_SEMI_OPEN:
            return "Semi-Open-Scan";
        case JIT_INDEX_SCAN_POINT:
            return "Point-Scan";
        case JIT_INDEX_SCAN_FULL:
            return "Full-Scan";
        default:
            return "Invalid-Scan-Type";
    }
}

static const char* JitIndexScanDirectionToString(JitIndexScanDirection scanDirection)
{
    switch (scanDirection) {
        case JIT_INDEX_SCAN_FORWARD:
            return "Forward-Scan";
        case JIT_INDEX_SCAN_BACKWARDS:
            return "Backwards-Scan";
        default:
            return "Invalid-Scan-Direction";
    }
}

static void ExplainIndexScan(Query* query, JitPlan* plan, int indent, JitIndexScan* indexScan,
    const char* scanName = "", bool isSubQuery = false)
{
    if (indexScan->m_oneTimeFilters._filter_count > 0) {
        ExplainFilterArray(query, plan, indent, &indexScan->m_oneTimeFilters, isSubQuery, true);
        indent += 2;
    }
    if (indexScan->_filters._filter_count > 0) {
        ExplainFilterArray(query, plan, indent, &indexScan->_filters, isSubQuery);
        indent += 2;
    }
    MOT::Index* index = indexScan->_index;
    if (isSubQuery) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
            "%s%sSCAN %s index %s (%s, %s) ON (",
            scanName,
            scanName[0] ? " " : "",
            index->GetName().c_str(),
            JitQuerySortOrderToString(indexScan->_sort_order),
            JitIndexScanTypeToString(indexScan->_scan_type),
            JitIndexScanDirectionToString(indexScan->_scan_direction));
    } else {
        MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE,
            "%*s%s%sSCAN %s index %s (%s, %s) ON (",
            indent,
            "",
            scanName,
            scanName[0] ? " " : "",
            index->GetName().c_str(),
            JitQuerySortOrderToString(indexScan->_sort_order),
            JitIndexScanTypeToString(indexScan->_scan_type),
            JitIndexScanDirectionToString(indexScan->_scan_direction));
    }

    ExplainIndexExprArray(query, plan, indexScan);
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    if (!isSubQuery) {
        MOT_LOG_END(MOT::LogLevel::LL_TRACE);
    }
}

static void ExplainRangeUpdatePlan(Query* query, JitRangeUpdatePlan* plan)
{
    MOT_LOG_TRACE("[Plan] Range UPDATE table %s:", plan->_index_scan._table->GetTableName().c_str());
    ExplainUpdateExprArray(query, (JitPlan*)plan, 2, &plan->_update_exprs);
    ExplainIndexScan(query, (JitPlan*)plan, 2, &plan->_index_scan);
}

static const char* JitAggregateOperatorToString(JitAggregateOperator aggregateOp)
{
    switch (aggregateOp) {
        case JIT_AGGREGATE_AVG:
            return "AVG";
        case JIT_AGGREGATE_SUM:
            return "SUM";
        case JIT_AGGREGATE_MAX:
            return "MAX";
        case JIT_AGGREGATE_MIN:
            return "MIN";
        case JIT_AGGREGATE_COUNT:
            return "COUNT";
        default:
            return "AGG-Unknown";
    }
}

static void ExplainAggregateOperator(int indent, const JitAggregate* aggregate, bool isSubQuery = false)
{
    if (isSubQuery) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
            "%s [op %d](",
            JitAggregateOperatorToString(aggregate->_aggreaget_op),
            aggregate->_func_id);
    } else {
        MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE,
            "%*s%s [op %d](",
            indent,
            "",
            JitAggregateOperatorToString(aggregate->_aggreaget_op),
            aggregate->_func_id);
    }
    if (aggregate->_distinct) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "DISTINCT(");
    }
    if (aggregate->_table == nullptr) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "*)");
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
            "%s.%s type %d)",
            aggregate->_table->GetTableName().c_str(),
            aggregate->_table->GetFieldName(aggregate->_table_column_id),
            aggregate->_element_type);
    }
    if (aggregate->_distinct) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    }
    if (!isSubQuery) {
        MOT_LOG_END(MOT::LogLevel::LL_TRACE);
    }
}

static void ExplainColumnName(Query* query, JitExpr* expr)
{
    if (expr->_expr_type == JIT_EXPR_TYPE_VAR) {
        Var* varExpr = (Var*)expr->_source_expr;
        int columnId = varExpr->varattno;
        int tableRefId = varExpr->varno;
        ExplainRealColumnName(query, tableRefId, columnId);
    }
}

static void ExplainRangeSelectSort(Query* query, JitRangeSelectPlan* plan, int indent)
{
    JitNonNativeSortParams* sortParams = plan->m_nonNativeSortParams;
    MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sSORT ", indent, "");
    if (ScanDirectionIsBackward(sortParams->scanDir)) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "DESC");
    } else if (ScanDirectionIsForward(sortParams->scanDir)) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "ASC");
    }
    for (int i = 0; i < sortParams->numCols; ++i) {
        AttrNumber colId = sortParams->sortColIdx[i];
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " {Column %d [", colId);
        ExplainColumnName(query, plan->_select_exprs._exprs[colId - 1]._expr);
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE,
            "], SortOp %u, Collation %u, nulls-first %s}",
            sortParams->sortOperators[i],
            sortParams->collations[i],
            sortParams->nullsFirst[i] ? "true" : "false");
        if (i + 1 < plan->m_nonNativeSortParams->numCols) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ",");
        }
    }
    if (plan->m_nonNativeSortParams->bound == 0) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " no bound");
    } else {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " bound %" PRIu64, plan->m_nonNativeSortParams->bound);
    }
    MOT_LOG_END(MOT::LogLevel::LL_TRACE);
}

static void ExplainRangeSelectPlan(Query* query, JitRangeSelectPlan* plan, bool isSubQuery /* = false */)
{
    if (isSubQuery) {
        MOT_LOG_APPEND(
            MOT::LogLevel::LL_TRACE, "Range SELECT from table %s:", plan->_index_scan._table->GetTableName().c_str());
    } else {
        MOT_LOG_TRACE("[Plan] Range SELECT from table %s:", plan->_index_scan._table->GetTableName().c_str());
    }
    int indent = 0;
    for (int aggIndex = 0; aggIndex < plan->m_aggCount; ++aggIndex) {
        indent += 2;
        ExplainAggregateOperator(indent, &plan->m_aggregates[aggIndex], isSubQuery);
        if (aggIndex + 1 < plan->m_aggCount) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
        indent -= 2;
    }
    if (plan->_limit_count > 0) {
        if (isSubQuery) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, " LIMIT %d", plan->_limit_count);
        } else {
            indent += 2;
            MOT_LOG_TRACE("%*sLIMIT %d", indent, "", plan->_limit_count);
        }
    }
    if (plan->m_nonNativeSortParams != nullptr) {
        indent += 2;
        ExplainRangeSelectSort(query, plan, indent);
    }
    if (plan->m_aggCount == 0) {
        if (isSubQuery) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "%*sSELECT", indent, "");
            ExplainSelectExprArray(query, (JitPlan*)plan, &plan->_select_exprs);
        } else {
            indent += 2;
            MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sSELECT", indent, "");
            ExplainSelectExprArray(query, (JitPlan*)plan, &plan->_select_exprs);
            MOT_LOG_END(MOT::LogLevel::LL_TRACE);
        }
    }

    ExplainIndexScan(query, (JitPlan*)plan, indent + 2, &plan->_index_scan, "", isSubQuery);
}

static void ExplainRangeDeletePlan(Query* query, JitRangeDeletePlan* plan)
{
    MOT_LOG_TRACE("[Plan] Range DELETE table %s:", plan->_index_scan._table->GetTableName().c_str());
    ExplainIndexScan(query, (JitPlan*)plan, 2, &plan->_index_scan);
}

static void ExplainRangeScanPlan(Query* query, JitRangeScanPlan* plan)
{
    if (plan->_command_type == JIT_COMMAND_UPDATE) {
        ExplainRangeUpdatePlan(query, (JitRangeUpdatePlan*)plan);
    } else if (plan->_command_type == JIT_COMMAND_SELECT) {
        ExplainRangeSelectPlan(query, (JitRangeSelectPlan*)plan);
    } else if (plan->_command_type == JIT_COMMAND_DELETE) {
        ExplainRangeDeletePlan(query, (JitRangeDeletePlan*)plan);
    } else {
        MOT_LOG_TRACE("Invalid plan command type");
        return;
    }
}

static const char* JitJoinTypeToString(JitJoinType joinType)
{
    switch (joinType) {
        case JitJoinType::JIT_JOIN_INNER:
            return "INNER";
        case JitJoinType::JIT_JOIN_LEFT:
            return "LEFT";
        case JitJoinType::JIT_JOIN_FULL:
            return "FULL";
        case JitJoinType::JIT_JOIN_RIGHT:
            return "RIGHT";
        case JitJoinType::JIT_JOIN_INVALID:
        default:
            return "Invalid";
    }
}

static const char* JitJoinScanTypeToString(JitJoinScanType scanType)
{
    switch (scanType) {
        case JIT_JOIN_SCAN_POINT:
            return "Point-Join";
        case JIT_JOIN_SCAN_OUTER_POINT:
            return "Outer-Point-Join";
        case JIT_JOIN_SCAN_INNER_POINT:
            return "Inner-Point-Join";
        case JIT_JOIN_SCAN_RANGE:
            return "Range-Join";
        case JIT_JOIN_SCAN_INVALID:
        default:
            return "Invalid-Join-Scan";
    }
}

static void ExplainJoinPlan(Query* query, JitJoinPlan* plan)
{
    MOT_LOG_TRACE("[Plan] %s JOIN table %s on table %s (%s)",
        JitJoinTypeToString(plan->_join_type),
        plan->_outer_scan._table->GetTableName().c_str(),
        plan->_inner_scan._table->GetTableName().c_str(),
        JitJoinScanTypeToString(plan->_scan_type));
    int indent = 0;
    for (int aggIndex = 0; aggIndex < plan->m_aggCount; ++aggIndex) {
        indent += 2;
        ExplainAggregateOperator(indent, &plan->m_aggregates[aggIndex]);
        if (aggIndex + 1 < plan->m_aggCount) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
        indent -= 2;
    }
    if (plan->m_aggCount == 0) {
        indent += 2;
        MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "%*sSELECT", indent, "");
        ExplainSelectExprArray(query, (JitPlan*)plan, &plan->_select_exprs);
        MOT_LOG_END(MOT::LogLevel::LL_TRACE);
    }
    if (plan->_limit_count > 0) {
        indent += 2;
        MOT_LOG_TRACE("%*sLIMIT %d", indent, "", plan->_limit_count);
    }
    ExplainIndexScan(query, (JitPlan*)plan, indent + 2, &plan->_outer_scan, "OUTER");
    ExplainIndexScan(query, (JitPlan*)plan, indent + 2, &plan->_inner_scan, "INNER");
}

static void ExplainCompoundPlan(Query* query, JitCompoundPlan* plan)
{
    MOT_LOG_TRACE("[Plan] Compound Query on table %s", plan->_outer_query_plan->_query._table->GetTableName().c_str());
    int indent = 0;
    if (plan->_outer_query_plan->_query._filters._filter_count > 0) {
        ExplainFilterArray(query, (JitPlan*)plan, indent, &plan->_outer_query_plan->_query._filters, false);
        indent += 2;
    }
    if (plan->_command_type == JIT_COMMAND_SELECT) {
        ExplainSelectQueryPlan(query, (JitSelectPlan*)plan->_outer_query_plan, indent + 2, false);
    }

    ExplainSearchExprArray(query, (JitPlan*)plan, indent + 4, &plan->_outer_query_plan->_query._search_exprs, false);
}

static void ExplainInvokePlan(Query* query, JitInvokePlan* plan)
{
    MOT_LOG_TRACE("[Plan] Invoke stored procedure:");
    MOT_LOG_BEGIN(MOT::LogLevel::LL_TRACE, "  %s[id=%u] (", plan->_function_name, plan->_function_id);
    for (int i = 0; i < plan->_arg_count; ++i) {
        ExplainExpr(query, (JitPlan*)plan, plan->_args[i]);
        if (i < plan->_arg_count - 1) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
    }
    if ((plan->_arg_count > 0) && (plan->m_defaultParamCount > 0)) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
    }
    for (int i = 0; i < plan->m_defaultParamCount; ++i) {
        MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "<DEFAULT> ");
        if (plan->m_defaultParams[i] == nullptr) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, "null");
        } else {
            ExplainExpr(query, (JitPlan*)plan, plan->m_defaultParams[i]);
        }
        if (i < plan->m_defaultParamCount - 1) {
            MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ", ");
        }
    }
    MOT_LOG_APPEND(MOT::LogLevel::LL_TRACE, ")");
    MOT_LOG_END(MOT::LogLevel::LL_TRACE);
}

extern void JitExplainPlan(Query* query, JitPlan* plan)
{
    if (plan != nullptr) {
        switch (plan->_plan_type) {
            case JIT_PLAN_INSERT_QUERY:
                ExplainInsertPlan(query, (JitInsertPlan*)plan);
                break;

            case JIT_PLAN_POINT_QUERY:
                ExplainPointQueryPlan(query, (JitPointQueryPlan*)plan);
                break;

            case JIT_PLAN_RANGE_SCAN:
                ExplainRangeScanPlan(query, (JitRangeScanPlan*)plan);
                break;

            case JIT_PLAN_JOIN:
                ExplainJoinPlan(query, (JitJoinPlan*)plan);
                break;

            case JIT_PLAN_COMPOUND:
                ExplainCompoundPlan(query, (JitCompoundPlan*)plan);
                break;

            case JIT_PLAN_INVOKE:
                ExplainInvokePlan(query, (JitInvokePlan*)plan);
                break;

            case JIT_PLAN_INVALID:
            default:
                break;
        }
    }
}

extern void JitExplainPlan(PLpgSQL_function* function, JitPlan* plan)
{
    // not implemented yet
}
}  // namespace JitExec
