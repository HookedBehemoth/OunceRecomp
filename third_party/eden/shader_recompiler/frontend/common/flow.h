// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// (fork) A program's control flow graph as a front builds it for BuildASL: functions of blocks between locations, each
// ending in a branch, call, exit, return or kill. A front's builder derives from CFG and fills its functions.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/intrusive/set.hpp>

#include "shader_recompiler/frontend/common/location.h"
#include "shader_recompiler/frontend/ir/condition.h"
#include "shader_recompiler/frontend/ir/reg.h"
#include "shader_recompiler/object_pool.h"

namespace Shader::Flow {

struct Block;

using FunctionId = size_t;

enum class EndClass {
    Branch,
    IndirectBranch,
    Call,
    Exit,
    Return,
    Kill,
};

enum class Token {
    SSY,
    PBK,
    PEXIT,
    PRET,
    PCNT,
    PLONGJMP,
};

struct StackEntry {
    auto operator<=>(const StackEntry&) const noexcept = default;

    Token token;
    Location target;
};

class Stack {
public:
    void Push(Token token, Location target);
    [[nodiscard]] std::pair<Location, Stack> Pop(Token token) const;
    [[nodiscard]] std::optional<Location> Peek(Token token) const;
    [[nodiscard]] Stack Remove(Token token) const;

private:
    std::vector<StackEntry> entries;
};

struct IndirectBranch {
    Block* block;
    u32 address;
};

struct Block : boost::intrusive::set_base_hook<
                   // Normal link is ~2.5% faster compared to safe link
                   boost::intrusive::link_mode<boost::intrusive::normal_link>> {
    [[nodiscard]] bool Contains(Location pc) const noexcept;

    bool operator<(const Block& rhs) const noexcept {
        return begin < rhs.begin;
    }

    Location begin;
    Location end;
    EndClass end_class{};
    IR::Condition cond{};
    Stack stack;
    Block* branch_true{};
    Block* branch_false{};
    FunctionId function_call{};
    Block* return_block{};
    IR::Reg branch_reg{};
    s32 branch_offset{};
    std::vector<IndirectBranch> indirect_branches;
};

struct Label {
    Location address;
    Block* block;
    Stack stack;
};

struct Function {
    explicit Function(ObjectPool<Block>& block_pool, Location start_address);

    Location entrypoint;
    boost::container::small_vector<Label, 16> labels;
    boost::intrusive::set<Block> blocks;
};

class CFG {
public:
    CFG& operator=(const CFG&) = delete;
    CFG(const CFG&) = delete;

    CFG& operator=(CFG&&) = delete;
    CFG(CFG&&) = delete;

    [[nodiscard]] std::string Dot() const;

    [[nodiscard]] std::span<const Function> Functions() const noexcept {
        return std::span(functions.data(), functions.size());
    }
    [[nodiscard]] std::span<Function> Functions() noexcept {
        return std::span(functions.data(), functions.size());
    }

    [[nodiscard]] bool ExitsToDispatcher() const {
        return exits_to_dispatcher;
    }

protected:
    explicit CFG(bool exits_to_dispatcher_) : exits_to_dispatcher{exits_to_dispatcher_} {}
    ~CFG() = default;

    boost::container::small_vector<Function, 1> functions;
    bool exits_to_dispatcher{};
};

} // namespace Shader::Flow
