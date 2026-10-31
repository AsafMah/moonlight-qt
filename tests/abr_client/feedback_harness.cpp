#include <QCoreApplication>
#include <QDebug>
#include <QJsonObject>
#include <QRunnable>
#include <QSslCertificate>
#include <QNetworkReply>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include "streaming/sunshineabrruntime.h"
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>

struct NvAddress
{
};
// EXCEPTIONS

class NvHTTP
{
public:
    inline static int calls = 0;
    inline static int status = 0;
    inline static bool supported = true;
    inline static bool networkFailure = false;
    inline static int feedbackHttpStatus = 0;
    inline static bool feedbackHttpHasProtocolVerdict = true;
    inline static QJsonObject feedbackResponse = {
        { "success", true },          { "sessionToken", "fixture-token" },
        { "bitrateRequested", true }, { "bitrateApplied", true },
        { "newBitrate", 2000 },       { "reason", "fixture" }
    };
    inline static bool sessionBinding = true;
    inline static bool bindingFeature = true;
    inline static int transientFailures = 0;
    inline static int transientStatus = 409;
    inline static QJsonObject transientBody = { { "success", false },
                                                { "error_code", "session_starting" },
                                                { "error", "Stream is starting" },
                                                { "retryable", true } };
    inline static int configureCalls = 0;
    inline static int capabilitiesDelayMs = 0;
    inline static int configureDelayMs = 0;
    inline static int lastConfigureTimeout = 0;
    inline static QString lastFeedbackToken;
    inline static QString lastDisableToken;
    inline static int disableCalls = 0;
    inline static QJsonObject response = { { "success", true },
                                           { "enabled", true },
                                           { "sessionToken", "fixture-token" },
                                           { "bitrateRequested", true },
                                           { "bitrateApplied", true },
                                           { "initialBitrate", 1000 },
                                           { "newBitrate", 2000 },
                                           { "reason", "fixture" } };
    inline static std::function<void()> duringRequest;
    NvHTTP(NvAddress, uint16_t, QSslCertificate, bool, void*, QString) {}
    void setRequestCancellation(std::shared_ptr<std::atomic_bool> cancelled)
    {
        m_Cancelled = std::move(cancelled);
    }
    void check()
    {
        if (m_Cancelled->load()) {
            throw QtNetworkReplyException(QNetworkReply::OperationCanceledError, "cancelled");
        }
        ++calls;
        if (duringRequest)
            duringRequest();
        if (status) {
            throw GfeHttpResponseException(
                status, "fixture rejection", {},
                { { "error", "No active streaming session for this client" },
                  { "error_code", "no_active_stream" } });
        }
        if (networkFailure) {
            throw QtNetworkReplyException(QNetworkReply::TimeoutError, "fixture timeout");
        }
    }
    bool getAbrCapabilities(int* = nullptr, QJsonObject* capabilities = nullptr,
                            int timeoutMs = 2000)
    {
        check();
        if (capabilitiesDelayMs > 0) {
            QEventLoop loop;
            QTimer::singleShot(qMin(capabilitiesDelayMs, timeoutMs), Qt::PreciseTimer, &loop,
                               &QEventLoop::quit);
            loop.exec();
            if (capabilitiesDelayMs >= timeoutMs) {
                throw QtNetworkReplyException(QNetworkReply::TimeoutError,
                                              "Capabilities request timed out");
            }
        }
        if (capabilities) {
            *capabilities = { { "version", sessionBinding ? 2 : 1 },
                              { "features", sessionBinding && bindingFeature
                                                ? QJsonArray{ "session_binding" }
                                                : QJsonArray{} } };
        }
        return supported;
    }
    QJsonObject configureAbr(bool enabled, int, int, QString, int timeoutMs, QString token = {})
    {
        check();
        if (!enabled) {
            ++disableCalls;
            lastDisableToken = token;
            return { { "success", true }, { "enabled", false }, { "sessionToken", token } };
        }
        ++configureCalls;
        lastConfigureTimeout = timeoutMs;
        if (configureDelayMs > 0) {
            QEventLoop loop;
            QTimer::singleShot(qMin(configureDelayMs, timeoutMs), Qt::PreciseTimer, &loop,
                               &QEventLoop::quit);
            loop.exec();
        }
        if (transientFailures-- > 0) {
            throw GfeHttpResponseException(transientStatus, "starting", {}, transientBody);
        }
        return response;
    }
    QJsonObject sendAbrFeedback(double, double, double, int, int, int, QString token = {})
    {
        lastFeedbackToken = token;
        check();
        if (feedbackHttpStatus) {
            throw GfeHttpResponseException(feedbackHttpStatus, "fixture server error", {},
                                           feedbackHttpHasProtocolVerdict
                                               ? QJsonObject{ { "success", false },
                                                              { "error_code", "internal_error" },
                                                              { "error", "Temporary host error" },
                                                              { "retryable", false } }
                                               : QJsonObject{});
        }
        return feedbackResponse;
    }

private:
    std::shared_ptr<std::atomic_bool> m_Cancelled = std::make_shared<std::atomic_bool>(false);
};

