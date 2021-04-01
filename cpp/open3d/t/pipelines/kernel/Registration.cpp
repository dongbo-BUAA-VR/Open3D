// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/pipelines/kernel/Registration.h"

#include "open3d/core/CoreUtil.h"
#include "open3d/t/pipelines/kernel/RegistrationImpl.h"

namespace open3d {
namespace t {
namespace pipelines {
namespace kernel {
namespace registration {

core::Tensor ComputePosePointToPlane(
        const core::Tensor &source_points,
        const core::Tensor &target_points,
        const core::Tensor &target_normals,
        const std::pair<core::Tensor, core::Tensor> &corres) {
    // Get dtype and device.
    core::Dtype dtype = source_points.GetDtype();
    core::Device device = source_points.GetDevice();

    // Checks.
    target_points.AssertDtype(dtype);
    target_normals.AssertDtype(dtype);
    target_points.AssertDevice(device);
    target_normals.AssertDevice(device);

    // Pose {6,} tensor [ouput].
    core::Tensor pose = core::Tensor::Empty({6}, dtype, device);

    core::Tensor source_points_contiguous = source_points.Contiguous();
    core::Tensor target_points_contiguous = target_points.Contiguous();
    core::Tensor target_normals_contiguous = target_normals.Contiguous();
    std::pair<core::Tensor, core::Tensor> corres_contiguous;
    corres_contiguous.first = corres.first.Contiguous();
    corres_contiguous.second = corres.second.Contiguous();

    core::Device::DeviceType device_type = device.GetType();
    if (device_type == core::Device::DeviceType::CPU) {
        ComputePosePointToPlaneCPU(source_points, target_points, target_normals,
                                   corres_contiguous, pose, dtype, device);
    } else if (device_type == core::Device::DeviceType::CUDA) {
#ifdef BUILD_CUDA_MODULE
        ComputePosePointToPlaneCUDA(source_points, target_points,
                                    target_normals, corres_contiguous, pose,
                                    dtype, device);
#else
        utility::LogError("Not compiled with CUDA, but CUDA device is used.");
#endif
    } else {
        utility::LogError("Unimplemented device.");
    }
    return pose;
}

std::tuple<core::Tensor, core::Tensor> ComputeRtPointToPoint(
        const core::Tensor &source_points,
        const core::Tensor &target_points,
        const std::pair<core::Tensor, core::Tensor> &corres) {
    // Get dtype and device.
    core::Dtype dtype = source_points.GetDtype();
    core::Device device = source_points.GetDevice();
    // Checks.
    target_points.AssertDtype(dtype);
    target_points.AssertDevice(device);
    // [Output] Rotation and translation tensor.
    core::Tensor R, t;

    core::Device::DeviceType device_type = device.GetType();
    if (device_type == core::Device::DeviceType::CPU) {
        // Pointer to point cloud data - indexed according to correspondences.
        core::Tensor source_points_contiguous = source_points.Contiguous();
        core::Tensor target_points_contiguous = target_points.Contiguous();
        std::pair<core::Tensor, core::Tensor> corres_contiguous;
        corres_contiguous.first = corres.first.Contiguous();
        corres_contiguous.second = corres.second.Contiguous();

        ComputeRtPointToPointCPU(source_points, target_points,
                                 corres_contiguous, R, t, dtype, device);
    } else if (device_type == core::Device::DeviceType::CUDA) {
#ifdef BUILD_CUDA_MODULE
        int n = corres.first.GetLength();
        // TODO: Implement optimised CUDA reduction kernel.
        core::Tensor source_select = source_points.IndexGet({corres.first});
        core::Tensor target_select = target_points.IndexGet({corres.second});

        // https://ieeexplore.ieee.org/document/88573
        core::Tensor mean_s = source_select.Mean({0}, true);
        core::Tensor mean_t = target_select.Mean({0}, true);
        core::Tensor Sxy = (target_select - mean_t)
                                   .T()
                                   .Matmul(source_select - mean_s)
                                   .Div_(static_cast<float>(n));

        core::Tensor U, D, VT;
        std::tie(U, D, VT) = Sxy.SVD();
        core::Tensor S = core::Tensor::Eye(3, dtype, device);
        if (U.Det() * (VT.T()).Det() < 0) {
            S[-1][-1] = -1;
        }
        R = U.Matmul(S.Matmul(VT));
        t = mean_t.Reshape({-1}) - R.Matmul(mean_s.T()).Reshape({-1});
#else
        utility::LogError("Not compiled with CUDA, but CUDA device is used.");
#endif
    } else {
        utility::LogError("Unimplemented device.");
    }

    return std::make_tuple(R, t);
}

}  // namespace registration
}  // namespace kernel
}  // namespace pipelines
}  // namespace t
}  // namespace open3d