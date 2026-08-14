/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 International Business Machines Corp. (IBM)
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

/* A D4N monitor records a monotonic database-backed value, captures and
 * validates observations of it. FoundationDB watches are also supported.
 *
 * Note that touch() records a change but doesn't perform the underlying
 * D4N mutation-- it should be called by the mutation path (for FDB, ideally
 * in the same transaction):
 *
 *
 * FoundationDB and Redis:
 *
 *   rgw::d4n::monitor object_monitor {"d4n/monitor/object"};
 *   auto observed = object_monitor.observe(dbh, cache_generation);
 *
 *   object_monitor.touch(dbh);
 *
 *   if (object_monitor.changed(dbh, observed))
 *     invalidate_d4n_thingie(observed.marker);
 *
 * FoundationDB can also wait for a change instead (more efficient for
 * repeated checks):
 *   auto watched = object_monitor.watch(dbh);
 *
 *   object_monitor.touch(dbh);
 *   watched.watch.wait();  // blocks
 *
 *   invalidate_d4n_thingie();
 *
 */

#ifndef CEPH_RGW_DRIVER_D4N_MONITOR_H
 #define CEPH_RGW_DRIVER_D4N_MONITOR_H

#include <memory>
#include <string>
#include <utility>
#include <tuple>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <concepts>

// Support for FoundationDB (via libfdb):
#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)
 #include "rgw/ceph_fdb.h"
#endif

// Support for Redis (via Boost.Redis):
#if !defined(WITH_RADOSGW_FDB) || defined(D4N_MONITOR_TEST_REDIS)
 #include "common/async/blocked_completion.h"

 #include <charconv>
 #include <optional>
 #include <system_error>

 #include <boost/asio/async_result.hpp>
 #include <boost/asio/consign.hpp>
 #include <boost/asio/dispatch.hpp>
 #include <boost/redis/connection.hpp>
 #include <boost/redis/request.hpp>
 #include <boost/redis/response.hpp>
 #include <boost/system/system_error.hpp>
#endif

namespace rgw::d4n {

/* Monitor observations protect logical D4N state, not individual storage keys.
 * Use the same monitor key for every operation that changes the observed state. */
struct observation final
{
  std::uint64_t value = 0;
  std::uint64_t marker = 0;

  // We really only care about equality, not other comparisons:
  constexpr bool operator==(const std::uint64_t current) const noexcept
  {
    return value == current;
  }
};

// A monitor observation plus a backend-specific watch:
template <typename WatchHandleT>
struct watched_value final
{
  observation observed;

  WatchHandleT watch;
};

template <typename BackendT>
class d4n_monitor final
{
 using backend = BackendT;

 std::string counter_key_;

 public:
  explicit d4n_monitor(const std::string_view key)
    : counter_key_(key)
  {}

  std::uint64_t current(typename backend::operation_handle op) const
  {
    return backend::current(op, counter_key_);
  }

  template <typename B = backend>
    requires (!std::same_as<typename B::database_handle,
                            typename B::operation_handle>)
  std::uint64_t current(typename B::database_handle dbh) const
  {
    return B::in_transaction(dbh, [this](auto& txn) {
                                    return current(txn); });
  }

  observation observe(typename backend::operation_handle op,
                      const std::uint64_t marker = 0) const
  {
    return { .value = current(op), .marker = marker };
  }

  template <typename B = backend>
    requires (!std::same_as<typename B::database_handle,
                            typename B::operation_handle>)
  observation observe(typename B::database_handle dbh,
                      const std::uint64_t marker = 0) const
  {
    return B::in_transaction(dbh, [this, marker](auto& txn) {
                                    return observe(txn, marker); });
  }

  bool changed(typename backend::operation_handle op,
               const observation& observed) const
  {
    return observed != current(op);
  }

  template <typename WatchHandleT>
  bool changed(typename backend::operation_handle op,
               const watched_value<WatchHandleT>& watched) const
  {
    return changed(op, watched.observed);
  }

  template <typename B = backend>
    requires (!std::same_as<typename B::database_handle,
                            typename B::operation_handle>)
  bool changed(typename B::database_handle dbh,
               const observation& observed) const
  {
    return B::in_transaction(dbh, [this, &observed](auto& txn) {
                                    return changed(txn, observed); });
  }

  template <typename WatchHandleT, typename B = backend>
    requires (!std::same_as<typename B::database_handle,
                            typename B::operation_handle>)
  bool changed(typename B::database_handle dbh,
               const watched_value<WatchHandleT>& watched) const
  {
    return changed(dbh, watched.observed);
  }

  void touch(typename backend::operation_handle op) const
  {
    backend::touch(op, counter_key_);
  }

  template <typename B = backend>
    requires (!std::same_as<typename B::database_handle,
                            typename B::operation_handle>)
  void touch(typename B::database_handle dbh) const
  {
    B::in_transaction(dbh, [this](auto& txn) {
                            return touch(txn); });
  }

