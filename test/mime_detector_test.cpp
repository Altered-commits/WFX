/*
 * Build: g++ -O3 -s -I. test/mime_detector_test.cpp http/common/http_detector.cpp
 */

#include <iostream>
#include <string_view>
#include <cassert>

#include "http/common/http_detector.hpp"

using namespace WFX::Http;

void RunMimeDetectorTests() {
    struct TestCase {
        std::string_view ext;
        std::string_view expectedMime;
    };

    const TestCase tests[] = {
        // ─── Text / Code / Markup ───
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "text/javascript"},
        {".mjs", "text/javascript"},
        {".ts", "application/typescript"},
        {".tsx", "application/typescript"},
        {".json", "application/json"},
        {".xml", "application/xml"},
        {".txt", "text/plain"},
        {".csv", "text/csv"},
        {".tsv", "text/tab-separated-values"},
        {".md", "text/markdown"},
        {".markdown", "text/markdown"},
        {".yml", "text/yaml"},
        {".yaml", "text/yaml"},
        {".toml", "application/toml"},
        {".sh", "application/x-sh"},

        // ─── Images ───
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".bmp", "image/bmp"},
        {".ico", "image/x-icon"},
        {".svg", "image/svg+xml"},
        {".webp", "image/webp"},
        {".avif", "image/avif"},
        {".tiff", "image/tiff"},
        {".tif", "image/tiff"},

        // ─── Audio / Video ───
        {".mp3", "audio/mpeg"},
        {".flac", "audio/flac"},
        {".m4a", "audio/mp4"},
        {".wav", "audio/wav"},
        {".ogg", "audio/ogg"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".mov", "video/quicktime"},
        {".mkv", "video/x-matroska"},

        // ─── Fonts ───
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".otf", "font/otf"},

        // ─── Documents / Archives ───
        {".pdf", "application/pdf"},
        {".epub", "application/epub+zip"},
        {".zip", "application/zip"},
        {".tar", "application/x-tar"},
        {".gz", "application/gzip"},
        {".bz2", "application/x-bzip2"},
        {".7z", "application/x-7z-compressed"},
        {".rar", "application/vnd.rar"},

        // ─── Web / App Specific ───
        {".webmanifest", "application/manifest+json"},
        {".map", "application/json"},
        {".appcache", "text/cache-manifest"},

        // ─── Fallbacks ───
        {".unknown", "application/octet-stream"},
        {".", "application/octet-stream"},
        {"", "application/octet-stream"}
    };

    int passed = 0;
    int failed = 0;

    for (const auto& test : tests) {
        auto result = MimeDetector::DetectMimeFromExt(test.ext);
        if (result == test.expectedMime) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "[FAIL] ext: \"" << test.ext << "\" -> Got: \"" << result
                      << "\" | Expected: \"" << test.expectedMime << "\"\n";
        }
    }

    std::cout << "[MimeDetector Test] Passed: " << passed << " / " << (passed + failed) << "\n";
    if (failed > 0) {
        std::cerr << "[MimeDetector Test] FAILED!\n";
        std::exit(1);
    }
}

int main() {
    RunMimeDetectorTests();
    return 0;
}