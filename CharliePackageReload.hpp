#pragma once

#include <cstddef>
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

auto releaseCharliePackage()
    -> CharliePackageReleaseResult;

auto releasePackages(
    const std::vector<std::string>& packagePaths)
    -> PackageReleaseResult;
