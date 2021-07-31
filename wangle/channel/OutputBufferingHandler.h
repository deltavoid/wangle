/*
 * Copyright 2017-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/futures/SharedPromise.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <wangle/channel/Handler.h>

namespace wangle {

/*
 * OutputBufferingHandler buffers writes in order to minimize syscalls. The
 * transport will be written to once per event loop instead of on every write.
 *
 * This handler may only be used in a single Pipeline.
 */
class OutputBufferingHandler : public OutboundBytesToBytesHandler,
                               protected folly::EventBase::LoopCallback {
 public:
  folly::Future<folly::Unit> write(
      Context* ctx,
      std::unique_ptr<folly::IOBuf> buf) override 
  {

    DLOG(INFO) << "wangle::OutputBufferingHandler::write: 1";
    CHECK(buf);
    if (!queueSends_) {

      DLOG(INFO) << "wangle::OutputBufferingHandler::write: 2";
      // return ctx->fireWrite(std::move(buf));
      auto ret = ctx->fireWrite(std::move(buf));
      
      DLOG(INFO) << "wangle::OutputBufferingHandler::write: 2.1, end";
      return ret;
    } else {

      DLOG(INFO) << "wangle::OutputBufferingHandler::write: 3";
      // Delay sends to optimize for fewer syscalls
      if (!sends_) {

        DLOG(INFO) << "wangle::OutputBufferingHandler::write: 4";
        DCHECK(!isLoopCallbackScheduled());
        // Buffer all the sends, and call writev once per event loop.
        sends_ = std::move(buf);
        ctx->getTransport()->getEventBase()->runInLoop(this);
      } else {

        DLOG(INFO) << "wangle::OutputBufferingHandler::write: 5";
        DCHECK(isLoopCallbackScheduled());
        sends_->prependChain(std::move(buf));
      }

      DLOG(INFO) << "wangle::OutputBufferingHandler::write: 6, end";
      return sharedPromise_.getFuture();
    }

    // DLOG(INFO) << "wangle::OutputBufferingHandler::write: 7, end";
  }

  void runLoopCallback() noexcept override 
  {

    DLOG(INFO) << "wangle::OutputBufferingHandler::runLoopCallback: 1";
    folly::SharedPromise<folly::Unit> sharedPromise;
    std::swap(sharedPromise, sharedPromise_);

    DLOG(INFO) << "wangle::OutputBufferingHandler::runLoopCallback: 2";
    getContext()
        ->fireWrite(std::move(sends_))
        .then([sharedPromise = std::move(sharedPromise)](
            folly::Try<folly::Unit> t) mutable {

          DLOG(INFO) << "wangle::OutputBufferingHandler::runLoopCallback: 3";
          sharedPromise.setTry(std::move(t));
        });

    DLOG(INFO) << "wangle::OutputBufferingHandler::runLoopCallback: 4, end";
  }

  void cleanUp() {
    if (isLoopCallbackScheduled()) {
      cancelLoopCallback();
    }

    sends_.reset();
    sharedPromise_ = folly::SharedPromise<folly::Unit>();
  }

  folly::Future<folly::Unit> close(Context* ctx) override {
    if (isLoopCallbackScheduled()) {
      cancelLoopCallback();
    }

    // If there are sends queued, cancel them
    sharedPromise_.setException(
      folly::make_exception_wrapper<std::runtime_error>(
        "close() called while sends still pending"));
    sends_.reset();
    sharedPromise_ = folly::SharedPromise<folly::Unit>();
    return ctx->fireClose();
  }

  folly::SharedPromise<folly::Unit> sharedPromise_;
  std::unique_ptr<folly::IOBuf> sends_{nullptr};
  bool queueSends_{true};
};

} // namespace wangle
