#pragma once

#include <string>

struct MountResolverResult
{
    bool succeeded{false};
    std::string message;
    void* platformFile{};
    void* mountFunction{};
};

auto resolveMountFunctions() -> MountResolverResult;
