//
// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "common/libs/security/channel.h"
#include "common/libs/utils/result.h"
#include "host/commands/secure_env/oemlock/oemlock.h"

namespace cuttlefish {
namespace oemlock {

class OemLockResponder {
 public:
  OemLockResponder(secure_env::Channel& channel,
                   OemLock& oemlock);

  Result<void> ProcessMessage();

 private:
  secure_env::Channel& channel_;
  OemLock& oemlock_;
};

}  // namespace oemlock
}  // namespace cuttlefish
