#ifndef WFX_INC_ASYNC_MACROS_HPP
#define WFX_INC_ASYNC_MACROS_HPP

#include "interface.hpp"

// vvv Helper Macros vvv
#define CoAwaitHelper(awaitable, returnPtr, onError, counter) \
        if(Async::Await(__AsyncSelf, awaitable, returnPtr)) { \
            __AsyncSelf->SetState(counter);                   \
            return;                                           \
        }                                                     \
                                                              \
        if(__AsyncSelf->HasError()) {                         \
            onError                                           \
            __AsyncSelf->Finish();                            \
            return;                                           \
        }                                                     \
                                                              \
        [[fallthrough]];                                      \
    case counter:

// vvv Main Macros vvv
// NOTE: Macros here are not upper case because i'm trying to make them feel natural integrated-
//       -inside of a function. Thats the entire point of this header file
#define CoSelf AsyncPtr __AsyncSelf

#define CoStart                        \
    switch(__AsyncSelf->GetState()) {  \
        case 0:

#define CoEnd                      \
        default:                   \
            __AsyncSelf->Finish(); \
            break;                 \
    }

// vvv Await vvv
#define CoAwait(awaitable, onError)            CoAwaitHelper(awaitable, static_cast<void*>(nullptr), onError, __COUNTER__ + 1)
#define CoFetch(awaitable, returnVar, onError) CoAwaitHelper(awaitable, &returnVar, onError, __COUNTER__ + 1)

// vvv Return vvv
#define CoReturn(val)                                                                        \
    do {                                                                                     \
        if(auto* __Ptr = Async::SafeCastReturnPtr<std::decay_t<decltype(val)>>(__AsyncSelf)) \
            *__Ptr = std::move(val);                                                         \
        else                                                                                 \
            __AsyncSelf->SetError(Async::Error::INTERNAL_FAILURE);                           \
        __AsyncSelf->Finish();                                                               \
        return;                                                                              \
    }                                                                                        \
    while(0);

// vvv Misc Handling vvv
#define CoGetError()     __AsyncSelf->GetError()
#define CoSetError(err)  __AsyncSelf->SetError(err)

#define CoVariable(type, name, ...) \
    auto& name = __AsyncSelf->PersistLocal(#name).InitOrGet<type>(__VA_ARGS__);

#endif // WFX_INC_ASYNC_MACROS_HPP