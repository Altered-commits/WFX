#ifndef WFX_INCLUDE_EXPORT_SIGNATURE_HPP
#define WFX_INCLUDE_EXPORT_SIGNATURE_HPP

#if defined(_WIN32)
    #define WFX_API __declspec(dllexport)
#else
    #define WFX_API __attribute__((visibility("default")))
#endif // defined(_WIN32)

#endif // WFX_INCLUDE_EXPORT_SIGNATURE_HPP