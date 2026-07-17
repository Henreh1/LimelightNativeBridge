#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

#include <Unreal/Core/Containers/FString.hpp>

#include "LivePakMount.hpp"
#include "MountResolver.hpp"

namespace
{
    using MountPakFunction =
        void* (__fastcall*)(
            void* platformFile,
            const RC::Unreal::FString& pakPath,
            std::int32_t mountOrder);

    using UnmountPakFunction =
        bool (__fastcall*)(
            void* platformFile,
            const RC::Unreal::FString& pakPath);

    struct MountInvocationResult
    {
        void* mountedPak{};
        unsigned long exceptionCode{};
    };

    struct UnmountInvocationResult
    {
        bool unmounted{false};
        unsigned long exceptionCode{};
    };

    auto getStagingDirectory() -> std::filesystem::path
    {
        std::array<wchar_t, 32768> executableBuffer{};

        const DWORD characterCount =
            GetModuleFileNameW(
                nullptr,
                executableBuffer.data(),
                static_cast<DWORD>(
                    executableBuffer.size()));

        if (characterCount == 0 ||
            characterCount >= executableBuffer.size())
        {
            return {};
        }

        const std::filesystem::path executablePath(
            std::wstring(
                executableBuffer.data(),
                characterCount));

        // The shipping executable lives in Pagoda/Binaries/Win64. Work back
        // to Pagoda before entering Limelight's private staging directory.
        const std::filesystem::path pagodaDirectory =
            executablePath
                .parent_path()
                .parent_path()
                .parent_path();

               // Keep live containers outside Content/Paks so Unreal cannot discover
        // and mount them before Limelight asks it to.
        return
            pagodaDirectory /
            L"Saved" /
            L"Limelight" /
            L"LivePaks";
    }

    auto pathIsInside(
        const std::filesystem::path& child,
        const std::filesystem::path& parent) -> bool
    {
        std::error_code pathError;

        const std::filesystem::path relativePath =
            std::filesystem::relative(
                child,
                parent,
                pathError);

        if (pathError ||
            relativePath.empty() ||
            relativePath.is_absolute())
        {
            return false;
        }

        for (const std::filesystem::path& part :
             relativePath)
        {
            if (part == L"..")
            {
                return false;
            }
        }

        return true;
    }

    auto invokeMount(
        MountPakFunction mountFunction,
        void* platformFile,
        const RC::Unreal::FString& pakPath,
        std::int32_t mountOrder) -> MountInvocationResult
    {
        // Keep the first native call behind a small safety boundary. If the
        // game's ABI ever changes, Limelight can report it instead of taking
        // the whole process down immediately.
        __try
        {
            return {
                mountFunction(
                    platformFile,
                    pakPath,
                    mountOrder),
                0
            };
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return {
                nullptr,
                GetExceptionCode()
            };
        }
    }

    auto invokeUnmount(
        UnmountPakFunction unmountFunction,
        void* platformFile,
        const RC::Unreal::FString& pakPath)
        -> UnmountInvocationResult
    {
        // Unreal's own unmount path cancels or waits for outstanding reads.
        // The safety boundary still protects the game if its ABI changes.
        __try
        {
            return {
                unmountFunction(
                    platformFile,
                    pakPath),
                0
            };
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return {
                false,
                GetExceptionCode()
            };
        }
    }

    auto formatExceptionCode(
        unsigned long exceptionCode) -> std::string
    {
        std::ostringstream message;

        message
            << "0x"
            << std::uppercase
            << std::hex
            << exceptionCode;

        return message.str();
    }
}

