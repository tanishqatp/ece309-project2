#pragma once
#include <cstddef>
#include <string>
#include <string_view>

struct SentinelScannerTestAccess;

class SentinelScanner {
public:
    explicit SentinelScanner(std::string sentinel);

    struct Out { std::string safe_text; bool sentinel_found; };

    Out feed(std::string_view chunk);
    Out flush();

private:
    friend struct SentinelScannerTestAccess;

    std::size_t held_suffix_length(std::string_view text) const;

    std::string sentinel_;
    std::string pending_;
};
