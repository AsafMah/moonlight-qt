#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>

// Provisional, name-based Desktop opt-in until the host advertises an app kind.
// Fail closed for renamed apps, collectors, invalid identities and unpaired launches.
inline bool restartRecoveryAppEligible(std::string_view name, bool collector, int id,
                                       bool authenticatedLaunch)
{
    return name == "Desktop" && !collector && id > 0 && authenticatedLaunch;
}

class RecoveryBudget
{
public:
    static constexpr int OutageMs = 60000;

    static int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool begin(int64_t now, int durationMs = OutageMs)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Deadline == 0) {
            m_Deadline = now + durationMs;
        }
        return now < m_Deadline;
    }

    int remainingMs(int64_t now) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Deadline == 0 ? (std::numeric_limits<int>::max)()
                               : static_cast<int>(std::max<int64_t>(0, m_Deadline - now));
    }

    bool active() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Deadline != 0;
    }

    void disarmFrames()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_AcceptRecoveredFrames = false;
    }

    // Call only after the old connection and its decoder callbacks are joined.
    void awaitRecoveredFrame()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_AcceptRecoveredFrames = true;
    }

    bool receivedHostFrame(int64_t now)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_AcceptRecoveredFrames || m_Deadline == 0 || now >= m_Deadline) {
            return false;
        }
        m_AcceptRecoveredFrames = false;
        m_Deadline = 0;
        return true;
    }

    static int nextBackoffMs(int currentMs) { return (std::min)(currentMs * 2, 8000); }

private:
    mutable std::mutex m_Mutex;
    int64_t m_Deadline = 0;
    bool m_AcceptRecoveredFrames = false;
};
