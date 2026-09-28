#include "core/sentinel_scanner.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

SentinelScanner::SentinelScanner(std::string sentinel)
    : sentinel_(std::move(sentinel)) {
    if (sentinel_.empty()) {
        throw std::invalid_argument("SentinelScanner: sentinel must be non-empty");
    }
}

std::size_t SentinelScanner::held_suffix_length(std::string_view text) const {
    std::string_view s(sentinel_);
    std::size_t max_k = std::min(text.size(), s.size() - 1);
    for (std::size_t k = max_k; k > 0; --k) {
        if (text.substr(text.size() - k) == s.substr(0, k)) return k;
    }
    return 0;
}

SentinelScanner::Out SentinelScanner::feed(std::string_view chunk) {
    std::string buf;
    buf.reserve(pending_.size() + chunk.size());
    buf += pending_;
    buf += chunk;
    pending_.clear();

    std::size_t pos = buf.find(sentinel_);
    if (pos != std::string::npos) {
        buf.resize(pos);
        return {std::move(buf), true};
    }

    std::size_t keep = held_suffix_length(buf);
    pending_.assign(buf, buf.size() - keep, keep);
    buf.resize(buf.size() - keep);
    return {std::move(buf), false};
}

SentinelScanner::Out SentinelScanner::flush() {
    std::string rest = std::move(pending_);
    pending_.clear();
    return {std::move(rest), false};
}
