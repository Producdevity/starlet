#pragma once
namespace starlet::gx {
class GpuCompletionBackend {
public:
    virtual ~GpuCompletionBackend() = default;
    // Return once no CPU-visible result of previously submitted work is pending:
    // a backend keeping every result GPU-resident may return after submission.
    virtual void wait_until_idle() = 0;
};
}
