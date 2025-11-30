#ifndef WFX_INC_ASYNC_MACROS_HPP
#define WFX_INC_ASYNC_MACROS_HPP

#include "interface.hpp"

// vvv Helper Macros vvv
#define AwaitHelper(awaitable, onError, counter)   \
        if(Async::Await(__AsyncSelf, awaitable)) { \
            __AsyncSelf->SetState(counter);        \
            return;                                \
        }                                          \
                                                   \
        if(__AsyncSelf->HasError()) {              \
            onError                                \
            __AsyncSelf->Finish();                 \
            return;                                \
        }                                          \
                                                   \
        [[fallthrough]];                           \
    case counter:

// vvv Main Macros vvv
// NOTE: Macros here are not upper case because i'm trying to make them feel natural integrated-
//       -inside of a function. Thats the entire point of this header file
#define AsyncInit AsyncPtr __AsyncSelf

#define AsyncStart                      \
    switch(__AsyncSelf->GetState()) {   \
        case 0:

#define AsyncEnd                   \
        default:                   \
            __AsyncSelf->Finish(); \
            break;                 \
    }

#define Await(awaitable, onError) AwaitHelper(awaitable, onError, __COUNTER__)

// vvv Error Handling vvv
#define AsyncGetError()    __AsyncSelf->GetError()
#define AsyncSetError(err) __AsyncSelf->SetError(err)

#endif // WFX_INC_ASYNC_MACROS_HPP