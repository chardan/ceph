// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- // vim: ts=8 sw=2 smarttab ft=cpp
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

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>

#include "driver/d4n/monitor.h"
#include "test/rgw/test_fdb_common.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#if !defined(WITH_RADOSGW_FDB) || defined(D4N_MONITOR_TEST_REDIS)
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include "common/async/blocked_completion.h"
#endif

namespace d4n = rgw::d4n;

static_assert(0 == d4n::observation{}.value);
static_assert(d4n::observation{.value = 7} == 7);
static_assert(d4n::observation{.value = 7} != 8);
static_assert(std::is_constructible_v<d4n::monitor, std::string>);

#if !defined(WITH_RADOSGW_FDB) || defined(D4N_MONITOR_TEST_REDIS)
static_assert(std::is_constructible_v<d4n::monitor_redis, std::string>);
#endif

#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)
static_assert(std::is_move_constructible_v<d4n::watched>);
static_assert(!std::is_copy_constructible_v<d4n::watched>);

static_assert(std::is_constructible_v<d4n::monitor_fdb, std::string>);
#endif

namespace {

[[nodiscard]] std::string monitor_test_key(std::string_view suffix)
{
  static const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
  static std::atomic<std::uint64_t> counter = 0;

  return "d4n/monitor/" + std::to_string(seed) + "-" +
    std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + "-" +
    std::string {suffix};
}

#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)
bool foundationdb_available()
{
  static const bool available = [] {
    try {
      const auto dbh = lfdb::create_database(lfdb::database_options {
        { FDB_DB_OPTION_TRANSACTION_TIMEOUT, std::int64_t {100} }
      });

      auto txn = lfdb::make_transaction(dbh);
      (void)d4n::monitor_fdb {monitor_test_key("monitor-availability-probe")}.current(txn);

      return lfdb::commit(txn);
    } catch (const lfdb::libfdb_exception&) {
      return false;
    }
  }();

  return available;
}

std::unique_ptr<janitor>& foundationdb_janitor_storage()
{
  static std::unique_ptr<janitor> j;

  return j;
}

janitor& foundationdb_janitor()
{
  auto& j = foundationdb_janitor_storage();
  if (!j) {
    j = std::make_unique<janitor>();
  }

  return *j;
}

#endif

#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)
#  define D4N_MONITOR_TEST_BACKENDS d4n::monitor_fdb
#  define D4N_MONITOR_HAS_REDIS_BACKEND 0
#else
#  define D4N_MONITOR_TEST_BACKENDS d4n::monitor_redis
#  define D4N_MONITOR_HAS_REDIS_BACKEND 1
#endif

template <typename BackendT>
struct monitor_backend_traits;

#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)
template <>
struct monitor_backend_traits<d4n::monitor_fdb> final {
  using monitor_type = d4n::monitor_fdb;
  using database_handle = lfdb::database_handle;
  using operation_handle = lfdb::transaction_handle;

  static constexpr bool supports_watch = true;

  static void skip_if_unavailable()
  {
    if (!foundationdb_available()) {
      SKIP("FoundationDB is not available for live D4N monitor tests");
    }
  }

  [[nodiscard]] static database_handle make_database()
  {
    return foundationdb_janitor().dbh();
  }

  [[nodiscard]] static operation_handle make_operation(database_handle& dbh)
  {
    return lfdb::make_transaction(dbh);
  }

  [[nodiscard]] static bool commit(operation_handle& op)
  {
    return lfdb::commit(op);
  }
};
#endif

#if D4N_MONITOR_HAS_REDIS_BACKEND
class redis_server final {
public:
  using database_handle = d4n::monitor_redis_backend::database_handle;

  redis_server()
  {
    const auto host = redis_host();
    const auto port = redis_port();

    config_ = boost::redis::config {};
    config_.addr.host = host;
    config_.addr.port = port;
    config_.reconnect_wait_interval = std::chrono::seconds::zero();

    dbh_ = std::make_shared<boost::redis::connection>(boost::asio::make_strand(io_));
    dbh_->async_run(config_, {}, boost::asio::detached);

    boost::system::error_code ec;
    boost::redis::request req;
    boost::redis::response<std::string> resp;
    boost::asio::steady_timer timeout {io_, std::chrono::seconds {1}};

    timeout.async_wait([dbh = dbh_](const boost::system::error_code& timer_ec) {
      if (!timer_ec) {
        dbh->cancel(boost::redis::operation::exec);
      }
    });

    runner_ = std::jthread{[this] { io_.run(); }};

    req.push("PING");
    dbh_->async_exec(req, resp, ceph::async::use_blocked[ec]);
    timeout.cancel();

    if (ec) {
      error_ = ec;
      return;
    }
  }

  redis_server(const redis_server&) = delete;
  redis_server(redis_server&&) = delete;
  redis_server& operator=(const redis_server&) = delete;
  redis_server& operator=(redis_server&&) = delete;

  ~redis_server()
  {
    if (dbh_) {
      dbh_->cancel();
    }

    io_.stop();
  }

  [[nodiscard]] bool available() const
  {
    return !error_;
  }

  [[nodiscard]] database_handle dbh() const
  {
    return dbh_;
  }

  [[nodiscard]] static redis_server& instance()
  {
    static redis_server server;
    return server;
  }

private:
  [[nodiscard]] static std::string redis_host()
  {
    if (const char* host = std::getenv("D4N_REDIS_HOST")) {
      return host;
    }

    return "127.0.0.1";
  }

