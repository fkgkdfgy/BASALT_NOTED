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

#include <basalt/utils/assert.h>
#include <basalt/vi_estimator/keypoint_vio.h>

#include <basalt/optimization/accumulator.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <chrono>

namespace basalt {

KeypointVioEstimator::KeypointVioEstimator(
    const Eigen::Vector3d& g, const basalt::Calibration<double>& calib,
    const VioConfig& config)
    : take_kf(true),
      frames_after_kf(0),
      g(g),
      initialized(false),
      config(config),
      lambda(config.vio_lm_lambda_min),
      min_lambda(config.vio_lm_lambda_min),
      max_lambda(config.vio_lm_lambda_max),
      lambda_vee(2) {
  this->obs_std_dev = config.vio_obs_std_dev;
  this->huber_thresh = config.vio_obs_huber_thresh;
  this->calib = calib;

  // Setup marginalization
  marg_H.setZero(POSE_VEL_BIAS_SIZE, POSE_VEL_BIAS_SIZE);
  marg_b.setZero(POSE_VEL_BIAS_SIZE);

  // prior on position
  marg_H.diagonal().head<3>().setConstant(config.vio_init_pose_weight);
  // prior on yaw
  marg_H(5, 5) = config.vio_init_pose_weight;

  // small prior to avoid jumps in bias
  marg_H.diagonal().segment<3>(9).array() = config.vio_init_ba_weight;
  marg_H.diagonal().segment<3>(12).array() = config.vio_init_bg_weight;

  std::cout << "marg_H\n" << marg_H << std::endl;

  gyro_bias_weight = calib.gyro_bias_std.array().square().inverse();
  accel_bias_weight = calib.accel_bias_std.array().square().inverse();

  max_states = config.vio_max_states;
  max_kfs = config.vio_max_kfs;

  opt_started = false;

  vision_data_queue.set_capacity(10);
  imu_data_queue.set_capacity(300);
}

void KeypointVioEstimator::initialize(int64_t t_ns, const Sophus::SE3d& T_w_i,
                                      const Eigen::Vector3d& vel_w_i,
                                      const Eigen::Vector3d& bg,
                                      const Eigen::Vector3d& ba) {
  initialized = true;
  T_w_i_init = T_w_i;

  last_state_t_ns = t_ns;
  imu_meas[t_ns] = IntegratedImuMeasurement(t_ns, bg, ba);
  frame_states[t_ns] =
      PoseVelBiasStateWithLin<double>(t_ns, T_w_i, vel_w_i, bg, ba, true);

  marg_order.abs_order_map[t_ns] = std::make_pair(0, POSE_VEL_BIAS_SIZE);
  marg_order.total_size = POSE_VEL_BIAS_SIZE;
  marg_order.items = 1;

  initialize(bg, ba);
}

void KeypointVioEstimator::initialize(const Eigen::Vector3d& bg,
                                      const Eigen::Vector3d& ba) {
  auto proc_func = [&, bg, ba] {
    OpticalFlowResult::Ptr prev_frame, curr_frame;
    IntegratedImuMeasurement<double>::Ptr meas;

    const Eigen::Vector3d accel_cov =
        calib.dicrete_time_accel_noise_std().array().square();
    const Eigen::Vector3d gyro_cov =
        calib.dicrete_time_gyro_noise_std().array().square();

    ImuData<double>::Ptr data;
    imu_data_queue.pop(data);
    data->accel = calib.calib_accel_bias.getCalibrated(data->accel);
    data->gyro = calib.calib_gyro_bias.getCalibrated(data->gyro);

    while (true) {
      vision_data_queue.pop(curr_frame);

      if (config.vio_enforce_realtime) {
        // drop current frame if another frame is already in the queue.
        while (!vision_data_queue.empty()) vision_data_queue.pop(curr_frame);
      }

      if (!curr_frame.get()) {
        break;
      }

      // Correct camera time offset
      // curr_frame->t_ns += calib.cam_time_offset_ns;

      if (!initialized) {
        while (data->t_ns < curr_frame->t_ns) {
          imu_data_queue.pop(data);
          if (!data.get()) break;
          data->accel = calib.calib_accel_bias.getCalibrated(data->accel);
          data->gyro = calib.calib_gyro_bias.getCalibrated(data->gyro);
          // std::cout << "Skipping IMU data.." << std::endl;
        }

        Eigen::Vector3d vel_w_i_init;
        vel_w_i_init.setZero();

        T_w_i_init.setQuaternion(Eigen::Quaterniond::FromTwoVectors(
            data->accel, Eigen::Vector3d::UnitZ()));

        last_state_t_ns = curr_frame->t_ns;
        imu_meas[last_state_t_ns] =
            IntegratedImuMeasurement(last_state_t_ns, bg, ba);
        frame_states[last_state_t_ns] = PoseVelBiasStateWithLin<double>(
            last_state_t_ns, T_w_i_init, vel_w_i_init, bg, ba, true);

        marg_order.abs_order_map[last_state_t_ns] =
            std::make_pair(0, POSE_VEL_BIAS_SIZE);
        marg_order.total_size = POSE_VEL_BIAS_SIZE;
        marg_order.items = 1;

        std::cout << "Setting up filter: t_ns " << last_state_t_ns << std::endl;
        std::cout << "T_w_i\n" << T_w_i_init.matrix() << std::endl;
        std::cout << "vel_w_i " << vel_w_i_init.transpose() << std::endl;

        initialized = true;
      }

      if (prev_frame) {
        // preintegrate measurements

        auto last_state = frame_states.at(last_state_t_ns);

        meas.reset(new IntegratedImuMeasurement<double>(
            prev_frame->t_ns, last_state.getState().bias_gyro,
            last_state.getState().bias_accel));

        while (data->t_ns <= prev_frame->t_ns) {
          imu_data_queue.pop(data);
          if (!data.get()) break;
          data->accel = calib.calib_accel_bias.getCalibrated(data->accel);
          data->gyro = calib.calib_gyro_bias.getCalibrated(data->gyro);
        }

        while (data->t_ns <= curr_frame->t_ns) {
          meas->integrate(*data, accel_cov, gyro_cov);
          imu_data_queue.pop(data);
          if (!data.get()) break;
          data->accel = calib.calib_accel_bias.getCalibrated(data->accel);
          data->gyro = calib.calib_gyro_bias.getCalibrated(data->gyro);
        }

        if (meas->get_start_t_ns() + meas->get_dt_ns() < curr_frame->t_ns) {
          if (!data.get()) break;
          int64_t tmp = data->t_ns;
          data->t_ns = curr_frame->t_ns;
          meas->integrate(*data, accel_cov, gyro_cov);
          data->t_ns = tmp;
        }
      }

      measure(curr_frame, meas);
      prev_frame = curr_frame;
    }

    if (out_vis_queue) out_vis_queue->push(nullptr);
    if (out_marg_queue) out_marg_queue->push(nullptr);
    if (out_state_queue) out_state_queue->push(nullptr);

    finished = true;

    std::cout << "Finished VIOFilter " << std::endl;
  };

  processing_thread.reset(new std::thread(proc_func));
}

void KeypointVioEstimator::addIMUToQueue(const ImuData<double>::Ptr& data) {
  imu_data_queue.emplace(data);
}

void KeypointVioEstimator::addVisionToQueue(
    const OpticalFlowResult::Ptr& data) {
  vision_data_queue.push(data);
}

bool KeypointVioEstimator::measure(
    const OpticalFlowResult::Ptr& opt_flow_meas,
    const IntegratedImuMeasurement<double>::Ptr& meas) {
  // IMU 存在数值
  if (meas.get()) {

    BASALT_ASSERT(frame_states[last_state_t_ns].getState().t_ns ==
                  meas->get_start_t_ns());
    BASALT_ASSERT(opt_flow_meas->t_ns ==
                  meas->get_dt_ns() + meas->get_start_t_ns());

    PoseVelBiasState next_state = frame_states.at(last_state_t_ns).getState();
    
    // 使用imu 进行位姿预测
    meas->predictState(frame_states.at(last_state_t_ns).getState(), g,
                       next_state);

    last_state_t_ns = opt_flow_meas->t_ns;
    next_state.t_ns = opt_flow_meas->t_ns;

    frame_states[last_state_t_ns] = next_state;

    imu_meas[meas->get_start_t_ns()] = *meas;
  }

  // save results
  prev_opt_flow_res[opt_flow_meas->t_ns] = opt_flow_meas;

  // Make new residual for existing keypoints
  int connected0 = 0;
  std::map<int64_t, int> num_points_connected;
  std::unordered_set<int> unconnected_obs0;

  // kv_obs .first  存储 keypoint Id  .second 存储SE2 的数值，也就是存储了uv 和 theta
  // connected0 用于表示有多少特征点被当前帧的左目观测到了 [host frame ,int ] 代表当前的 optical_flow_result 
  // 内部观测到的点有多少是之间host frame 生成的点 eg. [1111,6] 有6个点是1111 这个 host frame 生成的
  // unconnected0 用于存储landmark data base 里面没有，但是当前帧的左目观测到(新创建的)的keypoint id

  // for 历遍每一个相机 (opt_flow_meas 的observations的数量和相机数量相同)
  //     for 历遍observation 里面的每一个观测
  //         通过 landmark data base 的查找，找到了，就给lmdb的obs 添加观测，如果还是左目上的观测就对connected0++
  //         如果没有找到对应的keypoint，就把这个观测的keyPointID  存储到 unconnected0
  //        

  for (size_t i = 0; i < opt_flow_meas->observations.size(); i++) {
    TimeCamId tcid_target(opt_flow_meas->t_ns, i);

    for (const auto& kv_obs : opt_flow_meas->observations[i]) {
      int kpt_id = kv_obs.first;

      if (lmdb.landmarkExists(kpt_id)) {
        const TimeCamId& tcid_host = lmdb.getLandmark(kpt_id).kf_id;

        KeypointObservation kobs;
        kobs.kpt_id = kpt_id;
        kobs.pos = kv_obs.second.translation().cast<double>();

        lmdb.addObservation(tcid_target, kobs);
        // obs[tcid_host][tcid_target].push_back(kobs);

        if (num_points_connected.count(tcid_host.frame_id) == 0) {
          num_points_connected[tcid_host.frame_id] = 0;
        }
        num_points_connected[tcid_host.frame_id]++;

        if (i == 0) connected0++;
      } 
      else 
      {
        if (i == 0) {
          unconnected_obs0.emplace(kpt_id);
        }
      }
    }
  }


  // 通过当前帧和整个lmdb 之间联系关系 判断要不要生成关键帧
  // condition1: 观测到的lmdb中的点比较少，联系已经比较弱了
  // condition2: 从上一次添加关键帧，已经很久没有关键帧了
  if (double(connected0) / (connected0 + unconnected_obs0.size()) <
          config.vio_new_kf_keypoints_thresh &&
      frames_after_kf > config.vio_min_frames_after_kf)
    take_kf = true;

  if (config.vio_debug) {
    std::cout << "connected0 " << connected0 << " unconnected0 "
              << unconnected_obs0.size() << std::endl;
  }

  // kv  .first 存储的是这个光流结果的时间戳 .second 光流结果
  // kp_obs [frameCameraid , keypoint observation] 一个新的特征，进行向前搜索中，
  //                                               如果存在其他的Frame 也观测到了这个特征，
  //                                               就把他的FrameCameraId 和他对这个点的观测放入 kp_obs
  // 如果创建关键帧:
  //  for keypointid(代码中是 lm_id 实际上两者是一个东西) in unconnected0   历遍上面存储在unconnected0 中的所有新keypointid
  //      1. for kv in prev_opt_flow_res                                  历遍之前所有的 光流跟踪结果， 看看是否有遗珠
  //             for 历遍 kv.second -> observations                        
  //                 在其中寻找是否有对应 keypointid 的观测，
  //                 如果有的就保存在 kp_obs
  //                 保存的是之帧的 FrameCameraID 和 之前帧对于特征的 uv 观测
  //        ? 这一步还有一些疑问，按理来说，光流跟丢了不可能出现隔帧观察到的情况，除非前端有 TrackLocalMap，这一个需要之后阅读前端进一步确认
  //          并且这里没有break 是不是说明 kp_obs 中的值也在被不断替换?(已解决，不会，原因看下面的解答)
  //          答: 不会因为每个帧只会出现一次，而且每个帧对一个点不可能出现多次观测
  //      2.  三角化
  //      3.  一旦三角化成功，就把当前帧的 FrameCameraID 作为 HostFrameID 给landmark Data Base 进行landmark的创建
  //      4.  一旦三角化成功，所有的对应观测添加到lmdb 当中
  //
  if (take_kf) {
    // Triangulate new points from stereo and make keyframe for camera 0
    take_kf = false;
    frames_after_kf = 0;
    kf_ids.emplace(last_state_t_ns);

    TimeCamId tcidl(opt_flow_meas->t_ns, 0);

    int num_points_added = 0;
    for (int lm_id : unconnected_obs0) {
        // Find all observations
        std::map<TimeCamId, KeypointObservation> kp_obs;

        for (const auto& kv : prev_opt_flow_res) {
          for (size_t k = 0; k < kv.second->observations.size(); k++) {
            auto it = kv.second->observations[k].find(lm_id);
            if (it != kv.second->observations[k].end()) {
              TimeCamId tcido(kv.first, k);

              KeypointObservation kobs;
              kobs.kpt_id = lm_id;
              kobs.pos = it->second.translation().cast<double>();

              // obs[tcidl][tcido].push_back(kobs);
              kp_obs[tcido] = kobs;
            }
          }
        }

        // triangulate
        bool valid_kp = false;
        const double min_triang_distance2 =
            config.vio_min_triangulation_dist * config.vio_min_triangulation_dist;
        for (const auto& kv_obs : kp_obs) {
          if (valid_kp) break;
          TimeCamId tcido = kv_obs.first;

          const Eigen::Vector2d p0 = opt_flow_meas->observations.at(0)
                                        .at(lm_id)
                                        .translation()
                                        .cast<double>();
          const Eigen::Vector2d p1 = prev_opt_flow_res[tcido.frame_id]
                                        ->observations[tcido.cam_id]
                                        .at(lm_id)
                                        .translation()
                                        .cast<double>();

          Eigen::Vector4d p0_3d, p1_3d;
          bool valid1 = calib.intrinsics[0].unproject(p0, p0_3d);
          bool valid2 = calib.intrinsics[tcido.cam_id].unproject(p1, p1_3d);
          if (!valid1 || !valid2) continue;

          Sophus::SE3d T_i0_i1 =
              getPoseStateWithLin(tcidl.frame_id).getPose().inverse() *
              getPoseStateWithLin(tcido.frame_id).getPose();
          Sophus::SE3d T_0_1 =
              calib.T_i_c[0].inverse() * T_i0_i1 * calib.T_i_c[tcido.cam_id];

          if (T_0_1.translation().squaredNorm() < min_triang_distance2) continue;

          Eigen::Vector4d p0_triangulated =
              triangulate(p0_3d.head<3>(), p1_3d.head<3>(), T_0_1);

          if (p0_triangulated.array().isFinite().all() &&
              p0_triangulated[3] > 0 && p0_triangulated[3] < 3.0) {
            KeypointPosition kpt_pos;
            kpt_pos.kf_id = tcidl;
            kpt_pos.dir = StereographicParam<double>::project(p0_triangulated);
            kpt_pos.id = p0_triangulated[3];
            lmdb.addLandmark(lm_id, kpt_pos);

            num_points_added++;
            valid_kp = true;
          }
        }

      if (valid_kp) {
        for (const auto& kv_obs : kp_obs) {
          lmdb.addObservation(kv_obs.first, kv_obs.second);
        }
      }
    }

    num_points_kf[opt_flow_meas->t_ns] = num_points_added;
  } else {
    frames_after_kf++;
  }

  // 进行优化
  optimize();

  // 进行边缘化的操作
  marginalize(num_points_connected);

  if (out_state_queue) {
    PoseVelBiasStateWithLin p = frame_states.at(last_state_t_ns);

    PoseVelBiasState<double>::Ptr data(new PoseVelBiasState(p.getState()));

    out_state_queue->push(data);
  }

  if (out_vis_queue) {
    VioVisualizationData::Ptr data(new VioVisualizationData);

    data->t_ns = last_state_t_ns;

    for (const auto& kv : frame_states) {
      data->states.emplace_back(kv.second.getState().T_w_i);
    }

    for (const auto& kv : frame_poses) {
      data->frames.emplace_back(kv.second.getPose());
    }

    get_current_points(data->points, data->point_ids);

    data->projections.resize(opt_flow_meas->observations.size());
    computeProjections(data->projections);

    data->opt_flow_res = prev_opt_flow_res[last_state_t_ns];

    out_vis_queue->push(data);
  }

  last_processed_t_ns = last_state_t_ns;

  return true;
}

void KeypointVioEstimator::checkMargNullspace() const {
  checkNullspace(marg_H, marg_b, marg_order, frame_states, frame_poses);
}

// 边缘化操作
// 边缘化的判断
void KeypointVioEstimator::marginalize(
    const std::map<int64_t, int>& num_points_connected) {
  if (!opt_started) return;

  // ? 所以 frame_poses 是和关键帧有关吗？？
  // frame_poses 或者 frame_states 大于一定数量 进行边缘化
  
  if (frame_poses.size() > max_kfs || frame_states.size() >= max_states) {
    // Marginalize

    // 从旧往新数 state_to_remove 代表有多少个老的state 需要被marg 掉
    const int states_to_remove = frame_states.size() - max_states + 1;

    auto it = frame_states.cbegin();
    for (int i = 0; i < states_to_remove; i++) it++;
    
    // 保存需要marg的state的时间戳
    int64_t last_state_to_marg = it->first;

    // 用于保存frame_poses的矩阵信息
    AbsOrderMap aom;

    // remove all frame_poses that are not kfs
    std::set<int64_t> poses_to_marg;
      // aom 添加index 和 dimension 信息

    for (const auto& kv : frame_poses) {
      aom.abs_order_map[kv.first] = std::make_pair(aom.total_size, POSE_SIZE);

      if (kf_ids.count(kv.first) == 0) poses_to_marg.emplace(kv.first);

      // Check that we have the same order as marginalization
      BASALT_ASSERT(marg_order.abs_order_map.at(kv.first) ==
                    aom.abs_order_map.at(kv.first));

      aom.total_size += POSE_SIZE;
      aom.items++;
    }

    // ? state to marg vel_bias ？？？ 所以这frame_poses 是因为frame states marg掉 vel 和bias 的结果？
    // 
    std::set<int64_t> states_to_marg_vel_bias;
    std::set<int64_t> states_to_marg_all;
    // marg 的逻辑，如果这一帧是KF 就只是 marg 掉 vel 和bias
    //             如果不是KF直接全部marg掉
    for (const auto& kv : frame_states) {
      if (kv.first > last_state_to_marg) break;

      if (kv.first != last_state_to_marg) {
        if (kf_ids.count(kv.first) > 0) {
          states_to_marg_vel_bias.emplace(kv.first);
        } else {
          states_to_marg_all.emplace(kv.first);
        }
      }

      // aom 添加index 和 dimension 信息
      aom.abs_order_map[kv.first] =
          std::make_pair(aom.total_size, POSE_VEL_BIAS_SIZE);

      // Check that we have the same order as marginalization
      if (aom.items < marg_order.abs_order_map.size())
        BASALT_ASSERT(marg_order.abs_order_map.at(kv.first) ==
                      aom.abs_order_map.at(kv.first));

      aom.total_size += POSE_VEL_BIAS_SIZE;
      aom.items++;
    }

    auto kf_ids_all = kf_ids;
    std::set<int64_t> kfs_to_marg;

    // 必须满足一下两个条件才进行marg:
    // 1. 有 states 是kf 需要被marg 掉 vel 和bias
    // 2. kf_ids 的数量大于max_kfs
    while (kf_ids.size() > max_kfs && !states_to_marg_vel_bias.empty()) {
      int64_t id_to_marg = -1;

      {
        // 存储所有的KF id
        std::vector<int64_t> ids;
        for (int64_t id : kf_ids) {
          ids.push_back(id);
        }

        // 历遍一下如果当前的帧的观测 只占了host frame 创建时，添加landmark数量的0.05 就把这个 这个id 标记为 marg
        for (size_t i = 0; i < ids.size() - 2; i++) {
          if (num_points_connected.count(ids[i]) == 0 ||
              (num_points_connected.at(ids[i]) / num_points_kf.at(ids[i]) <
               0.05)) {
            id_to_marg = ids[i];
            break;
          }
        }
      }

      if (id_to_marg < 0) {
        std::vector<int64_t> ids;
        for (int64_t id : kf_ids) {
          ids.push_back(id);
        }

        int64_t last_kf = *kf_ids.crbegin();
        double min_score = std::numeric_limits<double>::max();
        int64_t min_score_id = -1;

        // ? 这个类似于DSO的判断条件有点迷
        // 第一部分因该是保证前后不会太大
        // 第二部分应该是保证和最新的state 位移没有太大
        for (size_t i = 0; i < ids.size() - 2; i++) {
          double denom = 0;
          for (size_t j = 0; j < ids.size() - 2; j++) {
            denom += 1 / ((frame_poses.at(ids[i]).getPose().translation() -
                           frame_poses.at(ids[j]).getPose().translation())
                              .norm() +
                          1e-5);
          }

          double score =
              std::sqrt(
                  (frame_poses.at(ids[i]).getPose().translation() -
                   frame_states.at(last_kf).getState().T_w_i.translation())
                      .norm()) *
              denom;

          if (score < min_score) {
            min_score_id = ids[i];
            min_score = score;
          }
        }

        id_to_marg = min_score_id;
      }

      // 添加需要marg 的id
      // ? 为什么这里有里那个to_marg
      kfs_to_marg.emplace(id_to_marg);
      poses_to_marg.emplace(id_to_marg);

      kf_ids.erase(id_to_marg);
    }

    //    std::cout << "marg order" << std::endl;
    //    aom.print_order();

    //    std::cout << "marg prior order" << std::endl;
    //    marg_order.print_order();

    if (config.vio_debug) {
      std::cout << "states_to_remove " << states_to_remove << std::endl;
      std::cout << "poses_to_marg.size() " << poses_to_marg.size() << std::endl;
      std::cout << "states_to_marg.size() " << states_to_marg_all.size()
                << std::endl;
      std::cout << "state_to_marg_vel_bias.size() "
                << states_to_marg_vel_bias.size() << std::endl;
      std::cout << "kfs_to_marg.size() " << kfs_to_marg.size() << std::endl;
    }

    // 边缘化正式开始


    size_t asize = aom.total_size;

    double marg_prior_error;
    double imu_error, bg_error, ba_error;

    DenseAccumulator accum;
    accum.reset(asize);

    {
      // Linearize points

      Eigen::aligned_map<
          TimeCamId, Eigen::aligned_map<
                         TimeCamId, Eigen::aligned_vector<KeypointObservation>>>
          obs_to_lin;

      // 将需要marg 掉的观测放入到 obs_to_line 中
      for (auto it = lmdb.getObservations().cbegin();
           it != lmdb.getObservations().cend();) {
        if (kfs_to_marg.count(it->first.frame_id) > 0) {
          for (auto it2 = it->second.cbegin(); it2 != it->second.cend();
               ++it2) {
            if (it2->first.frame_id <= last_state_to_marg)
              obs_to_lin[it->first].emplace(*it2);
          }
        }
        ++it;
      }

    // Do Schur Complement(视觉、IMU、prior) 这个部分和优化时进行的操作基本一样
      double rld_error;
      Eigen::aligned_vector<RelLinData> rld_vec;

      linearizeHelper(rld_vec, obs_to_lin, rld_error);

      for (auto& rld : rld_vec) {
        rld.invert_keypoint_hessians();

        Eigen::MatrixXd rel_H;
        Eigen::VectorXd rel_b;
        linearizeRel(rld, rel_H, rel_b);

        linearizeAbs(rel_H, rel_b, rld, aom, accum);
      }
    }

    linearizeAbsIMU(aom, accum.getH(), accum.getB(), imu_error, bg_error,
                    ba_error, frame_states, imu_meas, gyro_bias_weight,
                    accel_bias_weight, g);
    linearizeMargPrior(marg_order, marg_H, marg_b, aom, accum.getH(),
                       accum.getB(), marg_prior_error);

    // 
    // Save marginalization prior
    if (out_marg_queue && !kfs_to_marg.empty()) {
      // int64_t kf_id = *kfs_to_marg.begin();

      {
        MargData::Ptr m(new MargData);
        m->aom = aom;
        m->abs_H = accum.getH();
        m->abs_b = accum.getB();
        m->frame_poses = frame_poses;
        m->frame_states = frame_states;
        m->kfs_all = kf_ids_all;
        m->kfs_to_marg = kfs_to_marg;
        m->use_imu = true;

        for (int64_t t : m->kfs_all) {
          m->opt_flow_res.emplace_back(prev_opt_flow_res.at(t));
        }

        out_marg_queue->push(m);
      }
    }

    std::set<int> idx_to_keep, idx_to_marg;
    for (const auto& kv : aom.abs_order_map) {
      if (kv.second.second == POSE_SIZE) {
        int start_idx = kv.second.first;
        if (poses_to_marg.count(kv.first) == 0) {
          for (size_t i = 0; i < POSE_SIZE; i++)
            idx_to_keep.emplace(start_idx + i);
        } else {
          for (size_t i = 0; i < POSE_SIZE; i++)
            idx_to_marg.emplace(start_idx + i);
        }
      } else {
        BASALT_ASSERT(kv.second.second == POSE_VEL_BIAS_SIZE);
        // state
        int start_idx = kv.second.first;
        if (states_to_marg_all.count(kv.first) > 0) {
          for (size_t i = 0; i < POSE_VEL_BIAS_SIZE; i++)
            idx_to_marg.emplace(start_idx + i);
        } else if (states_to_marg_vel_bias.count(kv.first) > 0) {
          for (size_t i = 0; i < POSE_SIZE; i++)
            idx_to_keep.emplace(start_idx + i);
          for (size_t i = POSE_SIZE; i < POSE_VEL_BIAS_SIZE; i++)
            idx_to_marg.emplace(start_idx + i);
        } else {
          BASALT_ASSERT(kv.first == last_state_to_marg);
          for (size_t i = 0; i < POSE_VEL_BIAS_SIZE; i++)
            idx_to_keep.emplace(start_idx + i);
        }
      }
    }

    if (config.vio_debug) {
      std::cout << "keeping " << idx_to_keep.size() << " marg "
                << idx_to_marg.size() << " total " << asize << std::endl;
      std::cout << "last_state_to_marg " << last_state_to_marg
                << " frame_poses " << frame_poses.size() << " frame_states "
                << frame_states.size() << std::endl;
    }

    Eigen::MatrixXd marg_H_new;
    Eigen::VectorXd marg_b_new;
    marginalizeHelper(accum.getH(), accum.getB(), idx_to_keep, idx_to_marg,
                      marg_H_new, marg_b_new);

    {
      BASALT_ASSERT(frame_states.at(last_state_to_marg).isLinearized() ==
                    false);
      frame_states.at(last_state_to_marg).setLinTrue();
    }

    for (const int64_t id : states_to_marg_all) {
      frame_states.erase(id);
      imu_meas.erase(id);
      prev_opt_flow_res.erase(id);
    }

    for (const int64_t id : states_to_marg_vel_bias) {
      const PoseVelBiasStateWithLin<double>& state = frame_states.at(id);
      PoseStateWithLin pose(state);

      frame_poses[id] = pose;
      frame_states.erase(id);
      imu_meas.erase(id);
    }

    for (const int64_t id : poses_to_marg) {
      frame_poses.erase(id);
      prev_opt_flow_res.erase(id);
    }

    lmdb.removeKeyframes(kfs_to_marg, poses_to_marg, states_to_marg_all);

    AbsOrderMap marg_order_new;

    for (const auto& kv : frame_poses) {
      marg_order_new.abs_order_map[kv.first] =
          std::make_pair(marg_order_new.total_size, POSE_SIZE);

      marg_order_new.total_size += POSE_SIZE;
      marg_order_new.items++;
    }

    {
      marg_order_new.abs_order_map[last_state_to_marg] =
          std::make_pair(marg_order_new.total_size, POSE_VEL_BIAS_SIZE);
      marg_order_new.total_size += POSE_VEL_BIAS_SIZE;
      marg_order_new.items++;
    }

    marg_H = marg_H_new;
    marg_b = marg_b_new;
    marg_order = marg_order_new;

    BASALT_ASSERT(size_t(marg_H.cols()) == marg_order.total_size);

    Eigen::VectorXd delta;
    computeDelta(marg_order, delta);
    marg_b -= marg_H * delta;

    if (config.vio_debug) {
      std::cout << "marginalizaon done!!" << std::endl;

      std::cout << "======== Marg nullspace ==========" << std::endl;
      checkMargNullspace();
      std::cout << "=================================" << std::endl;
    }

    //    std::cout << "new marg prior order" << std::endl;
    //    marg_order.print_order();
  }
}

void KeypointVioEstimator::optimize() {
  if (config.vio_debug) {
    std::cout << "=================================" << std::endl;
  }

  // 如果 frame_state >4 就开始进行优化 
  if (opt_started || frame_states.size() > 4) {
    // Optimize
    opt_started = true;

    // 目前猜测存储的是优化矩阵的信息
    AbsOrderMap aom;
    
    // frame_poses 存储的是 [timestamp, PoseState 的信息];
    // 这里应该是给 AbsOrderMap 进行各个位姿存在的序号 进行统计
    // ? 这个是什么地方进行统计还真知道
    for (const auto& kv : frame_poses) {
      aom.abs_order_map[kv.first] = std::make_pair(aom.total_size, POSE_SIZE);

      // Check that we have the same order as marginalization
      BASALT_ASSERT(marg_order.abs_order_map.at(kv.first) ==
                    aom.abs_order_map.at(kv.first));

      aom.total_size += POSE_SIZE;
      aom.items++;
    }

    // 添加 IMU  优化量 
    // ? POSE_VEL_BIAS_SIZE 这个是多少
    // ? frame_poses 和 frame_states 不会出现aom abs_order_map 内容出现替换的问题吗？因为感觉 frame_poses 和 frame_states 的时间戳可能是
    // 这个问题估计要看完margin 才能知道了
    // frame state 在kepoint_vio 里面每一帧都会创建一个
    for (const auto& kv : frame_states) {
      aom.abs_order_map[kv.first] =
          std::make_pair(aom.total_size, POSE_VEL_BIAS_SIZE);

      // Check that we have the same order as marginalization
      if (aom.items < marg_order.abs_order_map.size())
        BASALT_ASSERT(marg_order.abs_order_map.at(kv.first) ==
                      aom.abs_order_map.at(kv.first));

      aom.total_size += POSE_VEL_BIAS_SIZE;
      aom.items++;
    }

    //    std::cout << "opt order" << std::endl;
    //    aom.print_order();

    //    std::cout << "marg prior order" << std::endl;
    //    marg_order.print_order();

    // 正式开始进行优化循环

    for (int iter = 0; iter < config.vio_max_iterations; iter++) {
      
      auto t1 = std::chrono::high_resolution_clock::now();

      double rld_error;
      Eigen::aligned_vector<RelLinData> rld_vec;
      // TODO LWL: 内部注释需要看ppt 继续食用
      // 视觉Residual 进行求导
      linearizeHelper(rld_vec, lmdb.getObservations(), rld_error);


      // LinearizeAbsReduce 实际上就在组成 Pose 的舒尔补block (视觉残差)
      BundleAdjustmentBase::LinearizeAbsReduce<DenseAccumulator<double>> lopt(
          aom);

      tbb::blocked_range<Eigen::aligned_vector<RelLinData>::iterator> range(
          rld_vec.begin(), rld_vec.end());

      tbb::parallel_reduce(range, lopt);

      // IMU参差添加到H 和b
      // ? 但是如何使用Schur 补的问还需要研究一下啊
      double marg_prior_error = 0;
      double imu_error = 0, bg_error = 0, ba_error = 0;
      linearizeAbsIMU(aom, lopt.accum.getH(), lopt.accum.getB(), imu_error,
                      bg_error, ba_error, frame_states, imu_meas,
                      gyro_bias_weight, accel_bias_weight, g);
      // 先验添加到 H 和b 当中
      // ? 具体的计算还要在后面看一下
      linearizeMargPrior(marg_order, marg_H, marg_b, aom, lopt.accum.getH(),
                         lopt.accum.getB(), marg_prior_error);

      double error_total =
          rld_error + imu_error + marg_prior_error + ba_error + bg_error;

      if (config.vio_debug)
        std::cout << "[LINEARIZE] Error: " << error_total << " num points "
                  << std::endl;

      lopt.accum.setup_solver();
      Eigen::VectorXd Hdiag = lopt.accum.Hdiagonal();

      bool converged = false;

      if (config.vio_use_lm) {  // Use Levenberg–Marquardt
        bool step = false;
        int max_iter = 10;

        while (!step && max_iter > 0 && !converged) {
          Eigen::VectorXd Hdiag_lambda = Hdiag * lambda;
          for (int i = 0; i < Hdiag_lambda.size(); i++)
            Hdiag_lambda[i] = std::max(Hdiag_lambda[i], min_lambda);

          const Eigen::VectorXd inc = lopt.accum.solve(&Hdiag_lambda);
          double max_inc = inc.array().abs().maxCoeff();
          if (max_inc < 1e-4) converged = true;

          backup();

          // apply increment to poses
          for (auto& kv : frame_poses) {
            int idx = aom.abs_order_map.at(kv.first).first;
            kv.second.applyInc(-inc.segment<POSE_SIZE>(idx));
          }

          // apply increment to states
          for (auto& kv : frame_states) {
            int idx = aom.abs_order_map.at(kv.first).first;
            kv.second.applyInc(-inc.segment<POSE_VEL_BIAS_SIZE>(idx));
          }

          // Update points
          tbb::blocked_range<size_t> keys_range(0, rld_vec.size());
          auto update_points_func = [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i != r.end(); ++i) {
              const auto& rld = rld_vec[i];
              updatePoints(aom, rld, inc);
            }
          };
          tbb::parallel_for(keys_range, update_points_func);

          double after_update_marg_prior_error = 0;
          double after_update_vision_error = 0, after_update_imu_error = 0,
                 after_bg_error = 0, after_ba_error = 0;

          computeError(after_update_vision_error);
          computeImuError(aom, after_update_imu_error, after_bg_error,
                          after_ba_error, frame_states, imu_meas,
                          gyro_bias_weight, accel_bias_weight, g);
          computeMargPriorError(marg_order, marg_H, marg_b,
                                after_update_marg_prior_error);

          double after_error_total =
              after_update_vision_error + after_update_imu_error +
              after_update_marg_prior_error + after_bg_error + after_ba_error;

          double f_diff = (error_total - after_error_total);

          if (f_diff < 0) {
            if (config.vio_debug)
              std::cout << "\t[REJECTED] lambda:" << lambda
                        << " f_diff: " << f_diff << " max_inc: " << max_inc
                        << " Error: " << after_error_total << std::endl;
            lambda = std::min(max_lambda, lambda_vee * lambda);
            lambda_vee *= 2;

            restore();
          } else {
            if (config.vio_debug)
              std::cout << "\t[ACCEPTED] lambda:" << lambda
                        << " f_diff: " << f_diff << " max_inc: " << max_inc
                        << " Error: " << after_error_total << std::endl;

            lambda = std::max(min_lambda, lambda / 3);
            lambda_vee = 2;

            step = true;
          }
          max_iter--;
        }

        if (config.vio_debug && converged) {
          std::cout << "[CONVERGED]" << std::endl;
        }
      }
      else 
      {  // Use Gauss-Newton
        Eigen::VectorXd Hdiag_lambda = Hdiag * min_lambda;
        for (int i = 0; i < Hdiag_lambda.size(); i++)
          Hdiag_lambda[i] = std::max(Hdiag_lambda[i], min_lambda);

        const Eigen::VectorXd inc = lopt.accum.solve(&Hdiag_lambda);
        double max_inc = inc.array().abs().maxCoeff();
        if (max_inc < 1e-4) converged = true;

        // apply increment to poses
        for (auto& kv : frame_poses) {
          int idx = aom.abs_order_map.at(kv.first).first;
          kv.second.applyInc(-inc.segment<POSE_SIZE>(idx));
        }

        // apply increment to states
        for (auto& kv : frame_states) {
          int idx = aom.abs_order_map.at(kv.first).first;
          kv.second.applyInc(-inc.segment<POSE_VEL_BIAS_SIZE>(idx));
        }

        // Update points
        tbb::blocked_range<size_t> keys_range(0, rld_vec.size());
        auto update_points_func = [&](const tbb::blocked_range<size_t>& r) {
          for (size_t i = r.begin(); i != r.end(); ++i) {
            const auto& rld = rld_vec[i];
            updatePoints(aom, rld, inc);
          }
        };
        tbb::parallel_for(keys_range, update_points_func);
      }

      if (config.vio_debug) {
        double after_update_marg_prior_error = 0;
        double after_update_vision_error = 0, after_update_imu_error = 0,
               after_bg_error = 0, after_ba_error = 0;

        computeError(after_update_vision_error);
        computeImuError(aom, after_update_imu_error, after_bg_error,
                        after_ba_error, frame_states, imu_meas,
                        gyro_bias_weight, accel_bias_weight, g);
        computeMargPriorError(marg_order, marg_H, marg_b,
                              after_update_marg_prior_error);

        double after_error_total =
            after_update_vision_error + after_update_imu_error +
            after_update_marg_prior_error + after_bg_error + after_ba_error;

        double error_diff = error_total - after_error_total;

        auto t2 = std::chrono::high_resolution_clock::now();

        auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);

        std::cout << "iter " << iter
                  << " before_update_error: vision: " << rld_error
                  << " imu: " << imu_error << " bg_error: " << bg_error
                  << " ba_error: " << ba_error
                  << " marg_prior: " << marg_prior_error
                  << " total: " << error_total << std::endl;

        std::cout << "iter " << iter << "  after_update_error: vision: "
                  << after_update_vision_error
                  << " imu: " << after_update_imu_error
                  << " bg_error: " << after_bg_error
                  << " ba_error: " << after_ba_error
                  << " marg prior: " << after_update_marg_prior_error
                  << " total: " << after_error_total << " error_diff "
                  << error_diff << " time : " << elapsed.count()
                  << "(us),  num_states " << frame_states.size()
                  << " num_poses " << frame_poses.size() << std::endl;

        if (after_error_total > error_total) {
          std::cout << "increased error after update!!!" << std::endl;
        }
      }

      if (iter == config.vio_filter_iteration) {
        filterOutliers(config.vio_outlier_threshold, 4);
      }

      if (converged) break;

      // std::cerr << "LT\n" << LT << std::endl;
      // std::cerr << "z_p\n" << z_p.transpose() << std::endl;
      // std::cerr << "inc\n" << inc.transpose() << std::endl;
    }
  }

  if (config.vio_debug) {
    std::cout << "=================================" << std::endl;
  }
}  // namespace basalt

