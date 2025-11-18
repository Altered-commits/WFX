#ifndef WFX_INC_CORE_MACROS_HPP
#define WFX_INC_CORE_MACROS_HPP

// All the common macros used widely across code base ig
// Mostly for differentiation of wot to do in debug / prod mode

// vvv Core Includes vvv
#ifdef WFX_DEBUG
    #include "utils/logger/logger.hpp"
#endif

// vvv Main Stuff vvv
#ifdef WFX_DEBUG
    #define WFX_ABORT(msg) WFX::Utils::Logger::GetInstance().Fatal(msg);
#else
    #define WFX_ABORT(msg) std::terminate();
#endif

#endif // WFX_INC_CORE_MACROS_HPP