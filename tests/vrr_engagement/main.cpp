#include "streaming/video/ffmpeg-renderers/pacer/vrrengagementdetector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

static int s_Failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            s_Failures++;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

using Verdict = VrrEngagementDetector::Verdict;
using Source = VrrEngagementDetector::Source;
using GsyncState = VrrEngagementDetector::GsyncState;
using Action = VrrFallbackController::Action;

struct Rng
{
    uint64_t state;
    double uniform()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }
};

enum class Panel
{
    // Adaptive sync off: vblanks at a fixed rate whatever we present
    Fixed,
    // Adaptive sync on: each present starts a refresh, no faster than the
    // panel's maximum; the panel refreshes on its own below its minimum rate
    Vrr,
};

struct Model
{
    Panel panel = Panel::Fixed;
    double displayHz = 180;
    double minHz = 48;
    double presentFps = 168;
    // Uniform +/- spread of each present around its nominal time
    double jitterUs = 100;
    double seconds = 12;
    uint64_t seed = 1;
    bool virtualizationDisabled = true;
    std::function<GsyncState(double)> nvapi = [](double) {
        return GsyncState::Unavailable;
    };
    std::function<bool(double)> invalidAt = [](double) {
        return false;
    };
    // No presents in [stallStartS, stallStartS + stallS)
    double stallStartS = -1;
    double stallS = 0;
    // The swapchain is recreated: its counters restart from 0
    double countResetS = -1;
};

struct Outcome
{
    Verdict verdict = Verdict::Unknown;
    Source source = Source::None;
    double vblankHz = 0;
    double presentHz = 0;
    bool everNotEngaged = false;
    bool everEngaged = false;
    double firstNotEngagedS = -1;
    double firstEngagedS = -1;
    int nvapiPolls = 0;
};

// Feeds the detector what the D3D11 renderer reads after each Present():
// DXGI_FRAME_STATISTICS from a physical panel model, plus NVAPI polls.
Outcome run(const Model& model)
{
    Rng rng{ model.seed };
    VrrEngagementDetector::Config config;
    config.displayHz = model.displayHz;
    config.vblankVirtualizationDisabled = model.virtualizationDisabled;
    VrrEngagementDetector detector(config);

    const double periodUs = 1000000.0 / model.presentFps;
    const double vblankUs = 1000000.0 / model.displayHz;
    const double maxGapUs = 1000000.0 / model.minHz;
    // An arbitrary non-zero clock origin, like QPC
    const double originUs = 5000000.0;

    std::vector<double> presents;
    for (double nominal = 0; nominal < model.seconds * 1000000.0; nominal += periodUs) {
        const double s = nominal / 1000000.0;
        if (model.stallStartS >= 0 && s >= model.stallStartS &&
            s < model.stallStartS + model.stallS) {
            continue;
        }
        presents.push_back(std::max(0.0, nominal + (rng.uniform() * 2 - 1) * model.jitterUs));
    }

    Outcome outcome;
    double lastRefreshUs = 0;
    uint64_t refreshes = 0;
    uint64_t refreshBase = 0;
    uint64_t presentBase = 0;
    bool countsReset = false;
    for (size_t i = 0; i < presents.size(); i++) {
        const double t = presents[i];

        uint64_t refreshCount;
        double syncUs;
        if (model.panel == Panel::Fixed) {
            refreshCount = static_cast<uint64_t>(std::floor(t / vblankUs));
            syncUs = refreshCount * vblankUs;
        } else {
            while (t - lastRefreshUs > maxGapUs) {
                lastRefreshUs += maxGapUs;
                refreshes++;
            }
            lastRefreshUs = i == 0 ? t : std::max(t, lastRefreshUs + vblankUs);
            refreshes++;
            refreshCount = refreshes;
            syncUs = lastRefreshUs;
        }

        const double s = t / 1000000.0;
        uint64_t presentCount = i + 1;
        if (model.countResetS >= 0 && s >= model.countResetS) {
            if (!countsReset) {
                countsReset = true;
                refreshBase = refreshCount;
                presentBase = presentCount - 1;
            }
            refreshCount -= refreshBase;
            presentCount -= presentBase;
        }

        const uint64_t nowUs = static_cast<uint64_t>(originUs + t);
        if (detector.nvapiPollDue(nowUs)) {
            outcome.nvapiPolls++;
            detector.observeNvapi(nowUs, model.nvapi(s));
        }
        detector.observeFrameStatistics(
            nowUs, !model.invalidAt(s), static_cast<uint32_t>(presentCount),
            static_cast<uint32_t>(refreshCount), static_cast<uint64_t>(originUs + syncUs));

        if (detector.verdict() == Verdict::NotEngaged && !outcome.everNotEngaged) {
            outcome.everNotEngaged = true;
            outcome.firstNotEngagedS = s;
        }
        if (detector.verdict() == Verdict::Engaged && !outcome.everEngaged) {
            outcome.everEngaged = true;
            outcome.firstEngagedS = s;
        }
    }

    outcome.verdict = detector.verdict();
    outcome.source = detector.source();
    outcome.vblankHz = detector.vblankHz();
    outcome.presentHz = detector.presentHz();
    return outcome;
}

