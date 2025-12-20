#ifndef WFX_INC_FORM_RENDERS_HPP
#define WFX_INC_FORM_RENDERS_HPP

#include "fields.hpp"
#include <cstdlib>
#include <string>

namespace Form {

static inline void RenderInputAttributes(std::string& preRenderedForm, const Text& r)
{
    preRenderedForm += "type=\"text\" ";

    if(r.min)
        preRenderedForm += "minlength=\"" + std::to_string(r.min) + "\" ";
    if(r.max)
        preRenderedForm += "maxlength=\"" + std::to_string(r.max) + "\" ";

    // Strict printable characters
    if(r.ascii)
        preRenderedForm += "pattern=\"[\\x20-\\x7E]*\" ";
}

static inline void RenderInputAttributes(std::string& preRenderedForm, const Email&)
{
    preRenderedForm += "type=\"email\" ";
}

static inline void RenderInputAttributes(std::string& preRenderedForm, const Int& n)
{
    preRenderedForm += "type=\"number\" ";

    if(n.min)
        preRenderedForm += "min=\"" + std::to_string(n.min) + "\" ";
    if(n.max)
        preRenderedForm += "max=\"" + std::to_string(n.max) + "\" ";
}

static inline void RenderInputAttributes(std::string& preRenderedForm, const UInt& n)
{
    preRenderedForm += "type=\"number\" ";
    if(n.min)
        preRenderedForm += "min=\"" + std::to_string(n.min) + "\" ";
    if(n.max)
        preRenderedForm += "max=\"" + std::to_string(n.max) + "\" ";
}

static inline void RenderInputAttributes(std::string& preRenderedForm, const Float& f)
{
    preRenderedForm += "type=\"number\" ";
    if(f.min)
        preRenderedForm += "min=\"" + std::to_string(f.min) + "\" ";
    if(f.max)
        preRenderedForm += "max=\"" + std::to_string(f.max) + "\" ";
}

} // namespace Form

#endif // WFX_INC_FORM_RENDERS_HPP