#ifndef WFX_HTTP_MIME_HPP
#define WFX_HTTP_MIME_HPP

#include <string_view>
#include <unordered_map>

namespace WFX::Http {

class MimeDetector {
public: // .cpp file is a hell to read
    static std::string_view DetectMimeFromExt(std::string_view path);
    static std::string_view DetectExtFromMime(std::string_view mime);
};

} // namespace WFX::Http

#endif // WFX_HTTP_MIME_HPP