// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/yql/pggate/pggate.h"
#include "yb/util/status.h"

#include "yb/client/session.h"
#include "yb/client/transaction.h"

#include "yb/common/common.pb.h"

namespace yb {
namespace pggate {

using client::YBTransaction;
using client::AsyncClientInitialiser;
using client::TransactionManager;
using client::YBTransactionPtr;
using client::YBSession;
using client::YBSessionPtr;
using client::LocalTabletFilter;

// This should match XACT_SERIALIZABLE from xact.h.
constexpr int kSerializable = 3;

PgTxnManager::PgTxnManager(
    AsyncClientInitialiser* async_client_init,
    scoped_refptr<ClockBase> clock)
    : async_client_init_(async_client_init),
      clock_(std::move(clock)) {
}

PgTxnManager::~PgTxnManager() {
  // Abort the transaction before the transaction manager gets destroyed.
  if (txn_) {
    txn_->Abort();
  }
  ResetTxnAndSession();
}

Status PgTxnManager::BeginTransaction(int isolation_level, bool deferrable) {
  VLOG(2) << "BeginTransaction: txn_in_progress_=" << txn_in_progress_;
  if (txn_in_progress_) {
    return STATUS(IllegalState, "Transaction is already in progress");
  }
  ResetTxnAndSession();
  
  // TODO: Dedup this logic.
  isolation_level_ = deferrable ? XACT_REPEATABLE_READ : isolation_level;
  deferrable_ = deferrable;
  
  txn_in_progress_ = true;
  StartNewSession();
  return Status::OK();
}

Status PgTxnManager::SetIsolationLevel(int level, bool deferrable) {
  // TODO: handle READ ONLY at REPEATABLE READ level as well.
  // TODO: Dedup this logic.
  isolation_level_ = deferrable ? XACT_REPEATABLE_READ : level;
  deferrable_ = deferrable;
  
  return Status::OK();
}

void PgTxnManager::StartNewSession() {
  session_ = std::make_shared<YBSession>(async_client_init_->client(), clock_);
  session_->SetReadPoint(client::Restart::kFalse, client::Deferrable(deferrable_));
  session_->SetForceConsistentRead(client::ForceConsistentRead::kTrue);
}

Status PgTxnManager::BeginWriteTransactionIfNecessary(bool read_only_op) {
  VLOG(2) << "BeginWriteTransactionIfNecessary: txn_in_progress_="
          << txn_in_progress_;

  auto isolation = isolation_level_ == kSerializable
      ? IsolationLevel::SERIALIZABLE_ISOLATION : IsolationLevel::SNAPSHOT_ISOLATION;
  // Sanity check, query layer should ensure this does not happen.
  if (txn_ && txn_->isolation() != isolation) {
    return STATUS(IllegalState, "Changing txn isolation level in the middle of a transaction");
  }
  if (read_only_op && isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
    return Status::OK();
  }
  if (txn_) {
    return Status::OK();
  }
  txn_ = std::make_shared<YBTransaction>(GetOrCreateTransactionManager());
  if (session_ && isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
    txn_->InitWithReadPoint(isolation, std::move(*session_->read_point()));
  } else {
    RETURN_NOT_OK(txn_->Init(isolation));
  }
  if (!session_) {
    StartNewSession();
  }
  session_->SetTransaction(txn_);
  return Status::OK();
}

Status PgTxnManager::RestartTransaction() {
  if (!txn_in_progress_ || !txn_) {
    if (!session_->IsRestartRequired()) {
      return STATUS(IllegalState, "Attempted to restart when session does not require restart");
    }
    session_->SetReadPoint(client::Restart::kTrue);
    return Status::OK();
  }
  if (!txn_->IsRestartRequired()) {
    return STATUS(IllegalState, "Attempted to restart when transaction does not require restart");
  }
  txn_ = VERIFY_RESULT(txn_->CreateRestartedTransaction());
  session_->SetTransaction(txn_);

  DCHECK(can_restart_.load(std::memory_order_acquire));

  return Status::OK();
}

Status PgTxnManager::CommitTransaction() {
  if (!txn_in_progress_) {
    return Status::OK();
  }
  if (!txn_) {
    // This was a read-only transaction, nothing to commit.
    ResetTxnAndSession();
    return Status::OK();
  }
  Status status = txn_->CommitFuture().get();
  ResetTxnAndSession();
  return status;
}

Status PgTxnManager::AbortTransaction() {
  if (!txn_in_progress_) {
    return Status::OK();
  }
  if (!txn_) {
    // This was a read-only transaction, nothing to commit.
    ResetTxnAndSession();
    return Status::OK();
  }
  // TODO: how do we report errors if the transaction has already committed?
  txn_->Abort();
  ResetTxnAndSession();
  return Status::OK();
}

// TODO: dedup with similar logic in CQLServiceImpl.
// TODO: do we need lazy initialization of the txn manager?
TransactionManager* PgTxnManager::GetOrCreateTransactionManager() {
  auto result = transaction_manager_.load(std::memory_order_acquire);
  if (result) {
    return result;
  }
  std::lock_guard<decltype(transaction_manager_mutex_)> lock(transaction_manager_mutex_);
  if (transaction_manager_holder_) {
    return transaction_manager_holder_.get();
  }

  transaction_manager_holder_ = std::make_unique<client::TransactionManager>(
      async_client_init_->client(), clock_, LocalTabletFilter());

  transaction_manager_.store(transaction_manager_holder_.get(), std::memory_order_release);
  return transaction_manager_holder_.get();
}

Result<client::YBSession*> PgTxnManager::GetTransactionalSession() {
  if (!txn_in_progress_) {
    RETURN_NOT_OK(BeginTransaction(isolation_level_, deferrable_));
  }
  return session_.get();
}

void PgTxnManager::ResetTxnAndSession() {
  txn_in_progress_ = false;
  session_ = nullptr;
  txn_ = nullptr;
  can_restart_.store(true, std::memory_order_release);
}

void PgTxnManager::PreventRestart() {
  can_restart_.store(false, std::memory_order_release);
}

}  // namespace pggate
}  // namespace yb
