#include "profiler.hpp"

#include <algorithm>
#include <cstdio>

namespace {

// How much of each new measurement the smoothed value takes. Low enough that the readout
// holds still long enough to read, high enough that it follows a real change within a
// second or so rather than lagging behind whatever is being tuned.
constexpr float kSmoothing = 0.05f;

// Formats to two decimals without dragging <format> or iostreams in for one line.
std::string Milliseconds(std::string_view label, float value) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%-10.*s %6.2f ms", static_cast<int>(label.size()),
                  label.data(), value);
    return buffer;
}

} // namespace

void Profiler::BeginFrame() {
    frame_start_ = std::chrono::steady_clock::now();
}

void Profiler::Record(std::string_view name, float milliseconds) {
    for (Section& section : sections_) {
        if (section.name == name) {
            // Accumulated, not overwritten: a section entered twice in one frame reports
            // the total time spent in it, which is what "where did the frame go" means.
            section.milliseconds += milliseconds;
            return;
        }
    }
    sections_.push_back(Section{std::string(name), milliseconds, milliseconds});
}

void Profiler::EndFrame() {
    const std::chrono::duration<float, std::milli> elapsed =
        std::chrono::steady_clock::now() - frame_start_;
    frame_smoothed_ += (elapsed.count() - frame_smoothed_) * kSmoothing;

    for (Section& section : sections_) {
        // Sections that did not run this frame decay toward zero on their own, so a
        // system that stops running stops being reported rather than freezing at its
        // last value and reading as a live cost.
        section.smoothed += (section.milliseconds - section.smoothed) * kSmoothing;
        section.milliseconds = 0.0f;
    }

    // Widest first: the line that matters is the one at the top.
    std::vector<const Section*> ordered;
    ordered.reserve(sections_.size());
    for (const Section& section : sections_) {
        ordered.push_back(&section);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Section* a, const Section* b) { return a->smoothed > b->smoothed; });

    report_.clear();
    report_.reserve(ordered.size() + 1);
    char frame_line[96];
    const float fps = frame_smoothed_ > 0.0001f ? 1000.0f / frame_smoothed_ : 0.0f;
    std::snprintf(frame_line, sizeof(frame_line), "%-10s %6.2f ms  (%.0f fps)", "frame",
                  frame_smoothed_, fps);
    report_.emplace_back(frame_line);
    for (const Section* section : ordered) {
        report_.push_back(Milliseconds(section->name, section->smoothed));
    }
}
