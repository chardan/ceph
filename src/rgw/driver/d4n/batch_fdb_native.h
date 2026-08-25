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

#ifndef CEPH_RGW_DRIVER_D4N_BATCH_FDB_NATIVE_H
#define CEPH_RGW_DRIVER_D4N_BATCH_FDB_NATIVE_H

#include "driver/d4n/batch_fdb.h"

#ifdef WITH_RADOSGW_FDB

#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

/* Native libfdb transactions for operations outside Batch's general mutation
 * set. Callbacks may read and mutate the supplied transaction, but must not
 * commit it themselves. A callback and its captured state must tolerate replay
 * after a definitely-uncommitted failure. Retrying an unknown result still
 * requires explicit convergence policy for the whole callback transaction.
 */
namespace rgw::d4n::batch::fdb_native {

using transaction_handle = ::ceph::libfdb::transaction_handle;
using operation = ::std::move_only_function<void(transaction_handle&)>;

template <typename OperationT>
concept transaction_operation =
    ::std::constructible_from<::std::decay_t<OperationT>, OperationT&&> &&
    ::std::invocable<::std::decay_t<OperationT>&, transaction_handle&>;

struct transaction_spec final {
  mutable operation execute;
  unknown_result_policy unknown_policy = unknown_result_policy::stop;
};

template <typename ...OperationTs>
  requires(
      sizeof...(OperationTs) > 0 && (transaction_operation<OperationTs> && ...))
[[nodiscard]] transaction_spec
transaction(unknown_result_policy policy, OperationTs&& ...operations)
{
  const auto operation_present = []<typename OperationT>(
      const OperationT& candidate) {
    if constexpr (
        ::std::same_as<::std::remove_cvref_t<OperationT>, operation>) {
      return static_cast<bool>(candidate);
    }

    return true;
  };

  if (!(operation_present(operations) && ...)) {
    throw ::std::invalid_argument {
        "native FDB batch transaction must not contain an empty operation"};
  }

  auto execute = [operations =
      ::std::tuple<::std::decay_t<OperationTs> ...> {
          ::std::forward<OperationTs>(operations)...}](
      transaction_handle& transaction_handle) mutable {
    ::std::apply(
        [&transaction_handle](auto& ...operation) {
          (::std::invoke(operation, transaction_handle), ...);
        },
        operations);
  };

  return {
      .execute = operation {::std::move(execute)},
      .unknown_policy = policy
  };
}

template <typename ...OperationTs>
  requires(
      sizeof...(OperationTs) > 0 && (transaction_operation<OperationTs> && ...))
[[nodiscard]] transaction_spec
transaction(OperationTs&& ...operations)
{
  return transaction(
      unknown_result_policy::stop,
      ::std::forward<OperationTs>(operations)...);
}

struct plan final {
private:
  ::std::deque<transaction_spec> transactions;

public:
  plan() = default;

  template <transaction_operation ...OperationTs>
    requires(sizeof...(OperationTs) > 0)
  explicit plan(OperationTs&& ...operations)
  {
    (emplace(::std::forward<OperationTs>(operations)), ...);
  }

  explicit plan(transaction_spec transaction)
  {
    emplace(::std::move(transaction));
  }

  template <transaction_operation OperationT>
  void emplace(OperationT&& operation)
  {
    emplace(transaction(::std::forward<OperationT>(operation)));
  }

  void emplace(transaction_spec transaction)
  {
    if (!transaction.execute) {
      throw ::std::invalid_argument {
          "native FDB batch transaction must not be empty"};
    }

    transactions.push_back(::std::move(transaction));
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return transactions.empty();
  }

  [[nodiscard]] ::std::size_t size() const noexcept
  {
    return ::std::size(transactions);
  }

  [[nodiscard]] const transaction_spec& front() const
  {
    if (empty()) {
      throw ::std::logic_error {
          "empty native FDB batch plan has no front transaction"};
    }

    return transactions.front();
  }

  // Advance only after the front transaction is applied or repaired.
  void acknowledge_front()
  {
    if (empty()) {
      throw ::std::logic_error {
          "empty native FDB batch plan has no front transaction to "
          "acknowledge"};
    }

    transactions.pop_front();
  }
};

namespace detail {

struct apply_transaction final {
  void operator()(
      transaction_handle& transaction_handle,
      const transaction_spec& specification) const
  {
    ::std::invoke(specification.execute, transaction_handle);
  }
};

class fdb_backend final {
private:
  ::ceph::libfdb::database_handle database;
  ::ceph::libfdb::transaction_options transaction_options;

public:
  explicit fdb_backend(
      ::ceph::libfdb::database_handle dbh,
      const fdb_options options = {}) :
    database(::std::move(dbh)),
    transaction_options(
        ::rgw::d4n::batch::detail::make_fdb_transaction_options(options))
  {}

  auto begin(const transaction_spec& transaction, const limits&)
  {
    if (!database || !*database) {
      throw ::std::invalid_argument {
          "native FDB batch requires an open database handle"};
    }

    return ::rgw::d4n::batch::detail::basic_fdb_session {
        database, transaction_options, transaction, apply_transaction {}};
  }
};

} // namespace detail

[[nodiscard]] inline auto
make_transactor(
    ::ceph::libfdb::database_handle dbh,
    const fdb_options options = {})
{
  return basic_transactor<detail::fdb_backend, plan> {
      detail::fdb_backend {::std::move(dbh), options}};
}

} // namespace rgw::d4n::batch::fdb_native

#endif // WITH_RADOSGW_FDB

#endif // CEPH_RGW_DRIVER_D4N_BATCH_FDB_NATIVE_H
