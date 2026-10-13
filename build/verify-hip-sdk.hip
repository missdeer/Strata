#include <hip/hip_runtime.h>
#include <cstdio>

__global__ void add(const float* x, const float* y, float* output, int n) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) output[i] = x[i] + y[i];
}

int main() {
    // Offline compilation/link check only. This executable is not run on the NVIDIA build host.
    float *x = nullptr, *y = nullptr, *output = nullptr;
    constexpr int n = 16;
    if (hipMalloc(&x, n * sizeof(float)) != hipSuccess || hipMalloc(&y, n * sizeof(float)) != hipSuccess ||
        hipMalloc(&output, n * sizeof(float)) != hipSuccess) return 1;
    hipLaunchKernelGGL(add, dim3(1), dim3(64), 0, 0, x, y, output, n);
    const auto rc = hipDeviceSynchronize();
    hipFree(x); hipFree(y); hipFree(output);
    std::printf("%s\n", hipGetErrorString(rc));
    return rc == hipSuccess ? 0 : 1;
}