void KeypointVioEstimator::computeProjections(
    std::vector<Eigen::aligned_vector<Eigen::Vector4d>>& data) const {
  for (const auto& kv : lmdb.getObservations()) {
    const TimeCamId& tcid_h = kv.first;

    for (const auto& obs_kv : kv.second) {
      const TimeCamId& tcid_t = obs_kv.first;

      if (tcid_t.frame_id != last_state_t_ns) continue;

      if (tcid_h != tcid_t) {
        PoseStateWithLin state_h = getPoseStateWithLin(tcid_h.frame_id);
        PoseStateWithLin state_t = getPoseStateWithLin(tcid_t.frame_id);

        Sophus::SE3d T_t_h_sophus =
            computeRelPose(state_h.getPose(), calib.T_i_c[tcid_h.cam_id],
                           state_t.getPose(), calib.T_i_c[tcid_t.cam_id]);

        Eigen::Matrix4d T_t_h = T_t_h_sophus.matrix();

        FrameRelLinData rld;

        std::visit(
            [&](const auto& cam) {
              for (size_t i = 0; i < obs_kv.second.size(); i++) {
                const KeypointObservation& kpt_obs = obs_kv.second[i];
                const KeypointPosition& kpt_pos =
                    lmdb.getLandmark(kpt_obs.kpt_id);

                Eigen::Vector2d res;
                Eigen::Vector4d proj;

                linearizePoint(kpt_obs, kpt_pos, T_t_h, cam, res, nullptr,
                               nullptr, &proj);

                proj[3] = kpt_obs.kpt_id;
                data[tcid_t.cam_id].emplace_back(proj);
              }
            },
            calib.intrinsics[tcid_t.cam_id].variant);

      } else {
        // target and host are the same
        // residual does not depend on the pose
        // it just depends on the point

        std::visit(
            [&](const auto& cam) {
              for (size_t i = 0; i < obs_kv.second.size(); i++) {
                const KeypointObservation& kpt_obs = obs_kv.second[i];
                const KeypointPosition& kpt_pos =
                    lmdb.getLandmark(kpt_obs.kpt_id);

                Eigen::Vector2d res;
                Eigen::Vector4d proj;

                linearizePoint(kpt_obs, kpt_pos, Eigen::Matrix4d::Identity(),
                               cam, res, nullptr, nullptr, &proj);

                proj[3] = kpt_obs.kpt_id;
                data[tcid_t.cam_id].emplace_back(proj);
              }
            },
            calib.intrinsics[tcid_t.cam_id].variant);
      }
    }
  }
}

}  // namespace basalt
