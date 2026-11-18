// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>

#include <fmt/ranges.h>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/common/flow.h"

namespace Shader::Flow {
namespace {
std::string NameOf(const Block& block) {
    if (block.begin.IsVirtual()) {
        return fmt::format("\"Virtual {}\"", block.begin);
    } else {
        return fmt::format("\"{}\"", block.begin);
    }
}
} // Anonymous namespace

void Stack::Push(Token token, Location target) {
    entries.push_back({
        .token = token,
        .target{target},
    });
}

std::pair<Location, Stack> Stack::Pop(Token token) const {
    const std::optional<Location> pc{Peek(token)};
    if (!pc) {
        throw LogicError("Token could not be found");
    }
    return {*pc, Remove(token)};
}

std::optional<Location> Stack::Peek(Token token) const {
    const auto it{std::find_if(entries.rbegin(), entries.rend(),
                               [token](const auto& entry) { return entry.token == token; })};
    if (it == entries.rend()) {
        return std::nullopt;
    }
    return it->target;
}

Stack Stack::Remove(Token token) const {
    const auto it{std::find_if(entries.rbegin(), entries.rend(),
                               [token](const auto& entry) { return entry.token == token; })};
    const auto pos{std::distance(entries.rbegin(), it)};
    Stack result;
    result.entries.insert(result.entries.end(), entries.begin(), entries.end() - pos - 1);
    return result;
}

bool Block::Contains(Location pc) const noexcept {
    return pc >= begin && pc < end;
}

Function::Function(ObjectPool<Block>& block_pool, Location start_address)
    : entrypoint{start_address} {
    Label& label{labels.emplace_back()};
    label.address = start_address;
    label.block = block_pool.Create(Block{});
    label.block->begin = start_address;
    label.block->end = start_address;
    label.block->end_class = EndClass::Branch;
    label.block->cond = IR::Condition(true);
    label.block->branch_true = nullptr;
    label.block->branch_false = nullptr;
}

std::string CFG::Dot() const {
    int node_uid{0};

    std::string dot{"digraph shader {\n"};
    for (const Function& function : functions) {
        dot += fmt::format("\tsubgraph cluster_{} {{\n", function.entrypoint);
        dot += fmt::format("\t\tnode [style=filled];\n");
        for (const Block& block : function.blocks) {
            const std::string name{NameOf(block)};
            const auto add_branch = [&](Block* branch, bool add_label) {
                dot += fmt::format("\t\t{}->{}", name, NameOf(*branch));
                if (add_label && block.cond != IR::Condition{true} &&
                    block.cond != IR::Condition{false}) {
                    dot += fmt::format(" [label=\"{}\"]", block.cond);
                }
                dot += '\n';
            };
            dot += fmt::format("\t\t{};\n", name);
            switch (block.end_class) {
            case EndClass::Branch:
                if (block.cond != IR::Condition{false}) {
                    add_branch(block.branch_true, true);
                }
                if (block.cond != IR::Condition{true}) {
                    add_branch(block.branch_false, false);
                }
                break;
            case EndClass::IndirectBranch:
                for (const IndirectBranch& branch : block.indirect_branches) {
                    add_branch(branch.block, false);
                }
                break;
            case EndClass::Call:
                dot += fmt::format("\t\t{}->N{};\n", name, node_uid);
                dot += fmt::format("\t\tN{}->{};\n", node_uid, NameOf(*block.return_block));
                dot += fmt::format("\t\tN{} [label=\"Call {}\"][shape=square][style=stripped];\n",
                                   node_uid, block.function_call);
                dot += '\n';
                ++node_uid;
                break;
            case EndClass::Exit:
                dot += fmt::format("\t\t{}->N{};\n", name, node_uid);
                dot += fmt::format("\t\tN{} [label=\"Exit\"][shape=square][style=stripped];\n",
                                   node_uid);
                ++node_uid;
                break;
            case EndClass::Return:
                dot += fmt::format("\t\t{}->N{};\n", name, node_uid);
                dot += fmt::format("\t\tN{} [label=\"Return\"][shape=square][style=stripped];\n",
                                   node_uid);
                ++node_uid;
                break;
            case EndClass::Kill:
                dot += fmt::format("\t\t{}->N{};\n", name, node_uid);
                dot += fmt::format("\t\tN{} [label=\"Kill\"][shape=square][style=stripped];\n",
                                   node_uid);
                ++node_uid;
                break;
            }
        }
        if (function.entrypoint.Offset() == 8) {
            dot += fmt::format("\t\tlabel = \"main\";\n");
        } else {
            dot += fmt::format("\t\tlabel = \"Function {}\";\n", function.entrypoint);
        }
        dot += "\t}\n";
    }
    if (!functions.empty()) {
        auto& function{functions.front()};
        if (function.blocks.empty()) {
            dot += "Start;\n";
        } else {
            dot += fmt::format("\tStart -> {};\n", NameOf(*function.blocks.begin()));
        }
        dot += fmt::format("\tStart [shape=diamond];\n");
    }
    dot += "}\n";
    return dot;
}

} // namespace Shader::Flow