auto mountLivePak(
    const std::filesystem::path& pakPath,
    std::int32_t mountOrder) -> LivePakMountResult
{
    if (pakPath.empty() ||
        !pakPath.is_absolute())
    {
        return {
            false,
            "The staged pak path must be absolute"
        };
    }

    std::error_code pathError;

    const std::filesystem::path stagingDirectory =
        std::filesystem::weakly_canonical(
            getStagingDirectory(),
            pathError);

    if (pathError ||
        stagingDirectory.empty())
    {
        return {
            false,
            "Limelight's live staging directory could not be resolved"
        };
    }

    pathError.clear();

    const std::filesystem::path canonicalPakPath =
        std::filesystem::weakly_canonical(
            pakPath,
            pathError);

    if (pathError ||
        !pathIsInside(
            canonicalPakPath,
            stagingDirectory))
    {
        return {
            false,
            "The pak is outside Limelight's live staging directory"
        };
    }

    if (_wcsicmp(
            canonicalPakPath.extension().c_str(),
            L".pak") != 0)
    {
        return {
            false,
            "The staged file is not a pak archive"
        };
    }

    std::filesystem::path utocPath =
        canonicalPakPath;

    std::filesystem::path ucasPath =
        canonicalPakPath;

    utocPath.replace_extension(L".utoc");
    ucasPath.replace_extension(L".ucas");

    if (!std::filesystem::is_regular_file(
            canonicalPakPath,
            pathError))
    {
        return {
            false,
            "The staged pak file is missing"
        };
    }

    pathError.clear();

    if (!std::filesystem::is_regular_file(
            utocPath,
            pathError))
    {
        return {
            false,
            "The staged utoc file is missing"
        };
    }

    pathError.clear();

    if (!std::filesystem::is_regular_file(
            ucasPath,
            pathError))
    {
        return {
            false,
            "The staged ucas file is missing"
        };
    }

    const MountResolverResult resolver =
        resolveMountFunctions();

    if (!resolver.succeeded ||
        resolver.platformFile == nullptr ||
        resolver.mountFunction == nullptr)
    {
        return {
            false,
            "Mount resolver failed: " +
                resolver.message
        };
    }

    const std::wstring pakPathText =
        canonicalPakPath.wstring();

    const RC::Unreal::FString unrealPakPath(
        pakPathText.c_str());

    const auto mountFunction =
        reinterpret_cast<MountPakFunction>(
            resolver.mountFunction);

    const MountInvocationResult invocation =
        invokeMount(
            mountFunction,
            resolver.platformFile,
            unrealPakPath,
            std::clamp<std::int32_t>(
                mountOrder,
                0,
                100000));

    if (invocation.exceptionCode != 0)
    {
        return {
            false,
            "Unreal raised exception " +
                formatExceptionCode(
                    invocation.exceptionCode) +
                " while mounting the container"
        };
    }

    if (invocation.mountedPak == nullptr)
    {
        return {
            false,
            "Unreal rejected the staged IoStore container"
        };
    }

    return {
        true,
        "Unreal mounted the staged IoStore container successfully"
    };
}

auto unmountLivePak(
    const std::filesystem::path& pakPath) -> LivePakUnmountResult
{
    if (pakPath.empty() ||
        !pakPath.is_absolute())
    {
        return {
            false,
            "The staged pak path must be absolute"
        };
    }

    std::error_code pathError;

    const std::filesystem::path stagingDirectory =
        std::filesystem::weakly_canonical(
            getStagingDirectory(),
            pathError);

    if (pathError ||
        stagingDirectory.empty())
    {
        return {
            false,
            "Limelight's live staging directory could not be resolved"
        };
    }

    pathError.clear();

    const std::filesystem::path canonicalPakPath =
        std::filesystem::weakly_canonical(
            pakPath,
            pathError);

    if (pathError ||
        !pathIsInside(
            canonicalPakPath,
            stagingDirectory))
    {
        return {
            false,
            "The pak is outside Limelight's live staging directory"
        };
    }

    if (_wcsicmp(
            canonicalPakPath.extension().c_str(),
            L".pak") != 0)
    {
        return {
            false,
            "The staged file is not a pak archive"
        };
    }

    const MountResolverResult resolver =
        resolveMountFunctions();

    if (!resolver.succeeded ||
        resolver.platformFile == nullptr ||
        resolver.unmountFunction == nullptr)
    {
        return {
            false,
            "Unmount resolver failed: " +
                resolver.message
        };
    }

    const std::wstring pakPathText =
        canonicalPakPath.wstring();

    const RC::Unreal::FString unrealPakPath(
        pakPathText.c_str());

    const auto unmountFunction =
        reinterpret_cast<UnmountPakFunction>(
            resolver.unmountFunction);

    const UnmountInvocationResult invocation =
        invokeUnmount(
            unmountFunction,
            resolver.platformFile,
            unrealPakPath);

    if (invocation.exceptionCode != 0)
    {
        return {
            false,
            "Unreal raised exception " +
                formatExceptionCode(
                    invocation.exceptionCode) +
                " while unmounting the container"
        };
    }

    if (!invocation.unmounted)
    {
        return {
            false,
            "Unreal could not find the staged IoStore container to unmount"
        };
    }

    return {
        true,
        "Unreal unmounted the staged IoStore container successfully"
    };
}
