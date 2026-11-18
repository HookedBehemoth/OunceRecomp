// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// (fork) An instruction's byte offset in a program, stepped by the stride of its instruction set.

#pragma once

#include <fmt/ranges.h>

#include "common/common_types.h"
#include "shader_recompiler/exception.h"

namespace Shader {

enum class LocationStride : u8 {
    Maxwell, ///< 8-byte instructions; the first word of each 32 bytes is scheduling, never a location
    Ampere,  ///< 16-byte instructions, scheduling inline
};

class Location {
    static constexpr u32 VIRTUAL_BIAS{4};

public:
    constexpr Location() = default;

    constexpr Location(u32 initial_offset, LocationStride stride_) : offset{initial_offset}, stride{stride_} {
        const u32 alignment{stride == LocationStride::Ampere ? 16U : 8U};
        if (initial_offset % alignment != 0) {
            throw InvalidArgument("initial_offset={} is not a multiple of {}", initial_offset, alignment);
        }
        Align();
    }

    constexpr Location Virtual() const noexcept {
        Location virtual_location{*this};
        virtual_location.offset = offset - VIRTUAL_BIAS;
        return virtual_location;
    }

    [[nodiscard]] constexpr u32 Offset() const noexcept {
        return offset;
    }

    [[nodiscard]] constexpr LocationStride Stride() const noexcept {
        return stride;
    }

    [[nodiscard]] constexpr bool IsVirtual() const {
        return offset % 8 == VIRTUAL_BIAS;
    }

    constexpr auto operator<=>(const Location&) const noexcept = default;

    constexpr Location operator++() noexcept {
        const Location copy{*this};
        Step();
        return copy;
    }

    constexpr Location operator++(int) noexcept {
        Step();
        return *this;
    }

    constexpr Location operator--() noexcept {
        const Location copy{*this};
        Back();
        return copy;
    }

    constexpr Location operator--(int) noexcept {
        Back();
        return *this;
    }

    constexpr Location operator+(int number) const {
        Location new_pc{*this};
        while (number > 0) {
            --number;
            ++new_pc;
        }
        while (number < 0) {
            ++number;
            --new_pc;
        }
        return new_pc;
    }

    constexpr Location operator-(int number) const {
        return operator+(-number);
    }

private:
    constexpr void Align() {
        if (stride == LocationStride::Maxwell) {
            offset += offset % 32 == 0 ? 8 : 0;
        }
    }

    constexpr void Step() {
        if (stride == LocationStride::Ampere) {
            offset += 16;
        } else {
            offset += 8 + (offset % 32 == 24 ? 8 : 0);
        }
    }

    constexpr void Back() {
        if (stride == LocationStride::Ampere) {
            offset -= 16;
        } else {
            offset -= 8 + (offset % 32 == 8 ? 8 : 0);
        }
    }

    u32 offset{0xcccccccc};
    LocationStride stride{};
};

} // namespace Shader

template <>
struct fmt::formatter<Shader::Location> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const Shader::Location& location, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{:04x}", location.Offset());
    }
};
