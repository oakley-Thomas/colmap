// Copyright (c) 2023, ETH Zurich and UNC Chapel Hill.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of ETH Zurich and UNC Chapel Hill nor the names of
//       its contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "colmap/estimators/covariance.h"

#include "colmap/estimators/bundle_adjustment.h"
#include "colmap/estimators/manifold.h"
#include "colmap/math/random.h"
#include "colmap/scene/reconstruction.h"
#include "colmap/scene/synthetic.h"

#include <gtest/gtest.h>

namespace colmap {
namespace {

void ExpectNearEigenMatrixXd(const Eigen::MatrixXd& mat1,
                             const Eigen::MatrixXd& mat2,
                             double tol) {
  ASSERT_EQ(mat1.rows(), mat2.rows());
  ASSERT_EQ(mat1.cols(), mat2.cols());
  for (int i = 0; i < mat1.rows(); ++i) {
    for (int j = 0; j < mat1.cols(); ++j) {
      ASSERT_NEAR(mat1(i, j), mat2(i, j), tol);
    }
  }
}

struct BACovarianceTestOptions {
  bool fixed_points = false;
  bool fixed_cam_poses = false;
  bool fixed_cam_intrinsics = false;
};

class ParameterizedBACovarianceTests
    : public ::testing::TestWithParam<
          std::pair<BACovarianceOptions, BACovarianceTestOptions>> {};

TEST_P(ParameterizedBACovarianceTests, CompareWithCeres) {
  SetPRNGSeed(42);

  const auto [options, test_options] = GetParam();

  const bool estimate_point_covs =
      options.params == BACovarianceOptions::Params::kOnlyPoints ||
      options.params == BACovarianceOptions::Params::kPosesAndPoints ||
      options.params == BACovarianceOptions::Params::kAll;
  const bool estimate_pose_covs =
      options.params == BACovarianceOptions::Params::kOnlyPoses ||
      options.params == BACovarianceOptions::Params::kPosesAndPoints ||
      options.params == BACovarianceOptions::Params::kAll;
  const bool estimate_other_covs =
      options.params == BACovarianceOptions::Params::kAll;

  Reconstruction reconstruction;
  SyntheticDatasetOptions synthetic_dataset_options;
  synthetic_dataset_options.num_cameras = 3;
  synthetic_dataset_options.num_images = 8;
  synthetic_dataset_options.num_points3D = 1000;
  synthetic_dataset_options.point2D_stddev = 0.01;
  SynthesizeDataset(synthetic_dataset_options, &reconstruction);

  BundleAdjustmentConfig config;
  for (const auto& [image_id, image] : reconstruction.Images()) {
    config.AddImage(image_id);
    if (test_options.fixed_cam_poses) {
      config.SetConstantCamPose(image_id);
    }
    if (test_options.fixed_cam_intrinsics) {
      config.SetConstantCamIntrinsics(image.CameraId());
    }
  }

  // Fix the Gauge by always setting at least 3 points as constant.
  CHECK_GT(reconstruction.NumPoints3D(), 3);
  int num_constant_points = 0;
  for (const auto& [point3D_id, _] : reconstruction.Points3D()) {
    if (++num_constant_points <= 3 || test_options.fixed_points) {
      config.AddConstantPoint(point3D_id);
    }
  }

  auto bundle_adjuster = CreateDefaultBundleAdjuster(
      BundleAdjustmentOptions(), std::move(config), reconstruction);
  auto problem = bundle_adjuster->Problem();

  const std::optional<BACovariance> ba_cov =
      EstimateBACovariance(options, reconstruction, *bundle_adjuster);
  ASSERT_TRUE(ba_cov.has_value());

  const std::vector<detail::PointParam> points =
      detail::GetPointParams(reconstruction, *problem);
  const std::vector<detail::PoseParam> poses =
      detail::GetPoseParams(reconstruction, *problem);
  const std::vector<const double*> others =
      GetOtherParams(*problem, poses, points);

  if (!test_options.fixed_cam_poses && estimate_pose_covs) {
    LOG(INFO) << "Comparing pose covariances";

    std::vector<std::pair<const double*, const double*>> cov_param_pairs;
    for (const auto& pose : poses) {
      if (pose.qvec != nullptr) {
        cov_param_pairs.emplace_back(pose.qvec, pose.qvec);
      }
      if (pose.tvec != nullptr) {
        cov_param_pairs.emplace_back(pose.tvec, pose.tvec);
      }
      if (pose.qvec != nullptr && pose.tvec != nullptr) {
        cov_param_pairs.emplace_back(pose.qvec, pose.tvec);
      }
    }

    ceres::Covariance::Options ceres_cov_options;
    ceres::Covariance ceres_cov_computer(ceres_cov_options);
    ASSERT_TRUE(ceres_cov_computer.Compute(cov_param_pairs, problem.get()));

    for (const auto& pose : poses) {
      int tangent_size = 0;
      std::vector<const double*> param_blocks;
      if (pose.qvec != nullptr) {
        tangent_size += ParameterBlockTangentSize(*problem, pose.qvec);
        param_blocks.push_back(pose.qvec);
      }
      if (pose.tvec != nullptr) {
        tangent_size += ParameterBlockTangentSize(*problem, pose.tvec);
        param_blocks.push_back(pose.tvec);
      }

      Eigen::MatrixXd ceres_cov(tangent_size, tangent_size);
      ceres_cov_computer.GetCovarianceMatrixInTangentSpace(param_blocks,
                                                           ceres_cov.data());

      const std::optional<Eigen::MatrixXd> cov =
          ba_cov->GetCamFromWorldCov(pose.image_id);
      ASSERT_TRUE(cov.has_value());
      ExpectNearEigenMatrixXd(ceres_cov, *cov, /*tol=*/1e-8);
    }

    ASSERT_FALSE(ba_cov->GetCamFromWorldCov(kInvalidImageId).has_value());
  }

  if (!test_options.fixed_cam_intrinsics && estimate_other_covs) {
    LOG(INFO) << "Comparing other covariances";

    std::vector<std::pair<const double*, const double*>> cov_param_pairs;
    for (const double* other : others) {
      if (other != nullptr) {
        cov_param_pairs.emplace_back(other, other);
      }
    }

    ceres::Covariance::Options ceres_cov_options;
    ceres::Covariance ceres_cov_computer(ceres_cov_options);
    ASSERT_TRUE(ceres_cov_computer.Compute(cov_param_pairs, problem.get()));

    for (const double* other : others) {
      const int tangent_size = ParameterBlockTangentSize(*problem, other);

      Eigen::MatrixXd ceres_cov(tangent_size, tangent_size);
      ceres_cov_computer.GetCovarianceMatrixInTangentSpace({other},
                                                           ceres_cov.data());

      const std::optional<Eigen::MatrixXd> cov =
          ba_cov->GetOtherParamsCov(other);
      ASSERT_TRUE(cov.has_value());
      ExpectNearEigenMatrixXd(ceres_cov, *cov, /*tol=*/1e-8);
    }

    ASSERT_FALSE(ba_cov->GetOtherParamsCov(nullptr).has_value());
  }

  if (!test_options.fixed_points && estimate_point_covs) {
    LOG(INFO) << "Comparing point covariances";

    for (const auto& pose : poses) {
      if (pose.qvec != nullptr) {
        problem->SetParameterBlockConstant(pose.qvec);
      }
      if (pose.tvec != nullptr) {
        problem->SetParameterBlockConstant(pose.tvec);
      }
    }
    for (const double* other : others) {
      if (other != nullptr) {
        problem->SetParameterBlockConstant(other);
      }
    }

    std::vector<std::pair<const double*, const double*>> cov_param_pairs;
    for (const auto& point : points) {
      if (point.xyz != nullptr) {
        cov_param_pairs.emplace_back(point.xyz, point.xyz);
      }
    }

    ceres::Covariance::Options ceres_cov_options;
    ceres::Covariance ceres_cov_computer(ceres_cov_options);
    ASSERT_TRUE(ceres_cov_computer.Compute(cov_param_pairs, problem.get()));

    for (const auto& point : points) {
      const int tangent_size = ParameterBlockTangentSize(*problem, point.xyz);

      Eigen::MatrixXd ceres_cov(tangent_size, tangent_size);
      ceres_cov_computer.GetCovarianceMatrixInTangentSpace({point.xyz},
                                                           ceres_cov.data());

      const std::optional<Eigen::Matrix3d> cov =
          ba_cov->GetPointCov(point.point3D_id);
      ASSERT_TRUE(cov.has_value());
      ExpectNearEigenMatrixXd(ceres_cov, *cov, /*tol=*/1e-8);
    }

    ASSERT_FALSE(ba_cov->GetPointCov(kInvalidPoint3DId).has_value());
  }
}

INSTANTIATE_TEST_SUITE_P(
    BACovarianceTests,
    ParameterizedBACovarianceTests,
    ::testing::Values(
        std::make_pair(BACovarianceOptions(), BACovarianceTestOptions()),
        []() {
          BACovarianceOptions options;
          options.params = BACovarianceOptions::Params::kAll;
          BACovarianceTestOptions test_options;
          test_options.fixed_points = true;
          return std::make_pair(options, test_options);
        }(),
        []() {
          BACovarianceOptions options;
          options.params = BACovarianceOptions::Params::kAll;
          BACovarianceTestOptions test_options;
          test_options.fixed_cam_intrinsics = true;
          return std::make_pair(options, test_options);
        }(),
        []() {
          BACovarianceOptions options;
          options.params = BACovarianceOptions::Params::kAll;
          BACovarianceTestOptions test_options;
          test_options.fixed_cam_poses = true;
          return std::make_pair(options, test_options);
        }(),
        []() {
          BACovarianceOptions options;
          options.params = BACovarianceOptions::Params::kOnlyPoints;
          BACovarianceTestOptions test_options;
          return std::make_pair(options, test_options);
        }(),
        []() {
          BACovarianceOptions options;
          options.params = BACovarianceOptions::Params::kOnlyPoses;
          BACovarianceTestOptions test_options;
          return std::make_pair(options, test_options);
        }(),
        []() {
          BACovarianceOptions options;
          options.params = BACovarianceOptions::Params::kPosesAndPoints;
          BACovarianceTestOptions test_options;
          return std::make_pair(options, test_options);
        }()));

TEST(EstimatePointCovariances, RankDeficientPoints) {
  Reconstruction reconstruction;
  SyntheticDatasetOptions synthetic_dataset_options;
  synthetic_dataset_options.num_cameras = 1;
  synthetic_dataset_options.num_images = 2;
  synthetic_dataset_options.num_points3D = 10;
  synthetic_dataset_options.point2D_stddev = 0;
  SynthesizeDataset(synthetic_dataset_options, &reconstruction);

  const image_t image_id1 = reconstruction.Images().begin()->first;
  const image_t image_id2 = std::next(reconstruction.Images().begin())->first;
  Image& image1 = reconstruction.Image(image_id1);
  image1.SetCamFromWorld(Rigid3d());
  Image& image2 = reconstruction.Image(image_id2);
  image2.SetCamFromWorld(
      Rigid3d(Eigen::Quaterniond::Identity(), Eigen::Vector3d(0, 1, 0)));

  double distance = 1;
  double x = 0.1;
  double y = 0;
  for (const point3D_t point3D_id : reconstruction.Point3DIds()) {
    Point3D& point3D = reconstruction.Point3D(point3D_id);
    point3D.xyz = Eigen::AngleAxisd(EIGEN_PI / 2, Eigen::Vector3d::UnitZ()) *
                  Eigen::Vector3d(x, y, distance);
    x = point3D.xyz.x();
    y = point3D.xyz.y();
    distance *= 10;
    for (const auto& track_el : point3D.track.Elements()) {
      reconstruction.Image(track_el.image_id).Point2D(track_el.point2D_idx).xy =
          image1.ProjectPoint(point3D.xyz).second;
    }
  }

  BundleAdjustmentConfig config;
  config.AddImage(image_id1);
  config.AddImage(image_id2);
  config.SetConstantCamPose(image_id1);
  config.SetConstantCamPositions(image_id2, {0});
  config.SetConstantCamIntrinsics(image1.CameraId());
  config.SetConstantCamIntrinsics(image2.CameraId());

  auto bundle_adjuster = CreateDefaultBundleAdjuster(
      BundleAdjustmentOptions(), std::move(config), reconstruction);

  BACovarianceOptions options;
  ASSERT_TRUE(EstimateBACovariance(options, reconstruction, *bundle_adjuster)
                  .has_value());
  options.damping = 0;
  ASSERT_FALSE(EstimateBACovariance(options, reconstruction, *bundle_adjuster)
                   .has_value());
}

}  // namespace
}  // namespace colmap
