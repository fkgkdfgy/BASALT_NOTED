/**
BSD 3-Clause License

This file is part of the Basalt project.
https://gitlab.com/VladyslavUsenko/basalt.git

Copyright (c) 2019, Vladyslav Usenko and Nikolaus Demmel.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#include <basalt/vi_estimator/landmark_database.h>

#include <tbb/blocked_range.h>

namespace basalt {

class BundleAdjustmentBase {
 public:
  struct RelLinDataBase {
    // 这里存储的是[host frame id , target frame id ]的 vector
    // 这里存储的 host frame id 基本都是一样的
    // 这里存储的 target frame id 每个都不一样
    std::vector<std::pair<TimeCamId, TimeCamId>> order;

    // Residual = T_i^{c_t}*T_{c_t}^w*T_w^{c_h}*T_{c^h}^i
    // d Residual = d T_w^{c_h} 存储在 d_rel_d_h
    // d Residual = d T_w^{c_t} 存储在 d_rel_d_t
    // 顺序按照 order 中的顺序进行存储
    Eigen::aligned_vector<Sophus::Matrix6d> d_rel_d_h;
    Eigen::aligned_vector<Sophus::Matrix6d> d_rel_d_t;
  };


  // 这个数据结构存的是 target frame 相关的 Hessian 和 b
  struct FrameRelLinData {

    // 存储 pose 相关的 hessian 和 b
    // Hpp = d_res_dxi * d_res_d_xi.transpose()
    // bp  = d_res_dxi.transpose() * res 
    // 这里的 xi = T^ct_ch (也可能顺序记错了???)
    // 对应 刘浩敏 BA 讲解中的  V
    Sophus::Matrix6d Hpp;
    Sophus::Vector6d bp;


    // 按顺序存储 lm_id 这里存储的lm_id 顺序上直接对应 Hpl 的存储顺序
    std::vector<int> lm_id;
    // Hpl = d_res_d_xi * d_res_d_landmark
    // 对应 刘浩敏 BA 讲解中的  W 
    Eigen::aligned_vector<Eigen::Matrix<double, 6, 3>> Hpl;

    FrameRelLinData() {
      Hpp.setZero();
      bp.setZero();
    }

    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  };


  // 每一个 Host Frame 都会对应一个 RelLinData
  struct RelLinData : public RelLinDataBase {
    RelLinData(size_t num_keypoints, size_t num_rel_poses) {
      
      // num_keypoint 代表landmark 的数量
      // num_rel_pose 代表 target frame 的数量

      // 每一次优化的时候 landmark的数量是一致的，
      // target frame 的数量不一致
      Hll.reserve(num_keypoints);
      bl.reserve(num_keypoints);
      lm_to_obs.reserve(num_keypoints);

      Hpppl.reserve(num_rel_poses);
      order.reserve(num_rel_poses);

      d_rel_d_h.reserve(num_rel_poses);
      d_rel_d_t.reserve(num_rel_poses);

      error = 0;
    }

    void invert_keypoint_hessians() {
      for (auto& kv : Hll) {
        Eigen::Matrix3d Hll_inv;
        // 这里的操作就是 使用 Hx = I
        // 然后使用了 solveInPlace 直接保存进了 Hll_inv
        // x = H^{-1} 
        Hll_inv.setIdentity();
        kv.second.ldlt().solveInPlace(Hll_inv);
        kv.second = Hll_inv;
      }
    }
    
    // 在 invert_keypoint_hessian知乎，这里保存着 Hll的cholesky 的分解结果
    Eigen::aligned_unordered_map<int, Eigen::Matrix3d> Hll;
    Eigen::aligned_unordered_map<int, Eigen::Vector3d> bl;
    // [keypoint id,[Hpppl(对应 FraneRelLinData), 以及 frameRelLinData 当中这个keppoint 对应的Hpl的序号 ]]
    Eigen::aligned_unordered_map<int, std::vector<std::pair<size_t, size_t>>> 
      lm_to_obs;
    // 每一个target Frame 都对应一个FrameRelLinData
    Eigen::aligned_vector<FrameRelLinData> Hpppl;

    double error;
  };

  void computeError(double& error,
                    std::map<int, std::vector<std::pair<TimeCamId, double>>>*
                        outliers = nullptr,
                    double outlier_threshold = 0) const;

  void linearizeHelper(
      Eigen::aligned_vector<RelLinData>& rld_vec,
      const Eigen::aligned_map<
          TimeCamId,
          Eigen::aligned_map<TimeCamId,
                             Eigen::aligned_vector<KeypointObservation>>>&
          obs_to_lin,
      double& error) const;

  static void linearizeRel(const RelLinData& rld, Eigen::MatrixXd& H,
                           Eigen::VectorXd& b);

  void filterOutliers(double outlier_threshold, int min_num_obs);

  template <class CamT>
  static bool linearizePoint(
      const KeypointObservation& kpt_obs, const KeypointPosition& kpt_pos,
      const Eigen::Matrix4d& T_t_h, const CamT& cam, Eigen::Vector2d& res,
      Eigen::Matrix<double, 2, POSE_SIZE>* d_res_d_xi = nullptr,
      Eigen::Matrix<double, 2, 3>* d_res_d_p = nullptr,
      Eigen::Vector4d* proj = nullptr) {
    // Todo implement without jacobians
    Eigen::Matrix<double, 4, 2> Jup;
    Eigen::Vector4d p_h_3d;
    p_h_3d = StereographicParam<double>::unproject(kpt_pos.dir, &Jup);
    p_h_3d[3] = kpt_pos.id;

    Eigen::Vector4d p_t_3d = T_t_h * p_h_3d;

    Eigen::Matrix<double, 4, POSE_SIZE> d_point_d_xi;
    d_point_d_xi.topLeftCorner<3, 3>() =
        Eigen::Matrix3d::Identity() * kpt_pos.id;
    d_point_d_xi.topRightCorner<3, 3>() = -Sophus::SO3d::hat(p_t_3d.head<3>());
    d_point_d_xi.row(3).setZero();

    Eigen::Matrix<double, 2, 4> Jp;
    bool valid = cam.project(p_t_3d, res, &Jp);
    valid &= res.array().isFinite().all();

    if (!valid) {
      //      std::cerr << " Invalid projection! kpt_pos.dir "
      //                << kpt_pos.dir.transpose() << " kpt_pos.id " <<
      //                kpt_pos.id
      //                << " idx " << kpt_obs.kpt_id << std::endl;

      //      std::cerr << "T_t_h\n" << T_t_h << std::endl;
      //      std::cerr << "p_h_3d\n" << p_h_3d.transpose() << std::endl;
      //      std::cerr << "p_t_3d\n" << p_t_3d.transpose() << std::endl;

      return false;
    }

    if (proj) {
      proj->head<2>() = res;
      (*proj)[2] = p_t_3d[3] / p_t_3d.head<3>().norm();
    }
    res -= kpt_obs.pos;

    if (d_res_d_xi) {
      *d_res_d_xi = Jp * d_point_d_xi;
    }

    if (d_res_d_p) {
      Eigen::Matrix<double, 4, 3> Jpp;
      Jpp.setZero();
      Jpp.block<3, 2>(0, 0) = T_t_h.topLeftCorner<3, 4>() * Jup;
      Jpp.col(2) = T_t_h.col(3);

      *d_res_d_p = Jp * Jpp;
    }

    return true;
  }

  template <class CamT>
  inline static bool  linearizePoint(
      const KeypointObservation& kpt_obs, const KeypointPosition& kpt_pos,
      const CamT& cam, Eigen::Vector2d& res,
      Eigen::Matrix<double, 2, 3>* d_res_d_p = nullptr,
      Eigen::Vector4d* proj = nullptr) {
    // Todo implement without jacobians
    Eigen::Matrix<double, 4, 2> Jup;
    Eigen::Vector4d p_h_3d;
    p_h_3d = StereographicParam<double>::unproject(kpt_pos.dir, &Jup);

    Eigen::Matrix<double, 2, 4> Jp;
    bool valid = cam.project(p_h_3d, res, &Jp);
    valid &= res.array().isFinite().all();

    if (!valid) {
      //      std::cerr << " Invalid projection! kpt_pos.dir "
      //                << kpt_pos.dir.transpose() << " kpt_pos.id " <<
      //                kpt_pos.id
      //                << " idx " << kpt_obs.kpt_id << std::endl;
      //      std::cerr << "p_h_3d\n" << p_h_3d.transpose() << std::endl;

      return false;
    }

    if (proj) {
      proj->head<2>() = res;
      (*proj)[2] = kpt_pos.id;
    }
    res -= kpt_obs.pos;

    if (d_res_d_p) {
      Eigen::Matrix<double, 4, 3> Jpp;
      Jpp.setZero();
      Jpp.block<4, 2>(0, 0) = Jup;
      Jpp.col(2).setZero();

      *d_res_d_p = Jp * Jpp;
    }

    return true;
  }

  void updatePoints(const AbsOrderMap& aom, const RelLinData& rld,
                    const Eigen::VectorXd& inc);

  static Sophus::SE3d computeRelPose(const Sophus::SE3d& T_w_i_h,
                                     const Sophus::SE3d& T_w_i_t,
                                     const Sophus::SE3d& T_i_c_h,
                                     const Sophus::SE3d& T_i_c_t,
                                     Sophus::Matrix6d* d_rel_d_h = nullptr,
                                     Sophus::Matrix6d* d_rel_d_t = nullptr);

  void get_current_points(Eigen::aligned_vector<Eigen::Vector3d>& points,
                          std::vector<int>& ids) const;

  // Modifies abs_H and abs_b as a side effect.
  static void marginalizeHelper(Eigen::MatrixXd& abs_H, Eigen::VectorXd& abs_b,
                                const std::set<int>& idx_to_keep,
                                const std::set<int>& idx_to_marg,
                                Eigen::MatrixXd& marg_H,
                                Eigen::VectorXd& marg_b);

  void computeDelta(const AbsOrderMap& marg_order,
                    Eigen::VectorXd& delta) const;

  void linearizeMargPrior(const AbsOrderMap& marg_order,
                          const Eigen::MatrixXd& marg_H,
                          const Eigen::VectorXd& marg_b, const AbsOrderMap& aom,
                          Eigen::MatrixXd& abs_H, Eigen::VectorXd& abs_b,
                          double& marg_prior_error) const;

  void computeMargPriorError(const AbsOrderMap& marg_order,
                             const Eigen::MatrixXd& marg_H,
                             const Eigen::VectorXd& marg_b,
                             double& marg_prior_error) const;

  static Eigen::VectorXd checkNullspace(
      const Eigen::MatrixXd& marg_H, const Eigen::VectorXd& marg_b,
      const AbsOrderMap& marg_order,
      const Eigen::aligned_map<int64_t, PoseVelBiasStateWithLin<double>>&
          frame_states,
      const Eigen::aligned_map<int64_t, PoseStateWithLin<double>>& frame_poses);

  /// Triangulates the point and returns homogenous representation. First 3
  /// components - unit-length direction vector. Last component inverse
  /// distance.
  template <class Derived>
  static Eigen::Matrix<typename Derived::Scalar, 4, 1> triangulate(
      const Eigen::MatrixBase<Derived>& f0,
      const Eigen::MatrixBase<Derived>& f1,
      const Sophus::SE3<typename Derived::Scalar>& T_0_1) {
    EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Derived, 3);

    using Scalar = typename Derived::Scalar;
    using Vec4 = Eigen::Matrix<Scalar, 4, 1>;

    Eigen::Matrix<Scalar, 3, 4> P1, P2;
    P1.setIdentity();
    P2 = T_0_1.inverse().matrix3x4();

    Eigen::Matrix<Scalar, 4, 4> A(4, 4);
    A.row(0) = f0[0] * P1.row(2) - f0[2] * P1.row(0);
    A.row(1) = f0[1] * P1.row(2) - f0[2] * P1.row(1);
    A.row(2) = f1[0] * P2.row(2) - f1[2] * P2.row(0);
    A.row(3) = f1[1] * P2.row(2) - f1[2] * P2.row(1);

    Eigen::JacobiSVD<Eigen::Matrix<Scalar, 4, 4>> mySVD(A, Eigen::ComputeFullV);
    Vec4 worldPoint = mySVD.matrixV().col(3);
    worldPoint /= worldPoint.template head<3>().norm();

    // Enforce same direction of bearing vector and initial point
    if (f0.dot(worldPoint.template head<3>()) < 0) worldPoint *= -1;

    return worldPoint;
  }

  template <class AccumT>
  static void linearizeAbs(const Eigen::MatrixXd& rel_H,
                           const Eigen::VectorXd& rel_b,
                           const RelLinDataBase& rld, const AbsOrderMap& aom,
                           AccumT& accum) {
    // int asize = aom.total_size;

    //  BASALT_ASSERT(abs_H.cols() == asize);
    //  BASALT_ASSERT(abs_H.rows() == asize);
    //  BASALT_ASSERT(abs_b.rows() == asize);

    for (size_t i = 0; i < rld.order.size(); i++) {
      // Host Frame 的 TimeCamId
      const TimeCamId& tcid_h = rld.order[i].first;
      // Target Frame 的 TimeCamId
      const TimeCamId& tcid_ti = rld.order[i].second;

      // abs_h_idx 对应的是host frame    在hessian矩阵的idx
      // abs_ti_idx对应的是 target frame 在 hessian矩阵的idx
      int abs_h_idx = aom.abs_order_map.at(tcid_h.frame_id).first;
      int abs_ti_idx = aom.abs_order_map.at(tcid_ti.frame_id).first;

      // addB 和 addH 就是往 accum 内部填充数值
      // 最后就可以填充出一个abs index 下面的Hessian 和 b
      // 具体内容推导见 47. basalt/code_reading/schur_unwarp_solve.md
      accum.template addB<POSE_SIZE>(
          abs_h_idx, rld.d_rel_d_h[i].transpose() *
                         rel_b.segment<POSE_SIZE>(i * POSE_SIZE));
      accum.template addB<POSE_SIZE>(
          abs_ti_idx, rld.d_rel_d_t[i].transpose() *
                          rel_b.segment<POSE_SIZE>(i * POSE_SIZE));

      for (size_t j = 0; j < rld.order.size(); j++) {
        BASALT_ASSERT(rld.order[i].first == rld.order[j].first);

        const TimeCamId& tcid_tj = rld.order[j].second;

        int abs_tj_idx = aom.abs_order_map.at(tcid_tj.frame_id).first;

        if (tcid_h.frame_id == tcid_ti.frame_id ||
            tcid_h.frame_id == tcid_tj.frame_id)
          continue;

        accum.template addH<POSE_SIZE, POSE_SIZE>(
            abs_h_idx, abs_h_idx,
            rld.d_rel_d_h[i].transpose() *
                rel_H.block<POSE_SIZE, POSE_SIZE>(POSE_SIZE * i,
                                                  POSE_SIZE * j) *
                rld.d_rel_d_h[j]);

        accum.template addH<POSE_SIZE, POSE_SIZE>(
            abs_ti_idx, abs_h_idx,
            rld.d_rel_d_t[i].transpose() *
                rel_H.block<POSE_SIZE, POSE_SIZE>(POSE_SIZE * i,
                                                  POSE_SIZE * j) *
                rld.d_rel_d_h[j]);

        accum.template addH<POSE_SIZE, POSE_SIZE>(
            abs_h_idx, abs_tj_idx,
            rld.d_rel_d_h[i].transpose() *
                rel_H.block<POSE_SIZE, POSE_SIZE>(POSE_SIZE * i,
                                                  POSE_SIZE * j) *
                rld.d_rel_d_t[j]);

        accum.template addH<POSE_SIZE, POSE_SIZE>(
            abs_ti_idx, abs_tj_idx,
            rld.d_rel_d_t[i].transpose() *
                rel_H.block<POSE_SIZE, POSE_SIZE>(POSE_SIZE * i,
                                                  POSE_SIZE * j) *
                rld.d_rel_d_t[j]);
      }
    }
  }

  template <class AccumT>
  struct LinearizeAbsReduce {
    using RelLinDataIter = Eigen::aligned_vector<RelLinData>::iterator;

    LinearizeAbsReduce(AbsOrderMap& aom) : aom(aom) {
      accum.reset(aom.total_size);
    }

    LinearizeAbsReduce(const LinearizeAbsReduce& other, tbb::split)
        : aom(other.aom) {
      accum.reset(aom.total_size);
    }

    void operator()(const tbb::blocked_range<RelLinDataIter>& range) {
      for (RelLinData& rld : range) {
        // 对Hll使用cholesky 来求解逆矩阵 Hll^{-1}
        rld.invert_keypoint_hessians();

        Eigen::MatrixXd rel_H;
        Eigen::VectorXd rel_b;
        // 这里计算的H和b 只是计算了 T_
        linearizeRel(rld, rel_H, rel_b);
        // 放入 linearizeAbs 的 rel_H rel_b 是相对位姿作为优化参数的H 和 b 还没有拓展到 host frame 和 target frame 的位姿
        // accum 存储真正的H 和 b
        linearizeAbs(rel_H, rel_b, rld, aom, accum);
      }
    }

    void join(LinearizeAbsReduce& rhs) { accum.join(rhs.accum); }

    AbsOrderMap& aom;
    AccumT accum;
  };

  inline void backup() {
    for (auto& kv : frame_states) kv.second.backup();
    for (auto& kv : frame_poses) kv.second.backup();
    lmdb.backup();
  }

  inline void restore() {
    for (auto& kv : frame_states) kv.second.restore();
    for (auto& kv : frame_poses) kv.second.restore();
    lmdb.restore();
  }

  // protected:
  PoseStateWithLin<double> getPoseStateWithLin(int64_t t_ns) const {
    auto it = frame_poses.find(t_ns);
    if (it != frame_poses.end()) return it->second;

    auto it2 = frame_states.find(t_ns);
    if (it2 == frame_states.end()) {
      std::cerr << "Could not find pose " << t_ns << std::endl;
      std::abort();
    }

    return PoseStateWithLin(it2->second);
  }

  Eigen::aligned_map<int64_t, PoseVelBiasStateWithLin<double>> frame_states; // 存储所有的frame Pose Vel Bias 和State 数据
                                                                             // 基本帧
  Eigen::aligned_map<int64_t, PoseStateWithLin<double>> frame_poses;         // 存储所有滑窗内的 margin prior 的信息 存疑，估计是为了方便线性化结果的整合，所以和上面的东西分开放了。
                                                                             // 是从KF当中产生的
  // Point management
  LandmarkDatabase lmdb;

  double obs_std_dev;
  double huber_thresh;

  basalt::Calibration<double> calib;
};
}  // namespace basalt
