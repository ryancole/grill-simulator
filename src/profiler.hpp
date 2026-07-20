#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Where the frame's time goes, on the CPU, in named sections.
//
// This exists because a hunch about cost is not a measurement. The soft-body meats skin
// thousands of vertices per frame on the CPU, and the honest answer to "is that
// expensive?" was, for a while, that nobody had looked. A section here is one `Scope` in
// the frame loop and one line in the debug overlay.
//
// Two things it deliberately does not do. It does not time the GPU: every number here is
// how long the CPU spent, and a pass that merely *submits* quickly can still cost the GPU
// dearly. That wants D3D12 timestamp queries, which is a bigger tool than this one. And
// it does not try to be exact -- the readout is exponentially smoothed, because a number
// that changes sixty times a second cannot be read.
//
// The frame total includes waiting on the swapchain, so with vsync on it sits at the
// refresh interval however little work is being done. Read the sections against each
// other, not against the total.
class Profiler {
public:
    // Starts a frame's measurement. Call once at the top of the loop.
    void BeginFrame();
    // Closes the frame: totals it, ages every section's smoothed value, and rebuilds
    // Report(). Sections not recorded this frame decay toward zero rather than sticking
    // at their last value, so a system that stops running stops being reported.
    void EndFrame();

    // Adds `milliseconds` to the named section for this frame. Named sections accumulate
    // within a frame, so a section timed in two places reports their sum.
    void Record(std::string_view name, float milliseconds);

    // The formatted overlay lines, rebuilt by EndFrame: the frame total first, then each
    // section, widest first.
    std::span<const std::string> Report() const { return report_; }

    // Times a block and records it on destruction. The ordinary way to use a Profiler:
    //     { Profiler::Scope s{profiler, "skin"}; ...work... }
    class Scope {
    public:
        Scope(Profiler& profiler, std::string_view name)
            : profiler_(&profiler), name_(name), start_(std::chrono::steady_clock::now()) {}
        ~Scope() {
            const std::chrono::duration<float, std::milli> elapsed =
                std::chrono::steady_clock::now() - start_;
            profiler_->Record(name_, elapsed.count());
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Profiler* profiler_;
        std::string_view name_;
        std::chrono::steady_clock::time_point start_;
    };

private:
    struct Section {
        std::string name;
        // This frame's accumulated time, reset by EndFrame.
        float milliseconds = 0.0f;
        // The smoothed value the overlay shows.
        float smoothed = 0.0f;
    };

    std::vector<Section> sections_;
    std::chrono::steady_clock::time_point frame_start_;
    float frame_smoothed_ = 0.0f;
    std::vector<std::string> report_;
};
