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

#ifndef CEPH_RGW_DRIVER_D4N_BATCH_FDB_H
#define CEPH_RGW_DRIVER_D4N_BATCH_FDB_H

/* FoundationDB adapter for the general Batch contract. It relies on the public
 * result-reporting commit API so definitely-uncommitted failures remain
 * distinguishable from maybe-committed failures. A transaction body is never
 * split, even if it approaches an FDB size limit.
 */

#include "driver/d4n/batch.h"

#ifdef WITH_RADOSGW_FDB

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "rgw/ceph_fdb.h"

namespace rgw::d4n::batch {

struct fdb_options final {
  ::std::chrono::milliseconds transaction_timeout = ::std::chrono::seconds {3};
};

namespace detail {

struct fdb_error_category_impl final : ::std::error_category {
  const char *name() const noexcept override
  {
    return "foundationdb";
  }

  ::std::string message(const int error) const override
  {
    return 0 < error ? fdb_get_error(error) : "unknown FoundationDB error";
  }
};

} // namespace detail

inline const ::std::error_category&
fdb_category() noexcept
{
  static const detail::fdb_error_category_impl category;
  return category;
}

namespace detail {

inline backend_error
make_fdb_error(const fdb_error_t error)
{
  const ::std::error_code code {error, fdb_category()};

  return {
      .code = code,
      .diagnostic = code.message()
  };
}

inline bool
fdb_error_matches(
    const FDBErrorPredicate predicate,
    const fdb_error_t error) noexcept
{
  return 0 < error && 0 != fdb_error_predicate(predicate, error);
}

inline attempt_result
classify_fdb_commit_error(const fdb_error_t error)
{
  if (fdb_error_matches(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, error)) {
    return {
        .observed_effect = effect::unknown,
        .failure_class = failure::transient,
        .error = make_fdb_error(error)
    };
  }

  if (fdb_error_matches(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, error)) {
    return {
        .observed_effect = effect::not_applied,
        .failure_class = failure::transient,
        .error = make_fdb_error(error)
    };
  }

  return {
      .observed_effect = effect::unknown,
      .failure_class = failure::permanent,
      .error = make_fdb_error(error)
  };
}

inline attempt_result
classify_fdb_body_error(const fdb_error_t error)
{
  const auto retryable = fdb_error_matches(FDB_ERROR_PREDICATE_RETRYABLE, error);

  return {
      .observed_effect = effect::not_applied,
      .failure_class = retryable ? failure::transient : failure::permanent,
      .error = make_fdb_error(error)
  };
}

inline ::ceph::libfdb::transaction_options
make_fdb_transaction_options(const fdb_options& options)
{
  if (::std::chrono::milliseconds {0} >= options.transaction_timeout) {
    throw ::std::invalid_argument {
        "FDB batch transaction timeout must be positive"};
  }

  return {
      {FDB_TR_OPTION_TIMEOUT,
       static_cast<::std::int64_t>(options.transaction_timeout.count())}
  };
}

template <typename TransactionT, typename ApplyT>
class basic_fdb_session final {
private:
  ::ceph::libfdb::database_handle database;
  ::ceph::libfdb::transaction_options transaction_options;

  const TransactionT& specification;
  ApplyT apply_transaction;

  ::ceph::libfdb::transaction_handle transaction_handle;
  bool transaction_needs_replacement = false;

  [[nodiscard]] ::ceph::libfdb::transaction_handle make_transaction() const
  {
    return ::ceph::libfdb::make_transaction(database, transaction_options);
  }

public:
  basic_fdb_session(
      ::ceph::libfdb::database_handle dbh,
      ::ceph::libfdb::transaction_options options,
      const TransactionT& transaction,
      ApplyT apply_function) :
    database(::std::move(dbh)),
    transaction_options(::std::move(options)),
    specification(transaction),
    apply_transaction(::std::move(apply_function)),
    transaction_handle(make_transaction())
  {}

  attempt_result attempt(::optional_yield)
  {
    if (transaction_needs_replacement) {
      transaction_handle = make_transaction();
      transaction_needs_replacement = false;
    }

    try {
      ::std::invoke(apply_transaction, transaction_handle, specification);
    } catch (const ::ceph::libfdb::libfdb_exception& e) {
      const auto result = classify_fdb_body_error(e.fdb_error_value);

      if (failure::transient == result.failure_class) {
        try {
          ::ceph::libfdb::prepare_replay(transaction_handle, e.fdb_error_value);
        } catch (const ::ceph::libfdb::libfdb_exception& replay_error) {
          transaction_needs_replacement = true;
          return classify_fdb_body_error(replay_error.fdb_error_value);
        }
      }

      return result;
    }

    try {
      // A false result has already run FDB on_error(), so Batch must not
      // prepare this transaction a second time before a policy-driven retry.
      const auto commit =
          ::ceph::libfdb::commit(
              ::ceph::libfdb::with_result, transaction_handle);

      if (commit.committed) {
        return {
            .observed_effect = effect::applied,
            .failure_class = failure::none
        };
      }

      return classify_fdb_commit_error(commit.replay_error);
    } catch (const ::ceph::libfdb::libfdb_exception& e) {
      // A thrown commit-path error can invalidate the old handle. Replace it
      // lazily only if common policy explicitly retries this session.
      transaction_needs_replacement = true;
      return classify_fdb_commit_error(e.fdb_error_value);
    }
  }
};

struct apply_mutations final {
  void operator()(
      ::ceph::libfdb::transaction_handle& transaction_handle,
      const transaction_spec& specification) const
  {
    for (const auto& mutation : specification.mutations) {
      ::std::visit(
          [&transaction_handle](const auto& operation) {
            apply(transaction_handle, operation);
          },
          mutation);
    }
  }

private:
  static void apply(
      ::ceph::libfdb::transaction_handle& transaction_handle,
      const put& operation)
  {
    ::ceph::libfdb::set(transaction_handle, operation.key, operation.value);
  }

  static void apply(
      ::ceph::libfdb::transaction_handle& transaction_handle,
      const erase& operation)
  {
    ::ceph::libfdb::erase(transaction_handle, operation.key);
  }
};

} // namespace detail

class fdb_backend final {
private:
  ::ceph::libfdb::database_handle database;
  ::ceph::libfdb::transaction_options transaction_options;

public:
  explicit fdb_backend(
      ::ceph::libfdb::database_handle dbh,
      const fdb_options options = {}) :
    database(::std::move(dbh)),
    transaction_options(detail::make_fdb_transaction_options(options))
  {}

  auto begin(const transaction_spec& transaction, const limits&)
  {
    if (!database || !*database) {
      throw ::std::invalid_argument {
          "FDB batch requires an open database handle"};
    }

    return detail::basic_fdb_session {
        database, transaction_options, transaction, detail::apply_mutations {}};
  }
};

[[nodiscard]] inline auto
make_transactor(
    ::ceph::libfdb::database_handle dbh,
    const fdb_options options = {})
{
  return basic_transactor {fdb_backend {::std::move(dbh), options}};
}

} // namespace rgw::d4n::batch

#endif // WITH_RADOSGW_FDB

#endif // CEPH_RGW_DRIVER_D4N_BATCH_FDB_H
