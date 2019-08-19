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

// No include guards here because this file is expected to be included multiple times.

#ifdef YBC_CXX_DECLARATION_MODE
#include <mutex>

#include "yb/gutil/macros.h"
#include "yb/client/client_fwd.h"
#include "yb/client/transaction_manager.h"
#include "yb/common/clock.h"
#include "yb/gutil/ref_counted.h"
#include "yb/util/result.h"
#endif  // YBC_CXX_DECLARATION_MODE

#ifdef YBC_CXX_DECLARATION_MODE
namespace yb {
namespace pggate {
#endif  // YBC_CXX_DECLARATION_MODE

#define YBC_CURRENT_CLASS PgTxnManager

YBC_CLASS_START_REF_COUNTED_THREAD_SAFE

YBC_VIRTUAL_DESTRUCTOR

YBC_STATUS_METHOD(
    BeginTransaction,
    ((int, isolation))
    ((bool, deferrable))
)
YBC_STATUS_METHOD_NO_ARGS(CommitTransaction)
YBC_STATUS_METHOD_NO_ARGS(AbortTransaction)
YBC_STATUS_METHOD(
    SetIsolationLevel,
    ((int, isolation))
    ((bool, deferrable))
);

#ifdef YBC_CXX_DECLARATION_MODE
  PgTxnManager(client::AsyncClientInitialiser* async_client_init,
               scoped_refptr<ClockBase> clock);

  // Returns the transactional session, starting a new transaction if necessary.
  yb::Result<client::YBSession*> GetTransactionalSession();

  Status BeginWriteTransactionIfNecessary(bool read_only_op);
  Status RestartTransaction();

  bool CanRestart() { return can_restart_.load(std::memory_order_acquire); }
  void PreventRestart();

 private:

  client::TransactionManager* GetOrCreateTransactionManager();
  void ResetTxnAndSession();
  void StartNewSession();

  client::AsyncClientInitialiser* async_client_init_ = nullptr;
  scoped_refptr<ClockBase> clock_;

  bool txn_in_progress_ = false;
  client::YBTransactionPtr txn_;
  client::YBSessionPtr session_;

  std::atomic<client::TransactionManager*> transaction_manager_{nullptr};
  std::mutex transaction_manager_mutex_;
  std::unique_ptr<client::TransactionManager> transaction_manager_holder_;
  int isolation_level_ = 1;
  bool deferrable_ = false;

  std::atomic<bool> can_restart_{true};

  DISALLOW_COPY_AND_ASSIGN(PgTxnManager);
#endif  // YBC_CXX_DECLARATION_MODE

YBC_CLASS_END

#undef YBC_CURRENT_CLASS

#ifdef YBC_CXX_DECLARATION_MODE
}  // namespace pggate
}  // namespace yb
#endif  // YBC_CXX_DECLARATION_MODE
