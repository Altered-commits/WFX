/*
 * Build: g++ -O3 -s -I. test/router_speed.cpp utils/logger/logger.cpp http/routing/*.cpp utils/uuid/uuid.cpp
 */

/*
 * OUTDATED, WON'T COMPILE, BUT U GET THE IDEA :)
 */

#include <iostream>
#include <chrono>
#include <string_view>
#include <variant>
#include <random>
#include <sstream>

#include "http/routing/router.hpp"

using namespace WFX::Http;
using namespace WFX::Utils;

void RegisterBenchmarkRoutes() {
    Router& router = Router::GetInstance();

    // GET routes
    router.RegisterRoute(HttpMethod::GET, "/", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::GET, "/about", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::GET, "/docs/api", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::GET, "/send-file/<genre:string>", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::GET, "/send-file/<genre:string>/<index:uint>", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::GET, "/send-file/<genre:string>/<index:uint>/<id:uuid>", [](HttpRequest&, Response&) {});

    // POST routes
    router.RegisterRoute(HttpMethod::POST, "/upload", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::POST, "/submit/<formId:uint>", [](HttpRequest&, Response&) {});
    router.RegisterRoute(HttpMethod::POST, "/submit/<formId:uint>/<token:uuid>", [](HttpRequest&, Response&) {});
}

std::string GenerateRandomUUID(std::mt19937& rng) {
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFF);
    std::ostringstream oss;
    oss << std::hex;
    oss << dist(rng) << dist(rng) << "-" << dist(rng);
    oss << "-" << dist(rng) << "-" << dist(rng);
    oss << "-" << dist(rng) << dist(rng) << dist(rng);
    return oss.str();
}

std::string GenerateRandomString(std::mt19937& rng, int len) {
    static const std::string charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<> dist(0, charset.size() - 1);
    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; ++i)
        out += charset[dist(rng)];
    return out;
}

std::string GenerateRandomPath(std::mt19937& rng, int id) {
    switch (id % 6) {
        case 0: return "/";
        case 1: return "/about";
        case 2: return "/docs/api";
        case 3: {
            return "/send-file/" + GenerateRandomString(rng, 5);
        }
        case 4: {
            return "/send-file/" + GenerateRandomString(rng, 6) + "/" + std::to_string(rng());
        }
        case 5: {
            return "/send-file/" + GenerateRandomString(rng, 7) + "/" + std::to_string(rng()) + "/" + GenerateRandomUUID(rng);
        }
        default: return "/";
    }
}

void BenchmarkRouter(std::size_t iterations) {
    using namespace std::chrono;
    Router& router = Router::GetInstance();

    std::mt19937 rng(std::random_device{}());

    size_t matches      = 0;
    size_t stringCount  = 0;
    size_t uintCount    = 0;
    size_t uuidCount    = 0;
    size_t methodSwitch = 0;

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        PathSegments segments;  // Do not reuse — forces new alloc
        HttpMethod method = (++methodSwitch % 2 == 0) ? HttpMethod::GET : HttpMethod::POST;
        std::string path = GenerateRandomPath(rng, i);

        const HttpCallbackType* fn = router.MatchRoute(method, path, segments);
        if (fn != nullptr) {
            ++matches;

            for (const auto& seg : segments) {
                std::visit([&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, std::string_view>) ++stringCount;
                    else if constexpr (std::is_same_v<T, uint64_t>)     ++uintCount;
                    else if constexpr (std::is_same_v<T, UUID>)         ++uuidCount;
                }, seg);
            }
        }
    }

    auto end = high_resolution_clock::now();
    auto duration_us = duration_cast<microseconds>(end - start).count();

    size_t misses     = iterations - matches;
    double perMatchUs = matches > 0 ? static_cast<double>(duration_us) / matches : 0.0;
    double perMatchNs = perMatchUs * 1000.0;
    double throughput = (iterations * 1'000'000.0) / duration_us;

    // Final Printout
    std::cout << "\n[WFX Router TORTURE Benchmark]\n";
    std::cout << "Iterations         : " << iterations << "\n";
    std::cout << "Matched Routes     : " << matches << "\n";
    std::cout << "Missed Routes      : " << misses << "\n";
    std::cout << "Total Time         : " << duration_us << " us\n";
    std::cout << "Avg Time per Match : " << perMatchUs << " us (" << perMatchNs << " ns)\n";
    std::cout << "Throughput         : " << static_cast<size_t>(throughput) << " matches/sec\n";

    std::cout << "\n[Path Segment Type Stats]\n";
    std::cout << "Total Segments     : " << (stringCount + uintCount + uuidCount) << "\n";
    std::cout << "    - string       : " << stringCount << "\n";
    std::cout << "    - uint         : " << uintCount << "\n";
    std::cout << "    - uuid         : " << uuidCount << "\n\n";
}

int main() {
    RegisterBenchmarkRoutes();
    BenchmarkRouter(10'000'000);
    
    return 0;
}