#ifndef WFX_SHARED_EXPORT_MACRO_HPP
#define WFX_SHARED_EXPORT_MACRO_HPP

// Macros to tell compiler (when compiling user code into dll) that a specific-
// -part of code is used and it must not be optimized out (in simpler words that is, not the actual definition)
#if defined(_MSC_VER)
    #define WFX_EXPORT __declspec(dllexport)
#elif defined(__MINGW32__) || defined(__GNUC__) || defined(__clang__)
    #define WFX_EXPORT __attribute__((used)) __attribute__((visibility("default")))
#else
    #define WFX_EXPORT
#endif

#endif // WFX_SHARED_EXPORT_MACRO_HPP