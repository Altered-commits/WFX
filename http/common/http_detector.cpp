#include "http_detector.hpp"

#include <unordered_map>

namespace WFX::Http {

// THIS IS GOING TO BE FUN AMIRITE
inline const std::unordered_map<std::string_view, std::string_view> MimeFromExt = {
    // ───── General Archive / Package Formats ─────
    {"epub", "application/epub+zip"}, {"gz", "application/gzip"}, {"tgz", "application/gzip"}, // common tarball alias
    {"jar", "application/java-archive"}, {"mpkg", "application/vnd.apple.installer+xml"},
    {"apk", "application/vnd.android.package-archive"}, {"cab", "application/vnd.ms-cab-compressed"},
    {"oxt", "application/vnd.openofficeorg.extension"}, {"xps", "application/vnd.ms-xpsdocument"},
    {"7z", "application/x-7z-compressed"}, {"rar", "application/vnd.rar"}, {"zip", "application/zip"},
    {"tar", "application/x-tar"}, {"bz2", "application/x-bzip2"},

    // ───── Document / Publishing Formats ─────
    {"pdf", "application/pdf"}, {"opf", "application/oebps-package+xml"}, {"xhtml", "application/xhtml+xml"},
    {"xml", "application/xml"}, {"rss", "application/rss+xml"}, {"rdf", "application/rdf+xml"},
    {"rtf", "application/rtf"}, {"smil", "application/smil+xml"}, {"smi", "application/smil+xml"},

    // ───── PostScript Family ─────
    {"ps", "application/postscript"}, {"ai", "application/postscript"}, {"eps", "application/postscript"},

    // ───── Adobe XML Forms ─────
    {"xdp", "application/vnd.adobe.xdp+xml"}, {"xfdf", "application/vnd.adobe.xfdf"},

    // ───── eBook ─────
    {"azw", "application/vnd.amazon.ebook"},

    // ───── Microsoft Word ─────
    {"doc", "application/msword"}, {"dot", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"dotx", "application/vnd.openxmlformats-officedocument.wordprocessingml.template"},
    {"docm", "application/vnd.ms-word.document.macroenabled.12"},
    {"dotm", "application/vnd.ms-word.template.macroenabled.12"},

    // ───── Microsoft Excel ─────
    {"xls", "application/vnd.ms-excel"}, {"xlt", "application/vnd.ms-excel"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"xltx", "application/vnd.openxmlformats-officedocument.spreadsheetml.template"},
    {"xlsm", "application/vnd.ms-excel.sheet.macroenabled.12"},
    {"xltm", "application/vnd.ms-excel.template.macroenabled.12"},
    {"xlsb", "application/vnd.ms-excel.sheet.binary.macroenabled.12"},
    {"xlam", "application/vnd.ms-excel.addin.macroenabled.12"},

    // ───── Microsoft PowerPoint ─────
    {"ppt", "application/vnd.ms-powerpoint"}, {"pps", "application/vnd.ms-powerpoint"}, {"pot", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"ppsx", "application/vnd.openxmlformats-officedocument.presentationml.slideshow"},
    {"potx", "application/vnd.openxmlformats-officedocument.presentationml.template"},
    {"pptm", "application/vnd.ms-powerpoint.presentation.macroenabled.12"},
    {"ppsm", "application/vnd.ms-powerpoint.slideshow.macroenabled.12"},
    {"potm", "application/vnd.ms-powerpoint.template.macroenabled.12"},
    {"ppam", "application/vnd.ms-powerpoint.addin.macroenabled.12"},
    {"sldm", "application/vnd.ms-powerpoint.slide.macroenabled.12"},
    {"sldx", "application/vnd.openxmlformats-officedocument.presentationml.slide"},

    // ───── OpenDocument Formats ─────
    {"odt", "application/vnd.oasis.opendocument.text"}, {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
    {"odp", "application/vnd.oasis.opendocument.presentation"}, {"odg", "application/vnd.oasis.opendocument.graphics"},
    {"odc", "application/vnd.oasis.opendocument.chart"}, {"odb", "application/vnd.oasis.opendocument.database"},

    // ───── Fonts ─────
    {"eot",  "application/vnd.ms-fontobject"},

    // ───── Google Earth ─────
    {"kml", "application/vnd.google-earth.kml+xml"}, {"kmz", "application/vnd.google-earth.kmz"},

    // ───── Fujixerox DocuWorks ─────
    {"xdw", "application/vnd.fujixerox.docuworks"}, {"xbd", "application/vnd.fujixerox.docuworks.binder"},

    // ───── Education / CAD / Graphics ─────
    {"ggb", "application/vnd.geogebra.file"}, {"ggt", "application/vnd.geogebra.tool"},
    {"gbr", "application/vnd.gerber"}, {"jam", "application/vnd.jam"},

    // ───── Color Profiles ─────
    {"icc", "application/vnd.iccprofile"}, {"icm", "application/vnd.iccprofile"},

    // ───── Finance ─────
    {"qbo", "application/vnd.intu.qbo"}, {"qfx", "application/vnd.intu.qfx"},

    // ───── Debian Packages ─────
    {"deb",  "application/vnd.debian.binary-package"}, {"udeb", "application/vnd.debian.binary-package"},

    // ───── Forms / Help / Legacy ─────
    {"fdf", "application/vnd.fdf"}, {"chm", "application/vnd.ms-htmlhelp"},   // Windows .chm help files
    {"xul", "application/vnd.mozilla.xul+xml"}, // Legacy Firefox

    // ───── Windows / Legacy Formats ─────
    {"hlp", "application/winhlp"}, {"wspolicy", "application/wspolicy+xml"}, {"wsdl", "application/wsdl+xml"},

    // ───── SQLite ─────
    {"sqlite", "application/vnd.sqlite3"}, {"sqlite3", "application/vnd.sqlite3"}, {"db", "application/vnd.sqlite3"},

    // ───── Visio ─────
    {"vsd", "application/vnd.visio"}, {"vsdx", "application/vnd.visio"},
    {"vssx", "application/vnd.visio"}, {"vstx", "application/vnd.visio"},

    // ───── WordPerfect ─────
    {"wpd", "application/vnd.wordperfect"},

    // ───── Unity Web Player (Legacy) ─────
    {"unityweb", "application/vnd.unity"},

    // ───── Voice / IVR / Telephony ─────
    {"vxml", "application/voicexml+xml"},

    // ───── BitTorrent ─────
    {"torrent", "application/x-bittorrent"},

    // ───── Fonts ─────
    {"ttf", "font/ttf"}, {"otf", "font/otf"},
    {"woff", "font/woff"}, {"woff2", "font/woff2"},

    // ───── Java / Browser Extensions ─────
    {"jnlp", "application/x-java-jnlp-file"}, {"xpi", "application/x-xpinstall"},
    {"swf", "application/x-shockwave-flash"}, {"xap", "application/x-silverlight-app"},

    // ───── TeX / XML / Markup / Config / Code ─────
    {"dvi", "application/x-dvi"}, {"tex", "application/x-tex"},
    {"latex", "application/x-latex"}, {"xml", "application/xml"},
    {"xhtml", "application/xhtml+xml"}, {"xht", "application/xhtml+xml"},
    {"xsl", "application/xml"}, {"xslt", "application/xslt+xml"},
    {"yaml", "application/x-yaml"}, {"yml",  "application/x-yaml"},
    {"json", "application/json"}, {"toml", "application/toml"},
    {"ts", "application/typescript"}, {"tsx", "application/typescript"},

    // ───── Audio ─────
    {"mp3", "audio/mpeg"}, {"aac", "audio/aac"}, {"m4a", "audio/mp4"},
    {"oga", "audio/ogg"}, {"ogg", "audio/ogg"}, {"opus", "audio/opus"},
    {"flac", "audio/flac"}, {"wav", "audio/wav"}, {"mid", "audio/midi"},
    {"kar", "audio/midi"},

    // ───── Images ─────
    {"avif", "image/avif"}, {"avifs", "image/avif-sequence"}, {"bmp", "image/bmp"},
    {"gif", "image/gif"}, {"heif", "image/heic"}, {"heic", "image/heic"}, {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"}, {"png", "image/png"}, {"svg", "image/svg+xml"}, {"svgz", "image/svg+xml"},
    {"tif", "image/tiff"}, {"tiff", "image/tiff"}, {"psd", "image/vnd.adobe.photoshop"},
    {"webp", "image/webp"}, {"ico", "image/x-icon"}, {"cr2", "image/x-canon-cr2"},
    {"nef", "image/x-nikon-nef"}, {"arw", "image/x-sony-arw"}, {"dng", "image/x-adobe-dng"},

    // ───── Email / Calendar ─────
    {"eml", "message/rfc822"}, {"mht", "message/rfc822"},
    {"mhtml", "message/rfc822"}, {"ics", "text/calendar"},

    // ───── Plain / Code / Text ─────
    {"css", "text/css"}, {"csv", "text/csv"}, {"html", "text/html"},
    {"htm", "text/html"}, {"js", "text/javascript"}, {"mjs", "text/javascript"},
    {"md", "text/markdown"}, {"markdown", "text/markdown"}, {"txt", "text/plain"},
    {"log", "text/plain"}, {"tsv", "text/tab-separated-values"},
    {"c", "text/x-c"}, {"cpp", "text/x-c"}, {"h", "text/x-c"},
    {"hpp", "text/x-c"}, {"java", "text/x-java-source"},
    {"py", "text/x-python"}, {"jsx", "text/jsx"}, {"vue", "text/vue"},
    {"rs", "text/rust"}, {"go", "text/x-go"},

    // ───── 3D / CAD / Vector ─────
    {"igs", "model/iges"}, {"iges", "model/iges"}, {"stl", "model/stl"}, {"mesh", "model/mesh"},
    {"msh", "model/mesh"}, {"vrml", "model/vrml"}, {"wrl", "model/vrml"}, {"dwg", "image/vnd.dwg"},
    {"dxf", "image/vnd.dxf"},

    // ───── Video ─────
    {"3gp", "video/3gpp"}, {"3g2", "video/3gpp2"}, {"h261", "video/h261"}, {"h263", "video/h263"},
    {"h264", "video/h264"}, {"jpgv", "video/jpeg"}, {"jpm", "video/jpm"}, {"jpgm", "video/jpm"},
    {"mj2", "video/mj2"}, {"mjp2", "video/mj2"}, {"ts", "video/mp2t"}, {"mp4", "video/mp4"},
    {"mp4v", "video/mp4"}, {"mpg4", "video/mp4"}, {"m1v", "video/mpeg"}, {"m2v", "video/mpeg"},
    {"mpa", "video/mpeg"}, {"mpe", "video/mpeg"}, {"mpeg", "video/mpeg"}, {"mpg", "video/mpeg"},
    {"ogv", "video/ogg"}, {"mov", "video/quicktime"}, {"qt", "video/quicktime"}, {"fvt", "video/vnd.fvt"},
    {"m4u", "video/vnd.mpegurl"}, {"mxu", "video/vnd.mpegurl"}, {"pyv", "video/vnd.ms-playready.media.pyv"},
    {"viv", "video/vnd.vivo"}, {"webm", "video/webm"}, {"f4v", "video/x-f4v"}, {"fli", "video/x-fli"},
    {"flv", "video/x-flv"}, {"m4v", "video/x-m4v"}, {"mkv", "video/x-matroska"}, {"asf", "video/x-ms-asf"},
    {"asx", "video/x-ms-asf"}, {"wm", "video/x-ms-wm"}, {"wmv", "video/x-ms-wmv"}, {"wmx", "video/x-ms-wmx"},
    {"wvx", "video/x-ms-wvx"}, {"avi", "video/x-msvideo"}, {"movie", "video/x-sgi-movie"},
};

// vvv Minimal version of above ^^^
inline const std::unordered_map<std::string_view, std::string_view> ExtFromMime = {
    // ───── Archive / Package ─────
    {"application/epub+zip", "epub"}, {"application/gzip", "gz"}, {"application/java-archive", "jar"},
    {"application/vnd.apple.installer+xml", "mpkg"}, {"application/vnd.android.package-archive", "apk"},
    {"application/vnd.ms-cab-compressed", "cab"}, {"application/vnd.openofficeorg.extension", "oxt"},
    {"application/vnd.ms-xpsdocument", "xps"}, {"application/x-bittorrent", "torrent"},

    // ───── Documents ─────
    {"application/pdf", "pdf"}, {"application/oebps-package+xml", "opf"}, {"application/xhtml+xml", "xhtml"},
    {"application/xml", "xml"}, {"application/rss+xml", "rss"}, {"application/rdf+xml", "rdf"},
    {"application/rtf", "rtf"}, {"application/smil+xml", "smil"},

    // ───── PostScript / Forms ─────
    {"application/postscript", "ps"}, {"application/vnd.adobe.xdp+xml", "xdp"}, {"application/vnd.adobe.xfdf", "xfdf"},

    // ───── Office: Word ─────
    {"application/msword", "doc"}, {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx"},

    // ───── Office: Excel ─────
    {"application/vnd.ms-excel", "xls"}, {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx"},

    // ───── Office: PowerPoint ─────
    {"application/vnd.ms-powerpoint", "ppt"}, {"application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx"},

    // ───── OpenDocument ─────
    {"application/vnd.oasis.opendocument.text", "odt"}, {"application/vnd.oasis.opendocument.spreadsheet", "ods"},

    // ───── Fonts ─────
    {"application/vnd.ms-fontobject", "eot"}, {"font/ttf", "ttf"}, {"font/woff", "woff"}, {"font/woff2", "woff2"},

    // ───── SQLite / Data ─────
    {"application/vnd.sqlite3", "sqlite"}, {"application/vnd.visio", "vsd"}, {"application/vnd.wordperfect", "wpd"},

    // ───── Audio ─────
    {"audio/mpeg", "mp3"}, {"audio/aac", "aac"}, {"audio/mp4", "m4a"},
    {"audio/ogg", "ogg"}, {"audio/opus", "opus"}, {"audio/flac", "flac"},
    {"audio/wav", "wav"}, {"audio/midi", "mid"},

    // ───── Images ─────
    {"image/avif", "avif"}, {"image/bmp", "bmp"}, {"image/gif", "gif"},
    {"image/heic", "heic"}, {"image/jpeg", "jpg"}, {"image/png", "png"},
    {"image/svg+xml", "svg"}, {"image/tiff", "tif"}, {"image/webp", "webp"}, {"image/x-icon", "ico"},

    // ───── Email / Calendar ─────
    {"message/rfc822", "eml"}, {"text/calendar", "ics"},

    // ───── Text / Source Code ─────
    {"text/html", "html"}, {"text/css", "css"}, {"text/csv", "csv"}, {"text/plain", "txt"},
    {"text/javascript", "js"}, {"text/markdown", "md"}, {"application/json", "json"},

    // ───── 3D / CAD ─────
    {"model/stl", "stl"}, {"model/iges", "igs"}, {"model/mesh", "mesh"}, {"model/vrml", "wrl"},

    // ───── Video ─────
    {"video/mp4", "mp4"}, {"video/mpeg", "mpeg"}, {"video/webm", "webm"},
    {"video/x-matroska", "mkv"}, {"video/quicktime", "mov"}, {"video/x-msvideo", "avi"}, {"video/x-flv", "flv"}
};

inline const std::unordered_map<std::string_view, std::string_view> PortFromProtocol = {
    {"http", "80"}, {"https", "443"}, {"ws", "80"}, {"wss", "443"}, {"ftp", "21"},
    {"ftps", "990"}, {"sftp", "22"}, {"ssh", "22"}, {"telnet", "23"}, {"smtp", "25"},
    {"smtps", "465"}, {"imap", "143"}, {"imaps", "993"}, {"pop3", "110"}, {"pop3s", "995"},
    {"ldap", "389"}, {"ldaps", "636"}, {"nntp", "119"}, {"nntps", "563"}, {"rtsp", "554"},
    {"sip", "5060"}, {"sips", "5061"}, {"xmpp", "5222"}, {"xmpps", "5223"}, {"mqtt", "1883"},
    {"mqtts", "8883"}, {"redis", "6379"}, {"rediss", "6380"}, {"mysql", "3306"},
    {"postgres", "5432"}, {"mongodb", "27017"}, {"amqp", "5672"}, {"amqps", "5671"},
    {"grpc", "50051"}, {"grpc+tls", "50052"}, {"kafka", "9092"}, {"kafkas", "9093"}
};

// vvv Helper function vvv
static inline std::string_view ExtractExtension(std::string_view path)
{
    size_t pos = path.rfind('.');
    if(pos == std::string_view::npos || pos + 1 >= path.size()) return {};
    
    return path.substr(pos + 1);
}

// vvv MimeDetector vvvv
std::string_view MimeDetector::DetectMimeFromExt(std::string_view path)
{
    std::string_view ext = ExtractExtension(path);
    if(ext.empty()) return "application/octet-stream";

    auto it = MimeFromExt.find(ext);
    return (it != MimeFromExt.end()) ? it->second : "application/octet-stream";
}

std::string_view MimeDetector::DetectExtFromMime(std::string_view mime)
{
    auto it = ExtFromMime.find(mime);
    return (it != ExtFromMime.end()) ? it->second : std::string_view{};
}

// vvv PortDetector vvv
std::string_view PortDetector::DetectFromProtocol(std::string_view protocol)
{
    auto it = PortFromProtocol.find(protocol);
    return (it != PortFromProtocol.end()) ? it->second : std::string_view{};
}

} // namespace WFX::Http