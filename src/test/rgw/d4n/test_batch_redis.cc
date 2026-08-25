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

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/redis/config.hpp>
#include <boost/redis/resp3/type.hpp>
#include <boost/system/errc.hpp>

#include "driver/d4n/batch_redis.h"

namespace batch = ::rgw::d4n::batch;
namespace batch_detail = ::rgw::d4n::batch::detail;
namespace resp3 = ::boost::redis::resp3;

namespace {

void
add_node(
    batch_detail::redis_transaction_response& response,
    const ::std::size_t response_index,
    const resp3::type type,
    ::std::string value = {},
    const ::std::size_t depth = 0,
    const ::std::size_t aggregate_size = 0)
{
  response.nodes.push_back({
      .response_index = response_index,
      .data_type = type,
      .aggregate_size = aggregate_size,
      .depth = depth,
      .value = ::std::move(value)
  });
}

batch_detail::redis_transaction_response
successful_response()
{
  batch_detail::redis_transaction_response response;
  add_node(response, 0, resp3::type::simple_string, "OK");
  add_node(response, 1, resp3::type::simple_string, "QUEUED");
  add_node(response, 2, resp3::type::simple_string, "QUEUED");
  add_node(response, 3, resp3::type::array, {}, 0, 2);
  add_node(response, 3, resp3::type::simple_string, "OK", 1);
  add_node(response, 3, resp3::type::number, "1", 1);
  return response;
}

batch::transaction_spec
put_and_erase()
{
  return batch::transaction(
      batch::put {"{object}:value", "data"}, batch::erase {"{object}:old"});
}

} // anonymous namespace

TEST_CASE("d4n Redis batch classifies complete EXEC success as applied")
{
  const auto result = batch_detail::classify_redis_response(
      successful_response(), put_and_erase());

  CHECK(batch::effect::applied == result.observed_effect);
  CHECK(batch::failure::none == result.failure_class);
  CHECK(!result.error.has_value());
}

TEST_CASE("d4n Redis batch classifies queue errors as not applied")
{
  auto response = successful_response();
  response.nodes.clear();
  add_node(response, 0, resp3::type::simple_string, "OK");
  add_node(
      response, 1, resp3::type::simple_error, "ERR wrong number of arguments");
  add_node(response, 2, resp3::type::simple_string, "QUEUED");
  add_node(
      response, 3, resp3::type::simple_error,
      "EXECABORT Transaction discarded because of previous errors.");

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::not_applied == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
  REQUIRE(result.error.has_value());
  CHECK(batch::redis_errc::command_error == result.error->code);
  CHECK("ERR wrong number of arguments" == result.error->diagnostic);
}

TEST_CASE("d4n Redis batch treats MULTI errors as unknown")
{
  auto response = successful_response();
  response.nodes.front() = {
      .response_index = 0,
      .data_type = resp3::type::simple_error,
      .value = "ERR MULTI calls can not be nested"
  };

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::unknown == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
  REQUIRE(result.error.has_value());
  CHECK(batch::redis_errc::command_error == result.error->code);
}

TEST_CASE("d4n Redis batch requires a queue error to trust EXECABORT")
{
  auto response = successful_response();
  response.nodes.resize(3);
  add_node(
      response, 3, resp3::type::simple_error,
      "EXECABORT Transaction discarded because of previous errors.");

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::unknown == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
}

TEST_CASE("d4n Redis batch requires EXECABORT after a queue error")
{
  auto response = successful_response();
  response.nodes[1] = {
      .response_index = 1,
      .data_type = resp3::type::simple_error,
      .value = "ERR queue failure"
  };

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::unknown == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
  REQUIRE(result.error.has_value());
  CHECK(
      "Redis EXEC did not abort after a queue error" ==
      result.error->diagnostic);
}

TEST_CASE("d4n Redis batch never hides nested runtime errors")
{
  auto response = successful_response();
  response.nodes.back() = {
      .response_index = 3,
      .data_type = resp3::type::simple_error,
      .depth = 1,
      .value = "ERR runtime failure"
  };

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::partially_applied == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
  REQUIRE(result.error.has_value());
  CHECK("ERR runtime failure" == result.error->diagnostic);
}