void print(const char* name, const Outcome& o)
{
    std::printf(
        "%-42s verdict=%d source=%d V=%.1f P=%.1f notEngagedAt=%.2fs engagedAt=%.2fs polls=%d\n",
        name, static_cast<int>(o.verdict), static_cast<int>(o.source), o.vblankHz, o.presentHz,
        o.firstNotEngagedS, o.firstEngagedS, o.nvapiPolls);
}

// a5f3 (monitor adaptive sync off, fork VRR with tearing): Rx 178.4, Rd 168.0,
// frame times 5.95 ms avg / 6.00 ms 99th, 5.85% dropped (log lines 704-717,
// 828-833). The panel stayed at a fixed 180 Hz.
void testFixedPanelFromFieldLog()
{
    Model model;
    model.presentFps = 168.0;
    model.jitterUs = 50;
    Outcome o = run(model);
    print("a5f3 fork VRR, fixed 180 Hz, Rd 168", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.source == Source::VblankCadence);
    CHECK(o.firstNotEngagedS >= 4.0 && o.firstNotEngagedS < 4.2);
    CHECK(std::fabs(o.vblankHz - 180.0) < 0.5);
    CHECK(std::fabs(o.presentHz - 168.0) < 0.5);
    CHECK(!o.everEngaged);

    // Third a5f3 session: 8.31% dropped, 99th 13.30 ms (lines 1262-1264)
    model.presentFps = 164.4;
    model.jitterUs = 1500;
    model.seed = 7;
    o = run(model);
    print("a5f3 third session, fixed 180 Hz, Rd 164.4", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(!o.everEngaged);

    // The same display streaming 171 FPS (the below-refresh default) or a
    // 120 FPS game: still a fixed 180 Hz vblank
    model.presentFps = 171;
    model.jitterUs = 800;
    o = run(model);
    print("fixed 180 Hz, 171 FPS", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    model.presentFps = 120;
    o = run(model);
    print("fixed 180 Hz, 120 FPS", o);
    CHECK(o.verdict == Verdict::NotEngaged);

    // A 179.96 Hz timing
    model.displayHz = 179.96;
    model.presentFps = 171;
    o = run(model);
    print("fixed 179.96 Hz, 171 FPS", o);
    CHECK(o.verdict == Verdict::NotEngaged);
}

void testEngagedVrr()
{
    // Adaptive sync on, 171 FPS stream: the panel refreshes once per present
    Model model;
    model.panel = Panel::Vrr;
    model.presentFps = 171;
    model.jitterUs = 800;
    Outcome o = run(model);
    print("VRR on, 171 FPS", o);
    CHECK(o.verdict == Verdict::Engaged);
    CHECK(o.source == Source::VblankCadence);
    CHECK(!o.everNotEngaged);

    // cd28: VRR on, NVIDIA cap, Rd 170.2
    model.presentFps = 170.2;
    model.jitterUs = 150;
    o = run(model);
    print("cd28 VRR on + driver cap, Rd 170.2", o);
    CHECK(o.verdict == Verdict::Engaged);
    CHECK(!o.everNotEngaged);

    // e1d1: VRR on, native 180-on-180, Rd 179.0 (lines 469-474). No headroom
    // to tell VRR from a fixed refresh: the detector must not guess.
    model.presentFps = 179.0;
    model.jitterUs = 300;
    o = run(model);
    print("e1d1 VRR on, native 179/180", o);
    CHECK(!o.everNotEngaged);
    model.panel = Panel::Fixed;
    o = run(model);
    print("fixed 180 Hz, 179 FPS (no headroom)", o);
    CHECK(o.verdict == Verdict::Unknown);
}

void testAmbiguousCadences()
{
    // Integer ratios: a fixed panel showing every 2nd or 3rd vblank looks like
    // low framerate compensation on a VRR panel
    Model model;
    model.jitterUs = 300;
    model.presentFps = 90;
    Outcome o = run(model);
    print("fixed 180 Hz, 90 FPS", o);
    CHECK(o.verdict == Verdict::Unknown);
    model.presentFps = 60;
    o = run(model);
    print("fixed 180 Hz, 60 FPS", o);
    CHECK(o.verdict == Verdict::Unknown);

    model.panel = Panel::Vrr;
    model.presentFps = 30;
    o = run(model);
    print("VRR on, 30 FPS below the range", o);
    CHECK(!o.everNotEngaged);

    // 59 FPS at 60 Hz: no headroom
    model.panel = Panel::Fixed;
    model.displayHz = 60;
    model.presentFps = 59;
    o = run(model);
    print("fixed 60 Hz, 59 FPS", o);
    CHECK(o.verdict == Verdict::Unknown);
}

void testVblankVirtualization()
{
    // With vblank virtualization on, DXGI reports a steady virtual vblank
    // even when the panel follows the presents
    Model model;
    model.virtualizationDisabled = false;
    Outcome o = run(model);
    print("virtualization on, fixed 180 Hz, 168 FPS", o);
    CHECK(o.verdict == Verdict::Unknown);
    CHECK(!o.everEngaged && !o.everNotEngaged);

    // Unknown refresh rate
    VrrEngagementDetector detector({ 0, true });
    CHECK(!detector.observeFrameStatistics(10000000, true, 100, 100, 10000000));
    CHECK(!detector.observeFrameStatistics(13000000, true, 600, 640, 13000000));
    CHECK(!detector.observeFrameStatistics(16000000, true, 1100, 1180, 16000000));
    CHECK(detector.verdict() == Verdict::Unknown);
}

void testSampleHygiene()
{
    // A failed query (DISJOINT after a mode change) every 1.5 s: no window
    // ever completes
    Model model;
    model.invalidAt = [](double s) {
        return std::fmod(s, 1.5) < 0.01;
    };
    Outcome o = run(model);
    print("invalid sample every 1.5 s", o);
    CHECK(o.verdict == Verdict::Unknown);

    model.invalidAt = [](double s) {
        return s < 5.0;
    };
    o = run(model);
    print("invalid samples until 5 s", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.firstNotEngagedS >= 7.0);

    // A 5 s hole in the samples is not one window
    model.invalidAt = [](double) {
        return false;
    };
    model.stallStartS = 2.5;
    model.stallS = 5;
    o = run(model);
    print("5 s stall at 2.5 s, fixed 180 Hz", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.firstNotEngagedS >= 9.5);

    // VRR engaged with a 0.5 s stall: the panel self-refreshes at its minimum
    model.panel = Panel::Vrr;
    model.presentFps = 171;
    model.stallStartS = 5;
    model.stallS = 0.5;
    o = run(model);
    print("VRR on, 171 FPS, 0.5 s stall", o);
    CHECK(!o.everNotEngaged);
    CHECK(o.verdict == Verdict::Engaged);

    // Swapchain recreated: the counters restart
    model.stallStartS = -1;
    model.countResetS = 6;
    o = run(model);
    print("VRR on, 171 FPS, counters reset at 6 s", o);
    CHECK(!o.everNotEngaged);
    CHECK(o.verdict == Verdict::Engaged);
}

void testNvapi()
{
    Model model;
    model.panel = Panel::Vrr;
    model.presentFps = 171;
    model.jitterUs = 800;
    model.nvapi = [](double) {
        return GsyncState::Active;
    };
    Outcome o = run(model);
    print("NVAPI active", o);
    CHECK(o.verdict == Verdict::Engaged);
    CHECK(o.source == Source::Nvapi);
    CHECK(o.firstEngagedS >= 4.0 && o.firstEngagedS < 4.2);
    CHECK(o.nvapiPolls >= 9 && o.nvapiPolls <= 11);

    model.panel = Panel::Fixed;
    model.nvapi = [](double) {
        return GsyncState::Inactive;
    };
    o = run(model);
    print("NVAPI inactive", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.source == Source::Nvapi);
    CHECK(o.firstNotEngagedS >= 4.0 && o.firstNotEngagedS < 4.2);

    // Unavailable (AMD, Intel, old driver): one poll, then the heuristic
    model.nvapi = [](double) {
        return GsyncState::Unavailable;
    };
    o = run(model);
    print("NVAPI unavailable, fixed 180 Hz", o);
    CHECK(o.nvapiPolls == 1);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.source == Source::VblankCadence);

    // One inactive poll is not a verdict
    model.panel = Panel::Vrr;
    model.nvapi = [](double s) {
        return s > 6.0 && s < 6.9 ? GsyncState::Inactive : GsyncState::Active;
    };
    o = run(model);
    print("NVAPI one inactive flicker", o);
    CHECK(o.verdict == Verdict::Engaged);
    CHECK(!o.everNotEngaged);

    // NVAPI outranks the cadence heuristic
    model.panel = Panel::Fixed;
    model.presentFps = 168;
    model.nvapi = [](double) {
        return GsyncState::Active;
    };
    o = run(model);
    print("NVAPI active over a fixed cadence", o);
    CHECK(o.verdict == Verdict::Engaged);
    CHECK(o.source == Source::Nvapi);
    CHECK(!o.everNotEngaged);

    // Adaptive sync switched off mid-stream
    model.nvapi = [](double s) {
        return s < 8.0 ? GsyncState::Active : GsyncState::Inactive;
    };
    o = run(model);
    print("NVAPI active, then inactive at 8 s", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.firstNotEngagedS >= 10.0 && o.firstNotEngagedS < 11.2);

    // A failed query (handle invalidated by a mode change) is not "no NVIDIA
    // driver": NVAPI keeps being asked and still decides afterwards
    model.nvapi = [](double s) {
        if (s >= 6.0 && s < 7.9) {
            return GsyncState::Error;
        }
        return s < 6.0 ? GsyncState::Active : GsyncState::Inactive;
    };
    o = run(model);
    print("NVAPI active, 2 failed queries, inactive", o);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.source == Source::Nvapi);
    CHECK(o.firstNotEngagedS >= 10.0 && o.firstNotEngagedS < 11.2);

    // A query that keeps failing gives way to the cadence heuristic
    model.nvapi = [](double) {
        return GsyncState::Error;
    };
    o = run(model);
    print("NVAPI query always failing", o);
    CHECK(o.nvapiPolls == VrrEngagementDetector::MaxNvapiErrors);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.source == Source::VblankCadence);

    // Also after NVAPI has answered
    model.seconds = 16;
    model.nvapi = [](double s) {
        return s < 6.0 ? GsyncState::Inactive : GsyncState::Error;
    };
    o = run(model);
    print("NVAPI inactive, then failing from 6 s", o);
    CHECK(o.nvapiPolls == 4 + VrrEngagementDetector::MaxNvapiErrors);
    CHECK(o.verdict == Verdict::NotEngaged);
    CHECK(o.source == Source::VblankCadence);

    // Notices the renderer forwards to the session
    CHECK(VrrEngagementDetector::noticeWanted(false, Verdict::NotEngaged, Source::Nvapi));
    CHECK(VrrEngagementDetector::noticeWanted(false, Verdict::NotEngaged, Source::VblankCadence));
    CHECK(!VrrEngagementDetector::noticeWanted(false, Verdict::Engaged, Source::Nvapi));
    CHECK(!VrrEngagementDetector::noticeWanted(false, Verdict::Unknown, Source::None));
    CHECK(VrrEngagementDetector::noticeWanted(true, Verdict::Engaged, Source::Nvapi));
    CHECK(!VrrEngagementDetector::noticeWanted(true, Verdict::Engaged, Source::VblankCadence));
    CHECK(!VrrEngagementDetector::noticeWanted(true, Verdict::NotEngaged, Source::Nvapi));
}

