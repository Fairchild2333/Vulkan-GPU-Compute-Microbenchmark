/*
 * matmul_naive.cl
 *
 * Naive matrix multiplication kernel.
 * Each work-item computes one element of the output matrix C = A * B.
 * All reads from A and B go directly to global memory, which results in
 * high memory latency and low bandwidth utilisation.
 *
 * Matrix layout: row-major, square matrices of dimension N x N.
 */

__kernel void matmul_naive(
    __global const float* A,
    __global const float* B,
    __global       float* C,
    const          int    N)
{
    const int row = get_global_id(0);
    const int col = get_global_id(1);

    if (row >= N || col >= N)
        return;

    float sum = 0.0f;
    for (int k = 0; k < N; ++k) {
        sum += A[row * N + k] * B[k * N + col];
    }
    C[row * N + col] = sum;
}
