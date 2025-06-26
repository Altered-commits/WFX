/*
 * Build: g++ -O3 -s -I. test/parser_test.cpp\
          http/formatters/parser/http_parser.cpp\
          htpp/headers/http_headers.cpp
 */

#include <cassert>
#include <cstring>
#include <iostream>

#include "http/formatters/parser/http_parser.hpp"
#include "http/connection/http_connection.hpp"

using namespace WFX::Http;

void RunHttpParserTests()
{
    auto RunTest = [](const char* rawRequest, size_t length, bool expectSuccess, const char* testName) {
        ConnectionContext ctx;
        ctx.buffer     = const_cast<char*>(rawRequest); // safe since we don't modify
        ctx.dataLength = length;

        HttpRequest req;
        bool result = HttpParser::Parse(ctx, req);

        if (result != expectSuccess) {
            std::cerr << "[FAIL] " << testName << " — expected "
                      << (expectSuccess ? "success" : "fail")
                      << " but got " << (result ? "success" : "fail") << "\n";
        } else {
            std::cout << "[PASS] " << testName << "\n";
        }
    };

    // === Test 1: Valid GET ===
    const char* test1 =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    RunTest(test1, std::strlen(test1), true, "Valid GET");

    // === Test 2: Valid POST with body ===
    const char* test2 =
        "POST /submit HTTP/1.1\r\n"
        "Host: test\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "hello world";
    RunTest(test2, std::strlen(test2), true, "Valid POST with body");

    // === Test 3: Missing CRLF ===
    const char* test3 =
        "GET / HTTP/1.1\r\n"
        "Host: test\r\n"
        "Content-Length: 5\r\n";
    RunTest(test3, std::strlen(test3), false, "Missing CRLF after headers");

    // === Test 4: Content-Length exceeds body max ===
    const char* test4 =
        "POST /data HTTP/1.1\r\n"
        "Host: test\r\n"
        "Content-Length: 9000\r\n"
        "\r\n"
        "0123456789"; // Short body
    RunTest(test4, std::strlen(test4), false, "Exceeds MAX_BODY_TOTAL_SIZE");

    // === Test 5: Too many headers ===
    std::string test5 = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 65; ++i)
        test5 += "X-Header-" + std::to_string(i) + ": a\r\n";
    test5 += "\r\n";
    RunTest(test5.c_str(), test5.size(), false, "Too many headers (>64)");

    // === Test 6: Header total size exceeds limit ===
    std::string test6 = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 10; ++i)
        test6 += "X-Pad-" + std::to_string(i) + ": " + std::string(1000, 'x') + "\r\n"; // ~10KB
    test6 += "\r\n";
    RunTest(test6.c_str(), test6.size(), false, "Header block exceeds 8KB");

    // === Test 7: Invalid request line ===
    const char* test7 =
        "GETTTTTTTTTTT / HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n";
    RunTest(test7, std::strlen(test7), false, "Invalid method");

    // === Test 8: Missing Content-Length but body present ===
    const char* test8 =
        "POST / HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n"
        "hello";
    RunTest(test8, std::strlen(test8), true, "No Content-Length (no body expected)");

    // === Test 9: Empty request ===
    const char* test9 = "";
    RunTest(test9, 0, false, "Empty request");

    // === Test 10: No colon in header ===
    const char* test10 =
        "GET / HTTP/1.1\r\n"
        "InvalidHeaderLine\r\n"
        "\r\n";
    RunTest(test10, std::strlen(test10), false, "Header line without colon");
}

int main(void)
{
    RunHttpParserTests();
    return 0;
}