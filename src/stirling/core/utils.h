/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>

#include "src/common/system/clock.h"

namespace px {
namespace stirling {

/**
 * Manages the frequency of periodical action.
 */
class FrequencyManager {
 public:
  /**
   * Returns true if the current cycle has expired.
   */
  bool Expired() const { return px::chrono::coarse_steady_clock::now() >= next_; }

  /**
   * Ends the current cycle, and starts the next one.
   */
  void Reset();

  void set_period(std::chrono::milliseconds period) { period_ = period; }
  const auto& period() const { return period_; }
  const auto& next() const { return next_; }
  uint32_t count() const { return count_; }

 private:
  // The cycle's period.
  std::chrono::milliseconds period_ = {};

  // When the current cycle should end.
  px::chrono::coarse_steady_clock::time_point next_ = {};

  // The count of expired cycle so far.
  uint32_t count_ = 0;
};

// Manages how often data sample and pushing should be performed.
class SamplePushFrequencyManager {
 public:
  /**
   * Returns true if sampling is required, for whatever reason (elapsed time, etc.).
   *
   * @return bool
   */
  bool SamplingRequired() const { return sampling_freq_mgr_.Expired(); }

  /**
   * Called when sampling data to update timestamps.
   */
  void Sample() { sampling_freq_mgr_.Reset(); }

  /**
   * Returns true if a data push is required, for whatever reason (elapsed time, occupancy, etc.).
   *
   * @return bool
   */
  bool PushRequired(double occupancy_percentage, uint32_t occupancy) const;

  /**
   * Called when pushing data to update timestamps.
   */
  void Push() { push_freq_mgr_.Reset(); }

  /**
   * Returns the next time the source needs to be sampled, according to the sampling period.
   *
   * @return std::chrono::milliseconds
   */
  px::chrono::coarse_steady_clock::time_point NextSamplingTime() const {
    return push_freq_mgr_.next();
  }

  /**
   * Returns the next time the data table needs to be pushed upstream, according to the push period.
   *
   * @return std::chrono::milliseconds
   */
  px::chrono::coarse_steady_clock::time_point NextPushTime() const { return push_freq_mgr_.next(); }

  void set_sampling_period(std::chrono::milliseconds period) {
    sampling_freq_mgr_.set_period(period);
  }
  void set_push_period(std::chrono::milliseconds period) { push_freq_mgr_.set_period(period); }
  const auto& sampling_period() const { return sampling_freq_mgr_.period(); }
  const auto& push_period() const { return push_freq_mgr_.period(); }
  uint32_t sampling_count() const { return sampling_freq_mgr_.count(); }

 private:
  FrequencyManager sampling_freq_mgr_;
  FrequencyManager push_freq_mgr_;
};

}  // namespace stirling
}  // namespace px
