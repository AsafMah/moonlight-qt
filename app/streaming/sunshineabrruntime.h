#pragma once

#include <QString>
#include <QJsonObject>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

class SunshineAbrRuntime
{
public:
    enum class State
    {
        Disabled,
        Enabling,
        Active,
        Unsupported,
        HostRejected,
        AuthenticationFailed,
        Degraded,
        Stopped
    };
    struct Snapshot
    {
        State state = State::Disabled;
        QString detail;
        int bitrateKbps = 0;
        QString sessionToken;
        bool sessionBinding = false;
    };
    struct Job
    {
        uint64_t generation;
        std::shared_ptr<std::atomic_bool> cancelled;
        int bitrateKbps;
        QString sessionToken;
        bool sessionBinding = false;
    };

    Job begin(bool requested, int bitrateKbps)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Cancelled->store(true);
        m_Cancelled = std::make_shared<std::atomic_bool>(!requested);
        ++m_Generation;
        m_Snapshot = { requested ? State::Enabling : State::Disabled, {}, bitrateKbps };
        m_InFlight = false;
        m_DisableQueued = false;
        return { m_Generation, m_Cancelled, bitrateKbps };
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Cancelled->store(true);
        if (m_Snapshot.state != State::Disabled) {
            m_Snapshot.state = State::Stopped;
        }
        m_Snapshot.detail.clear();
        m_InFlight = false;
    }

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Snapshot;
    }

    std::optional<Job> beginFeedback()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Cancelled->load() || m_InFlight || m_Snapshot.state != State::Active) {
            return std::nullopt;
        }
        m_InFlight = true;
        return Job{ m_Generation, m_Cancelled, m_Snapshot.bitrateKbps, m_Snapshot.sessionToken,
                    m_Snapshot.sessionBinding };
    }

    std::optional<Job> takeDisableJob()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_DisableQueued || !m_Snapshot.sessionBinding || m_Snapshot.sessionToken.isEmpty()) {
            return std::nullopt;
        }
        m_DisableQueued = true;
        return Job{ m_Generation, m_Cancelled, m_Snapshot.bitrateKbps, m_Snapshot.sessionToken,
                    true };
    }

    void finish(const Job& job, State state, QString detail = {}, int bitrateKbps = 0,
                QString sessionToken = {}, bool sessionBinding = false)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (job.generation != m_Generation || job.cancelled->load()) {
            return;
        }
        m_Snapshot.state = state;
        m_Snapshot.detail = std::move(detail);
        if (bitrateKbps > 0) {
            m_Snapshot.bitrateKbps = bitrateKbps;
        }
        if (sessionBinding) {
            m_Snapshot.sessionToken = std::move(sessionToken);
            m_Snapshot.sessionBinding = true;
        }
        m_InFlight = false;
    }

    uint64_t generation() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Generation;
    }

    bool setBitrate(uint64_t generation, int bitrateKbps)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (generation == m_Generation && m_Snapshot.state != State::Stopped) {
            m_Snapshot.bitrateKbps = bitrateKbps;
            return true;
        }
        return false;
    }

    static State httpFailureState(int status, const QString& code = {})
    {
        if (status == 401 || status == 403)
            return State::AuthenticationFailed;
        if (status == 404 || status == 405 || status == 501)
            return State::Unsupported;
        if (status == 400 || status == 422 ||
            (status == 409 && (code == QStringLiteral("no_active_stream") ||
                               code == QStringLiteral("session_starting") ||
                               code == QStringLiteral("ambiguous_session") ||
                               code == QStringLiteral("stale_session") ||
                               code == QStringLiteral("abr_not_enabled"))))
            return State::HostRejected;
        return State::Degraded;
    }

    static bool retryableConfigure(int status, const QJsonObject& error, bool sessionBinding)
    {
        const QString code = error.value("error_code").toString();
        return sessionBinding && status == 409 && error.value("success") == QJsonValue(false) &&
               error.value("retryable") == QJsonValue(true) &&
               (code == QStringLiteral("no_active_stream") ||
                code == QStringLiteral("session_starting"));
    }

private:
    mutable std::mutex m_Mutex;
    Snapshot m_Snapshot;
    uint64_t m_Generation = 0;
    std::shared_ptr<std::atomic_bool> m_Cancelled = std::make_shared<std::atomic_bool>(true);
    bool m_InFlight = false;
    bool m_DisableQueued = false;
};
