#ifndef WFX_INC_HTTP_ALIASES_HPP
#define WFX_INC_HTTP_ALIASES_HPP

/*
 * This file may seem redundant and is like 99% redundant
 * But what my thought process is simple
 * This included user alises, basically instead of user using WFX::Http::SomeRandomStuff::Idk
 * User can directly do 'Idk', and its easier for them as well
 * Easier for user (Mostly for beginners). IMPORTANT -> This file is for Http aliases
 * You get the point... hopefully
 */

#include "http/request/http_request.hpp"

// In sync with 'Response'
using Request = WFX::Http::HttpRequest;

// HTTP Constants
using Status  = WFX::Http::HttpStatus;
using Method  = WFX::Http::HttpMethod;
using Version = WFX::Http::HttpVersion;

#endif // WFX_INC_HTTP_ALIASES_HPP