// FEEDBACK_TASK

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using State = SunshineAbrRuntime::State;
    auto runtime = std::make_shared<SunshineAbrRuntime>();
    auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    auto configure = [&](bool requested = true) {
        auto job = runtime->begin(requested, 1000);
        AbrConfigureTask task({}, 47984, QSslCertificate(), {}, runtime, job);
        task.run();
        return job;
    };
    auto feedback = [&] {
        auto job = runtime->beginFeedback();
        require(job.has_value(), "Active generation must permit feedback");
        AbrFeedbackTask task({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime, *job);
        task.run();
    };
    try {
        if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "transient-red") {
            configure();
            NvHTTP::networkFailure = true;
            feedback();
            NvHTTP::networkFailure = false;
            require(runtime->snapshot().state == State::Degraded,
                    "Transport failure was not represented");
            auto recoveryJob = runtime->beginFeedback();
            require(recoveryJob.has_value(),
                    "Transient feedback failure permanently disabled retries");
            require(runtime->snapshot().detail == "request_timeout",
                    "Transport failure was not represented with a stable diagnostic code");
            NvHTTP::networkFailure = false;
            AbrFeedbackTask recovered({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                      *recoveryJob);
            recovered.run();
            require(runtime->snapshot().state == State::Active,
                    "Later feedback did not recover after a transient transport failure");
            qInfo("feedback-regressions passed");
            return 0;
        }
        if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "ambiguous-red") {
            NvHTTP::transientFailures = 1;
            NvHTTP::transientBody["error_code"] = "ambiguous_session";
            const int before = NvHTTP::configureCalls;
            configure();
            require(NvHTTP::configureCalls == before + 2 &&
                        runtime->snapshot().state == State::Active,
                    "Retryable ambiguous unbound initial configure was not retried");
            qInfo("feedback-regressions passed");
            return 0;
        }
        const auto oldJob = configure();
        const auto oldGeneration = runtime->generation();
        require(runtime->snapshot().state == State::Active, "Configure never became active");
        auto oldFeedback = runtime->beginFeedback();
        require(oldFeedback.has_value(), "Active feedback unavailable");
        require(!runtime->beginFeedback(), "Concurrent feedback allowed");
        runtime->stop();
        const auto replacement = runtime->begin(true, 1000);
        runtime->setBitrate(oldGeneration, 9000);
        require(runtime->snapshot().bitrateKbps == 1000,
                "Old manual bitrate completion contaminated new ABR generation");
        const int calls = NvHTTP::calls;
        AbrFeedbackTask oldTask({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                *oldFeedback);
        oldTask.run();
        require(NvHTTP::calls == calls, "Cancelled queued task sent a request");
        require(runtime->snapshot().bitrateKbps == 1000,
                "Old connection feedback mutated replacement bitrate");
        require(runtime->snapshot().state == State::Enabling, "Old callback mutated new state");
        runtime->finish(oldJob, State::Active, {}, 9999);
        require(runtime->snapshot().bitrateKbps == 1000, "Late configure mutated replacement");
        runtime->finish(replacement, State::Active);
        NvHTTP::duringRequest = [&] {
            runtime->begin(true, 3000);
        };
        feedback();
        require(runtime->snapshot().bitrateKbps == 3000, "In-flight stale feedback was applied");
        require(runtime->snapshot().state == State::Enabling,
                "Stale completion enabled new stream");
        NvHTTP::duringRequest = {};
        configure();
        feedback();
        require(runtime->snapshot().bitrateKbps == 2000, "Accepted bitrate was not recorded");
        require(NvHTTP::lastFeedbackToken == "fixture-token",
                "V2 feedback omitted the negotiated session token");
        NvHTTP::feedbackResponse = { { "bitrateApplied", true }, { "reason", "stable" } };
        auto legacyJob = runtime->begin(true, 1000);
        auto legacyConfigureResponse = NvHTTP::response;
        NvHTTP::sessionBinding = false;
        NvHTTP::response.remove("sessionToken");
        AbrConfigureTask legacyConfigure({}, 47984, QSslCertificate(), {}, runtime, legacyJob);
        legacyConfigure.run();
        feedback();
        require(runtime->snapshot().state == State::Active &&
                    runtime->snapshot().bitrateKbps == 1000,
                "Legacy stable response without success/newBitrate was rejected or changed target");
        NvHTTP::feedbackResponse = { { "bitrateApplied", false },
                                     { "bitrateApplyError", "legacy target unavailable" },
                                     { "reason", "stable" } };
        feedback();
        require(runtime->snapshot().state == State::Active &&
                    runtime->snapshot().bitrateKbps == 1000 &&
                    runtime->snapshot().detail.contains("legacy target unavailable"),
                "Legacy bitrateApplyError incorrectly disabled ABR or changed target");
        NvHTTP::sessionBinding = true;
        NvHTTP::response = legacyConfigureResponse;
        configure();
        NvHTTP::feedbackResponse = { { "success", true },
                                     { "sessionToken", "fixture-token" },
                                     { "bitrateRequested", false },
                                     { "bitrateApplied", true },
                                     { "reason", "stable" } };
        feedback();
        require(runtime->snapshot().state == State::Active &&
                    runtime->snapshot().bitrateKbps == 1000 && runtime->snapshot().detail.isEmpty(),
                "V2 successful no-change feedback lost prior target or retained stale error");
        NvHTTP::feedbackResponse = {
            { "success", true },          { "sessionToken", "fixture-token" },
            { "bitrateRequested", true }, { "bitrateApplied", true },
            { "newBitrate", 2000 },       { "reason", "increase" }
        };
        feedback();
        require(runtime->snapshot().bitrateKbps == 2000, "Valid new bitrate was ignored");
        NvHTTP::feedbackResponse.remove("newBitrate");
        feedback();
        require(runtime->snapshot().state == State::Degraded &&
                    runtime->snapshot().detail == "invalid_abr_feedback_response",
                "V2 queued bitrate change without newBitrate was accepted");
        configure();
        NvHTTP::feedbackResponse["newBitrate"] = 2147483648.0;
        feedback();
        require(runtime->snapshot().state == State::Degraded &&
                    runtime->snapshot().detail == "invalid_abr_feedback_response",
                "Out-of-range v2 bitrate was converted to an integer");
        configure();
        NvHTTP::feedbackResponse["newBitrate"] = 2000;
        NvHTTP::feedbackResponse["bitrateRequested"] = false;
        feedback();
        require(runtime->snapshot().state == State::Degraded,
                "V2 no-change response unexpectedly included newBitrate");
        configure();
        NvHTTP::feedbackResponse = { { "success", true },
                                     { "sessionToken", "fixture-token" },
                                     { "bitrateRequested", false },
                                     { "bitrateApplied", true },
                                     { "reason", "stable" } };
        NvHTTP::networkFailure = true;
        feedback();
        require(runtime->snapshot().state == State::Degraded,
                "Transient transport failure was not visible");
        NvHTTP::networkFailure = false;
        auto recoveryJob = runtime->beginFeedback();
        require(recoveryJob.has_value(), "Transient feedback failure permanently disabled retries");
        AbrFeedbackTask recovered({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                  *recoveryJob);
        recovered.run();
        require(runtime->snapshot().state == State::Active &&
                    runtime->snapshot().detail.isEmpty() && runtime->snapshot().bitrateKbps == 1000,
                "Valid later feedback did not recover without resetting target");
        NvHTTP::feedbackHttpStatus = 503;
        NvHTTP::feedbackHttpHasProtocolVerdict = false;
        auto serverFailureJob = runtime->beginFeedback();
        require(serverFailureJob.has_value(), "Active feedback unavailable before 5xx fixture");
        AbrFeedbackTask serverFailure({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                      *serverFailureJob);
        serverFailure.run();
        require(runtime->snapshot().state == State::Degraded,
                "Transient 5xx prevented feedback recovery on a later tick");
        NvHTTP::feedbackHttpStatus = 0;
        NvHTTP::feedbackHttpHasProtocolVerdict = true;
        auto serverRecoveryJob = runtime->beginFeedback();
        require(serverRecoveryJob.has_value(), "Retry after 5xx was not eligible");
        AbrFeedbackTask serverRecovered({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                        *serverRecoveryJob);
        serverRecovered.run();
        require(runtime->snapshot().state == State::Active,
                "Successful response did not reset transient 5xx degradation");
        NvHTTP::feedbackHttpStatus = 500;
        auto terminalFailureJob = runtime->beginFeedback();
        require(terminalFailureJob.has_value(),
                "Active feedback unavailable before terminal fixture");
        AbrFeedbackTask terminalFailure({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                        *terminalFailureJob);
        terminalFailure.run();
        require(runtime->snapshot().state == State::Degraded && !runtime->canSendFeedback(),
                "Explicit non-retryable host error did not stop ABR feedback");
        NvHTTP::feedbackHttpStatus = 0;
        NvHTTP::feedbackHttpHasProtocolVerdict = false;
        configure();
        NvHTTP::response["newBitrate"] = 2000;
        NvHTTP::response["bitrateRequested"] = true;
        const int configureBefore = NvHTTP::configureCalls;
        NvHTTP::transientFailures = 1;
        configure();
        require(runtime->snapshot().state == State::Active &&
                    NvHTTP::configureCalls == configureBefore + 2,
                "Typed transient initial configure was not retried");
        QElapsedTimer retryClock;
        retryClock.start();
        const int beforeExhaustion = NvHTTP::configureCalls;
        NvHTTP::transientFailures = 100;
        configure();
        require(NvHTTP::configureCalls == beforeExhaustion + 5,
                "Retry limit is not five total attempts");
        require(retryClock.elapsed() >= 2400 && retryClock.elapsed() < 6500,
                "Real retry timers violated the bounded backoff/deadline");
        require(runtime->snapshot().state == State::HostRejected &&
                    runtime->snapshot().detail.contains("session_starting"),
                "Exhausted typed transient did not expose its error code");
        NvHTTP::configureDelayMs = 2000;
        NvHTTP::transientFailures = 100;
        const int beforeDeadline = NvHTTP::configureCalls;
        retryClock.restart();
        configure();
        require(NvHTTP::configureCalls <= beforeDeadline + 3 && NvHTTP::lastConfigureTimeout > 0 &&
                    NvHTTP::lastConfigureTimeout < 2000 && retryClock.elapsed() >= 5900 &&
                    retryClock.elapsed() < 6500,
                "Configure did not cap HTTP timeout to its six-second deadline");
        NvHTTP::configureDelayMs = 0;
        NvHTTP::capabilitiesDelayMs = 1500;
        NvHTTP::configureDelayMs = 2000;
        NvHTTP::transientFailures = 100;
        const int beforeCapabilitiesDeadline = NvHTTP::configureCalls;
        retryClock.restart();
        configure();
        require(NvHTTP::configureCalls == beforeCapabilitiesDeadline + 2 &&
                    retryClock.elapsed() >= 5500 && retryClock.elapsed() < 6500,
                "Capabilities probe was not charged to the total configure deadline");
        NvHTTP::capabilitiesDelayMs = 0;
        NvHTTP::configureDelayMs = 0;
        NvHTTP::transientFailures = 100;
        const int beforeCancellation = NvHTTP::configureCalls;
        retryClock.restart();
        QTimer::singleShot(50, [&] { runtime->stop(); });
        configure();
        require(NvHTTP::configureCalls == beforeCancellation + 1 && retryClock.elapsed() < 500,
                "Cancel during actual retry wait dispatched another configure");
        require(runtime->snapshot().state == State::Stopped, "Cancelled retry stayed Enabling");
        NvHTTP::transientFailures = 0;

        for (const QString& code : { "stale_session", "abr_not_enabled", "invalid_body",
                                     "missing_client_identity", "internal_error" }) {
            NvHTTP::transientBody["error_code"] = code;
            NvHTTP::transientFailures = 100;
            const int before = NvHTTP::configureCalls;
            configure();
            require(NvHTTP::configureCalls == before + 1, "Non-lifecycle error retried");
        }
        NvHTTP::transientBody["error_code"] = "session_starting";
        for (auto retryable : { QJsonValue(false), QJsonValue("true"), QJsonValue() }) {
            NvHTTP::transientBody["retryable"] = retryable;
            NvHTTP::transientFailures = 100;
            const int before = NvHTTP::configureCalls;
            configure();
            require(NvHTTP::configureCalls == before + 1, "Untyped retryable flag was accepted");
        }
        NvHTTP::transientBody["retryable"] = true;
        for (int status : { 400, 401, 403, 429, 500, 503 }) {
            NvHTTP::transientStatus = status;
            NvHTTP::transientFailures = 100;
            const int before = NvHTTP::configureCalls;
            configure();
            require(NvHTTP::configureCalls == before + 1, "Non-409 response retried");
        }
        NvHTTP::transientStatus = 409;
        NvHTTP::sessionBinding = false;
        NvHTTP::transientFailures = 100;
        const int beforeLegacy = NvHTTP::configureCalls;
        configure();
        require(NvHTTP::configureCalls == beforeLegacy + 1, "Legacy host auto-retried");
        NvHTTP::transientFailures = 0;
        configure();
        feedback();
        require(NvHTTP::lastFeedbackToken.isEmpty(), "Legacy feedback got a v2 token");
        runtime->stop();
        require(!runtime->takeDisableJob(), "Legacy host got an unbound cleanup request");
        NvHTTP::sessionBinding = true;
        configure();
        runtime->stop();
        auto cleanup = runtime->takeDisableJob();
        require(cleanup && cleanup->sessionToken == "fixture-token", "Bound cleanup lost token");
        require(!runtime->takeDisableJob(), "Duplicate disable enqueued for one generation");
        NvHTTP::response["sessionToken"] = "replacement-token";
        configure();
        AbrDisableTask disable({}, 47984, QSslCertificate(), {}, cleanup->sessionToken);
        disable.run();
        require(NvHTTP::lastDisableToken == "fixture-token",
                "Old disable reused replacement token");
        require(runtime->snapshot().sessionToken == "replacement-token" &&
                    runtime->snapshot().state == State::Active,
                "Old cleanup mutated replacement client state");
        NvHTTP::response["sessionToken"] = "fixture-token";
        NvHTTP::bindingFeature = false;
        const int beforeInvalidCapabilities = NvHTTP::configureCalls;
        configure();
        require(runtime->snapshot().state == State::Degraded &&
                    NvHTTP::configureCalls == beforeInvalidCapabilities,
                "V2 missing binding feature silently fell back to tokenless traffic");
        NvHTTP::bindingFeature = true;
        NvHTTP::response["initialBitrate"] = 0;
        configure();
        require(runtime->snapshot().state == State::Degraded,
                "Invalid v2 bitrate fell back to success-shaped Active");
        NvHTTP::response["initialBitrate"] = 1000;
        configure();
        NvHTTP::feedbackResponse["sessionToken"] = "wrong-token";
        feedback();
        require(runtime->snapshot().state == State::Degraded && !runtime->beginFeedback(),
                "Wrong echoed token was accepted or auto-rebound");
        NvHTTP::feedbackResponse["sessionToken"] = "fixture-token";
        NvHTTP::response.remove("sessionToken");
        configure();
        require(runtime->snapshot().state == State::Degraded && !runtime->beginFeedback(),
                "V2 missing session token was accepted");
        NvHTTP::response["sessionToken"] = "fixture-token";

        NvHTTP::status = 400;
        configure();
        require(runtime->snapshot().state == State::HostRejected, "400 silently appears Off/On");
        require(runtime->snapshot().detail.contains("HTTP 400") &&
                    runtime->snapshot().detail.contains("No active streaming session"),
                "Captured rejection reason was lost");
        require(!runtime->beginFeedback(), "Feedback sent after configure rejection");
        require(NvHTTP::calls > 0, "Fake request path not exercised");
        for (int status : { 401, 403, 404, 409, 503 }) {
            NvHTTP::status = status;
            const int before = NvHTTP::calls;
            configure();
            require(runtime->snapshot().state ==
                        SunshineAbrRuntime::httpFailureState(status, "no_active_stream"),
                    "Host error was misclassified");
            require(NvHTTP::calls == before + 1, "Untyped host failure retried");
        }
        NvHTTP::status = 0;
        NvHTTP::supported = false;
        configure();
        require(runtime->snapshot().state == State::Unsupported, "Unsupported host appears Active");
        NvHTTP::supported = true;
        configure();
        NvHTTP::networkFailure = true;
        feedback();
        require(runtime->snapshot().state == State::Degraded, "Feedback failure leaves On stale");
        auto transportRecoveryJob = runtime->beginFeedback();
        require(transportRecoveryJob.has_value(), "Transient feedback failure was not retryable");
        NvHTTP::networkFailure = false;
        AbrFeedbackTask transportRecovery({}, 47984, QSslCertificate(), {}, 0, 0, 60, 0, runtime,
                                          *transportRecoveryJob);
        transportRecovery.run();
        require(runtime->snapshot().state == State::Active,
                "Later feedback did not recover after transport loss");
        configure();
        NvHTTP::status = 401;
        feedback();
        require(runtime->snapshot().state == State::AuthenticationFailed,
                "Feedback auth failure only marked transient");
        require(!runtime->beginFeedback(), "Auth failure retried");
        NvHTTP::status = 0;
        runtime->begin(false, 1000);
        require(runtime->snapshot().state == State::Disabled, "Preference changed by rejection");
        runtime->stop();
        require(runtime->snapshot().state == State::Disabled, "Disabled preference lost on stop");
        configure();
        runtime->stop();
        require(runtime->snapshot().state == State::Stopped, "Stopped connection appears active");
        require(!runtime->beginFeedback(), "Stopped connection allows feedback");
    } catch (const std::exception& error) {
        qCritical("FAIL: %s", error.what());
        return 1;
    }
    qInfo("feedback-regressions passed");
    return 0;
}
