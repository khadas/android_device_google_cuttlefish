/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "host/commands/cvd/server.h"

#include <signal.h>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <fruit/fruit.h>

#include "cvd_server.pb.h"

#include "common/libs/fs/shared_buf.h"
#include "common/libs/fs/shared_fd.h"
#include "common/libs/fs/shared_select.h"
#include "common/libs/utils/files.h"
#include "common/libs/utils/flag_parser.h"
#include "common/libs/utils/result.h"
#include "common/libs/utils/shared_fd_flag.h"
#include "common/libs/utils/subprocess.h"
#include "host/commands/cvd/epoll_loop.h"
#include "host/commands/cvd/server_constants.h"
#include "host/libs/config/cuttlefish_config.h"
#include "host/libs/config/known_paths.h"

namespace cuttlefish {

static fruit::Component<> RequestComponent(CvdServer* server,
                                           InstanceManager* instance_manager) {
  return fruit::createComponent()
      .bindInstance(*server)
      .bindInstance(*instance_manager)
      .install(cvdCommandComponent)
      .install(cvdShutdownComponent)
      .install(cvdVersionComponent);
}

static constexpr int kNumThreads = 10;

CvdServer::CvdServer(EpollPool& epoll_pool, InstanceManager& instance_manager)
    : epoll_pool_(epoll_pool),
      instance_manager_(instance_manager),
      running_(true) {
  for (auto i = 0; i < kNumThreads; i++) {
    threads_.emplace_back([this]() {
      while (running_) {
        auto result = epoll_pool_.HandleEvent();
        if (!result.ok()) {
          LOG(ERROR) << "Epoll worker error:\n" << result.error();
        }
      }
      auto wakeup = BestEffortWakeup();
      CHECK(wakeup.ok()) << wakeup.error().message();
    });
  }
}

CvdServer::~CvdServer() {
  running_ = false;
  auto wakeup = BestEffortWakeup();
  CHECK(wakeup.ok()) << wakeup.error().message();
  Join();
}

Result<void> CvdServer::BestEffortWakeup() {
  // This attempts to cascade through the responder threads, forcing them
  // to wake up and see that running_ is false, then exit and wake up
  // further threads.
  auto eventfd = SharedFD::Event();
  CF_EXPECT(eventfd->IsOpen(), eventfd->StrError());
  CF_EXPECT(eventfd->EventfdWrite(1) == 0, eventfd->StrError());

  auto cb = [](EpollEvent) -> Result<void> { return {}; };
  CF_EXPECT(epoll_pool_.Register(eventfd, EPOLLIN, cb));
  return {};
}

void CvdServer::Stop() { running_ = false; }

void CvdServer::Join() {
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

static Result<CvdServerHandler*> RequestHandler(
    const RequestWithStdio& request,
    const std::vector<CvdServerHandler*>& handlers) {
  Result<cvd::Response> response;
  std::vector<CvdServerHandler*> compatible_handlers;
  for (auto& handler : handlers) {
    if (CF_EXPECT(handler->CanHandle(request))) {
      compatible_handlers.push_back(handler);
    }
  }
  CF_EXPECT(compatible_handlers.size() == 1,
            "Expected exactly one handler for message, found "
                << compatible_handlers.size());
  return compatible_handlers[0];
}

class ScopeGuard {
 public:
  ScopeGuard(std::function<void()> fn) : fn_(fn) {}
  ~ScopeGuard() {
    if (fn_) {
      fn_();
    }
  }
  void Cancel() { fn_ = nullptr; }

 private:
  std::function<void()> fn_;
};

Result<void> CvdServer::StartServer(SharedFD server_fd) {
  auto cb = [this](EpollEvent ev) -> Result<void> {
    CF_EXPECT(AcceptClient(ev));
    return {};
  };
  CF_EXPECT(epoll_pool_.Register(server_fd, EPOLLIN, cb));
  return {};
}

Result<void> CvdServer::AcceptClient(EpollEvent event) {
  ScopeGuard stop_on_failure([this] { Stop(); });

  CF_EXPECT(event.events & EPOLLIN);
  auto client_fd = SharedFD::Accept(*event.fd);
  CF_EXPECT(client_fd->IsOpen(), client_fd->StrError());
  auto client_cb = [this](EpollEvent ev) -> Result<void> {
    CF_EXPECT(HandleMessage(ev));
    return {};
  };
  CF_EXPECT(epoll_pool_.Register(client_fd, EPOLLIN, client_cb));

  auto self_cb = [this](EpollEvent ev) -> Result<void> {
    CF_EXPECT(AcceptClient(ev));
    return {};
  };
  CF_EXPECT(epoll_pool_.Register(event.fd, EPOLLIN, self_cb));

  stop_on_failure.Cancel();
  return {};
}

Result<void> CvdServer::HandleMessage(EpollEvent event) {
  ScopeGuard abandon_client([this, event] { epoll_pool_.Remove(event.fd); });

  if (event.events & EPOLLHUP) {  // Client went away.
    epoll_pool_.Remove(event.fd);
    return {};
  }

  CF_EXPECT(event.events & EPOLLIN);
  auto request = CF_EXPECT(GetRequest(event.fd));
  if (!request) {  // End-of-file / client went away.
    epoll_pool_.Remove(event.fd);
    return {};
  }

  fruit::Injector<> injector(RequestComponent, this, &instance_manager_);
  auto possible_handlers = injector.getMultibindings<CvdServerHandler>();

  // Even if the interrupt callback outlives the request handler, it'll only
  // hold on to this struct which will be cleaned out when the request handler
  // exits.
  struct SharedState {
    CvdServerHandler* handler;
    std::mutex mutex;
  };
  auto shared = std::make_shared<SharedState>();
  shared->handler = CF_EXPECT(RequestHandler(*request, possible_handlers));

  auto interrupt_cb = [shared](EpollEvent) -> Result<void> {
    std::lock_guard lock(shared->mutex);
    CF_EXPECT(shared->handler != nullptr);
    CF_EXPECT(shared->handler->Interrupt());
    return {};
  };
  CF_EXPECT(epoll_pool_.Register(event.fd, EPOLLHUP, interrupt_cb));

  auto response = CF_EXPECT(shared->handler->Handle(*request));
  CF_EXPECT(SendResponse(event.fd, response));

  {
    std::lock_guard lock(shared->mutex);
    shared->handler = nullptr;
  }
  CF_EXPECT(epoll_pool_.Remove(event.fd));  // Delete interrupt handler

  auto self_cb = [this](EpollEvent ev) -> Result<void> {
    CF_EXPECT(HandleMessage(ev));
    return {};
  };
  CF_EXPECT(epoll_pool_.Register(event.fd, EPOLLIN, self_cb));

  abandon_client.Cancel();
  return {};
}

static fruit::Component<CvdServer> ServerComponent() {
  return fruit::createComponent()
      .install(EpollLoopComponent);
}

static Result<int> CvdServerMain(int argc, char** argv) {
  android::base::InitLogging(argv, android::base::StderrLogger);

  LOG(INFO) << "Starting server";

  signal(SIGPIPE, SIG_IGN);

  std::vector<Flag> flags;
  SharedFD server_fd;
  flags.emplace_back(
      SharedFDFlag("server_fd", server_fd)
          .Help("File descriptor to an already created vsock server"));
  std::vector<std::string> args =
      ArgsToVec(argc - 1, argv + 1);  // Skip argv[0]
  CF_EXPECT(ParseFlags(flags, args));

  CF_EXPECT(server_fd->IsOpen(), "Did not receive a valid cvd_server fd");

  fruit::Injector<CvdServer> injector(ServerComponent);
  CvdServer& server = injector.get<CvdServer&>();
  server.StartServer(server_fd);
  server.Join();

  return 0;
}

}  // namespace cuttlefish

int main(int argc, char** argv) {
  auto res = cuttlefish::CvdServerMain(argc, argv);
  CHECK(res.ok()) << "cvd server failed: " << res.error().message();
  return *res;
}
