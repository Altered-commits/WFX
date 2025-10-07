#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

/*
 * Bunch of stuff to help with other stuff
 * More to be added here, someday
 */

#include "http/common/route_common.hpp"

template<typename... MWs>
inline MiddlewareStack MakeMiddlewareFromFunctions(MWs&&... mws)
{
    MiddlewareStack stack;
    stack.reserve(sizeof...(mws));
    (stack.emplace_back(std::forward<MWs>(mws)), ...);    // fold-expression
    return stack;
}

#endif // WFX_INC_HTTP_HELPER_HPP