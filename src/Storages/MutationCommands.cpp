#include <Storages/MutationCommands.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <Parsers/formatAST.h>
#include <Parsers/ParserAlterQuery.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ASTAssignment.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTStatisticDeclaration.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Common/typeid_cast.h>
#include <Common/quoteString.h>
#include <Core/Defines.h>
#include <DataTypes/DataTypeFactory.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_MUTATION_COMMAND;
    extern const int MULTIPLE_ASSIGNMENTS_TO_COLUMN;
}


bool MutationCommand::isBarrierCommand() const
{
    return type == RENAME_COLUMN;
}

std::optional<MutationCommand> MutationCommand::parse(ASTAlterCommand * command, bool parse_alter_commands)
{
    if (command->type == ASTAlterCommand::DELETE)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = DELETE;
        res.predicate = command->predicate;
        res.partition = command->partition;
        return res;
    }
    else if (command->type == ASTAlterCommand::UPDATE)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = UPDATE;
        res.predicate = command->predicate;
        res.partition = command->partition;
        for (const ASTPtr & assignment_ast : command->update_assignments->children)
        {
            const auto & assignment = assignment_ast->as<ASTAssignment &>();
            auto insertion = res.column_to_update_expression.emplace(assignment.column_name, assignment.expression());
            if (!insertion.second)
                throw Exception(ErrorCodes::MULTIPLE_ASSIGNMENTS_TO_COLUMN,
                                "Multiple assignments in the single statement to column {}",
                                backQuote(assignment.column_name));
        }
        return res;
    }
    else if (command->type == ASTAlterCommand::MATERIALIZE_INDEX)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MATERIALIZE_INDEX;
        res.partition = command->partition;
        res.predicate = nullptr;
        res.index_name = command->index->as<ASTIdentifier &>().name();
        return res;
    }
    else if (command->type == ASTAlterCommand::MATERIALIZE_STATISTIC)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MATERIALIZE_STATISTIC;
        res.partition = command->partition;
        res.predicate = nullptr;
        res.statistic_columns = command->statistic_decl->as<ASTStatisticDeclaration &>().getColumnNames();
        return res;
    }
    else if (command->type == ASTAlterCommand::MATERIALIZE_PROJECTION)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MATERIALIZE_PROJECTION;
        res.partition = command->partition;
        res.predicate = nullptr;
        res.projection_name = command->projection->as<ASTIdentifier &>().name();
        return res;
    }
    else if (command->type == ASTAlterCommand::MATERIALIZE_COLUMN)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MATERIALIZE_COLUMN;
        res.partition = command->partition;
        res.column_name = getIdentifierName(command->column);
        return res;
    }
    else if (parse_alter_commands && command->type == ASTAlterCommand::MODIFY_COLUMN)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MutationCommand::Type::READ_COLUMN;
        const auto & ast_col_decl = command->col_decl->as<ASTColumnDeclaration &>();
        res.column_name = ast_col_decl.name;
        res.data_type = DataTypeFactory::instance().get(ast_col_decl.type);
        return res;
    }
    else if (parse_alter_commands && command->type == ASTAlterCommand::DROP_COLUMN)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MutationCommand::Type::DROP_COLUMN;
        res.column_name = getIdentifierName(command->column);
        if (command->partition)
            res.partition = command->partition;
        if (command->clear_column)
            res.clear = true;

        return res;
    }
    else if (parse_alter_commands && command->type == ASTAlterCommand::DROP_INDEX)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MutationCommand::Type::DROP_INDEX;
        res.column_name = command->index->as<ASTIdentifier &>().name();
        if (command->partition)
            res.partition = command->partition;
        if (command->clear_index)
            res.clear = true;
        return res;
    }
    else if (parse_alter_commands && command->type == ASTAlterCommand::DROP_STATISTIC)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MutationCommand::Type::DROP_STATISTIC;
        if (command->partition)
            res.partition = command->partition;
        if (command->clear_index)
            res.clear = true;
        res.statistic_columns = command->statistic_decl->as<ASTStatisticDeclaration &>().getColumnNames();
        return res;
    }
    else if (parse_alter_commands && command->type == ASTAlterCommand::DROP_PROJECTION)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MutationCommand::Type::DROP_PROJECTION;
        res.column_name = command->projection->as<ASTIdentifier &>().name();
        if (command->partition)
            res.partition = command->partition;
        if (command->clear_projection)
            res.clear = true;
        return res;
    }
    else if (parse_alter_commands && command->type == ASTAlterCommand::RENAME_COLUMN)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MutationCommand::Type::RENAME_COLUMN;
        res.column_name = command->column->as<ASTIdentifier &>().name();
        res.rename_to = command->rename_to->as<ASTIdentifier &>().name();
        return res;
    }
    else if (command->type == ASTAlterCommand::MATERIALIZE_TTL)
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = MATERIALIZE_TTL;
        res.partition = command->partition;
        return res;
    }
    else
    {
        MutationCommand res;
        res.ast = command->ptr();
        res.type = ALTER_WITHOUT_MUTATION;
        return res;
    }
}


std::shared_ptr<ASTExpressionList> MutationCommands::ast(bool with_pure_metadata_commands) const
{
    auto res = std::make_shared<ASTExpressionList>();
    for (const MutationCommand & command : *this)
    {
        if (command.type != MutationCommand::ALTER_WITHOUT_MUTATION || with_pure_metadata_commands)
            res->children.push_back(command.ast->clone());
    }
    return res;
}


void MutationCommands::writeText(WriteBuffer & out, bool with_pure_metadata_commands) const
{
    WriteBufferFromOwnString commands_buf;
    formatAST(*ast(with_pure_metadata_commands), commands_buf, /* hilite = */ false, /* one_line = */ true);
    writeEscapedString(commands_buf.str(), out);
}

void MutationCommands::readText(ReadBuffer & in)
{
    String commands_str;
    readEscapedString(commands_str, in);

    ParserAlterCommandList p_alter_commands;
    auto commands_ast = parseQuery(
        p_alter_commands, commands_str.data(), commands_str.data() + commands_str.length(), "mutation commands list", 0, DBMS_DEFAULT_MAX_PARSER_DEPTH);

    for (const auto & child : commands_ast->children)
    {
        auto * command_ast = child->as<ASTAlterCommand>();
        auto command = MutationCommand::parse(command_ast, true);
        if (!command)
            throw Exception(ErrorCodes::UNKNOWN_MUTATION_COMMAND, "Unknown mutation command type: {}", DB::toString<int>(command_ast->type));
        push_back(std::move(*command));
    }
}

std::string MutationCommands::toString() const
{
    WriteBufferFromOwnString commands_buf;
    formatAST(*ast(), commands_buf, /* hilite = */ false, /* one_line = */ true);
    return commands_buf.str();
}


bool MutationCommands::hasNonEmptyMutationCommands() const
{
    for (const auto & command : *this)
    {
        if (command.type != MutationCommand::Type::EMPTY && command.type != MutationCommand::Type::ALTER_WITHOUT_MUTATION)
            return true;
    }
    return false;
}

bool MutationCommands::containBarrierCommand() const
{
    for (const auto & command : *this)
    {
        if (command.isBarrierCommand())
            return true;
    }
    return false;
}

}
