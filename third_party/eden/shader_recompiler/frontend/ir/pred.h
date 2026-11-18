// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <fmt/ranges.h>

namespace Shader::IR {

enum class Pred : u64 {
    P0,
    P1,
    P2,
    P3,
    P4,
    P5,
    P6,
    PT,
    // (fork) Ampere's uniform predicates; UPT is the immediate true, the decoder gives PT for it
    UP0,
    UP1,
    UP2,
    UP3,
    UP4,
    UP5,
    UP6,
};

constexpr size_t NUM_USER_PREDS = 7;
constexpr size_t NUM_PREDS = 16;   // (fork: P0-P6, PT, UP0-UP6 at 8-14)

[[nodiscard]] constexpr bool IsUniformPred(Pred pred) noexcept {
    return pred >= Pred::UP0 && pred <= Pred::UP6;
}

[[nodiscard]] constexpr size_t PredIndex(Pred pred) noexcept {
    return static_cast<size_t>(pred);
}

} // namespace Shader::IR

template <>
struct fmt::formatter<Shader::IR::Pred> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const Shader::IR::Pred& pred, FormatContext& ctx) const {
        if (pred == Shader::IR::Pred::PT) {
            return fmt::format_to(ctx.out(), "PT");
        } else if (Shader::IR::IsUniformPred(pred)) {
            return fmt::format_to(ctx.out(), "UP{}", static_cast<int>(pred) - static_cast<int>(Shader::IR::Pred::UP0));
        } else {
            return fmt::format_to(ctx.out(), "P{}", static_cast<int>(pred));
        }
    }
};
