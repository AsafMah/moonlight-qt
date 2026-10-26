#include "streaming/video/ffmpeg-renderers/pacer/presentstalldetector.h"

#include <cstdio>

static int s_Failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            s_Failures++;                                                                          \
        }                                                                                          \
    } while (0)

int main()
{
    using Detector = PresentStallDetector;

    CHECK(Detector::effectivePresentationFps(120, 60, true, false, 0) == 60);
    CHECK(Detector::effectivePresentationFps(60, 30, true, false, 0) == 30);
    CHECK(Detector::effectivePresentationFps(120, 60, false, false, 0) == 120);
    CHECK(Detector::effectivePresentationFps(120, 0, true, false, 0) == 120);
    CHECK(Detector::effectivePresentationFps(0, 60, true, false, 0) == 0);
    CHECK(Detector::effectivePresentationFps(120, 144, false, true, 138) == 120);
    CHECK(Detector::effectivePresentationFps(240, 144, false, true, 138) == 138);

    Detector detector(120, 60, true, false, 0);
    CHECK(detector.thresholdUs() == 20834);
    for (uint64_t i = 0; i < Detector::RequiredSlowFrames - 1; i++) {
        CHECK(!detector.observe(1000000 + i * 16667, 25000, 1, 10000, 5000));
    }
    CHECK(detector.observe(1100000, 25000, 1, 10000, 5000));
    CHECK(detector.triggerAverageRenderUs() == 25000);
    CHECK(detector.triggerAveragePrePresentUs() == 10000);
    CHECK(detector.triggerAveragePresentUs() == 5000);
    CHECK(detector.shouldSkipFrame(1100001));
    CHECK(!detector.shouldSkipFrame(1600000));
    CHECK(detector.takeDrainEnded());
    CHECK(!detector.observe(1700000, 50000, 3));

    Detector reset(120, 60, true, false, 0);
    for (uint64_t i = 0; i < Detector::RequiredSlowFrames - 1; i++) {
        reset.observe(2000000 + i * 16667, 25000, 1);
    }
    CHECK(!reset.observe(2200000, 25000, 0));
    CHECK(!reset.observe(2210000, reset.thresholdUs(), 1));
    for (uint64_t i = 0; i < Detector::RequiredSlowFrames - 1; i++) {
        reset.observe(2300000 + i * 16667, 25000, 1);
    }
    reset.resetStreak();
    CHECK(!reset.observe(2400000, 25000, 1));

    return s_Failures == 0 ? 0 : 1;
}
