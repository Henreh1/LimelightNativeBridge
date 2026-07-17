#pragma once

#include <string>

struct MountResolverResult
{
    bool succeeded{false};
    std::string message;
    void* platformFile{};
    void* mountFunction{};
    void* unmountFunction{};
};

auto resolveMountFunctions() -> MountResolverResult;
