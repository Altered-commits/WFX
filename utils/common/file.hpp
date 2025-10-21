#ifndef WFX_UTILS_FILE_COMMON_HPP
#define WFX_UTILS_FILE_COMMON_HPP

/* Common stuff in file operations */
#ifdef _WIN32
    #include <windows.h>
    using WFXFileDescriptor = HANDLE;
    using WFXFileSize       = std::uint64_t;

    constexpr WFXFileDescriptor WFX_INVALID_FILE = INVALID_HANDLE_VALUE;
#else
    #include <sys/types.h>
    using WFXFileDescriptor = int;
    using WFXFileSize       = off_t;

    constexpr WFXFileDescriptor WFX_INVALID_FILE = -1;
#endif

#endif // WFX_UTILS_FILE_COMMON_HPP