#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include "MountResolver.hpp"
#include "LivePakMount.hpp"
#include "CharliePackageReload.hpp"

using namespace RC;

class LimelightNativeBridge : public CppUserModBase
{
private:
    using ValueMap =
        std::unordered_map<std::string, std::string>;

    struct PendingUnrealCommand
    {
        std::string requestId;
        std::string action;
        std::vector<std::string> packagePaths;
        std::filesystem::path pakPath;
        std::int32_t mountOrder{1000};
    };

    std::filesystem::path m_runtimeDirectory;
    std::chrono::steady_clock::time_point m_nextHeartbeat{};
    std::chrono::steady_clock::time_point m_nextCommandCheck{};
    std::string m_lastRequestId;
    bool m_runtimeDirectoryReady{false};
    std::mutex m_pendingUnrealCommandMutex;
    std::optional<PendingUnrealCommand> m_pendingUnrealCommand;
    std::atomic_bool m_worldTransitioning{false};
    std::atomic_bool m_worldSettling{false};
    std::chrono::steady_clock::time_point m_worldReadyAfter{};
    Unreal::Hook::GlobalCallbackId m_engineTickCallbackId{
        Unreal::Hook::ERROR_ID};
    Unreal::Hook::GlobalCallbackId m_loadMapPreCallbackId{
        Unreal::Hook::ERROR_ID};
    Unreal::Hook::GlobalCallbackId m_loadMapPostCallbackId{
        Unreal::Hook::ERROR_ID};

    static auto replaceFile(
        const std::filesystem::path& temporaryPath,
        const std::filesystem::path& finalPath) -> bool
    {
        std::error_code fileError;

        std::filesystem::remove(
            finalPath,
            fileError);

        fileError.clear();

        std::filesystem::rename(
            temporaryPath,
            finalPath,
            fileError);

        return !fileError;
    }

    auto prepareRuntimeDirectory() -> void
    {
        wchar_t* localAppData = nullptr;
        size_t characterCount = 0;

        if (_wdupenv_s(
                &localAppData,
                &characterCount,
                L"LOCALAPPDATA") != 0 ||
            localAppData == nullptr)
        {
            Output::send<LogLevel::Error>(
                STR("[Limelight] LOCALAPPDATA could not be found.\n"));

            return;
        }

        // Keep the native files beside the Lua bridge files so Limelight only
        // has one runtime folder to keep an eye on.
        m_runtimeDirectory =
            std::filesystem::path(localAppData) /
            L"Limelight" /
            L"Runtime";

        std::free(localAppData);

        std::error_code directoryError;

        std::filesystem::create_directories(
            m_runtimeDirectory,
            directoryError);

        if (directoryError)
        {
            Output::send<LogLevel::Error>(
                STR("[Limelight] The native runtime folder could not be created.\n"));

            return;
        }

        m_runtimeDirectoryReady = true;
    }

    auto writeHeartbeat() -> void
    {
        const std::filesystem::path temporaryPath =
            m_runtimeDirectory /
            L"native-heartbeat.txt.tmp";

        const std::filesystem::path heartbeatPath =
            m_runtimeDirectory /
            L"native-heartbeat.txt";

        std::ofstream heartbeatFile(
            temporaryPath,
            std::ios::trunc);

        if (!heartbeatFile.is_open())
        {
            return;
        }

        const auto timestamp =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now()
                    .time_since_epoch())
                .count();

        heartbeatFile
            << "timestamp=" << timestamp << '\n'
            << "version=0.1.0\n";

        heartbeatFile.close();

