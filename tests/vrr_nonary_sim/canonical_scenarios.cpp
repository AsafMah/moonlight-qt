// Writes the canonical VRR comparison scenarios as vrr_nonary_sim input CSVs,
// so the Nonary pacer runs on exactly the arrivals that tests/vrr_policy's
// simulateVrrLoop() uses for the fork's on-arrival and Smooth pacers.
//
// generateArrivals() is copied verbatim from tests/vrr_policy/main.cpp at
// 7a305d55 (same seed, same 10 s runs). Build and run:
//   cl /O2 /EHsc /std:c++17 canonical_scenarios.cpp
//   canonical_scenarios.exe <outdir>
//   vrr_nonary_sim.exe <outdir>\<name>-m<mode>.csv --output <name>-m<mode>.json
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Arrival
{
    uint64_t sourceUs;  // host capture time (the RTP timestamp)
    uint64_t arrivalUs; // decoded and handed to the Pacer
};

struct ArrivalModel
{
    int hostHz;
    double contentFps;
    double pairedFraction;
    bool stalls;
};

static std::vector<Arrival> generateArrivals(const ArrivalModel& m, uint64_t durationUs)
{
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto uniform = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (state >> 11) * (1.0 / 9007199254740992.0);
    };
    auto normal = [&]() {
        const double u1 = std::max(uniform(), 1e-12);
        const double u2 = uniform();
        return std::sqrt(-2 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    };

    std::vector<Arrival> out;
    const double tick = 1e6 / m.hostHz;
    double gameUs = 100000;
    uint64_t stallStart = 0, stallEnd = 0;
    uint64_t nextStall = 1500000 + static_cast<uint64_t>(uniform() * 1000000);
    while (gameUs < durationUs) {
        gameUs += 1e6 / m.contentFps * (1.0 + 0.1 * normal());
        const uint64_t source = static_cast<uint64_t>(std::ceil(gameUs / tick) * tick);
        if (!out.empty() && source <= out.back().sourceUs) {
            continue; // the host keeps only the newest frame of a tick
        }
        uint64_t arrival = source + 9000 + static_cast<uint64_t>(std::fabs(normal()) * 700);
        if (m.stalls) {
            if (source >= nextStall) {
                stallStart = source;
                stallEnd = source + 7000 + static_cast<uint64_t>(uniform() * 30000);
                nextStall = source + 1000000 + static_cast<uint64_t>(uniform() * 2000000);
            }
            if (source >= stallStart && source < stallEnd) {
                arrival = std::max<uint64_t>(arrival, stallEnd + 9000);
            }
        }
        out.push_back({ source, arrival });
    }
    for (size_t i = 0; i + 1 < out.size(); i++) {
        if (uniform() < m.pairedFraction) {
            out[i].arrivalUs = std::max(out[i].arrivalUs, out[i + 1].arrivalUs - 100);
        }
    }
    for (size_t i = 1; i < out.size(); i++) {
        out[i].arrivalUs = std::max(out[i].arrivalUs, out[i - 1].arrivalUs + 30);
    }
    return out;
}

int main(int argc, char** argv)
{
    const std::string outDir = argc > 1 ? argv[1] : ".";
    struct Named
    {
        const char* name;
        ArrivalModel model;
    };
    // The scenarios testLimiterLoop() and testSmoothLoop() print
    const Named scenarios[] = {
        { "h180-c200", { 180, 200, 0, false } },
        { "h180-c130-paired30", { 180, 130, 0.3, false } },
        { "h180-c130-stalls", { 180, 130, 0, true } },
        { "h1000-c165", { 1000, 165, 0, false } },
        { "h180-c170-paired30", { 180, 170, 0.3, false } },
        { "h1000-c130", { 1000, 130, 0, false } },
        { "h1000-c130-stalls", { 1000, 130, 0, true } },
    };
    for (const Named& s : scenarios) {
        const auto arrivals = generateArrivals(s.model, 10000000);
        for (int mode = 0; mode <= 2; mode++) {
            const std::string path = outDir + "/" + s.name + "-m" + std::to_string(mode) + ".csv";
            FILE* f = std::fopen(path.c_str(), "w");
            if (f == nullptr) {
                return 1;
            }
            std::fprintf(f, "arrival_sequence,frame,rtp_timestamp,rtp_valid,decode_complete_us,"
                            "pacer_arrival_us,display_refresh_hz,stream_rate_hz,"
                            "session_latency_mode,prepare_us,prepare_acquire_us,present_call_us,"
                            "controller_call_us,can_latch_present,source_period_us\n");
            for (size_t i = 0; i < arrivals.size(); i++) {
                const uint32_t rtp = static_cast<uint32_t>(arrivals[i].sourceUs * 9 / 100);
                std::fprintf(f, "%zu,%zu,%u,1,%llu,%llu,180,180,%d,200,0,50,5,1,0\n", i + 1, i + 1,
                             rtp, static_cast<unsigned long long>(arrivals[i].arrivalUs),
                             static_cast<unsigned long long>(arrivals[i].arrivalUs), mode);
            }
            std::fclose(f);
        }
    }
    return 0;
}
