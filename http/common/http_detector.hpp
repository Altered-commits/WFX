#ifndef WFX_HTTP_MIME_HPP
#define WFX_HTTP_MIME_HPP

#include <string_view>

namespace WFX::Http {

namespace MimeDetector {
    std::string_view DetectMimeFromExt(std::string_view path);
    std::string_view DetectExtFromMime(std::string_view mime);
} // namespace MimeDetector

namespace PortDetector {
    std::string_view DetectFromProtocol(std::string_view protocol);
}

} // namespace WFX::Http

#endif // WFX_HTTP_MIME_HPP