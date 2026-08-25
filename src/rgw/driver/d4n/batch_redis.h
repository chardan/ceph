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

#ifndef CEPH_RGW_DRIVER_D4N_BATCH_REDIS_H
#define CEPH_RGW_DRIVER_D4N_BATCH_REDIS_H

/* Boost.Redis adapter for the general Batch contract. Each transaction uses
 * one request containing MULTI, its mutations, and EXEC. The custom response
 * adapter retains nested command errors so a runtime error is never mistaken
 * for atomic success. Transport failure after dispatch is conservatively
 * unknown, and all keys are required to share a Redis Cluster hash slot.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/async_result.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/resp3/node.hpp>
#include <boost/redis/resp3/type.hpp>
#include <boost/system/error_code.hpp>

#include "common/async/blocked_completion.h"
#include "driver/d4n/batch.h"
#include "driver/d4n/d4n_directory.h"

namespace rgw::d4n::batch {

enum struct redis_errc {
  client_error = 1,
  command_error,
  malformed_response,
  transaction_not_applied
};

namespace detail {

struct redis_error_category_impl final : ::std::error_category {
  const char *name() const noexcept override
  {
    return "redis";
  }

  ::std::string message(const int error) const override
  {
    switch (static_cast<redis_errc>(error)) {
      default:
        return "unknown Redis batch error";

      case redis_errc::client_error:
        return "Redis client error";

      case redis_errc::command_error:
        return "Redis transaction command error";

      case redis_errc::malformed_response:
        return "malformed Redis transaction response";

      case redis_errc::transaction_not_applied:
        return "Redis transaction was not applied";
    }
  }
};

} // namespace detail

inline const ::std::error_category&
redis_category() noexcept
{
  static const detail::redis_error_category_impl category;
  return category;
}

inline ::std::error_code
make_error_code(const redis_errc error) noexcept
{
  return {static_cast<int>(error), redis_category()};
}

} // namespace rgw::d4n::batch

namespace std {

template <>
struct is_error_code_enum<::rgw::d4n::batch::redis_errc> : true_type {};

} // namespace std

namespace rgw::d4n::batch::detail {

struct redis_response_node final {
  ::std::size_t response_index = 0;
  ::boost::redis::resp3::type data_type =
      ::boost::redis::resp3::type::invalid;
  ::std::size_t aggregate_size = 0;
  ::std::size_t depth = 0;
  ::std::string value;
};

struct redis_transaction_response final {
  ::std::vector<redis_response_node> nodes;
};

class redis_response_adapter final {
private:
  redis_transaction_response *response;

public:
  explicit redis_response_adapter(
      redis_transaction_response& transaction_response) :
    response(&transaction_response)
  {
    response->nodes.clear();
  }

  [[nodiscard]] ::std::size_t get_supported_response_size() const noexcept
  {
    return ::std::numeric_limits<::std::size_t>::max();
  }

  // Note: somewhat unfortunately, Boost.redis doesn't yet support std::error_code, but
  // we can of course convert from boost::system::error_code:
  template <typename StringT>
  void operator()(
      const ::std::size_t response_index,
      const ::boost::redis::resp3::basic_node<StringT>& node,
      ::boost::system::error_code&)
  {
    response->nodes.push_back({
        .response_index = response_index,
        .data_type = node.data_type,
        .aggregate_size = node.aggregate_size,
        .depth = node.depth,
        .value = ::std::string {node.value}
    });
  }
};

// Found by Boost.Redis through ADL. Unlike generic_response, this collector
// retains every nested error and leaves transport error reporting to async_exec.
inline auto
boost_redis_adapt(redis_transaction_response& response) noexcept
{
  return redis_response_adapter {response};
}

class redis_lease final {
private:
  ::rgw::d4n::RedisPool *pool;
  ::std::shared_ptr<::boost::redis::connection> redis_connection;

public:
  explicit redis_lease(::rgw::d4n::RedisPool& connection_pool) :
    pool(&connection_pool), redis_connection(connection_pool.acquire())
  {}

  redis_lease(const redis_lease&) = delete;
  redis_lease& operator=(const redis_lease&) = delete;
  redis_lease& operator=(redis_lease&&) = delete;

  redis_lease(redis_lease&& other) noexcept :
    pool(::std::exchange(other.pool, nullptr)),
    redis_connection(::std::move(other.redis_connection))
  {}

  ~redis_lease()
  {
    if (pool) {
      pool->release(::std::move(redis_connection));
    }
  }

  [[nodiscard]] const ::std::shared_ptr<::boost::redis::connection>&
  connection() const noexcept
  {
    return redis_connection;
  }
};

// Completion-token adapters may derive from an Asio initiation object.
struct redis_initiate_exec {
  ::std::shared_ptr<::boost::redis::connection> connection;

  using executor_type = ::boost::redis::connection::executor_type;

  [[nodiscard]] executor_type get_executor() const noexcept
  {
    return connection->get_executor();
  }

  template <typename Handler, typename ResponseT>
  void operator()(
      Handler handler,
      const ::boost::redis::request& request,
      ResponseT& response)
  {
    auto completion = ::boost::asio::consign(
        ::std::move(handler), connection);

    ::boost::asio::dispatch(
        get_executor(), [connection = connection, &request, &response,
                         completion = ::std::move(completion)]() mutable {
          connection->async_exec(
              request, response, ::std::move(completion));
        });
  }
};

template <typename ResponseT, typename CompletionTokenT>
auto
redis_async_exec(
    ::std::shared_ptr<::boost::redis::connection> connection,
    const ::boost::redis::request& request,
    ResponseT& response,
    CompletionTokenT&& token)
{
  return ::boost::asio::async_initiate<
      CompletionTokenT, void(::boost::system::error_code, ::std::size_t)>(
      redis_initiate_exec {::std::move(connection)}, token, request, response);
}

template <typename ResponseT>
::boost::system::error_code
redis_exec(
    ::std::shared_ptr<::boost::redis::connection> connection,
    const ::boost::redis::request& request,
    ResponseT& response,
    ::optional_yield y)
{
  ::boost::system::error_code error;

  if (y) {
    redis_async_exec(
        ::std::move(connection), request, response,
        y.get_yield_context()[error]);
    return error;
  }

  redis_async_exec(
      ::std::move(connection), request, response,
      ::ceph::async::use_blocked[error]);

  return error;
}

inline backend_error
make_redis_error(const redis_errc error, ::std::string diagnostic)
{
  return {
      .code = make_error_code(error),
      .diagnostic = ::std::move(diagnostic)
  };
}

inline backend_error
make_redis_transport_error(const ::boost::system::error_code& error)
{
  return {
      .code = static_cast<::std::error_code>(error),
      .diagnostic = error.message()
  };
}

inline attempt_result
malformed_redis_response(::std::string diagnostic)
{
  return {
      .observed_effect = effect::unknown,
      .failure_class = failure::permanent,
      .error = make_redis_error(
          redis_errc::malformed_response, ::std::move(diagnostic))
  };
}

inline bool
redis_error_type(const ::boost::redis::resp3::type type) noexcept
{
  return ::boost::redis::resp3::type::simple_error == type ||
         ::boost::redis::resp3::type::blob_error == type;
}

inline const redis_response_node *
redis_root(
    const redis_transaction_response& response,
    const ::std::size_t index)
{
  const auto found = ::std::ranges::find_if(
      response.nodes, [index](const redis_response_node& node) {
        return index == node.response_index && 0 == node.depth;
      });

  return ::std::end(response.nodes) == found ? nullptr : &*found;
}

inline ::std::vector<const redis_response_node *>
redis_exec_values(
    const redis_transaction_response& response,
    const ::std::size_t index)
{
  ::std::vector<const redis_response_node *> result;

  for (const auto& node : response.nodes) {
    if (index == node.response_index && 1 == node.depth) {
      result.push_back(&node);
    }
  }

  return result;
}

inline attempt_result
classify_redis_response(
    const redis_transaction_response& response,
    const transaction_spec& transaction)
{
  using ::boost::redis::resp3::type;

  const auto mutation_count = ::std::size(transaction.mutations);
  const auto exec_index = 1 + mutation_count;
  const auto *multi = redis_root(response, 0);

  if (!multi || type::simple_string != multi->data_type ||
      "OK" != multi->value) {
    if (multi && redis_error_type(multi->data_type)) {
      return {
          // Later commands may have run outside the intended transaction, or
          // on a connection that was already inside MULTI.
          .observed_effect = effect::unknown,
          .failure_class = failure::permanent,
          .error = make_redis_error(redis_errc::command_error, multi->value)
      };
    }

    return malformed_redis_response("malformed Redis MULTI response");
  }

  ::std::optional<::std::string> queue_error;
  for (::std::size_t index = 1; mutation_count >= index; ++index) {
    const auto *queued = redis_root(response, index);

    if (!queued) {
      return malformed_redis_response("missing Redis command queue response");
    }

    if (redis_error_type(queued->data_type)) {
      if (!queue_error) {
        queue_error = queued->value;
      }
      continue;
    }

    if (type::simple_string != queued->data_type || "QUEUED" != queued->value) {
      return malformed_redis_response("malformed Redis command queue response");
    }
  }

  const auto *exec = redis_root(response, exec_index);
  if (!exec) {
    return malformed_redis_response("missing Redis EXEC response");
  }

  if (queue_error) {
    if (!redis_error_type(exec->data_type) ||
        !exec->value.starts_with("EXECABORT")) {
      return malformed_redis_response(
          "Redis EXEC did not abort after a queue error");
    }

    return {
        .observed_effect = effect::not_applied,
        .failure_class = failure::permanent,
        .error = make_redis_error(
            redis_errc::command_error, ::std::move(*queue_error))
    };
  }

  if (type::null == exec->data_type) {
    return {
        .observed_effect = effect::not_applied,
        .failure_class = failure::transient,
        .error = make_redis_error(
            redis_errc::transaction_not_applied,
            "Redis transaction precondition changed")
    };
  }

  if (redis_error_type(exec->data_type)) {
    return {
        // A valid queue error was handled above. Any other EXEC error is not
        // enough evidence that the generated commands were never applied.
        .observed_effect = effect::unknown,
        .failure_class = failure::permanent,
        .error = make_redis_error(redis_errc::command_error, exec->value)
    };
  }

  if (type::array != exec->data_type || mutation_count != exec->aggregate_size) {
    return malformed_redis_response("malformed Redis EXEC aggregate");
  }

  const auto values = redis_exec_values(response, exec_index);
  if (mutation_count != ::std::size(values)) {
    return malformed_redis_response("incomplete Redis EXEC results");
  }

  for (const auto *value : values) {
    if (redis_error_type(value->data_type)) {
      return {
          .observed_effect = effect::partially_applied,
          .failure_class = failure::permanent,
          .error = make_redis_error(redis_errc::command_error, value->value)
      };
    }
  }

  for (::std::size_t index = 0; mutation_count > index; ++index) {
    const auto valid = ::std::visit(
        [value = values[index]](const auto& operation) {
          using operation_type =
              ::std::remove_cvref_t<decltype(operation)>;

          if constexpr (::std::same_as<put, operation_type>) {
            return type::simple_string == value->data_type &&
                   "OK" == value->value;
          }

          return type::number == value->data_type;
        },
        transaction.mutations[index]);

    if (!valid) {
      return malformed_redis_response("unexpected Redis mutation result");
    }
  }

  return {
      .observed_effect = effect::applied,
      .failure_class = failure::none
  };
}

inline ::std::string_view
redis_hash_key(const ::std::string_view key) noexcept
{
  const auto open = key.find('{');
  if (::std::string_view::npos == open) {
    return key;
  }

  const auto close = key.find('}', 1 + open);
  if (::std::string_view::npos == close || 1 + open == close) {
    return key;
  }

  return key.substr(1 + open, close - open - 1);
}

inline ::std::uint16_t
redis_crc16(const ::std::string_view value) noexcept
{
  ::std::uint16_t crc = 0;

  for (const auto byte : value) {
    crc ^= static_cast<::std::uint16_t>(
        static_cast<unsigned char>(byte) << 8);

    for (auto bit = 8u; 0 < bit; --bit) {
      crc = static_cast<::std::uint16_t>(
          0 != (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
  }

  return crc;
}

inline ::std::uint16_t
redis_slot(const ::std::string_view key) noexcept
{
  return redis_crc16(redis_hash_key(key)) % 16384;
}

inline ::std::string_view
mutation_key(const mutation& operation)
{
  return ::std::visit(
      [](const auto& value) -> ::std::string_view { return value.key; },
      operation);
}

inline bool
one_redis_slot(const transaction_spec& transaction)
{
  const auto first_slot =
      redis_slot(mutation_key(transaction.mutations.front()));

  return ::std::ranges::all_of(
      transaction.mutations, [first_slot](const mutation& operation) {
        return first_slot == redis_slot(mutation_key(operation));
      });
}

class redis_session final {
private:
  ::rgw::d4n::RedisPool *pool;
  const transaction_spec& specification;

public:
  redis_session(
      ::rgw::d4n::RedisPool& connection_pool,
      const transaction_spec& transaction) :
    pool(&connection_pool), specification(transaction)
  {}

  attempt_result attempt(::optional_yield y)
  {
    ::std::optional<redis_lease> lease;

    try {
      lease.emplace(*pool);
    } catch (const ::std::exception& e) {
      return {
          .observed_effect = effect::not_applied,
          .failure_class = failure::permanent,
          .error = make_redis_error(redis_errc::client_error, e.what())
      };
    }

    ::boost::redis::request request;
    request.get_config().cancel_if_unresponded = true;
    request.push("MULTI");

    for (const auto& mutation : specification.mutations) {
      ::std::visit(
          [&request](const auto& operation) {
            using operation_type =
                ::std::remove_cvref_t<decltype(operation)>;

            if constexpr (::std::same_as<put, operation_type>) {
              request.push("SET", operation.key, operation.value);
              return;
            }

            request.push("DEL", operation.key);
          },
          mutation);
    }

    request.push("EXEC");
    redis_transaction_response response;

    try {
      if (const auto error =
              redis_exec(lease->connection(), request, response, y);
          error) {
        return {
            .observed_effect = effect::unknown,
            .failure_class = failure::transient,
            .error = make_redis_transport_error(error)
        };
      }
    } catch (const ::std::exception& e) {
      // async_exec was initiated, so an exception cannot prove the request was
      // unsent. Retrying therefore requires explicit convergence policy.
      return {
          .observed_effect = effect::unknown,
          .failure_class = failure::transient,
          .error = make_redis_error(redis_errc::client_error, e.what())
      };
    }

    return classify_redis_response(response, specification);
  }
};

} // namespace rgw::d4n::batch::detail

namespace rgw::d4n::batch {

class redis_backend final {
private:
  ::std::shared_ptr<::rgw::d4n::RedisPool> owned_pool;
  ::rgw::d4n::RedisPool *pool;

public:
  explicit redis_backend(::rgw::d4n::RedisPool& connection_pool) :
    pool(&connection_pool)
  {}

  explicit redis_backend(
      ::std::shared_ptr<::rgw::d4n::RedisPool> connection_pool) :
    owned_pool(::std::move(connection_pool)), pool(owned_pool.get())
  {}

  detail::redis_session begin(
      const transaction_spec& transaction, const limits&)
  {
    if (!pool) {
      throw ::std::invalid_argument {
          "Redis batch requires a connection pool"};
    }

    // Redis Cluster can execute MULTI only when every key is in one hash slot.
    if (!detail::one_redis_slot(transaction)) {
      throw ::std::invalid_argument {
          "Redis batch transaction keys must share one cluster slot"};
    }

    return detail::redis_session {*pool, transaction};
  }
};

[[nodiscard]] inline auto
make_transactor(::rgw::d4n::RedisPool& pool)
{
  return basic_transactor {redis_backend {pool}};
}

[[nodiscard]] inline auto
make_transactor(::std::shared_ptr<::rgw::d4n::RedisPool> pool)
{
  return basic_transactor {redis_backend {::std::move(pool)}};
}

} // namespace rgw::d4n::batch

#endif // CEPH_RGW_DRIVER_D4N_BATCH_REDIS_H
