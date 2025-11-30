#ifndef WFX_INC_HTTP_ALIASES_HPP
#define WFX_INC_HTTP_ALIASES_HPP

/*
 * This file may seem redundant and is like 99% redundant
 * But what my thought process is simple
 * This included user alises, basically instead of user using WFX::Http::SomeRandomStuff::Idk
 * User can directly do 'Http::SomeRandomStuff::Idk' or directly 'Idk' for some stuff
 * Pretty much either removing the common 'WFX' namespace or entirely aliasing for more-
 * -commonly used stuff
 */

#include "http/request/http_request.hpp"

using namespace WFX;

// In sync with 'Response'
using Request = Http::HttpRequest;

// HTTP Constants
using Http::HttpStatus;
using Http::HttpMethod;
using Http::HttpVersion;

#endif // WFX_INC_HTTP_ALIASES_HPP
