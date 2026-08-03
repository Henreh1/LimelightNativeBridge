#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "MountResolver.hpp"

namespace
{
    struct ImageView
    {
        std::uint8_t* base{};
        std::uintptr_t start{};
        std::uintptr_t end{};
        const IMAGE_NT_HEADERS64* headers{};
    };

    std::optional<MountResolverResult> cachedResolver;

    constexpr std::uint32_t resolverCacheFormatVersion = 2;
    constexpr std::size_t resolverSignatureSize = 24;

    struct ResolverCacheRecord
    {
        std::array<char, 8> magic{
            'L', 'M', 'R', 'E', 'S', '2', '\0', '\0'};
        std::uint32_t formatVersion{
            resolverCacheFormatVersion};
        std::uint32_t imageTimestamp{};
        std::uint32_t imageSize{};
        std::uint32_t imageChecksum{};
        std::uint64_t mountRva{};
        std::uint64_t unmountRva{};
        std::uint32_t methodOffset{};
        std::array<std::uint8_t, resolverSignatureSize>
            mountSignature{};
        std::array<std::uint8_t, resolverSignatureSize>
            unmountSignature{};
    };

    auto protectionCanBeRead(
        DWORD protection) -> bool
    {
        if ((protection & PAGE_GUARD) != 0 ||
            (protection & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        switch (protection & 0xff)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    auto protectionCanExecute(
        DWORD protection) -> bool
    {
        if ((protection & PAGE_GUARD) != 0)
        {
            return false;
        }

        switch (protection & 0xff)
        {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    struct MountOwnerCandidate
    {
        void* delegateInstance{};
        void* platformFile{};
        std::size_t methodOffset{};
    };

    struct ProcessOwnerScan
    {
        std::vector<MountOwnerCandidate> candidates;
        std::size_t methodHits{};
    };

    auto protectionCanBeWritten(
        DWORD protection) -> bool
    {
        if ((protection & PAGE_GUARD) != 0 ||
            (protection & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        switch (protection & 0xff)
        {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    auto rangeCanBeRead(
        std::uintptr_t address,
        std::size_t size) -> bool
    {
        if (address == 0 ||
            size == 0 ||
            address + size < address)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};

        if (VirtualQuery(
                reinterpret_cast<void*>(address),
                &memory,
                sizeof(memory)) == 0)
        {
            return false;
        }

        const std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(
                memory.BaseAddress) +
            memory.RegionSize;

        return
            memory.State == MEM_COMMIT &&
            protectionCanBeRead(memory.Protect) &&
            address + size <= regionEnd;
    }

    auto tryReadMemory(
        std::uintptr_t address,
        void* destination,
        std::size_t size) -> bool
    {
        SIZE_T bytesRead = 0;

        // Unreal can release or protect a page between VirtualQuery and the
        // actual read. ReadProcessMemory turns that race into a clean failure
        // instead of allowing the resolver to crash inside the game process.
        return
            ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(address),
                destination,
                size,
                &bytesRead) != FALSE &&
            bytesRead == size;
    }

    auto readPointer(
        std::uintptr_t address) -> std::uintptr_t
    {
        std::uintptr_t value = 0;

        tryReadMemory(
            address,
            &value,
            sizeof(value));

        return value;
    }

    auto addressIsExecutable(
        const ImageView& image,
        std::uintptr_t address) -> bool
    {
        if (address < image.start ||
            address >= image.end)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};

        if (VirtualQuery(
                reinterpret_cast<void*>(address),
                &memory,
                sizeof(memory)) == 0)
        {
            return false;
        }

        return
            memory.State == MEM_COMMIT &&
            protectionCanExecute(memory.Protect);
    }

    auto looksLikePolymorphicObject(
        const ImageView& image,
        std::uintptr_t objectAddress) -> bool
    {
        if (!rangeCanBeRead(
                objectAddress,
                sizeof(std::uintptr_t)))
        {
            return false;
        }

        const std::uintptr_t virtualTable =
            readPointer(objectAddress);

        if (virtualTable < image.start ||
            virtualTable >= image.end ||
            !rangeCanBeRead(
                virtualTable,
                sizeof(std::uintptr_t)))
        {
            return false;
        }

        const std::uintptr_t firstVirtualFunction =
            readPointer(virtualTable);

        return addressIsExecutable(
            image,
            firstVirtualFunction);
    }

    auto addMountOwnerCandidate(
        const ImageView& image,
        std::uintptr_t delegateInstance,
        std::uintptr_t mountFunction,
        std::size_t scanSize,
        std::vector<MountOwnerCandidate>& candidates) -> void
    {
        scanSize =
            std::min<std::size_t>(
                scanSize,
                96);

        if (scanSize < 24 ||
            !rangeCanBeRead(
                delegateInstance,
                scanSize) ||
            !looksLikePolymorphicObject(
                image,
                delegateInstance))
        {
            return;
        }

        for (std::size_t methodOffset = 16;
             methodOffset + sizeof(std::uintptr_t) <= scanSize;
             methodOffset += sizeof(std::uintptr_t))
        {
            const std::uintptr_t possibleMethod =
                readPointer(
                    delegateInstance +
                    methodOffset);

            if (possibleMethod != mountFunction)
            {
                continue;
            }

            const std::uintptr_t possibleOwner =
                readPointer(
                    delegateInstance +
                    methodOffset -
                    sizeof(std::uintptr_t));

            if (!looksLikePolymorphicObject(
                    image,
                    possibleOwner))
            {
                continue;
            }

            const auto duplicate =
                std::find_if(
                    candidates.begin(),
                    candidates.end(),
                    [possibleOwner](
                        const MountOwnerCandidate& candidate)
                    {
                        // I treat copied delegates as the same candidate when
                        // they still point to the same platform-file owner.
                        return
                            candidate.platformFile ==
                                reinterpret_cast<void*>(
                                    possibleOwner);
                    });

            if (duplicate == candidates.end())
            {
                candidates.push_back({
                    reinterpret_cast<void*>(
                        delegateInstance),
                    reinterpret_cast<void*>(
                        possibleOwner),
                    methodOffset
                });
            }
        }
    }

    auto findMountOwnerCandidates(
        const ImageView& image,
        std::uint8_t* mountFunction) ->
        std::vector<MountOwnerCandidate>
    {
        std::vector<MountOwnerCandidate> candidates;

        const std::uintptr_t mountFunctionAddress =
            reinterpret_cast<std::uintptr_t>(
                mountFunction);

        std::uintptr_t cursor = image.start;

        while (cursor < image.end)
        {
            MEMORY_BASIC_INFORMATION memory{};

            if (VirtualQuery(
                    reinterpret_cast<void*>(cursor),
                    &memory,
                    sizeof(memory)) == 0)
            {
                break;
            }

            const std::uintptr_t regionStart =
                std::max(
                    cursor,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress));

            const std::uintptr_t regionEnd =
                std::min(
                    image.end,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress) +
                        memory.RegionSize);

            if (memory.State == MEM_COMMIT &&
                protectionCanBeWritten(memory.Protect))
            {
                std::uintptr_t address =
                    (regionStart + 7) &
                    ~static_cast<std::uintptr_t>(7);

                for (;
                     address + 16 <= regionEnd;
                     address += sizeof(std::uintptr_t))
                {
                    const std::uintptr_t value =
                        readPointer(address);

                    // A heap-backed delegate stores its allocation pointer
                    // followed by the size of that allocation.
                    std::uint32_t delegateSize = 0;

                    if (!tryReadMemory(
                        address +
                            sizeof(std::uintptr_t),
                        &delegateSize,
                        sizeof(delegateSize)))
                    {
                        continue;
                    }

                    if (delegateSize >= 24 &&
                        delegateSize <= 128)
                    {
                        addMountOwnerCandidate(
                            image,
                            value,
                            mountFunctionAddress,
                            delegateSize,
                            candidates);
                    }

                    // Keep this fallback for builds which store a small
                    // delegate directly inside the global delegate object.
                    if (value == mountFunctionAddress)
                    {
                        for (std::size_t methodOffset = 16;
                             methodOffset <= 80;
                             methodOffset +=
                                 sizeof(std::uintptr_t))
                        {
                            if (address <
                                regionStart + methodOffset)
                            {
                                continue;
                            }

                            addMountOwnerCandidate(
                                image,
                                address - methodOffset,
                                mountFunctionAddress,
                                96,
                                candidates);
                        }
                    }
                }
            }

            if (regionEnd <= cursor)
            {
                break;
            }

            cursor = regionEnd;
        }

        return candidates;
    }

    auto findMountOwnerCandidatesProcessWide(
        const ImageView& image,
        std::uint8_t* mountFunction,
        bool privateMemoryOnly = false) -> ProcessOwnerScan
    {
        ProcessOwnerScan result;

        const std::uintptr_t mountFunctionAddress =
            reinterpret_cast<std::uintptr_t>(
                mountFunction);

        SYSTEM_INFO systemInformation{};
        GetSystemInfo(&systemInformation);

        std::uintptr_t cursor =
            reinterpret_cast<std::uintptr_t>(
                systemInformation.lpMinimumApplicationAddress);

        const std::uintptr_t maximumAddress =
            reinterpret_cast<std::uintptr_t>(
                systemInformation.lpMaximumApplicationAddress);

        while (cursor < maximumAddress)
        {
            MEMORY_BASIC_INFORMATION memory{};

            if (VirtualQuery(
                    reinterpret_cast<void*>(cursor),
                    &memory,
                    sizeof(memory)) == 0)
            {
                break;
            }

            const std::uintptr_t regionStart =
                std::max(
                    cursor,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress));

            const std::uintptr_t regionEnd =
                std::min(
                    maximumAddress,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress) +
                        memory.RegionSize);

            if (memory.State == MEM_COMMIT &&
                (!privateMemoryOnly || memory.Type == MEM_PRIVATE) &&
                protectionCanBeWritten(memory.Protect))
            {
                const std::size_t pageSize =
                    std::max<std::size_t>(
                        systemInformation.dwPageSize,
                        sizeof(std::uintptr_t));

                // I snapshot one page at a time before scanning it. Unreal is
                // free to reshape its heaps while Limelight works, but a page
                // that disappears simply gets skipped and can no longer take
                // the whole game down with an access violation.
                for (std::uintptr_t pageStart = regionStart;
                     pageStart < regionEnd;)
                {
                    const std::size_t pageLength =
                        static_cast<std::size_t>(
                            std::min<std::uintptr_t>(
                                pageSize,
                                regionEnd - pageStart));

                    std::vector<std::uint8_t> snapshot(pageLength);

                    if (!tryReadMemory(
                            pageStart,
                            snapshot.data(),
                            snapshot.size()))
                    {
                        pageStart += pageLength;
                        continue;
                    }

                    std::uintptr_t address =
                        (pageStart + 7) &
                        ~static_cast<std::uintptr_t>(7);

                    for (;
                         address + sizeof(std::uintptr_t) <=
                             pageStart + pageLength;
                         address += sizeof(std::uintptr_t))
                    {
                        std::uintptr_t value = 0;
                        const std::size_t snapshotOffset =
                            static_cast<std::size_t>(
                                address - pageStart);

                        std::memcpy(
                            &value,
                            snapshot.data() + snapshotOffset,
                            sizeof(value));

                        if (value != mountFunctionAddress)
                        {
                            continue;
                        }

                        ++result.methodHits;

                        // Walk backwards from the method pointer until the
                        // delegate's vtable is found. The owner sits directly
                        // before the raw member-function pointer.
                        for (std::size_t methodOffset = 16;
                             methodOffset <= 96;
                             methodOffset += sizeof(std::uintptr_t))
                        {
                            if (address <
                                regionStart + methodOffset)
                            {
                                continue;
                            }

                            addMountOwnerCandidate(
                                image,
                                address - methodOffset,
                                mountFunctionAddress,
                                methodOffset +
                                    sizeof(std::uintptr_t),
                                result.candidates);
                        }
                    }

                    pageStart += pageLength;
                }
            }

            if (regionEnd <= cursor)
            {
                break;
            }

            cursor = regionEnd;
        }

        return result;
    }

    auto formatAddress(
        const void* address) -> std::string
    {
        std::ostringstream output;

        output
            << "0x"
            << std::uppercase
            << std::hex
            << reinterpret_cast<std::uintptr_t>(
                address);

        return output.str();
    }

    auto getMainImage(
        ImageView& image) -> bool
    {
        image.base =
            reinterpret_cast<std::uint8_t*>(
                GetModuleHandleW(nullptr));

        if (image.base == nullptr)
        {
            return false;
        }

        const auto* dosHeader =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(
                image.base);

        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return false;
        }

        image.headers =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                image.base +
                dosHeader->e_lfanew);

        if (image.headers->Signature != IMAGE_NT_SIGNATURE)
        {
            return false;
        }

        image.start =
            reinterpret_cast<std::uintptr_t>(
                image.base);

        image.end =
            image.start +
            image.headers->OptionalHeader.SizeOfImage;

        return true;
    }