  template <typename B = backend>
  auto watch(typename B::operation_handle op) const
    -> watched_value<typename B::watch_handle>
  {
    // Transaction callers must commit before waiting on the watch:
    return {
      .observed = observe(op),
      .watch = B::watch(op, counter_key_),
    };
  }

  template <typename B = backend>
    requires (!std::same_as<typename B::database_handle,
                            typename B::operation_handle>)
  auto watch(typename B::database_handle dbh) const
    -> watched_value<typename B::watch_handle>
  {
    return B::in_transaction(dbh, [this](auto& txn) { return watch(txn); });
  }
};

#if !defined(WITH_RADOSGW_FDB) || defined(D4N_MONITOR_TEST_REDIS)
/* Boost.Redis back-end for D4N Monitors: */
namespace detail {

struct monitor_redis_initiate_exec {
  std::shared_ptr<boost::redis::connection> dbh;

  using executor_type = boost::redis::connection::executor_type;

  executor_type get_executor() const noexcept
  {
    return dbh->get_executor();
  }

  template <typename Handler, typename Response>
  void operator()(Handler handler, const boost::redis::request& req, Response& resp)
  {
    auto h = boost::asio::consign(std::move(handler), dbh);

    return boost::asio::dispatch(get_executor(),
      [c = dbh, &req, &resp, h = std::move(h)]() mutable {
        return c->async_exec(req, resp, std::move(h));
      });
  }
};

template <typename Response, typename CompletionToken>
auto monitor_redis_async_exec(std::shared_ptr<boost::redis::connection> dbh,
                              const boost::redis::request& req,
                              Response& resp,
                              CompletionToken&& token)
{
  return boost::asio::async_initiate<CompletionToken,
         void(boost::system::error_code, std::size_t)>(
                monitor_redis_initiate_exec {std::move(dbh)}, token, req, resp);
}

template <typename... Types>
void monitor_redis_exec(std::shared_ptr<boost::redis::connection> dbh,
                        const boost::redis::request& req,
                        boost::redis::response<Types...>& resp)
{
  boost::system::error_code ec;

  monitor_redis_async_exec(std::move(dbh), req, resp, ceph::async::use_blocked[ec]);

  if (ec) {
    throw boost::system::system_error {ec};
  }
}

} // namespace detail

/* Redis back-end implementation: */
struct monitor_redis_backend final {

  using operation_handle = std::shared_ptr<boost::redis::connection>;
  using database_handle = std::shared_ptr<boost::redis::connection>;

  static std::uint64_t current(operation_handle op, const std::string_view key)
  {
    boost::redis::request req;
    boost::redis::response<std::optional<std::string>> resp;

    req.push("GET", key);
    detail::monitor_redis_exec(op, req, resp);

    const auto& current = std::get<0>(resp).value();
    if (!current) {
      return 0;
    }

    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(current->data(), current->data() + current->size(), value);

    if (ec != std::errc {} || ptr != current->data() + current->size()) {
      throw std::invalid_argument {"D4N counter is not an integer"};
    }

    return value;
  }

  static void touch(operation_handle op, const std::string_view key)
  {
    boost::redis::request req;
    boost::redis::response<boost::redis::ignore_t> resp;

    req.push("INCR", key);
    detail::monitor_redis_exec(op, req, resp);
  }
};

// Bind the interface to our Redis implementation:
using monitor_redis = d4n_monitor<monitor_redis_backend>;
using monitor = monitor_redis;

#endif // !WITH_RADOSGW_FDB || D4N_MONITOR_TEST_REDIS

/* FoundationDB back-end for D4N Monitors: */
#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)

struct monitor_fdb_backend final {
  using operation_handle = ceph::libfdb::transaction_handle;
  using database_handle = ceph::libfdb::database_handle;
  using watch_handle = ceph::libfdb::watch_handle;

  static std::uint64_t current(operation_handle txn, const std::string_view key)
  {
    std::uint64_t value {};

    std::ignore = ceph::libfdb::get(txn, key, value);

    return value;
  }

  static watch_handle watch(operation_handle txn, const std::string_view key)
  {
    return ceph::libfdb::make_watch(txn, key);
  }

  static void touch(operation_handle txn, const std::string_view key)
  {
    ceph::libfdb::atomic::add(txn, key, std::uint64_t {1});
  }

  template <typename FnT>
  static decltype(auto) in_transaction(database_handle dbh, FnT&& fn)
  {
    return ceph::libfdb::make_transactor(dbh)(std::forward<FnT>(fn));
  }
};

// ...and, now, bind the implementation:
using monitor_fdb = d4n_monitor<monitor_fdb_backend>;
using watched = watched_value<monitor_fdb_backend::watch_handle>;
using monitor = monitor_fdb;

#endif // WITH_RADOSGW_FDB && !D4N_MONITOR_TEST_REDIS

} // namespace rgw::d4n

#endif // CEPH_RGW_DRIVER_D4N_MONITOR_H