TEST_CASE("d4n Redis batch classifies null EXEC as safely retryable")
{
  auto response = successful_response();
  response.nodes.resize(3);
  add_node(response, 3, resp3::type::null);

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::not_applied == result.observed_effect);
  CHECK(batch::failure::transient == result.failure_class);
  REQUIRE(result.error.has_value());
  CHECK(batch::redis_errc::transaction_not_applied == result.error->code);
}

TEST_CASE("d4n Redis batch treats malformed responses as unknown")
{
  auto response = successful_response();
  response.nodes.pop_back();

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::unknown == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
  REQUIRE(result.error.has_value());
  CHECK(batch::redis_errc::malformed_response == result.error->code);
  CHECK("incomplete Redis EXEC results" == result.error->diagnostic);
}

TEST_CASE("d4n Redis batch preserves Boost transport errors as standard errors")
{
  const auto source = ::boost::system::errc::make_error_code(
      ::boost::system::errc::connection_reset);
  const auto error = batch_detail::make_redis_transport_error(source);

  CHECK(static_cast<::std::error_code>(source) == error.code);
  CHECK(::std::errc::connection_reset == error.code);
  CHECK(source.message() == error.diagnostic);
  CHECK(batch::redis_category() != error.code.category());
}

TEST_CASE("d4n Redis batch rejects malformed top-level responses")
{
  auto response = successful_response();

  SECTION("missing queued command")
  {
    response.nodes.erase(::std::next(::std::begin(response.nodes), 2));
  }

  SECTION("incorrect EXEC aggregate size")
  {
    response.nodes[3].aggregate_size = 1;
  }

  SECTION("incorrect MULTI acknowledgement")
  {
    response.nodes.front().value = "QUEUED";
  }

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::unknown == result.observed_effect);
  CHECK(batch::failure::permanent == result.failure_class);
  REQUIRE(result.error.has_value());
}

TEST_CASE("d4n Redis batch validates mutation result types")
{
  auto response = successful_response();
  response.nodes.back().data_type = resp3::type::simple_string;
  response.nodes.back().value = "OK";

  const auto result =
      batch_detail::classify_redis_response(response, put_and_erase());

  CHECK(batch::effect::unknown == result.observed_effect);
  REQUIRE(result.error.has_value());
  CHECK("unexpected Redis mutation result" == result.error->diagnostic);
}

TEST_CASE("d4n Redis batch implements cluster hash tags")
{
  CHECK(0x31C3 == batch_detail::redis_crc16("123456789"));
  CHECK(12182 == batch_detail::redis_slot("foo"));
  CHECK(
      batch_detail::redis_slot("{user1000}.followers") ==
      batch_detail::redis_slot("{user1000}.following"));
  CHECK("foo {}{bar}" == batch_detail::redis_hash_key("foo {}{bar}"));
  CHECK("{bar" == batch_detail::redis_hash_key("foo {{bar}}zap"));

  const auto same_slot = batch::transaction(
      batch::put {"{bucket}:one", "1"}, batch::erase {"{bucket}:two"});
  const auto different_slots = batch::transaction(
      batch::put {"{first}:one", "1"}, batch::erase {"{second}:two"});

  CHECK(batch_detail::one_redis_slot(same_slot));
  CHECK_FALSE(batch_detail::one_redis_slot(different_slots));
}

TEST_CASE(
    "d4n Redis batch rejects cross-slot work before acquiring a connection")
{
  ::boost::asio::io_context io;
  ::boost::redis::config config;
  ::rgw::d4n::RedisPool pool {&io, config, 0};
  auto txr = batch::make_transactor(pool);
  batch::plan work {batch::transaction(
      batch::put {"{first}:one", "1"}, batch::erase {"{second}:two"})};

  CHECK_THROWS_AS(txr(work), ::std::invalid_argument);
  CHECK(1 == ::std::size(work));
}