  [[nodiscard]] static std::string redis_port()
  {
    if (const char* port = std::getenv("D4N_REDIS_PORT")) {
      return port;
    }

    return "6379";
  }

  boost::asio::io_context io_;
  std::jthread runner_;
  std::shared_ptr<boost::redis::connection> dbh_;
  boost::redis::config config_;
  boost::system::error_code error_;
};

template <>
struct monitor_backend_traits<d4n::monitor_redis> final {
  using monitor_type = d4n::monitor_redis;
  using database_handle = d4n::monitor_redis_backend::database_handle;
  using operation_handle = d4n::monitor_redis_backend::operation_handle;

  static constexpr bool supports_watch = false;

  static void skip_if_unavailable()
  {
    if (!redis_server::instance().available()) {
      SKIP("Redis monitor backend is not available for D4N monitor tests");
    }
  }

  [[nodiscard]] static database_handle make_database()
  {
    return redis_server::instance().dbh();
  }

  [[nodiscard]] static operation_handle make_operation(database_handle& dbh)
  {
    return operation_handle {dbh};
  }

  [[nodiscard]] static bool commit(operation_handle&)
  {
    return true;
  }
};
#endif

} // anonymous namespace

TEMPLATE_TEST_CASE("d4n monitor observations compare monitor counters", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  const d4n::observation observed {
    .value = 3,
  };

  CHECK(observed == 3);
  CHECK_FALSE(observed == 4);
}

TEMPLATE_TEST_CASE("d4n monitor records caller markers", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  using traits = monitor_backend_traits<TestType>;
  traits::skip_if_unavailable();

  const auto key = monitor_test_key("monitor-marker");
  auto dbh = traits::make_database();
  const typename traits::monitor_type object_state {key};

  auto observed = object_state.observe(dbh, 7);

  CHECK(0 == observed.value);
  CHECK(7 == observed.marker);
}

TEMPLATE_TEST_CASE("d4n monitor observes missing keys as zero", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  using traits = monitor_backend_traits<TestType>;
  traits::skip_if_unavailable();

  const auto key = monitor_test_key("monitor-missing");
  const typename traits::monitor_type object_state {key};
  auto dbh = traits::make_database();
  auto op = traits::make_operation(dbh);

  const auto observed = object_state.observe(op);

  CHECK(traits::commit(op));
  CHECK(0 == observed.value);
  CHECK_FALSE(object_state.changed(dbh, observed));
}

TEMPLATE_TEST_CASE("d4n monitor touch advances the logical epoch", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  using traits = monitor_backend_traits<TestType>;
  traits::skip_if_unavailable();

  auto dbh = traits::make_database();
  const auto key = monitor_test_key("monitor-touch");
  const typename traits::monitor_type object_state {key};

  object_state.touch(dbh);
  CHECK(object_state.current(dbh) == 1);
  object_state.touch(dbh);
  CHECK(object_state.current(dbh) == 2);
}

TEMPLATE_TEST_CASE("d4n monitor detects stale observed values", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  using traits = monitor_backend_traits<TestType>;
  traits::skip_if_unavailable();

  auto dbh = traits::make_database();
  const auto key = monitor_test_key("monitor-stale");
  const typename traits::monitor_type object_state {key};
  auto op = traits::make_operation(dbh);

  auto observed = object_state.observe(op);

  CHECK(traits::commit(op));
  CHECK_FALSE(object_state.changed(dbh, observed));

  object_state.touch(dbh);

  CHECK(object_state.changed(dbh, observed));
}

TEMPLATE_TEST_CASE("d4n monitor watches report observed state changes", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  using traits = monitor_backend_traits<TestType>;
  traits::skip_if_unavailable();

  if constexpr (!traits::supports_watch) {
    SKIP("Watch backend is not available for this monitor type");
  } else {
    auto dbh = traits::make_database();
    const auto key = monitor_test_key("monitor-watch");
    const typename traits::monitor_type object_state {key};
    auto watched = object_state.watch(dbh);

    CHECK(0 == watched.observed.value);
    CHECK_FALSE(object_state.changed(dbh, watched));

    object_state.touch(dbh);
    watched.watch.wait();

    CHECK(object_state.changed(dbh, watched));
  }
}

TEMPLATE_TEST_CASE("d4n monitor validation uses the caller transaction", "[d4n][monitor]",
                   D4N_MONITOR_TEST_BACKENDS)
{
  using traits = monitor_backend_traits<TestType>;
  traits::skip_if_unavailable();

  auto dbh = traits::make_database();
  const auto key = monitor_test_key("monitor-caller-transaction");
  const typename traits::monitor_type object_state {key};
  auto observed_op = traits::make_operation(dbh);
  const auto observed = object_state.observe(observed_op);

  CHECK(traits::commit(observed_op));
  object_state.touch(dbh);

  auto validating_op = traits::make_operation(dbh);

  // The final transaction decides whether stale state means retry, abort, or re-read.
  CHECK(object_state.changed(validating_op, observed));
}

int main(int argc, char *argv[])
{
  const auto result = Catch::Session().run(argc, argv);

#if defined(WITH_RADOSGW_FDB) && !defined(D4N_MONITOR_TEST_REDIS)
  foundationdb_janitor_storage().reset();
  ceph::libfdb::shutdown_libfdb();
#endif

  return result;
}
