// Copyright 2024 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "perception_online_evaluator/metrics_calculator.hpp"

#include "motion_utils/trajectory/trajectory.hpp"
#include "perception_online_evaluator/utils/objects_filtering.hpp"
#include "tier4_autoware_utils/geometry/geometry.hpp"

#include <tier4_autoware_utils/ros/uuid_helper.hpp>

namespace perception_diagnostics
{
std::optional<MetricStatMap> MetricsCalculator::calculate(const Metric & metric) const
{
  if (object_map_.empty()) {
    return {};
  }

  // time delay is max element of parameters_->prediction_time_horizons
  const double time_delay = getTimeDelay();
  const auto target_stamp = current_stamp_ - rclcpp::Duration::from_seconds(time_delay);
  if (!hasPassedTime(target_stamp)) {
    return {};
  }
  const auto target_objects = getObjectsByStamp(target_stamp);

  switch (metric) {
    case Metric::lateral_deviation:
      return calcLateralDeviationMetrics(target_objects);
    case Metric::yaw_deviation:
      return calcYawDeviationMetrics(target_objects);
    case Metric::predicted_path_deviation:
      return calcPredictedPathDeviationMetrics(target_objects);
    default:
      return {};
  }
}

double MetricsCalculator::getTimeDelay() const
{
  const auto max_element_it = std::max_element(
    parameters_->prediction_time_horizons.begin(), parameters_->prediction_time_horizons.end());
  if (max_element_it != parameters_->prediction_time_horizons.end()) {
    return *max_element_it;
  }
  throw std::runtime_error("prediction_time_horizons is empty");
}

bool MetricsCalculator::hasPassedTime(const std::string uuid, const rclcpp::Time stamp) const
{
  if (object_map_.find(uuid) == object_map_.end()) {
    return false;
  }
  const auto oldest_stamp = object_map_.at(uuid).begin()->first;
  return oldest_stamp <= stamp;
}

bool MetricsCalculator::hasPassedTime(const rclcpp::Time stamp) const
{
  std::vector<rclcpp::Time> timestamps;
  for (const auto & [uuid, stamp_and_objects] : object_map_) {
    if (!stamp_and_objects.empty()) {
      timestamps.push_back(stamp_and_objects.begin()->first);
    }
  }

  auto it = std::min_element(timestamps.begin(), timestamps.end());
  if (it != timestamps.end()) {
    rclcpp::Time oldest_stamp = *it;
    if (oldest_stamp > stamp) {
      return false;
    }
  }
  return true;
}

rclcpp::Time MetricsCalculator::getClosestStamp(const rclcpp::Time stamp) const
{
  rclcpp::Time closest_stamp;
  rclcpp::Duration min_duration =
    rclcpp::Duration::from_nanoseconds(std::numeric_limits<int64_t>::max());

  for (const auto & [uuid, stamp_and_objects] : object_map_) {
    if (!stamp_and_objects.empty()) {
      auto it = std::lower_bound(
        stamp_and_objects.begin(), stamp_and_objects.end(), stamp,
        [](const auto & pair, const rclcpp::Time & val) { return pair.first < val; });

      // check the upper bound
      if (it != stamp_and_objects.end()) {
        const auto duration = it->first - stamp;
        if (std::abs(duration.nanoseconds()) < min_duration.nanoseconds()) {
          min_duration = duration;
          closest_stamp = it->first;
        }
      }

      // check the lower bound (if it is not the first element)
      if (it != stamp_and_objects.begin()) {
        const auto prev_it = std::prev(it);
        const auto duration = stamp - prev_it->first;
        if (std::abs(duration.nanoseconds()) < min_duration.nanoseconds()) {
          min_duration = duration;
          closest_stamp = prev_it->first;
        }
      }
    }
  }

  return closest_stamp;
}

std::optional<PredictedObject> MetricsCalculator::getObjectByStamp(
  const std::string uuid, const rclcpp::Time stamp) const
{
  const auto closest_stamp = getClosestStamp(stamp);
  auto it = std::lower_bound(
    object_map_.at(uuid).begin(), object_map_.at(uuid).end(), closest_stamp,
    [](const auto & pair, const rclcpp::Time & val) { return pair.first < val; });

  if (it != object_map_.at(uuid).end() && it->first == closest_stamp) {
    return it->second;
  }
  return std::nullopt;
}

PredictedObjects MetricsCalculator::getObjectsByStamp(const rclcpp::Time stamp) const
{
  const auto closest_stamp = getClosestStamp(stamp);

  PredictedObjects objects;
  objects.header.stamp = stamp;
  for (const auto & [uuid, stamp_and_objects] : object_map_) {
    auto it = std::lower_bound(
      stamp_and_objects.begin(), stamp_and_objects.end(), closest_stamp,
      [](const auto & pair, const rclcpp::Time & val) { return pair.first < val; });

    // add the object only if the element pointed to by lower_bound is equal to stamp
    if (it != stamp_and_objects.end() && it->first == closest_stamp) {
      objects.objects.push_back(it->second);
    }
  }
  return objects;
}

MetricStatMap MetricsCalculator::calcLateralDeviationMetrics(const PredictedObjects & objects) const
{
  Stat<double> stat{};
  const auto stamp = rclcpp::Time(objects.header.stamp);

  for (const auto & object : objects.objects) {
    const auto uuid = tier4_autoware_utils::toHexString(object.object_id);
    if (!hasPassedTime(uuid, stamp)) {
      continue;
    }
    const auto object_pose = object.kinematics.initial_pose_with_covariance.pose;
    const auto history_path = history_path_map_.at(uuid).second;
    if (history_path.empty()) {
      continue;
    }
    stat.add(metrics::calcLateralDeviation(history_path, object_pose));
  }

  MetricStatMap metric_stat_map;
  metric_stat_map["lateral_deviation"] = stat;
  return metric_stat_map;
}

MetricStatMap MetricsCalculator::calcYawDeviationMetrics(const PredictedObjects & objects) const
{
  Stat<double> stat{};
  const auto stamp = rclcpp::Time(objects.header.stamp);
  for (const auto & object : objects.objects) {
    const auto uuid = tier4_autoware_utils::toHexString(object.object_id);
    if (!hasPassedTime(uuid, stamp)) {
      continue;
    }
    const auto object_pose = object.kinematics.initial_pose_with_covariance.pose;
    const auto history_path = history_path_map_.at(uuid).second;
    if (history_path.empty()) {
      continue;
    }
    stat.add(metrics::calcYawDeviation(history_path, object_pose));
  }

  MetricStatMap metric_stat_map;
  metric_stat_map["yaw_deviation"] = stat;
  return metric_stat_map;
}

MetricStatMap MetricsCalculator::calcPredictedPathDeviationMetrics(
  const PredictedObjects & objects) const
{
  const auto time_horizons = parameters_->prediction_time_horizons;

  MetricStatMap metric_stat_map;
  for (const double time_horizon : time_horizons) {
    const auto stat = calcPredictedPathDeviationMetrics(objects, time_horizon);
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << time_horizon;
    std::string time_horizon_str = stream.str();
    metric_stat_map["predicted_path_deviation_" + time_horizon_str] = stat;
  }

  return metric_stat_map;
}

Stat<double> MetricsCalculator::calcPredictedPathDeviationMetrics(
  const PredictedObjects & objects, const double time_horizon) const
{
  // For each object, select the predicted path that is closest to the history path and store the
  // distance to the history path
  std::unordered_map<std::string, std::unordered_map<size_t, std::vector<double>>>
    deviation_map_for_paths;
  // For debugging. Save the pairs of predicted path pose and history path pose.
  // Visualize the correspondence in rviz from the node.
  std::unordered_map<std::string, std::vector<std::pair<Pose, Pose>>>
    debug_predicted_path_pairs_map;

  // Find the corresponding pose in the history path for each pose of the predicted path of each
  // object, and record the distances
  const auto stamp = objects.header.stamp;
  for (const auto & object : objects.objects) {
    const auto uuid = tier4_autoware_utils::toHexString(object.object_id);
    const auto predicted_paths = object.kinematics.predicted_paths;
    for (size_t i = 0; i < predicted_paths.size(); i++) {
      const auto predicted_path = predicted_paths[i];
      const std::string path_id = uuid + "_" + std::to_string(i);
      for (size_t j = 0; j < predicted_path.path.size(); j++) {
        const double time_duration =
          rclcpp::Duration(predicted_path.time_step).seconds() * static_cast<double>(j);
        if (time_duration > time_horizon) {
          break;
        }
        const rclcpp::Time target_stamp =
          rclcpp::Time(stamp) + rclcpp::Duration::from_seconds(time_duration);
        if (!hasPassedTime(uuid, target_stamp)) {
          continue;
        }
        const auto history_object_opt = getObjectByStamp(uuid, target_stamp);
        if (!history_object_opt.has_value()) {
          continue;
        }
        const auto history_object = history_object_opt.value();
        const auto history_pose = history_object.kinematics.initial_pose_with_covariance.pose;
        const Pose & p = predicted_path.path[j];
        const double distance =
          tier4_autoware_utils::calcDistance2d(p.position, history_pose.position);
        deviation_map_for_paths[uuid][i].push_back(distance);
        // debug
        debug_predicted_path_pairs_map[path_id].push_back(std::make_pair(p, history_pose));
      }
    }
  }

  // Select the predicted path with the smallest deviation for each object
  std::unordered_map<std::string, std::vector<double>> deviation_map_for_objects;
  for (const auto & [uuid, deviation_map] : deviation_map_for_paths) {
    size_t min_deviation_index = 0;
    double min_sum_deviation = std::numeric_limits<double>::max();
    for (const auto & [i, deviations] : deviation_map) {
      if (deviations.empty()) {
        continue;
      }
      const double sum = std::accumulate(deviations.begin(), deviations.end(), 0.0);
      if (sum < min_sum_deviation) {
        min_sum_deviation = sum;
        min_deviation_index = i;
      }
    }
    deviation_map_for_objects[uuid] = deviation_map.at(min_deviation_index);

    // debug: save the delayed target object and the corresponding predicted path
    const auto path_id = uuid + "_" + std::to_string(min_deviation_index);
    const auto target_stamp_object = getObjectByStamp(uuid, stamp);
    if (target_stamp_object.has_value()) {
      ObjectData object_data;
      object_data.object = target_stamp_object.value();
      object_data.path_pairs = debug_predicted_path_pairs_map[path_id];
      debug_target_object_[uuid] = object_data;
    }
  }

  // Store the deviation as a metric
  Stat<double> stat;
  for (const auto & [uuid, deviations] : deviation_map_for_objects) {
    if (deviations.empty()) {
      continue;
    }
    for (const auto & deviation : deviations) {
      stat.add(deviation);
    }
  }
  return stat;
}

void MetricsCalculator::setPredictedObjects(const PredictedObjects & objects)
{
  // using TimeStamp = builtin_interfaces::msg::Time;
  current_stamp_ = objects.header.stamp;

  // store objects to check deviation
  {
    auto deviation_check_objects = objects;
    filterDeviationCheckObjects(deviation_check_objects, parameters_->object_parameters);
    using tier4_autoware_utils::toHexString;
    for (const auto & object : deviation_check_objects.objects) {
      std::string uuid = toHexString(object.object_id);
      updateObjects(uuid, current_stamp_, object);
    }
    deleteOldObjects(current_stamp_);
    updateHistoryPath();
  }
}

void MetricsCalculator::deleteOldObjects(const rclcpp::Time stamp)
{
  // delete the data older than 2*time_delay_
  const double time_delay = getTimeDelay();
  for (auto & [uuid, stamp_and_objects] : object_map_) {
    for (auto it = stamp_and_objects.begin(); it != stamp_and_objects.end();) {
      if (it->first < stamp - rclcpp::Duration::from_seconds(time_delay * 2)) {
        it = stamp_and_objects.erase(it);
      } else {
        break;
      }
    }
  }

  const auto object_map = object_map_;
  for (const auto & [uuid, stamp_and_objects] : object_map) {
    if (stamp_and_objects.empty()) {
      object_map_.erase(uuid);
      history_path_map_.erase(uuid);
      debug_target_object_.erase(uuid);  // debug
    }
  }
}

void MetricsCalculator::updateObjects(
  const std::string uuid, const rclcpp::Time stamp, const PredictedObject & object)
{
  object_map_[uuid][stamp] = object;
}

void MetricsCalculator::updateHistoryPath()
{
  const double window_size = parameters_->smoothing_window_size;

  for (const auto & [uuid, stamp_and_objects] : object_map_) {
    std::vector<Pose> history_path;
    for (const auto & [stamp, object] : stamp_and_objects) {
      history_path.push_back(object.kinematics.initial_pose_with_covariance.pose);
    }

    // pair of history_path(raw) and smoothed_history_path
    // history_path(raw) is just for debugging
    history_path_map_[uuid] =
      std::make_pair(history_path, averageFilterPath(history_path, window_size));
  }
}

std::vector<Pose> MetricsCalculator::generateHistoryPathWithPrev(
  const std::vector<Pose> & prev_history_path, const Pose & new_pose, const size_t window_size)
{
  std::vector<Pose> history_path;
  const size_t half_window_size = static_cast<size_t>(window_size / 2);
  history_path.insert(
    history_path.end(), prev_history_path.begin(), prev_history_path.end() - half_window_size);

  std::vector<Pose> updated_poses;
  updated_poses.insert(
    updated_poses.end(), prev_history_path.end() - half_window_size * 2, prev_history_path.end());
  updated_poses.push_back(new_pose);

  updated_poses = averageFilterPath(updated_poses, window_size);
  history_path.insert(
    history_path.end(), updated_poses.begin() + half_window_size, updated_poses.end());
  return history_path;
}

std::vector<Pose> MetricsCalculator::averageFilterPath(
  const std::vector<Pose> & path, const size_t window_size) const
{
  using tier4_autoware_utils::calcAzimuthAngle;
  using tier4_autoware_utils::calcDistance2d;
  using tier4_autoware_utils::createQuaternionFromYaw;

  std::vector<Pose> filtered_path;
  filtered_path.reserve(path.size());  // Reserve space to avoid reallocations

  const size_t half_window = static_cast<size_t>(window_size / 2);

  // Calculate the moving average for positions
  for (size_t i = 0; i < path.size(); ++i) {
    double sum_x = 0.0, sum_y = 0.0, sum_z = 0.0;
    size_t valid_points = 0;  // Correctly initialize and use as counter

    for (size_t j = std::max(i - half_window, static_cast<size_t>(0));
         j <= std::min(i + half_window, path.size() - 1); ++j) {
      sum_x += path[j].position.x;
      sum_y += path[j].position.y;
      sum_z += path[j].position.z;
      ++valid_points;
    }

    Pose average_pose;
    if (valid_points > 0) {  // Prevent division by zero
      average_pose.position.x = sum_x / valid_points;
      average_pose.position.y = sum_y / valid_points;
      average_pose.position.z = sum_z / valid_points;
    } else {
      average_pose.position = path.at(i).position;
    }

    // Placeholder for orientation to ensure structure integrity
    average_pose.orientation = path.at(i).orientation;
    filtered_path.push_back(average_pose);
  }

  // Calculate yaw and convert to quaternion after averaging positions
  for (size_t i = 0; i < filtered_path.size(); ++i) {
    Pose & p = filtered_path[i];

    // if the current pose is too close to the previous one, use the previous orientation
    if (i > 0) {
      const Pose & p_prev = filtered_path[i - 1];
      if (calcDistance2d(p_prev.position, p.position) < 0.1) {
        p.orientation = p_prev.orientation;
        continue;
      }
    }

    if (i < filtered_path.size() - 1) {
      const double yaw = calcAzimuthAngle(p.position, filtered_path[i + 1].position);
      filtered_path[i].orientation = createQuaternionFromYaw(yaw);
    } else if (filtered_path.size() > 1) {
      // For the last point, use the orientation of the second-to-last point
      p.orientation = filtered_path[i - 1].orientation;
    }
  }

  return filtered_path;
}

}  // namespace perception_diagnostics
