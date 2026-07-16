#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct LivePakMountResult
{
    bool succeeded{false};
    std::string message;
};

auto mountLivePak(
    const std::filesystem::path& pakPath,
    std::int32_t mountOrder) -> LivePakMountResult;