        // Swap in a complete file so Limelight never catches us halfway
        // through writing the heartbeat.
        replaceFile(
            temporaryPath,
            heartbeatPath);
    }

    static auto readValues(
        const std::filesystem::path& path) -> ValueMap
    {
        ValueMap values;
        std::ifstream inputFile(path);
        std::string line;

        while (std::getline(
            inputFile,
            line))
        {
            const size_t equalsPosition =
                line.find('=');

            if (equalsPosition == std::string::npos ||
                equalsPosition == 0)
            {
                continue;
            }

            values[
                line.substr(
                    0,
                    equalsPosition)] =
                line.substr(
                    equalsPosition + 1);
        }

        return values;
    }

    static auto splitPackagePaths(
        const std::string& value) -> std::vector<std::string>
    {
        std::vector<std::string> packagePaths;
        size_t start = 0;

        while (start <= value.size())
        {
            const size_t separator =
                value.find('|', start);

            const size_t length =
                separator == std::string::npos
                    ? std::string::npos
                    : separator - start;

            std::string packagePath =
                value.substr(start, length);

            if (!packagePath.empty())
            {
                packagePaths.push_back(
                    std::move(packagePath));
            }

            if (separator == std::string::npos)
            {
                break;
            }

            start = separator + 1;
        }

        return packagePaths;
    }

    auto writeResponse(
        const std::string& requestId,
        bool succeeded,
        const std::string& message) -> void
    {
        const std::filesystem::path temporaryPath =
            m_runtimeDirectory /
            L"native-response.txt.tmp";

        const std::filesystem::path responsePath =
            m_runtimeDirectory /
            L"native-response.txt";

        std::ofstream responseFile(
            temporaryPath,
            std::ios::trunc);

        if (!responseFile.is_open())
        {
            return;
        }

        responseFile
            << "requestId=" << requestId << '\n'
            << "success="
            << (succeeded ? "true" : "false")
            << '\n'
            << "message=" << message << '\n';

        responseFile.close();

        // Limelight only ever sees a complete response.
        replaceFile(
            temporaryPath,
            responsePath);
    }

    auto queueUnrealCommand(
        PendingUnrealCommand command) -> bool
    {
        std::scoped_lock lock(
            m_pendingUnrealCommandMutex);

        if (m_pendingUnrealCommand.has_value())
        {
            return false;
        }

        m_pendingUnrealCommand =
            std::move(command);

        return true;
    }

    auto processPendingUnrealCommand() -> void
    {
        if (m_worldTransitioning.load() ||
            std::chrono::steady_clock::now() <
                m_worldReadyAfter)
        {
            // Keep the request queued while Unreal is replacing its world.
            // Limelight's normal command timeout is long enough to wait for
            // the new level, so the user can press Activate without racing it.
            return;
        }

        // Reaching this point means LoadMap has finished and the short grace
        // period has passed. Manual live changes can be offered again.
        m_worldSettling.store(false);

        std::optional<PendingUnrealCommand> pendingCommand;

        {
            std::scoped_lock lock(
                m_pendingUnrealCommandMutex);

            if (!m_pendingUnrealCommand.has_value())
            {
                return;
            }

            pendingCommand =
                std::move(m_pendingUnrealCommand);

            m_pendingUnrealCommand.reset();
        }

        // UObject traversal and package renaming must happen on Unreal's game
        // thread. Running this from UE4SS's background update loop can look
        // successful at first, then leave a bad reference for the next map.
        if (pendingCommand->action ==
            "mount_pak")
        {
            const LivePakMountResult mountResult =
                mountLivePak(
                    pendingCommand->pakPath,
                    pendingCommand->mountOrder);

            Output::send<LogLevel::Normal>(
                mountResult.succeeded
                    ? STR("[Limelight] A staged IoStore container was mounted on the game thread.\n")
                    : STR("[Limelight] The staged IoStore container was not mounted on the game thread.\n"));

            writeResponse(
                pendingCommand->requestId,
                mountResult.succeeded,
                mountResult.message);
        }
        else if (pendingCommand->action ==
                 "unmount_pak")
        {
            const LivePakUnmountResult unmountResult =
                unmountLivePak(
                    pendingCommand->pakPath);

            Output::send<LogLevel::Normal>(
                unmountResult.succeeded
                    ? STR("[Limelight] A retired IoStore container was unmounted on the game thread.\n")
                    : STR("[Limelight] A retired IoStore container could not be unmounted on the game thread.\n"));

            writeResponse(
                pendingCommand->requestId,
                unmountResult.succeeded,
                unmountResult.message);
        }
        else if (pendingCommand->action ==
                 "release_charlie_package")
        {
            const CharliePackageReleaseResult releaseResult =
                releaseCharliePackage();

            Output::send<LogLevel::Normal>(
                releaseResult.succeeded
                    ? STR("[Limelight] The cached Charlie package was released on the game thread.\n")
                    : STR("[Limelight] The cached Charlie package could not be released on the game thread.\n"));

            writeResponse(
                pendingCommand->requestId,
                releaseResult.succeeded,
                releaseResult.message);
        }
        else if (pendingCommand->action ==
                 "release_packages")
        {
            const PackageReleaseResult releaseResult =
                releasePackages(
                    pendingCommand->packagePaths);

            Output::send<LogLevel::Normal>(
                releaseResult.succeeded
                    ? STR("[Limelight] Character dependency packages were released on the game thread.\n")
                    : STR("[Limelight] Character dependency packages could not all be released on the game thread.\n"));

            writeResponse(
                pendingCommand->requestId,
                releaseResult.succeeded,
                releaseResult.message);
        }
    }

    auto processCommand() -> void
    {
        const std::filesystem::path commandPath =
            m_runtimeDirectory /
            L"native-command.txt";

        std::error_code fileError;

        if (!std::filesystem::exists(
                commandPath,
                fileError))
        {
            return;
        }

        const ValueMap command =
            readValues(commandPath);

        const auto requestIdEntry =
            command.find("requestId");

        const auto actionEntry =
            command.find("action");

        if (requestIdEntry == command.end() ||
            requestIdEntry->second.empty() ||
            actionEntry == command.end())
        {
            // A broken command cannot be answered, so clear it out rather
            // than letting it block every command behind it.
            std::filesystem::remove(
                commandPath,
                fileError);

            return;
        }

        const std::string& requestId =
            requestIdEntry->second;

        if (requestId == m_lastRequestId)
        {
            std::filesystem::remove(
                commandPath,
                fileError);

            return;
        }

        m_lastRequestId = requestId;

        const std::string& action =
            actionEntry->second;

        if (action == "ping")
        {
            Output::send<LogLevel::Normal>(
                STR("[Limelight] Native ping received.\n"));

            writeResponse(
                requestId,
                true,
                "Limelight native bridge is online");
        }
        else if (action == "can_switch_mods")
        {
            const bool canSwitch =
                !m_worldTransitioning.load() &&
                !m_worldSettling.load();

            writeResponse(
                requestId,
                canSwitch,
                canSwitch
                    ? "Unreal is ready for a live mod change."
                    : "Dead as Disco is changing levels, so live mod switching is temporarily locked.");
        }
        else if (action == "resolve_mount")
        {
            const MountResolverResult resolverResult =
                resolveMountFunctions();

            if (resolverResult.succeeded)
            {
                // This only confirms that the two Unreal mount functions are
                // where we expect them to be. Nothing is mounted yet.
                Output::send<LogLevel::Normal>(
                    STR("[Limelight] Unreal's mount functions were resolved.\n"));
            }
            else
            {
                Output::send<LogLevel::Error>(
                    STR("[Limelight] Unreal's mount functions could not be resolved.\n"));
            }

            writeResponse(
                requestId,
                resolverResult.succeeded,
                resolverResult.message);
        }
        else if (action == "mount_pak")
        {
            const auto pakPathEntry =
                command.find("pakPath");

            if (pakPathEntry == command.end() ||
                pakPathEntry->second.empty())
            {
                writeResponse(
                    requestId,
                    false,
                    "The mount command did not include a staged pak path");
            }
            else
            {
                std::int32_t mountOrder = 1000;

                const auto mountOrderEntry =
                    command.find("mountOrder");

                if (mountOrderEntry != command.end())
                {
                    try
                    {
                        mountOrder =
                            std::stoi(
                                mountOrderEntry->second);
                    }
                    catch (...)
                    {
                        // A bad optional value should not invalidate the whole
                        // command; the normal Limelight priority is safe.
                        mountOrder = 1000;
                    }
                }

                const bool queued =
                    queueUnrealCommand({
                        requestId,
                        action,
                        {},
                        std::filesystem::path(
                            pakPathEntry->second),
                        mountOrder
                    });

                if (!queued)
                {
                    writeResponse(
                        requestId,
                        false,
                        "Unreal is still processing the previous live-loader command");
                }
            }
        }
        else if (action == "unmount_pak")
        {
            const auto pakPathEntry =
                command.find("pakPath");

            if (pakPathEntry == command.end() ||
                pakPathEntry->second.empty())
            {
                writeResponse(
                    requestId,
                    false,
                    "The unmount command did not include a pak path");
            }
            else
            {
                const bool queued =
                    queueUnrealCommand({
                        requestId,
                        action,
                        {},
                        std::filesystem::path(
                            pakPathEntry->second)
                    });

                if (!queued)
                {
                    writeResponse(
                        requestId,
                        false,
                        "Unreal is still processing the previous live-loader command");
                }
            }
        }
        else if (action == "release_charlie_package")
        {
            const bool queued =
                queueUnrealCommand({
                    requestId,
                    action,
                    {}
                });

            if (!queued)
            {
                writeResponse(
                    requestId,
                    false,
                    "Unreal is still processing the previous refresh command");
            }
        }
        else if (action == "release_packages")
        {
            const auto packagePathsEntry =
                command.find("packagePaths");

            if (packagePathsEntry == command.end() ||
                packagePathsEntry->second.empty())
            {
                writeResponse(
                    requestId,
                    false,
                    "The refresh command did not include any package paths");
            }
            else
            {
                const bool queued =
                    queueUnrealCommand({
                        requestId,
                        action,
                        splitPackagePaths(
                            packagePathsEntry->second)
                    });

                if (!queued)
                {
                    writeResponse(
                        requestId,
                        false,
                        "Unreal is still processing the previous refresh command");
                }
            }
        }
        else
        {
            writeResponse(
                requestId,
                false,
                "Unknown native command: " + action);
        }

        std::filesystem::remove(
            commandPath,
            fileError);
    }

