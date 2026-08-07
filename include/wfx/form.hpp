// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_FORM_HPP
#define WFX_INC_WFX_FORM_HPP

// -----------------------------------------------------------------------
// wfx/form.hpp
// Form parsing, validation, sanitization, and SSR rendering.
//
// Define a schema once (at file scope, or as a static local):
//
//   static const auto LoginForm = WFX::Form::Schema("login",
//       WFX::Form::Field("username", WFX::Form::Text{ .min = 3, .max = 32 }),
//       WFX::Form::Field("password", WFX::Form::Text{ .min = 8 })
//   );
//
// Then parse inside a handler:
//
//   WFX_POST("/login", [](WFX::Request req, WFX::Response res) {
//       decltype(LoginForm)::CleanedType data;
//       auto err = LoginForm.Parse(req, data);
//
//       if(err != WFX::Form::Ok) {
//           res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("bad form");
//           return;
//       }
//
//       // std::get<N>(data).present  -> was the field submitted?
//       // std::get<N>(data).value    -> the sanitized value
//       auto& username = std::get<0>(data);
//       if(username.present) { /* use username.value */ }
//   });
// -----------------------------------------------------------------------

#include "form/forms.hpp"
#include "form/fields.hpp"
#include "form/validators.hpp"
#include "form/sanitizers.hpp"
#include "form/renders.hpp"
#include "wfx/types.hpp"

namespace WFX::Form {

// -----------------------------------------------------------------------
// Re-export Field so users only need this header
// -----------------------------------------------------------------------
using WFX::Form::Field;

// -----------------------------------------------------------------------
// Parse result codes
//
//   WFX::Form::Ok             : all fields parsed and cleaned successfully
//   WFX::Form::BadContentType : Content-Type not supported (must be urlencoded)
//   WFX::Form::Malformed      : body structure is broken / wrong field order
//   WFX::Form::CleanFailed    : a field failed validation or sanitization
// -----------------------------------------------------------------------
// NOLINTBEGIN(readability-identifier-naming)
inline constexpr auto Ok = FormError::NONE;
inline constexpr auto BadContentType = FormError::UNSUPPORTED_CONTENT_TYPE;
inline constexpr auto Malformed = FormError::MALFORMED;
inline constexpr auto CleanFailed = FormError::CLEAN_FAILED;
// NOLINTEND(readability-identifier-naming)

// -----------------------------------------------------------------------
// Schema builder
//
//   auto form = WFX::Form::Schema("myform",
//       WFX::Form::Field("email", WFX::Form::Email{}),
//       WFX::Form::Field("age",   WFX::Form::UInt{ .min = 0, .max = 120 })
//   );
// -----------------------------------------------------------------------
template <std::size_t N, typename... Fields> constexpr auto Schema(const char (&name)[N], Fields&&... fields)
{
    return FormSchema<Fields...>(name, std::forward<Fields>(fields)...);
}

} // namespace WFX::Form

#endif // WFX_INC_WFX_FORM_HPP