#pragma once

#include <array>
#include <memory>
#include <optional>

class Observer;

namespace celestia::engine
{

class SkyLuminanceFeedback
{
public:
    SkyLuminanceFeedback();
    ~SkyLuminanceFeedback();

    SkyLuminanceFeedback(const SkyLuminanceFeedback&) = delete;
    SkyLuminanceFeedback& operator=(const SkyLuminanceFeedback&) = delete;

    void consume();
    void capture(const Observer&, const std::array<int, 4>& viewport,
                 bool active);
    bool hasPendingReadback() const;
    std::optional<float> luminance(const Observer&) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace celestia::engine
