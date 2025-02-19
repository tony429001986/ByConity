/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#include <Interpreters/Context.h>
#include <Interpreters/DatabaseAndTableWithAlias.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/InDepthNodeVisitor.h>
#include <Interpreters/InJoinSubqueriesPreprocessor.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Storages/DataLakes/StorageCnchLakeBase.h>
#include <Storages/StorageCnchMergeTree.h>
#include <Storages/StorageDistributed.h>
#include <Common/typeid_cast.h>
#include <Storages/RemoteFile/IStorageCnchFile.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int DISTRIBUTED_IN_JOIN_SUBQUERY_DENIED;
    extern const int LOGICAL_ERROR;
}


namespace
{

StoragePtr tryGetTable(const ASTPtr & database_and_table, ContextPtr context)
{
    auto table_id = context->tryResolveStorageID(database_and_table);
    if (!table_id)
        return {};
    return DatabaseCatalog::instance().tryGetTable(table_id, context);
}

using CheckShardsAndTables = InJoinSubqueriesPreprocessor::CheckShardsAndTables;

struct NonGlobalTableData : public WithContext
{
    using TypeToVisit = ASTTableExpression;

    NonGlobalTableData(
        ContextPtr context_,
        const CheckShardsAndTables & checker_,
        std::vector<ASTPtr> & renamed_tables_,
        ASTFunction * function_,
        ASTTableJoin * table_join_)
        : WithContext(context_), checker(checker_), renamed_tables(renamed_tables_), function(function_), table_join(table_join_)
    {
    }

    const CheckShardsAndTables & checker;
    std::vector<ASTPtr> & renamed_tables;
    ASTFunction * function = nullptr;
    ASTTableJoin * table_join = nullptr;

    void visit(ASTTableExpression & node, ASTPtr &)
    {
        ASTPtr & database_and_table = node.database_and_table_name;
        if (database_and_table)
            renameIfNeeded(database_and_table);
    }

private:
    void renameIfNeeded(ASTPtr & database_and_table)
    {
        const DistributedProductMode distributed_product_mode = getContext()->getSettingsRef().distributed_product_mode;

        /// Convert distributed table to corresponding remote table.
        if (distributed_product_mode == DistributedProductMode::LOCAL)
        {
            StoragePtr storage = tryGetTable(database_and_table, getContext());
            if (!storage || !checker.hasAtLeastTwoShards(*storage))
                return;

            std::string database;
            std::string table;
            std::tie(database, table) = checker.getRemoteDatabaseAndTableName(*storage);

            String alias = database_and_table->tryGetAlias();
            if (alias.empty())
                throw Exception("Distributed table should have an alias when distributed_product_mode set to local",
                                ErrorCodes::DISTRIBUTED_IN_JOIN_SUBQUERY_DENIED);

            auto & identifier = database_and_table->as<ASTTableIdentifier &>();
            renamed_tables.emplace_back(identifier.clone());
            identifier.resetTable(database, table);
        }
        else if (getContext()->getSettingsRef().prefer_global_in_and_join || distributed_product_mode == DistributedProductMode::GLOBAL)
        {
            if (function)
            {
                auto * concrete = function->as<ASTFunction>();

                if (concrete->name == "in")
                    concrete->name = "globalIn";
                else if (concrete->name == "notIn")
                    concrete->name = "globalNotIn";
                else if (concrete->name == "globalIn" || concrete->name == "globalNotIn")
                {
                    /// Already processed.
                }
                else
                    throw Exception("Logical error: unexpected function name " + concrete->name, ErrorCodes::LOGICAL_ERROR);
            }
            else if (table_join)
                table_join->locality = ASTTableJoin::Locality::Global;
            else
                throw Exception("Logical error: unexpected AST node", ErrorCodes::LOGICAL_ERROR);
        }
        else if (distributed_product_mode == DistributedProductMode::DENY)
        {
            throw Exception("Double-distributed IN/JOIN subqueries is denied (distributed_product_mode = 'deny')."
                " You may rewrite query to use local tables in subqueries, or use GLOBAL keyword, or set distributed_product_mode to suitable value.",
                ErrorCodes::DISTRIBUTED_IN_JOIN_SUBQUERY_DENIED);
        }
        else
            throw Exception("InJoinSubqueriesPreprocessor: unexpected value of 'distributed_product_mode' setting",
                            ErrorCodes::LOGICAL_ERROR);
    }
};

using NonGlobalTableMatcher = OneTypeMatcher<NonGlobalTableData>;
using NonGlobalTableVisitor = InDepthNodeVisitor<NonGlobalTableMatcher, true>;


class NonGlobalSubqueryMatcher
{
public:
    struct Data : public WithContext
    {
        using RenamedTables = std::vector<std::pair<ASTPtr, std::vector<ASTPtr>>>;

        Data(ContextPtr context_, const CheckShardsAndTables & checker_, RenamedTables & renamed_tables_)
        : WithContext(context_), checker(checker_), renamed_tables(renamed_tables_)
        {
        }

        const CheckShardsAndTables & checker;
        RenamedTables & renamed_tables;
    };

    static void visit(ASTPtr & node, Data & data)
    {
        if (auto * function = node->as<ASTFunction>())
            visit(*function, node, data);
        if (const auto * tables = node->as<ASTTablesInSelectQueryElement>())
            visit(*tables, node, data);
    }

