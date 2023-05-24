//
// Copyright (C) 2020 The Android Open Source Project
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

#include <thread>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <fruit/fruit.h>
#include <gflags/gflags.h>
#include <keymaster/android_keymaster.h>
#include <keymaster/contexts/pure_soft_keymaster_context.h>
#include <keymaster/soft_keymaster_logger.h>
#include <tss2/tss2_esys.h>
#include <tss2/tss2_rc.h>

#include "common/libs/fs/shared_fd.h"
#include "common/libs/security/channel_sharedfd.h"
#include "common/libs/security/confui_sign.h"
#include "common/libs/security/gatekeeper_channel_sharedfd.h"
#include "common/libs/security/keymaster_channel_sharedfd.h"
#include "host/commands/kernel_log_monitor/kernel_log_server.h"
#include "host/commands/kernel_log_monitor/utils.h"
#include "host/commands/secure_env/confui_sign_server.h"
#include "host/commands/secure_env/device_tpm.h"
#include "host/commands/secure_env/fragile_tpm_storage.h"
#include "host/commands/secure_env/gatekeeper_responder.h"
#include "host/commands/secure_env/in_process_tpm.h"
#include "host/commands/secure_env/insecure_fallback_storage.h"
#include "host/commands/secure_env/keymaster_responder.h"
#include "host/commands/secure_env/oemlock_responder.h"
#include "host/commands/secure_env/oemlock.h"
#include "host/commands/secure_env/proxy_keymaster_context.h"
#include "host/commands/secure_env/rust/kmr_ta.h"
#include "host/commands/secure_env/soft_gatekeeper.h"
#include "host/commands/secure_env/soft_oemlock.h"
#include "host/commands/secure_env/tpm_gatekeeper.h"
#include "host/commands/secure_env/tpm_keymaster_context.h"
#include "host/commands/secure_env/tpm_keymaster_enforcement.h"
#include "host/commands/secure_env/tpm_resource_manager.h"
#include "host/libs/config/logging.h"

DEFINE_int32(confui_server_fd, -1, "A named socket to serve confirmation UI");
DEFINE_int32(keymaster_fd_in, -1, "A pipe for keymaster communication");
DEFINE_int32(keymaster_fd_out, -1, "A pipe for keymaster communication");
DEFINE_int32(keymint_fd_in, -1, "A pipe for keymint communication");
DEFINE_int32(keymint_fd_out, -1, "A pipe for keymint communication");
DEFINE_int32(gatekeeper_fd_in, -1, "A pipe for gatekeeper communication");
DEFINE_int32(gatekeeper_fd_out, -1, "A pipe for gatekeeper communication");
DEFINE_int32(oemlock_fd_in, -1, "A pipe for oemlock communication");
DEFINE_int32(oemlock_fd_out, -1, "A pipe for oemlock communication");
DEFINE_int32(kernel_events_fd, -1,
             "A pipe for monitoring events based on "
             "messages written to the kernel log. This "
             "is used by secure_env to monitor for "
             "device reboots.");

DEFINE_string(tpm_impl, "in_memory",
              "The TPM implementation. \"in_memory\" or \"host_device\"");

DEFINE_string(keymint_impl, "tpm",
              "The KeyMint implementation. \"tpm\" or \"software\"");

DEFINE_string(gatekeeper_impl, "tpm",
              "The gatekeeper implementation. \"tpm\" or \"software\"");

DEFINE_string(oemlock_impl, "software",
              "The oemlock implementation. \"tpm\" or \"software\"");

