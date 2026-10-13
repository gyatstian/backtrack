#pragma once

#include <chrono>
#include <thread>

namespace backtrack {

// Bounded retry preserving per-site budgets: runs op(attempt) until it returns
// true or attempts run out, sleeping backoffs[attempt - 1] between tries.
// Attempt counts and delays stay at the call site; only the loop lives here.
// shortcut: skeleton only; publishWithRetry/muxToMp4WithRetry (mux) and the
// WASAPI reconnect backoff still run their own loops.
template <typename Rep, typename Period, typename Op>
bool retryWithBackoff(int attempts, const std::chrono::duration<Rep, Period>* backoffs, Op&& op) {
    for (int attempt = 1;; ++attempt) {
        if (op(attempt)) {
            return true;
        }
        if (attempt >= attempts) {
            return false;
        }
        std::this_thread::sleep_for(backoffs[attempt - 1]);
    }
}

} // namespace backtrack