void testFallbackController()
{
    const uint64_t s = 1000000;
    {
        VrrFallbackController c;
        CHECK(c.onNotice(4 * s, Verdict::Unknown, Source::None) == Action::None);
        CHECK(c.onNotice(4 * s, Verdict::Engaged, Source::Nvapi) == Action::None);
        CHECK(c.onNotice(5 * s, Verdict::NotEngaged, Source::Nvapi) == Action::EnterFallback);
        CHECK(c.inFallback());
        CHECK(c.onNotice(6 * s, Verdict::NotEngaged, Source::Nvapi) == Action::None);
        // VRR back on: wait out the dwell before switching back
        CHECK(c.onNotice(9 * s, Verdict::Engaged, Source::Nvapi) == Action::None);
        CHECK(c.inFallback());
        CHECK(c.onNotice(15 * s, Verdict::Engaged, Source::Nvapi) == Action::ExitFallback);
        CHECK(!c.inFallback());
        // Re-entering also waits out the dwell
        CHECK(c.onNotice(20 * s, Verdict::NotEngaged, Source::Nvapi) == Action::None);
        CHECK(c.onNotice(25 * s, Verdict::NotEngaged, Source::Nvapi) == Action::EnterFallback);
        CHECK(c.onNotice(35 * s, Verdict::Engaged, Source::Nvapi) == Action::ExitFallback);
        CHECK(c.onNotice(45 * s, Verdict::NotEngaged, Source::Nvapi) == Action::EnterFallback);
        // Two switch-backs used: flapping ends in fixed-refresh pacing
        CHECK(c.onNotice(60 * s, Verdict::Engaged, Source::Nvapi) == Action::None);
        CHECK(c.inFallback());
    }
    {
        // The cadence heuristic warns once and never switches pacing
        VrrFallbackController c;
        CHECK(c.onNotice(4 * s, Verdict::NotEngaged, Source::VblankCadence) ==
              Action::WarnHeuristic);
        CHECK(!c.inFallback());
        CHECK(c.onNotice(9 * s, Verdict::NotEngaged, Source::VblankCadence) == Action::None);
        CHECK(c.onNotice(30 * s, Verdict::Engaged, Source::VblankCadence) == Action::None);
        CHECK(!c.inFallback());
    }
    {
        // The session couldn't queue the renderer reset: the renderer keeps
        // its mode and repeats its notice, which retries the reset
        VrrFallbackController c;
        CHECK(c.onNotice(5 * s, Verdict::NotEngaged, Source::Nvapi) == Action::EnterFallback);
        c.resetFailed();
        CHECK(c.onNotice(10 * s, Verdict::NotEngaged, Source::Nvapi) == Action::RetryReset);
        CHECK(c.inFallback());
        CHECK(c.onNotice(15 * s, Verdict::NotEngaged, Source::Nvapi) == Action::None);
        // The dwell counts from the reset that went through
        CHECK(c.onNotice(19 * s, Verdict::Engaged, Source::Nvapi) == Action::None);
        CHECK(c.onNotice(20 * s, Verdict::Engaged, Source::Nvapi) == Action::ExitFallback);
        c.resetFailed();
        CHECK(c.onNotice(25 * s, Verdict::Engaged, Source::Nvapi) == Action::RetryReset);
        CHECK(!c.inFallback());
    }
}

} // namespace

int main()
{
    testFixedPanelFromFieldLog();
    testEngagedVrr();
    testAmbiguousCadences();
    testVblankVirtualization();
    testSampleHygiene();
    testNvapi();
    testFallbackController();

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", s_Failures);
        return 1;
    }
    std::printf("All VRR engagement tests passed\n");
    return 0;
}
