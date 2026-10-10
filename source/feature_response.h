#pragma once
#include <algorithm>
#include <cstdint>

namespace ManagerBackend {
// A receiver may expose the previous command's reply until the new one is ready.
// Read only: never resend a write command while waiting for a matching response.
template<class Read, class Wait, class Matches, class Clock>
bool AwaitMatchingFeatureResponse(Read read, Wait wait, Matches matches, Clock clock,
                                 std::uint64_t deadline) {
    for (;;) {
        const auto beforeRead = clock();
        if (beforeRead >= deadline) return false;
        if (!read(static_cast<std::uint32_t>(deadline - beforeRead))) return false;
        if (matches()) return true;
        const auto now = clock();
        if (now >= deadline || !wait(static_cast<std::uint32_t>(std::min<std::uint64_t>(20, deadline - now))))
            return false;
    }
}
}