    auto findWideText(
        const ImageView& image,
        const wchar_t* text,
        std::size_t textSize) -> std::uint8_t*
    {
        std::uintptr_t cursor = image.start;

        while (cursor < image.end)
        {
            MEMORY_BASIC_INFORMATION memory{};

            if (VirtualQuery(
                    reinterpret_cast<void*>(cursor),
                    &memory,
                    sizeof(memory)) == 0)
            {
                break;
            }

            const std::uintptr_t regionStart =
                std::max(
                    cursor,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress));

            const std::uintptr_t regionEnd =
                std::min(
                    image.end,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress) +
                        memory.RegionSize);

            if (memory.State == MEM_COMMIT &&
                protectionCanBeRead(memory.Protect))
            {
                for (std::uintptr_t address = regionStart;
                     address + textSize <= regionEnd;
                     ++address)
                {
                    if (std::memcmp(
                            reinterpret_cast<const void*>(address),
                            text,
                            textSize) == 0)
                    {
                        return reinterpret_cast<std::uint8_t*>(
                            address);
                    }
                }
            }

            if (regionEnd <= cursor)
            {
                break;
            }

            cursor = regionEnd;
        }

        return nullptr;
    }

    auto findContainingFunction(
        const ImageView& image,
        std::uintptr_t instructionAddress) -> std::uint8_t*
    {
        const IMAGE_DATA_DIRECTORY& exceptionDirectory =
            image.headers->OptionalHeader.DataDirectory[
                IMAGE_DIRECTORY_ENTRY_EXCEPTION];

        if (exceptionDirectory.VirtualAddress == 0 ||
            exceptionDirectory.Size < sizeof(RUNTIME_FUNCTION))
        {
            return nullptr;
        }

        const auto* functions =
            reinterpret_cast<const RUNTIME_FUNCTION*>(
                image.base +
                exceptionDirectory.VirtualAddress);

        const std::size_t functionCount =
            exceptionDirectory.Size /
            sizeof(RUNTIME_FUNCTION);

        const std::uint32_t instructionRva =
            static_cast<std::uint32_t>(
                instructionAddress -
                image.start);

        // Unreal's function table tells us where the compiler placed the real
        // function boundary, so we do not have to guess around padding bytes.
        for (std::size_t index = 0;
             index < functionCount;
             ++index)
        {
            const RUNTIME_FUNCTION& function =
                functions[index];

            if (instructionRva >= function.BeginAddress &&
                instructionRva < function.EndAddress)
            {
                return image.base +
                    function.BeginAddress;
            }
        }

        return nullptr;
    }

    auto findMountText(
        const ImageView& image) -> std::uint8_t*
    {
        // Unreal keeps this text beside both pak-mount entry points, which
        // gives us a reliable landmark without depending on fixed addresses.
        constexpr wchar_t mountText[] =
            L"Mounting pak file: %s \n";

        return findWideText(
            image,
            mountText,
            sizeof(mountText) - sizeof(wchar_t));
    }

    auto findUnmountText(
        const ImageView& image) -> std::uint8_t*
    {
        // The unmount delegate has its own shipping-build message. I use it
        // instead of assuming that Unreal placed the method beside MountPak.
        constexpr wchar_t unmountText[] =
            L"Unmounting pak file: %s \n";

        return findWideText(
            image,
            unmountText,
            sizeof(unmountText) - sizeof(wchar_t));
    }

    auto findFunctionSize(
        const ImageView& image,
        const std::uint8_t* functionStart) -> std::size_t
    {
        const IMAGE_DATA_DIRECTORY& exceptionDirectory =
            image.headers->OptionalHeader.DataDirectory[
                IMAGE_DIRECTORY_ENTRY_EXCEPTION];

        if (exceptionDirectory.VirtualAddress == 0 ||
            exceptionDirectory.Size < sizeof(RUNTIME_FUNCTION))
        {
            return static_cast<std::size_t>(-1);
        }

        const auto* functions =
            reinterpret_cast<const RUNTIME_FUNCTION*>(
                image.base +
                exceptionDirectory.VirtualAddress);

        const std::size_t functionCount =
            exceptionDirectory.Size /
            sizeof(RUNTIME_FUNCTION);

        const std::uint32_t functionRva =
            static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(
                    functionStart) -
                image.start);

        for (std::size_t index = 0;
             index < functionCount;
             ++index)
        {
            const RUNTIME_FUNCTION& function =
                functions[index];

            if (function.BeginAddress == functionRva)
            {
                return
                    function.EndAddress -
                    function.BeginAddress;
            }
        }

        return static_cast<std::size_t>(-1);
    }

    auto findReferencingFunctionStarts(
        const ImageView& image,
        const std::uint8_t* markerText) ->
        std::vector<std::uint8_t*>
    {
        std::vector<std::uint8_t*> functions;
        std::uintptr_t cursor = image.start;

        while (cursor < image.end)
        {
            MEMORY_BASIC_INFORMATION memory{};

            if (VirtualQuery(
                    reinterpret_cast<void*>(cursor),
                    &memory,
                    sizeof(memory)) == 0)
            {
                break;
            }

            const std::uintptr_t regionStart =
                std::max(
                    cursor,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress));

            const std::uintptr_t regionEnd =
                std::min(
                    image.end,
                    reinterpret_cast<std::uintptr_t>(
                        memory.BaseAddress) +
                        memory.RegionSize);

            if (memory.State == MEM_COMMIT &&
                protectionCanExecute(memory.Protect))
            {
                for (std::uintptr_t address = regionStart;
                     address + 7 <= regionEnd;
                     ++address)
                {
                    const auto* instruction =
                        reinterpret_cast<const std::uint8_t*>(
                            address);

                    // Look for a RIP-relative LEA instruction which points at
                    // the Unreal message used as this resolver's landmark.
                    if (instruction[0] < 0x40 ||
                        instruction[0] > 0x4f ||
                        instruction[1] != 0x8d ||
                        (instruction[2] & 0xc7) != 0x05)
                    {
                        continue;
                    }

                    std::int32_t displacement = 0;

                    std::memcpy(
                        &displacement,
                        instruction + 3,
                        sizeof(displacement));

                    const std::uintptr_t referencedAddress =
                        address +
                        7 +
                        static_cast<std::intptr_t>(
                            displacement);

                    if (referencedAddress !=
                        reinterpret_cast<std::uintptr_t>(
                            markerText))
                    {
                        continue;
                    }

                    std::uint8_t* functionStart =
                        findContainingFunction(
                            image,
                            address);

                    if (functionStart != nullptr &&
                        std::find(
                            functions.begin(),
                            functions.end(),
                            functionStart) == functions.end())
                    {
                        functions.push_back(
                            functionStart);
                    }
                }
            }

            if (regionEnd <= cursor)
            {
                break;
            }

            cursor = regionEnd;
        }

        std::sort(
            functions.begin(),
            functions.end());

        return functions;
    }

    auto formatRva(
        const ImageView& image,
        const void* address) -> std::string
    {
        const std::uintptr_t rva =
            reinterpret_cast<std::uintptr_t>(
                address) -
            image.start;

        std::ostringstream output;

        output
            << "0x"
            << std::uppercase
            << std::hex
            << rva;

        return output.str();
    }

    auto getResolverCachePath() ->
        std::optional<std::filesystem::path>
    {
        std::array<wchar_t, 32768> localAppData{};

        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));

        if (length == 0 ||
            length >= localAppData.size())
        {
            return std::nullopt;
        }

        return
            std::filesystem::path(localAppData.data()) /
            L"Limelight" /
            L"Cache" /
            L"native-resolver-v2.cache";
    }

    auto cacheRecordMatchesImage(
        const ResolverCacheRecord& record,
        const ImageView& image) -> bool
    {
        const std::array<char, 8> expectedMagic{
            'L', 'M', 'R', 'E', 'S', '2', '\0', '\0'};

        if (record.magic != expectedMagic ||
            record.formatVersion != resolverCacheFormatVersion)
        {
            return false;
        }

        if (record.imageTimestamp !=
                image.headers->FileHeader.TimeDateStamp ||
            record.imageSize !=
                image.headers->OptionalHeader.SizeOfImage ||
            record.imageChecksum !=
                image.headers->OptionalHeader.CheckSum)
        {
            return false;
        }

        if (record.methodOffset < 16 ||
            record.methodOffset > 96 ||
            record.methodOffset % sizeof(std::uintptr_t) != 0)
        {
            return false;
        }

        const auto signatureFits =
            [&image](std::uint64_t rva) -> bool
            {
                const std::uint64_t imageSize =
                    image.headers->OptionalHeader.SizeOfImage;

                return
                    rva < imageSize &&
                    resolverSignatureSize <= imageSize - rva;
            };

        if (!signatureFits(record.mountRva) ||
            !signatureFits(record.unmountRva))
        {
            return false;
        }

        const std::uint8_t* mountAddress =
            image.base + record.mountRva;

        const std::uint8_t* unmountAddress =
            image.base + record.unmountRva;

        return
            std::memcmp(
                mountAddress,
                record.mountSignature.data(),
                resolverSignatureSize) == 0 &&
            std::memcmp(
                unmountAddress,
                record.unmountSignature.data(),
                resolverSignatureSize) == 0;
    }

    auto readResolverCache(
        const ImageView& image) ->
        std::optional<ResolverCacheRecord>
    {
        try
        {
            const auto cachePath =
                getResolverCachePath();

            if (!cachePath.has_value())
            {
                return std::nullopt;
            }

            std::ifstream input(
                *cachePath,
                std::ios::binary);

            if (!input)
            {
                return std::nullopt;
            }

            ResolverCacheRecord record;

            input.read(
                reinterpret_cast<char*>(&record),
                sizeof(record));

            if (!input ||
                input.peek() !=
                    std::ifstream::traits_type::eof())
            {
                return std::nullopt;
            }

            if (!cacheRecordMatchesImage(record, image))
            {
                return std::nullopt;
            }

            return record;
        }
        catch (...)
        {
            // A cache problem should never stop the Live Loader. I simply
            // let the normal resolver perform a fresh scan instead.
            return std::nullopt;
        }
    }

    auto writeResolverCache(
        const ImageView& image,
        const void* mountFunction,
        const void* unmountFunction,
        std::size_t methodOffset) -> void
    {
        try
        {
            const auto cachePath =
                getResolverCachePath();

            if (!cachePath.has_value())
            {
                return;
            }

            std::filesystem::create_directories(
                cachePath->parent_path());

            ResolverCacheRecord record;
            record.imageTimestamp =
                image.headers->FileHeader.TimeDateStamp;
            record.imageSize =
                image.headers->OptionalHeader.SizeOfImage;
            record.imageChecksum =
                image.headers->OptionalHeader.CheckSum;
            record.mountRva =
                reinterpret_cast<std::uintptr_t>(mountFunction) -
                image.start;
            record.unmountRva =
                reinterpret_cast<std::uintptr_t>(unmountFunction) -
                image.start;
            record.methodOffset =
                static_cast<std::uint32_t>(methodOffset);

            std::memcpy(
                record.mountSignature.data(),
                mountFunction,
                resolverSignatureSize);

            std::memcpy(
                record.unmountSignature.data(),
                unmountFunction,
                resolverSignatureSize);

            std::filesystem::path temporaryPath =
                *cachePath;
            temporaryPath += L".tmp";

            std::ofstream output(
                temporaryPath,
                std::ios::binary | std::ios::trunc);

            if (!output)
            {
                return;
            }

            output.write(
                reinterpret_cast<const char*>(&record),
                sizeof(record));
            output.flush();

            if (!output)
            {
                return;
            }

            output.close();

            if (!MoveFileExW(
                    temporaryPath.c_str(),
                    cachePath->c_str(),
                    MOVEFILE_REPLACE_EXISTING |
                        MOVEFILE_WRITE_THROUGH))
            {
                std::error_code ignored;
                std::filesystem::remove(
                    temporaryPath,
                    ignored);
            }
        }
        catch (...)
        {
            // The resolver already succeeded, so a cache write failure is
            // not important enough to interrupt the current game launch.
        }
    }

    auto tryResolveFromPersistentCache(
        const ImageView& image,
        const ResolverCacheRecord& record) ->
        std::optional<MountResolverResult>
    {
        std::uint8_t* mountFunction =
            image.base +
                static_cast<std::size_t>(record.mountRva);
        std::uint8_t* unmountFunction =
            image.base +
                static_cast<std::size_t>(record.unmountRva);

        if (findFunctionSize(image, mountFunction) == 0 ||
            findFunctionSize(image, unmountFunction) == 0)
        {
            return std::nullopt;
        }

        // I only inspect writable private allocations on the cached path.
        // This confirms the live owner without repeating the full scan.
        ProcessOwnerScan processScan =
            findMountOwnerCandidatesProcessWide(
                image,
                mountFunction,
                true);

        std::size_t processMethodHits =
            processScan.methodHits;
        std::vector<MountOwnerCandidate> ownerCandidates =
            std::move(processScan.candidates);

        if (ownerCandidates.size() != 1 ||
            ownerCandidates.front().methodOffset !=
                record.methodOffset)
        {
            return std::nullopt;
        }

        const MountOwnerCandidate& owner =
            ownerCandidates.front();

        std::ostringstream message;
        message
            << "Persistent resolver cache accepted"
            << "; mount="
            << formatRva(image, mountFunction)
            << "; unmount="
            << formatRva(image, unmountFunction)
            << "; owner="
            << formatAddress(owner.platformFile)
            << "; methodOffset="
            << owner.methodOffset
            << "; processMethodHits="
            << processMethodHits;

        MountResolverResult result{
            true,
            message.str()
        };

        result.platformFile =
            owner.platformFile;
        result.mountFunction =
            mountFunction;
        result.unmountFunction =
            unmountFunction;

        return result;
    }
}