namespace cuttlefish {
namespace {

// Copied from AndroidKeymaster4Device
constexpr size_t kOperationTableSize = 16;

// Dup a command line file descriptor into a SharedFD.
SharedFD DupFdFlag(gflags::int32 fd) {
  CHECK(fd != -1);
  SharedFD duped = SharedFD::Dup(fd);
  CHECK(duped->IsOpen()) << "Could not dup output fd: " << duped->StrError();
  // The original FD is intentionally kept open so that we can re-exec this
  // process without having to do a bunch of argv book-keeping.
  return duped;
}

// Re-launch this process with all the same flags it was originallys started
// with.
[[noreturn]] void ReExecSelf() {
  // Allocate +1 entry for terminating nullptr.
  std::vector<char*> argv(gflags::GetArgvs().size() + 1, nullptr);
  for (size_t i = 0; i < gflags::GetArgvs().size(); ++i) {
    argv[i] = strdup(gflags::GetArgvs()[i].c_str());
    CHECK(argv[i] != nullptr) << "OOM";
  }
  execv("/proc/self/exe", argv.data());
  char buf[128];
  LOG(FATAL) << "Exec failed, secure_env is out of sync with the guest: "
             << errno << "(" << strerror_r(errno, buf, sizeof(buf)) << ")";
  abort();  // LOG(FATAL) isn't marked as noreturn
}

// Spin up a thread that monitors for a kernel loaded event, then re-execs
// this process. This way, secure_env's boot tracking matches up with the guest.
std::thread StartKernelEventMonitor(SharedFD kernel_events_fd) {
  return std::thread([kernel_events_fd]() {
    while (kernel_events_fd->IsOpen()) {
      auto read_result = monitor::ReadEvent(kernel_events_fd);
      CHECK(read_result.has_value()) << kernel_events_fd->StrError();
      if (read_result->event == monitor::Event::BootloaderLoaded) {
        LOG(DEBUG) << "secure_env detected guest reboot, restarting.";
        ReExecSelf();
      }
    }
  });
}

fruit::Component<fruit::Required<gatekeeper::SoftGateKeeper, TpmGatekeeper,
                                 TpmResourceManager>,
                 gatekeeper::GateKeeper, keymaster::KeymasterEnforcement>
ChooseGatekeeperComponent() {
  if (FLAGS_gatekeeper_impl == "software") {
    return fruit::createComponent()
        .bind<gatekeeper::GateKeeper, gatekeeper::SoftGateKeeper>()
        .registerProvider([]() -> keymaster::KeymasterEnforcement* {
          return new keymaster::SoftKeymasterEnforcement(64, 64);
        });
  } else if (FLAGS_gatekeeper_impl == "tpm") {
    return fruit::createComponent()
        .bind<gatekeeper::GateKeeper, TpmGatekeeper>()
        .registerProvider(
            [](TpmResourceManager& resource_manager,
               TpmGatekeeper& gatekeeper) -> keymaster::KeymasterEnforcement* {
              return new TpmKeymasterEnforcement(resource_manager, gatekeeper);
            });
  } else {
    LOG(FATAL) << "Invalid gatekeeper implementation: "
               << FLAGS_gatekeeper_impl;
    abort();
  }
}

fruit::Component<oemlock::OemLock> ChooseOemlockComponent() {
  if (FLAGS_oemlock_impl == "software") {
    return fruit::createComponent()
        .bind<oemlock::OemLock, oemlock::SoftOemLock>();
  } else if (FLAGS_oemlock_impl == "tpm") {
    LOG(FATAL) << "Oemlock doesn't support TPM implementation";
    abort();
  } else {
    LOG(FATAL) << "Invalid oemlock implementation: "
               << FLAGS_oemlock_impl;
    abort();
  }
}

fruit::Component<TpmResourceManager, gatekeeper::GateKeeper,
                 oemlock::OemLock, keymaster::KeymasterEnforcement>
SecureEnvComponent() {
  return fruit::createComponent()
      .registerProvider([]() -> Tpm* {  // fruit will take ownership
        if (FLAGS_tpm_impl == "in_memory") {
          return new InProcessTpm();
        } else if (FLAGS_tpm_impl == "host_device") {
          return new DeviceTpm("/dev/tpm0");
        } else {
          LOG(FATAL) << "Unknown TPM implementation: " << FLAGS_tpm_impl;
          abort();
        }
      })
      .registerProvider([](Tpm* tpm) {
        if (tpm->TctiContext() == nullptr) {
          LOG(FATAL) << "Unable to connect to TPM implementation.";
        }
        ESYS_CONTEXT* esys_ptr = nullptr;
        std::unique_ptr<ESYS_CONTEXT, void (*)(ESYS_CONTEXT*)> esys(
            nullptr, [](ESYS_CONTEXT* esys) { Esys_Finalize(&esys); });
        auto rc = Esys_Initialize(&esys_ptr, tpm->TctiContext(), nullptr);
        if (rc != TPM2_RC_SUCCESS) {
          LOG(FATAL) << "Could not initialize esys: " << Tss2_RC_Decode(rc)
                     << " (" << rc << ")";
        }
        esys.reset(esys_ptr);
        return esys;
      })
      .registerProvider(
          [](std::unique_ptr<ESYS_CONTEXT, void (*)(ESYS_CONTEXT*)>& esys) {
            return new TpmResourceManager(
                esys.get());  // fruit will take ownership
          })
      .registerProvider([](TpmResourceManager& resource_manager) {
        return new FragileTpmStorage(resource_manager, "gatekeeper_secure");
      })
      .registerProvider([](TpmResourceManager& resource_manager) {
        return new InsecureFallbackStorage(resource_manager,
                                           "gatekeeper_insecure");
      })
      .registerProvider([](TpmResourceManager& resource_manager,
                           FragileTpmStorage& secure_storage,
                           InsecureFallbackStorage& insecure_storage) {
        return new TpmGatekeeper(resource_manager, secure_storage,
                                 insecure_storage);
      })
      .registerProvider([]() { return new gatekeeper::SoftGateKeeper(); })
      .install(ChooseGatekeeperComponent)
      .install(ChooseOemlockComponent);
}

}  // namespace

int SecureEnvMain(int argc, char** argv) {
  DefaultSubprocessLogging(argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  keymaster::SoftKeymasterLogger km_logger;

  fruit::Injector<TpmResourceManager, gatekeeper::GateKeeper,
                  oemlock::OemLock, keymaster::KeymasterEnforcement>
      injector(SecureEnvComponent);
  TpmResourceManager* resource_manager = injector.get<TpmResourceManager*>();
  gatekeeper::GateKeeper* gatekeeper = injector.get<gatekeeper::GateKeeper*>();
  oemlock::OemLock* oemlock = injector.get<oemlock::OemLock*>();
  keymaster::KeymasterEnforcement* keymaster_enforcement =
      injector.get<keymaster::KeymasterEnforcement*>();
  std::unique_ptr<keymaster::KeymasterContext> keymaster_context;
  std::unique_ptr<keymaster::AndroidKeymaster> keymaster;

  std::vector<std::thread> threads;

  int security_level;
  if (FLAGS_keymint_impl == "software") {
    security_level = KM_SECURITY_LEVEL_SOFTWARE;
  } else if (FLAGS_keymint_impl == "tpm") {
    security_level = KM_SECURITY_LEVEL_TRUSTED_ENVIRONMENT;
  } else {
    LOG(FATAL) << "Unknown keymint implementation " << FLAGS_keymint_impl;
    return -1;
  }

  // The guest image may have either the C++ implementation of
  // KeyMint/Keymaster, xor the Rust implementation of KeyMint.  Those different
  // implementations each need to have a matching TA implementation in
  // secure_env, but they use distincts ports (/dev/hvc3 for C++, /dev/hvc11 for
  // Rust) so start threads for *both* TA implementations -- only one of them
  // will receive any traffic from the guest.

  // Start the Rust reference implementation of KeyMint.
  LOG(INFO) << "starting Rust KeyMint TA implementation in a thread";

  int keymint_in = FLAGS_keymint_fd_in;
  int keymint_out = FLAGS_keymint_fd_out;
  TpmResourceManager* rm = resource_manager;
  threads.emplace_back([rm, keymint_in, keymint_out, security_level]() {
    kmr_ta_main(keymint_in, keymint_out, security_level, rm);
  });

  // Start the C++ reference implementation of KeyMint.
  LOG(INFO) << "starting C++ KeyMint implementation in a thread with FDs in="
            << FLAGS_keymaster_fd_in << ", out=" << FLAGS_keymaster_fd_out;
  if (security_level == KM_SECURITY_LEVEL_SOFTWARE) {
    keymaster_context.reset(new keymaster::PureSoftKeymasterContext(
        keymaster::KmVersion::KEYMINT_3, KM_SECURITY_LEVEL_SOFTWARE));
  } else if (security_level == KM_SECURITY_LEVEL_TRUSTED_ENVIRONMENT) {
    keymaster_context.reset(
        new TpmKeymasterContext(*resource_manager, *keymaster_enforcement));
  } else {
    LOG(FATAL) << "Unknown keymaster security level " << security_level
               << " for " << FLAGS_keymint_impl;
    return -1;
  }
  // keymaster::AndroidKeymaster puts the context pointer into a UniquePtr,
  // taking ownership.
  keymaster.reset(new keymaster::AndroidKeymaster(
      new ProxyKeymasterContext(*keymaster_context), kOperationTableSize,
      keymaster::MessageVersion(keymaster::KmVersion::KEYMINT_3,
                                0 /* km_date */)));

  auto keymaster_in = DupFdFlag(FLAGS_keymaster_fd_in);
  auto keymaster_out = DupFdFlag(FLAGS_keymaster_fd_out);
  keymaster::AndroidKeymaster* borrowed_km = keymaster.get();
  threads.emplace_back([keymaster_in, keymaster_out, borrowed_km]() {
    while (true) {
      SharedFdKeymasterChannel keymaster_channel(keymaster_in, keymaster_out);

      KeymasterResponder keymaster_responder(keymaster_channel, *borrowed_km);

      while (keymaster_responder.ProcessMessage()) {
      }
    }
  });

  auto gatekeeper_in = DupFdFlag(FLAGS_gatekeeper_fd_in);
  auto gatekeeper_out = DupFdFlag(FLAGS_gatekeeper_fd_out);
  threads.emplace_back([gatekeeper_in, gatekeeper_out, &gatekeeper]() {
    while (true) {
      SharedFdGatekeeperChannel gatekeeper_channel(gatekeeper_in,
                                                   gatekeeper_out);

      GatekeeperResponder gatekeeper_responder(gatekeeper_channel, *gatekeeper);

      while (gatekeeper_responder.ProcessMessage()) {
      }
    }
  });

  auto oemlock_in = DupFdFlag(FLAGS_oemlock_fd_in);
  auto oemlock_out = DupFdFlag(FLAGS_oemlock_fd_out);
  threads.emplace_back([oemlock_in, oemlock_out, &oemlock]() {
    while (true) {
      secure_env::SharedFdChannel channel(oemlock_in, oemlock_out);
      oemlock::OemLockResponder responder(channel, *oemlock);
      while (responder.ProcessMessage().ok()) {
      }
    }
  });

  auto confui_server_fd = DupFdFlag(FLAGS_confui_server_fd);
  threads.emplace_back([confui_server_fd, resource_manager]() {
    ConfUiSignServer confui_sign_server(*resource_manager, confui_server_fd);
    // no return, infinite loop
    confui_sign_server.MainLoop();
  });

  auto kernel_events_fd = DupFdFlag(FLAGS_kernel_events_fd);
  threads.emplace_back(StartKernelEventMonitor(kernel_events_fd));

  for (auto& t : threads) {
    t.join();
  }

  return 0;
}

}  // namespace cuttlefish

int main(int argc, char** argv) {
  return cuttlefish::SecureEnvMain(argc, argv);
}