    static bool needChildVisit(ASTPtr & node, const ASTPtr & child)
    {
        if (auto * function = node->as<ASTFunction>())
            if (function->name == "in" || function->name == "notIn")
                return false; /// Processed, process others

        if (const auto * t = node->as<ASTTablesInSelectQueryElement>())
            if (t->table_join && t->table_expression)
                return false; /// Processed, process others

        /// Descent into all children, but not into subqueries of other kind (scalar subqueries), that are irrelevant to us.
        return !child->as<ASTSelectQuery>();
    }

private:
    static void visit(ASTFunction & node, ASTPtr &, Data & data)
    {
        if (node.name == "in" || node.name == "notIn")
        {
            if (node.arguments->children.size() != 2)
            {
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Function '{}' expects two arguments, given: '{}'",
                    node.name, node.formatForErrorMessage());
            }
            auto & subquery = node.arguments->children.at(1);
            std::vector<ASTPtr> renamed;
            NonGlobalTableVisitor::Data table_data(data.getContext(), data.checker, renamed, &node, nullptr);
            NonGlobalTableVisitor(table_data).visit(subquery);
            if (!renamed.empty()) //-V547
                data.renamed_tables.emplace_back(subquery, std::move(renamed));
        }
    }

    static void visit(const ASTTablesInSelectQueryElement & node, ASTPtr &, Data & data)
    {
        if (!node.table_join || !node.table_expression)
            return;

        ASTTableJoin * table_join = node.table_join->as<ASTTableJoin>();
        if (table_join->locality != ASTTableJoin::Locality::Global)
        {
            if (auto & subquery = node.table_expression->as<ASTTableExpression>()->subquery)
            {
                std::vector<ASTPtr> renamed;
                NonGlobalTableVisitor::Data table_data(data.getContext(), data.checker, renamed, nullptr, table_join);
                NonGlobalTableVisitor(table_data).visit(subquery);
                if (!renamed.empty()) //-V547
                    data.renamed_tables.emplace_back(subquery, std::move(renamed));
            }

            /// Need to visit ASTTableExpression in order to let the cnch table join use global join.
            auto table_expression = node.table_expression;
            if (auto database_table = table_expression->as<ASTTableExpression>()->database_and_table_name)
            {
                std::vector<ASTPtr> renamed;
                NonGlobalTableVisitor::Data table_data(data.getContext(), data.checker, renamed, nullptr, table_join);
                NonGlobalTableVisitor(table_data).visit(table_expression);
                if (!renamed.empty()) //-V547
                    data.renamed_tables.emplace_back(table_expression, std::move(renamed));
            }
        }
    }
};

using NonGlobalSubqueryVisitor = InDepthNodeVisitor<NonGlobalSubqueryMatcher, true>;

}


void InJoinSubqueriesPreprocessor::visit(ASTPtr & ast) const
{
    if (!ast)
        return;

    ASTSelectQuery * query = ast->as<ASTSelectQuery>();
    if (!query || !query->tables())
        return;

    if (getContext()->getSettingsRef().distributed_product_mode == DistributedProductMode::ALLOW)
        return;

    const auto & tables_in_select_query = query->tables()->as<ASTTablesInSelectQuery &>();
    if (tables_in_select_query.children.empty())
        return;

    const auto & tables_element = tables_in_select_query.children[0]->as<ASTTablesInSelectQueryElement &>();
    if (!tables_element.table_expression)
        return;

    const auto * table_expression = tables_element.table_expression->as<ASTTableExpression>();

    /// If not ordinary table, skip it.
    if (!table_expression->database_and_table_name)
        return;

    /// If not really distributed table, skip it.
    {
        StoragePtr storage = tryGetTable(table_expression->database_and_table_name, getContext());
        if (!storage || !checker->hasAtLeastTwoShards(*storage))
            return;
    }

    NonGlobalSubqueryVisitor::Data visitor_data{getContext(), *checker, renamed_tables};
    NonGlobalSubqueryVisitor(visitor_data).visit(ast);
}


bool InJoinSubqueriesPreprocessor::CheckShardsAndTables::hasAtLeastTwoShards(const IStorage & table) const
{
    const StorageDistributed * distributed = dynamic_cast<const StorageDistributed *>(&table);
    const StorageCnchMergeTree * cnch_merge_tree = dynamic_cast<const StorageCnchMergeTree *>(&table);
    const StorageCnchLakeBase * cnch_lake = dynamic_cast<const StorageCnchLakeBase *>(&table);
    const IStorageCnchFile * cnch_file = dynamic_cast<const IStorageCnchFile *>(&table);
    if (!distributed && !cnch_merge_tree && !cnch_lake && !cnch_file)
        return false;

    return cnch_merge_tree || cnch_lake || cnch_file || distributed->getShardCount() >= 2;
}


std::pair<std::string, std::string>
InJoinSubqueriesPreprocessor::CheckShardsAndTables::getRemoteDatabaseAndTableName(const IStorage & table) const
{
    const StorageDistributed & distributed = dynamic_cast<const StorageDistributed &>(table);
    return { distributed.getRemoteDatabaseName(), distributed.getRemoteTableName() };
}


}
