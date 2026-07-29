#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PackageReleaseResult
{
    bool succeeded{false};
    std::size_t releasedCount{0};
    std::size_t notLoadedCount{0};
    std::string message;
};

struct CharliePackageReleaseResult
{
    bool succeeded{false};
    std::string message;
};

struct PackageRetirementStatus
{
    bool ready{true};
    std::size_t retainedGenerations{0};
    std::uint32_t retryAfterMilliseconds{0};
};

struct PackageRetirementCleanupResult
{
    std::size_t generationsReleased{0};
    std::size_t objectsUnrooted{0};
};

auto releaseCharliePackage()
    -> CharliePackageReleaseResult;

auto releasePackages(
    const std::vector<std::string>& packagePaths)
    -> PackageReleaseResult;

auto getPackageRetirementStatus()
    -> PackageRetirementStatus;

auto confirmPackageRetirement()
    -> std::size_t;

auto cleanupRetiredPackages()
    -> PackageRetirementCleanupResult;
