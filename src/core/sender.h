#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "state.h"

namespace landrop {

struct SendRequest {
    uint64_t jobId = 0;
    std::string host;
    uint16_t port = 0;
    std::string deviceName;
    std::vector<fs::path> selectedPaths;
    std::shared_ptr<std::atomic<bool>> cancel;
};

class Sender {
public:
    Sender(CoreState& state, SendRequest request);
    void run() noexcept;

private:
    class Attempt;

    CoreState& state_;
    SendRequest request_;
};

}  // namespace landrop