auto resolveMountFunctions() -> MountResolverResult
{
    if (cachedResolver.has_value())
    {
        return *cachedResolver;
    }

    ImageView image;

    if (!getMainImage(image))
    {
        return {
            false,
            "The Dead as Disco executable could not be inspected"
        };
    }

    // I try the verified cache before repeating the expensive marker and
    // cross-reference scan. The live platform-file owner is still found
    // again below, so no process address is trusted between game launches.
    if (const auto cache = readResolverCache(image);
        cache.has_value())
    {
        if (auto cachedResult =
                tryResolveFromPersistentCache(
                    image,
                    *cache);
            cachedResult.has_value())
        {
            cachedResolver =
                *cachedResult;

            return *cachedResult;
        }
    }

    std::uint8_t* mountText =
        findMountText(image);

    if (mountText == nullptr)
    {
        return {
            false,
            "Unreal's pak mount marker was not found"
        };
    }

    const std::vector<std::uint8_t*> functions =
        findReferencingFunctionStarts(
            image,
            mountText);

    std::ostringstream message;

    message
        << "Mount marker RVA="
        << formatRva(image, mountText)
        << "; functions="
        << functions.size();

    for (std::size_t index = 0;
         index < functions.size();
         ++index)
    {
        message
            << "; function"
            << (index + 1)
            << "="
            << formatRva(
                image,
                functions[index]);
    }

    if (functions.size() != 2)
    {
        return {
            false,
            "Expected two Unreal mount functions; " +
                message.str()
        };
    }

    // MountPaksEx is the large worker. The compact wrapper is the raw method
    // stored by FCoreDelegates::MountPak, so use the smaller function here.
    const auto mountFunctionEntry =
        std::min_element(
            functions.begin(),
            functions.end(),
            [&image](
                const std::uint8_t* left,
                const std::uint8_t* right)
            {
                return
                    findFunctionSize(image, left) <
                    findFunctionSize(image, right);
            });

    std::uint8_t* mountFunction =
        *mountFunctionEntry;

    message
        << "; selected="
        << formatRva(image, mountFunction)
        << "; selectedSize="
        << findFunctionSize(image, mountFunction);

    std::uint8_t* unmountText =
        findUnmountText(image);

    if (unmountText == nullptr)
    {
        return {
            false,
            "Unreal's pak unmount marker was not found; " +
                message.str()
        };
    }

    const std::vector<std::uint8_t*> unmountFunctions =
        findReferencingFunctionStarts(
            image,
            unmountText);

    message
        << "; unmountMarker="
        << formatRva(image, unmountText)
        << "; unmountFunctions="
        << unmountFunctions.size();

    if (unmountFunctions.size() != 1)
    {
        return {
            false,
            "Expected one Unreal unmount function; " +
                message.str()
        };
    }

    std::uint8_t* unmountFunction =
        unmountFunctions.front();

    message
        << "; unmountSelected="
        << formatRva(image, unmountFunction)
        << "; unmountSelectedSize="
        << findFunctionSize(image, unmountFunction);

    std::vector<MountOwnerCandidate> ownerCandidates =
        findMountOwnerCandidates(
            image,
            mountFunction);

    std::size_t processMethodHits = 0;

    if (ownerCandidates.empty())
    {
        // Some shipping builds keep the delegate allocation outside the main
        // executable's writable image, so widen the search only when needed.
        ProcessOwnerScan processScan =
            findMountOwnerCandidatesProcessWide(
                image,
                mountFunction);

        processMethodHits =
            processScan.methodHits;

        ownerCandidates =
            std::move(processScan.candidates);
    }

    message
        << "; uniqueOwners="
        << ownerCandidates.size()
        << "; processMethodHits="
        << processMethodHits;

    for (std::size_t index = 0;
         index < ownerCandidates.size();
         ++index)
    {
        const MountOwnerCandidate& candidate =
            ownerCandidates[index];

        message
            << "; ownerCandidate"
            << (index + 1)
            << "="
            << formatAddress(candidate.platformFile)
            << "; delegateCandidate"
            << (index + 1)
            << "="
            << formatAddress(candidate.delegateInstance)
            << "; candidateMethodOffset"
            << (index + 1)
            << "="
            << candidate.methodOffset;
    }

    if (ownerCandidates.size() != 1)
    {
        return {
            false,
            "Expected one live FPakPlatformFile owner; " +
                message.str()
        };
    }

    const MountOwnerCandidate& owner =
        ownerCandidates.front();

    message
        << "; owner="
        << formatAddress(owner.platformFile)
        << "; delegate="
        << formatAddress(owner.delegateInstance)
        << "; methodOffset="
        << owner.methodOffset;

    MountResolverResult result{
        true,
        "Mount owner ready; " +
            message.str()
    };

    result.platformFile =
        owner.platformFile;

    result.mountFunction =
        mountFunction;

    result.unmountFunction =
        unmountFunction;

    // I only save code locations and the verified owner layout. Live heap
    // addresses are rediscovered on every launch and never enter this file.
    writeResolverCache(
        image,
        mountFunction,
        unmountFunction,
        owner.methodOffset);

    // These addresses remain valid until the game process closes, so keep
    // them ready for every live mount after the first successful scan.
    cachedResolver = result;

    return result;
}
