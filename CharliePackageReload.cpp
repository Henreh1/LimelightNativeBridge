#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include <Constructs/Loop.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "CharliePackageReload.hpp"

namespace
{
    constexpr auto CharliePackagePath =
        STR("/Game/Pagoda/Characters/Player/Meshes/SK_Charlie");

    std::atomic_uint64_t releasedPackageNumber{0};

    constexpr std::size_t MaxRetiredGenerations = 3;
    constexpr auto MinimumRetirementAge =
        std::chrono::seconds(8);
    constexpr auto MaximumRetirementAge =
        std::chrono::seconds(30);

    struct RetiredPackageGeneration
    {
        std::chrono::steady_clock::time_point retiredAt;
        std::vector<RC::Unreal::UObject*> rootedObjects;
    };

    std::mutex retiredPackageMutex;
    std::deque<RetiredPackageGeneration> retiredPackageGenerations;

    template <typename Character>
    constexpr auto lowerAscii(
        Character character) -> Character
    {
        const Character upperA =
            static_cast<Character>('A');

        const Character upperZ =
            static_cast<Character>('Z');

        if (character >= upperA &&
            character <= upperZ)
        {
            return static_cast<Character>(
                character +
                static_cast<Character>('a' - 'A'));
        }

        return character;
    }

    auto packagePathsEqual(
        const RC::File::StringType& left,
        const RC::File::StringType& right) -> bool
    {
        if (left.size() != right.size())
        {
            return false;
        }

        return std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            [](const auto leftCharacter,
               const auto rightCharacter)
            {
                return lowerAscii(leftCharacter) ==
                    lowerAscii(rightCharacter);
            });
    }

    struct RenameInvocationResult
    {
        bool renamed{false};
        unsigned long exceptionCode{};
    };

    auto invokeClearRoot(
        RC::Unreal::UObject* object) -> bool
    {
        if (object == nullptr)
        {
            return true;
        }

        // Clearing a root only makes the object eligible for normal Unreal
        // collection. I keep this behind an exception boundary because this
        // code runs inside the game process.
        __try
        {
            if (object->IsRootSet())
            {
                object->ClearRootSet();
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto clearGeneration(
        RetiredPackageGeneration& generation) -> std::size_t
    {
        std::size_t objectsUnrooted = 0;
        std::vector<RC::Unreal::UObject*> rootsStillOwned;

        for (RC::Unreal::UObject* object :
             generation.rootedObjects)
        {
            if (invokeClearRoot(object))
            {
                ++objectsUnrooted;
            }
            else
            {
                rootsStillOwned.push_back(object);
            }
        }

        generation.rootedObjects =
            std::move(rootsStillOwned);

        return objectsUnrooted;
    }

    auto makeRetirementRoom() -> bool
    {
        std::scoped_lock lock(
            retiredPackageMutex);

        if (retiredPackageGenerations.size() <
            MaxRetiredGenerations)
        {
            return true;
        }

        const auto now =
            std::chrono::steady_clock::now();

        RetiredPackageGeneration& oldest =
            retiredPackageGenerations.front();

        if (now - oldest.retiredAt <
            MinimumRetirementAge)
        {
            return false;
        }

        clearGeneration(oldest);

        if (!oldest.rootedObjects.empty())
        {
            return false;
        }

        retiredPackageGenerations.pop_front();

        return true;
    }

    auto rootForRetirement(
        RC::Unreal::UObject* object,
        std::vector<RC::Unreal::UObject*>& rootedObjects,
        std::unordered_set<RC::Unreal::UObject*>& seenObjects)
        -> void
    {
        if (object == nullptr ||
            seenObjects.contains(object))
        {
            return;
        }

        seenObjects.insert(object);

        // I only remember roots that Limelight added. Objects already rooted
        // by the game must stay under the game's ownership.
        if (!object->IsRootSet())
        {
            object->SetRootSet();
            rootedObjects.push_back(object);
        }
    }

    auto commitRetiredGeneration(
        std::vector<RC::Unreal::UObject*> rootedObjects) -> void
    {
        if (rootedObjects.empty())
        {
            return;
        }

        std::scoped_lock lock(
            retiredPackageMutex);

        retiredPackageGenerations.push_back({
            std::chrono::steady_clock::now(),
            std::move(rootedObjects)
        });
    }

    auto invokeRename(
        RC::Unreal::UObject* package,
        const RC::Unreal::TCHAR* newName,
        RC::Unreal::ERenameFlags flags)
        -> RenameInvocationResult
    {
        // Keep the experimental package operation behind the same safety
        // boundary as Limelight's first live-mount call.
        __try
        {
            return {
                package->Rename(
                    newName,
                    nullptr,
                    flags),
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

    auto narrowForMessage(
        const RC::File::StringType& text) -> std::string
    {
        std::string result;
        result.reserve(
            text.size());

        for (const auto character :
             text)
        {
            const auto value =
                static_cast<unsigned int>(
                    character);

            result.push_back(
                value <= 0x7f
                    ? static_cast<char>(value)
                    : '?');
        }

        return result;
    }
}

auto releaseCharliePackage()
    -> CharliePackageReleaseResult
{
    try
    {
        RC::Unreal::UObject* meshAsset =
            nullptr;

        std::size_t scannedObjectCount =
            0;

        std::size_t namedCandidateCount =
            0;

        RC::File::StringType firstCandidatePackage;

        // Walk the live object array directly. This avoids both native lookup
        // helpers that Dead as Disco's cooked asset index did not resolve.
        RC::Unreal::UObjectGlobals::ForEachUObject(
            [&](RC::Unreal::UObject* candidate,
                RC::Unreal::int32,
                RC::Unreal::int32) -> RC::LoopAction
            {
                ++scannedObjectCount;

                if (candidate == nullptr ||
                    candidate->GetName() !=
                        STR("SK_Charlie"))
                {
                    return RC::LoopAction::Continue;
                }

                ++namedCandidateCount;

                RC::Unreal::UObject* candidatePackage =
                    candidate->GetOutermost();

                if (candidatePackage == nullptr)
                {
                    return RC::LoopAction::Continue;
                }

                const RC::File::StringType candidatePackageName =
                    candidatePackage->GetName();

                if (firstCandidatePackage.empty())
                {
                    firstCandidatePackage =
                        candidatePackageName;
                }

                if (candidatePackageName ==
                    CharliePackagePath)
                {
                    meshAsset = candidate;
                    return RC::LoopAction::Break;
                }

                return RC::LoopAction::Continue;
            });

        if (meshAsset == nullptr)
        {
            std::string message =
                "Native object scan visited " +
                std::to_string(
                    scannedObjectCount) +
                " objects and found " +
                std::to_string(
                    namedCandidateCount) +
                " SK_Charlie candidates";

            if (!firstCandidatePackage.empty())
            {
                message +=
                    "; first package=" +
                    narrowForMessage(
                        firstCandidatePackage);
            }

            return {
                false,
                message
            };
        }

        RC::Unreal::UObject* package =
            meshAsset->GetOutermost();

        if (package == nullptr)
        {
            return {
                false,
                "SK_Charlie did not have a valid outer package"
            };
        }

        const RC::File::StringType packageName =
            package->GetName();

        if (packageName != CharliePackagePath)
        {
            return {
                false,
                "The loaded SK_Charlie asset belonged to an unexpected package"
            };
        }

        if (!makeRetirementRoom())
        {
            return {
                false,
                "The previous live assets are still settling; try again in a few seconds"
            };
        }

        std::vector<RC::Unreal::UObject*> objectsToRetain;

        RC::Unreal::UObjectGlobals::ForEachUObject(
            [&](RC::Unreal::UObject* candidate,
                RC::Unreal::int32,
                RC::Unreal::int32) -> RC::LoopAction
            {
                if (candidate != nullptr &&
                    candidate->GetOutermost() == package)
                {
                    objectsToRetain.push_back(
                        candidate);
                }

                return RC::LoopAction::Continue;
            });

        std::vector<RC::Unreal::UObject*> rootedObjects;
        std::unordered_set<RC::Unreal::UObject*> seenObjects;

        rootForRetirement(
            package,
            rootedObjects,
            seenObjects);

        for (RC::Unreal::UObject* retainedObject :
             objectsToRetain)
        {
            rootForRetirement(
                retainedObject,
                rootedObjects,
                seenObjects);
        }

        RC::File::StringType releasedPackageName =
            packageName;

        releasedPackageName +=
            STR("_LIMELIGHT_OLD_");

        releasedPackageName +=
            std::to_wstring(
                ++releasedPackageNumber);

        const auto renameFlags =
            static_cast<RC::Unreal::ERenameFlags>(
                REN_DoNotDirty |
                REN_DontCreateRedirectors |
                REN_NonTransactional |
                REN_SkipGeneratedClasses);

        const RenameInvocationResult invocation =
            invokeRename(
                package,
                releasedPackageName.c_str(),
                renameFlags);

        if (invocation.exceptionCode != 0)
        {
            commitRetiredGeneration(
                std::move(rootedObjects));

            return {
                false,
                "Unreal raised exception " +
                    formatExceptionCode(
                        invocation.exceptionCode) +
                    " while releasing the cached Charlie package"
            };
        }

        if (!invocation.renamed)
        {
            commitRetiredGeneration(
                std::move(rootedObjects));

            return {
                false,
                "Unreal refused to release the cached Charlie package"
            };
        }

        commitRetiredGeneration(
            std::move(rootedObjects));

        return {
            true,
            "The cached SK_Charlie package was released for a fresh load"
        };
    }
    catch (const std::exception& exception)
    {
        return {
            false,
            "Charlie package release failed: " +
                std::string(
                    exception.what())
        };
    }
    catch (...)
    {
        return {
            false,
            "Charlie package release failed with an unknown error"
        };
    }
}

auto releasePackages(
    const std::vector<std::string>& packagePaths)
    -> PackageReleaseResult
{
    try
    {
        struct RequestedPackage
        {
            RC::File::StringType path;
            RC::Unreal::UObject* package{nullptr};
        };

        std::vector<RequestedPackage> requestedPackages;
        std::vector<RC::Unreal::UObject*> objectsToRetain;

        for (const std::string& packagePath :
             packagePaths)
        {
            if (packagePath.empty() ||
                !packagePath.starts_with("/Game/"))
            {
                continue;
            }

            RC::File::StringType widePath;
            widePath.reserve(
                packagePath.size());

            for (const char character :
                 packagePath)
            {
                widePath.push_back(
                    static_cast<RC::Unreal::TCHAR>(
                        static_cast<unsigned char>(character)));
            }

            const bool alreadyRequested =
                std::any_of(
                    requestedPackages.begin(),
                    requestedPackages.end(),
                    [&](const RequestedPackage& requested)
                    {
                        return packagePathsEqual(
                            requested.path,
                            widePath);
                    });

            if (!alreadyRequested)
            {
                requestedPackages.push_back({
                    std::move(widePath),
                    nullptr
                });
            }
        }

        if (requestedPackages.empty())
        {
            return {
                false,
                0,
                0,
                "No safe package paths were supplied"
            };
        }

        // One walk of Unreal's object array is enough for the whole manifest.
        // An unloaded dependency will simply stay empty and load fresh later.
        RC::Unreal::UObjectGlobals::ForEachUObject(
            [&](RC::Unreal::UObject* candidate,
                RC::Unreal::int32,
                RC::Unreal::int32) -> RC::LoopAction
            {
                if (candidate == nullptr)
                {
                    return RC::LoopAction::Continue;
                }

                RC::Unreal::UObject* package =
                    candidate->GetOutermost();

                if (package == nullptr)
                {
                    return RC::LoopAction::Continue;
                }

                const RC::File::StringType packageName =
                    package->GetName();

                for (RequestedPackage& requested :
                     requestedPackages)
                {
                    // Unreal treats package names without regard to case. I
                    // match them the same way so Custom1 and custom1 release
                    // the same cached material package during a live switch.
                    if (packagePathsEqual(
                            requested.path,
                            packageName))
                    {
                        if (requested.package == nullptr)
                        {
                            requested.package = package;
                        }

                        objectsToRetain.push_back(
                            candidate);

                        break;
                    }
                }

                return RC::LoopAction::Continue;
            });

        const bool hasLoadedPackage =
            std::any_of(
                requestedPackages.begin(),
                requestedPackages.end(),
                [](const RequestedPackage& requested)
                {
                    return requested.package != nullptr;
                });

        if (hasLoadedPackage &&
            !makeRetirementRoom())
        {
            return {
                false,
                0,
                0,
                "The previous live assets are still settling; try again in a few seconds"
            };
        }

        const auto renameFlags =
            static_cast<RC::Unreal::ERenameFlags>(
                REN_DoNotDirty |
                REN_DontCreateRedirectors |
                REN_NonTransactional |
                REN_SkipGeneratedClasses);

        std::size_t releasedCount = 0;
        std::size_t notLoadedCount = 0;
        std::vector<RC::Unreal::UObject*> rootedObjects;
        std::unordered_set<RC::Unreal::UObject*> seenObjects;

        for (RequestedPackage& requested :
             requestedPackages)
        {
            if (requested.package == nullptr)
            {
                ++notLoadedCount;
                continue;
            }

            // A renderer or preview scene can still hold the previous mesh,
            // material, or texture briefly. I keep a bounded generation alive
            // while those references settle, then return it to Unreal.
            rootForRetirement(
                requested.package,
                rootedObjects,
                seenObjects);

            for (RC::Unreal::UObject* retainedObject :
                 objectsToRetain)
            {
                if (retainedObject != nullptr &&
                    retainedObject->GetOutermost() ==
                        requested.package)
                {
                    rootForRetirement(
                        retainedObject,
                        rootedObjects,
                        seenObjects);
                }
            }

            RC::File::StringType releasedName =
                requested.path;

            releasedName +=
                STR("_LIMELIGHT_OLD_");

            releasedName +=
                std::to_wstring(
                    ++releasedPackageNumber);

            const RenameInvocationResult invocation =
                invokeRename(
                    requested.package,
                    releasedName.c_str(),
                    renameFlags);

            if (invocation.exceptionCode != 0)
            {
                commitRetiredGeneration(
                    std::move(rootedObjects));

                return {
                    false,
                    releasedCount,
                    notLoadedCount,
                    "Unreal raised exception " +
                        formatExceptionCode(
                            invocation.exceptionCode) +
                        " while refreshing " +
                        narrowForMessage(
                            requested.path)
                };
            }

            if (!invocation.renamed)
            {
                commitRetiredGeneration(
                    std::move(rootedObjects));

                return {
                    false,
                    releasedCount,
                    notLoadedCount,
                    "Unreal refused to refresh " +
                        narrowForMessage(
                            requested.path)
                };
            }

            ++releasedCount;
        }

        commitRetiredGeneration(
            std::move(rootedObjects));

        return {
            true,
            releasedCount,
            notLoadedCount,
            "Released " +
                std::to_string(releasedCount) +
                " cached character packages; " +
                std::to_string(notLoadedCount) +
                " were not loaded yet"
        };
    }
    catch (const std::exception& exception)
    {
        return {
            false,
            0,
            0,
            "Character package refresh failed: " +
                std::string(exception.what())
        };
    }
    catch (...)
    {
        return {
            false,
            0,
            0,
            "Character package refresh failed with an unknown error"
        };
    }
}

auto getPackageRetirementStatus()
    -> PackageRetirementStatus
{
    std::scoped_lock lock(
        retiredPackageMutex);

    PackageRetirementStatus status;
    status.retainedGenerations =
        retiredPackageGenerations.size();

    if (retiredPackageGenerations.size() <
        MaxRetiredGenerations)
    {
        return status;
    }

    const auto now =
        std::chrono::steady_clock::now();

    const auto age =
        now - retiredPackageGenerations.front().retiredAt;

    if (age >= MinimumRetirementAge)
    {
        return status;
    }

    const auto wait =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            MinimumRetirementAge - age);

    status.ready = false;
    status.retryAfterMilliseconds =
        static_cast<std::uint32_t>(
            std::max<std::int64_t>(
                1,
                wait.count()));

    return status;
}

auto cleanupRetiredPackages()
    -> PackageRetirementCleanupResult
{
    std::scoped_lock lock(
        retiredPackageMutex);

    PackageRetirementCleanupResult result;
    const auto now =
        std::chrono::steady_clock::now();

    while (!retiredPackageGenerations.empty())
    {
        RetiredPackageGeneration& oldest =
            retiredPackageGenerations.front();

        if (now - oldest.retiredAt <
            MaximumRetirementAge)
        {
            break;
        }

        result.objectsUnrooted +=
            clearGeneration(oldest);

        if (!oldest.rootedObjects.empty())
        {
            break;
        }

        retiredPackageGenerations.pop_front();
        ++result.generationsReleased;
    }

    return result;
}