public:
    LimelightNativeBridge() : CppUserModBase()
    {
        ModName = STR("LimelightNativeBridge");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Native live-loading support for Limelight.");
        ModAuthors = STR("Limelight Team");

        // Leave a breadcrumb in the UE4SS log so we know the bridge actually
        // loaded.
        Output::send<LogLevel::Normal>(
            STR("[Limelight] Native bridge DLL loaded.\n"));

        prepareRuntimeDirectory();
    }

    ~LimelightNativeBridge() override
    {
        if (m_engineTickCallbackId !=
            Unreal::Hook::ERROR_ID)
        {
            Unreal::Hook::UnregisterCallback(
                m_engineTickCallbackId);
        }

        if (m_loadMapPreCallbackId !=
            Unreal::Hook::ERROR_ID)
        {
            Unreal::Hook::UnregisterCallback(
                m_loadMapPreCallbackId);
        }

        if (m_loadMapPostCallbackId !=
            Unreal::Hook::ERROR_ID)
        {
            Unreal::Hook::UnregisterCallback(
                m_loadMapPostCallbackId);
        }
    }

    auto on_unreal_init() -> void override
    {
        // Unreal is ready now, so it is safe for us to start looking at game
        // objects.
        Output::send<LogLevel::Normal>(
            STR("[Limelight] Native bridge is connected to Unreal.\n"));

        m_engineTickCallbackId =
            Unreal::Hook::RegisterEngineTickPreCallback(
                [this](auto&,
                       Unreal::UEngine*,
                       float,
                       bool)
                {
                    processPendingUnrealCommand();
                },
                {
                    false,
                    true,
                    STR("LimelightNativeBridge"),
                    STR("ProcessPendingRefresh")
                });

        m_loadMapPreCallbackId =
            Unreal::Hook::RegisterLoadMapPreCallback(
                [this](auto&,
                       Unreal::UEngine*,
                       Unreal::FWorldContext&,
                       Unreal::FURL,
                       Unreal::UPendingNetGame*,
                       Unreal::FString&)
                {
                    m_worldTransitioning.store(true);
                    m_worldSettling.store(true);

                    Output::send<LogLevel::Normal>(
                        STR("[Limelight] Level transition started; live changes will wait.\n"));
                },
                {
                    false,
                    true,
                    STR("LimelightNativeBridge"),
                    STR("PauseDuringLoadMap")
                });

        m_loadMapPostCallbackId =
            Unreal::Hook::RegisterLoadMapPostCallback(
                [this](auto&,
                       Unreal::UEngine*,
                       Unreal::FWorldContext&,
                       Unreal::FURL,
                       Unreal::UPendingNetGame*,
                       Unreal::FString&)
                {
                    m_worldTransitioning.store(false);
                    m_worldSettling.store(true);

                    // Give streamed actors and preview components a moment to
                    // finish registering before a queued live refresh resumes.
                    m_worldReadyAfter =
                        std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);

                    Output::send<LogLevel::Normal>(
                        STR("[Limelight] Level transition finished; queued live changes may resume shortly.\n"));
                },
                {
                    false,
                    true,
                    STR("LimelightNativeBridge"),
                    STR("ResumeAfterLoadMap")
                });

        if (m_engineTickCallbackId ==
            Unreal::Hook::ERROR_ID)
        {
            Output::send<LogLevel::Error>(
                STR("[Limelight] The game-thread refresh hook could not be installed.\n"));
        }


        if (m_loadMapPreCallbackId ==
                Unreal::Hook::ERROR_ID ||
            m_loadMapPostCallbackId ==
                Unreal::Hook::ERROR_ID)
        {
            Output::send<LogLevel::Error>(
                STR("[Limelight] The level-transition safety hooks could not be installed.\n"));
        }
    }

    auto on_update() -> void override
    {
        if (!m_runtimeDirectoryReady)
        {
            return;
        }

        const auto currentTime =
            std::chrono::steady_clock::now();

        if (currentTime >= m_nextHeartbeat)
        {
            // There is no reason to touch the heartbeat on every game frame.
            writeHeartbeat();

            m_nextHeartbeat =
                currentTime +
                std::chrono::seconds(1);
        }

        if (currentTime >= m_nextCommandCheck)
        {
            processCommand();

            m_nextCommandCheck =
                currentTime +
                std::chrono::milliseconds(100);
        }
    }
};

#define LIMELIGHT_NATIVE_API __declspec(dllexport)

extern "C"
{
    LIMELIGHT_NATIVE_API RC::CppUserModBase* start_mod()
    {
        return new LimelightNativeBridge();
    }

    LIMELIGHT_NATIVE_API void uninstall_mod(
        RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
