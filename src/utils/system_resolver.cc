// MIT License

// Copyright (c) 2017 Zhuhao Wang

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "nekit/utils/system_resolver.h"

#include "nekit/utils/boost_error.h"
#include "nekit/utils/error.h"
#include "nekit/utils/log.h"

#undef NECHANNEL
#define NECHANNEL "System resolver"

namespace nekit {
namespace utils {

SystemResolver::SystemResolver(boost::asio::io_context* io, size_t thread_count)
    : main_io_{io}, thread_count_{thread_count} {
  Reset();
}

Cancelable SystemResolver::Resolve(std::string domain,
                                   AddressPreference preference,
                                   EventHandler handler) {
  // Preference is ignored. Let the OS do the choice as of now.
  (void)preference;

  NETRACE << "Start resolving " << domain << ".";

  Cancelable cancelable{};

  boost::asio::post(*resolve_io_.get(), [this, domain, handler, cancelable,
                                         lifetime{lifetime_}]() mutable {
    // Note it should be guaranteed that one io_context should never be
    // released before all the instances implementing `AsyncIoInterface`
    // which will return that io_context are released. This resolver will
    // never be released before `main_io_` is released. In the destructor
    // this thread is guaranteed to exit before resolver is finished, so
    // there is no need to worry any thread issues here. Resolver and
    // `main_io_` will exist when this thread is running.

    if (cancelable.canceled() || lifetime.canceled()) {
      return;
    }

    auto resolver = boost::asio::ip::tcp::resolver(*resolve_io_.get());

    NEDEBUG << "Trying to resolve " << domain << ".";

    boost::system::error_code ec;
    auto result = resolver.resolve(domain, "", ec);

    if (ec) {
      auto error = ConvertBoostError(ec);
      NEERROR << "Failed to resolve " << domain << " due to " << error << ".";

      boost::asio::post(*main_io_,
                        [handler, error, cancelable, life_time{lifetime_}]() {
                          if (cancelable.canceled() || life_time.canceled()) {
                            return;
                          }

                          handler(nullptr, error);
                        });
      return;
    }

    auto addresses = std::make_shared<std::vector<boost::asio::ip::address>>();

    for (auto iter = result.begin(); iter != result.end(); iter++) {
      addresses->emplace_back(iter->endpoint().address());
    }

    NEINFO << "Successfully resolved domain " << domain << ".";

    boost::asio::post(*main_io_,
                      [handler, addresses, cancelable, life_time{lifetime_}]() {
                        if (cancelable.canceled() || life_time.canceled()) {
                          return;
                        }

                        handler(addresses, NEKitErrorCode::NoError);
                      });
  });

  return cancelable;
}

void SystemResolver::Stop() {
  work_guard_.reset();
  thread_group_.join_all();
  resolve_io_.reset();
}

void SystemResolver::Reset() {
  NEDEBUG << "Resetting system resolver";

  resolve_io_ = std::make_unique<boost::asio::io_context>();

  work_guard_ = std::make_unique<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
      boost::asio::make_work_guard(*resolve_io_));

  for (size_t i = 0; i < thread_count_; i++) {
    thread_group_.create_thread(
        boost::bind(&boost::asio::io_context::run, resolve_io_.get()));
  }
}

SystemResolver::~SystemResolver() {
  thread_group_.join_all();
  lifetime_.Cancel();
}

std::error_code SystemResolver::ConvertBoostError(
    const boost::system::error_code& ec) {
  if (ec.category() == boost::asio::error::system_category) {
    switch (ec.value()) {
      case boost::asio::error::basic_errors::operation_aborted:
        return NEKitErrorCode::Canceled;
    }
  }
  return std::make_error_code(ec);
}

boost::asio::io_context* SystemResolver::io() { return main_io_; }
}  // namespace utils
}  // namespace nekit
