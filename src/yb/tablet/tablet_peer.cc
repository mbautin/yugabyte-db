// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include "yb/tablet/tablet_peer.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include "yb/consensus/consensus.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/retryable_requests.h"
#include "yb/consensus/consensus_util.h"

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb.h"

#include "yb/gutil/mathlimits.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/sysinfo.h"

#include "yb/gutil/thread_annotations.h"
#include "yb/rocksdb/db/memtable.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/strand.h"
#include "yb/rpc/thread_pool.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet.pb.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer_mm_ops.h"
#include "yb/tablet/tablet_retention_policy.h"

#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/operations/history_cutoff_operation.h"
#include "yb/tablet/operations/operation_driver.h"
#include "yb/tablet/operations/split_operation.h"
#include "yb/tablet/operations/truncate_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"

#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/stopwatch.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

using namespace std::literals;
using namespace std::placeholders;
using std::shared_ptr;
using std::string;

DEFINE_test_flag(int32, delay_init_tablet_peer_ms, 0,
                 "Wait before executing init tablet peer for specified amount of milliseconds.");

DEFINE_int32(cdc_min_replicated_index_considered_stale_secs, 900,
    "If cdc_min_replicated_index hasn't been replicated in this amount of time, we reset its"
    "value to max int64 to avoid retaining any logs");

DEFINE_bool(propagate_safe_time, true, "Propagate safe time to read from leader to followers");

namespace yb {
namespace tablet {

METRIC_DEFINE_histogram(table, op_prepare_queue_length, "Operation Prepare Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be prepared within this tablet. "
                        "High queue lengths indicate that the server is unable to process "
                        "operations as fast as they are being written to the WAL.",
                        10000, 2);

METRIC_DEFINE_histogram(table, op_prepare_queue_time, "Operation Prepare Queue Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent waiting in the prepare queue before being "
                        "processed. High queue times indicate that the server is unable to "
                        "process operations as fast as they are being written to the WAL.",
                        10000000, 2);

METRIC_DEFINE_histogram(table, op_prepare_run_time, "Operation Prepare Run Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent being prepared in the tablet. "
                        "High values may indicate that the server is under-provisioned or "
                        "that operations are experiencing high contention with one another for "
                        "locks.",
                        10000000, 2);

using consensus::Consensus;
using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::ConsensusOptions;
using consensus::ConsensusRound;
using consensus::StateChangeContext;
using consensus::StateChangeReason;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using consensus::RaftConsensus;
using consensus::ReplicateMsg;
using consensus::OpIdType;
using log::Log;
using log::LogAnchorRegistry;
using rpc::Messenger;
using strings::Substitute;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Tablet Peer
// ============================================================================
TabletPeer::TabletPeer(
    const RaftGroupMetadataPtr& meta,
    const consensus::RaftPeerPB& local_peer_pb,
    const scoped_refptr<server::Clock>& clock,
    const std::string& permanent_uuid,
    Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
    MetricRegistry* metric_registry,
    TabletSplitter* tablet_splitter,
    const std::shared_future<client::YBClient*>& client_future)
    : meta_(meta),
      tablet_id_(meta->raft_group_id()),
      local_peer_pb_(local_peer_pb),
      state_(RaftGroupStatePB::NOT_STARTED),
      operation_tracker_(consensus::MakeTabletLogPrefix(tablet_id_, permanent_uuid)),
      status_listener_(new TabletStatusListener(meta)),
      clock_(clock),
      log_anchor_registry_(new LogAnchorRegistry()),
      mark_dirty_clbk_(std::move(mark_dirty_clbk)),
      permanent_uuid_(permanent_uuid),
      preparing_operations_counter_(operation_tracker_.LogPrefix()),
      metric_registry_(metric_registry),
      tablet_splitter_(tablet_splitter),
      client_future_(client_future) {}

TabletPeer::~TabletPeer() {
  std::lock_guard<simple_spinlock> lock(lock_);
  // We should either have called Shutdown(), or we should have never called
  // Init().
  LOG_IF_WITH_PREFIX(DFATAL, tablet_) << "TabletPeer not fully shut down.";
}

Status TabletPeer::InitTabletPeer(
    const TabletPtr& tablet,
    const std::shared_ptr<MemTracker>& server_mem_tracker,
    Messenger* messenger,
    rpc::ProxyCache* proxy_cache,
    const scoped_refptr<Log>& log,
    const scoped_refptr<MetricEntity>& table_metric_entity,
    const scoped_refptr<MetricEntity>& tablet_metric_entity,
    ThreadPool* raft_pool,
    ThreadPool* tablet_prepare_pool,
    consensus::RetryableRequests* retryable_requests,
    const consensus::SplitOpInfo& split_op_info) {
  DCHECK(tablet) << "A TabletPeer must be provided with a Tablet";
  DCHECK(log) << "A TabletPeer must be provided with a Log";

  if (FLAGS_TEST_delay_init_tablet_peer_ms > 0) {
    std::this_thread::sleep_for(FLAGS_TEST_delay_init_tablet_peer_ms * 1ms);
  }

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    auto state = state_.load(std::memory_order_acquire);
    if (state != RaftGroupStatePB::BOOTSTRAPPING) {
      return STATUS_FORMAT(
          IllegalState, "Invalid tablet state for init: $0", RaftGroupStatePB_Name(state));
    }
    tablet_ = tablet;
    proxy_cache_ = proxy_cache;
    log_ = log;
    // "Publish" the log pointer so it can be retrieved using the log() accessor.
    log_atomic_ = log.get();
    service_thread_pool_ = &messenger->ThreadPool();
    strand_.reset(new rpc::Strand(&messenger->ThreadPool()));
    messenger_ = messenger;

    tablet->SetMemTableFlushFilterFactory([log] {
      auto index = log->GetLatestEntryOpId().index;
      return [index] (const rocksdb::MemTable& memtable) -> Result<bool> {
        auto frontiers = memtable.Frontiers();
        if (frontiers) {
          const auto& largest = down_cast<const docdb::ConsensusFrontier&>(frontiers->Largest());
          // We can only flush this memtable if all operations written to it have also been written
          // to the log (maybe not synced, if durable_wal_write is disabled, but that's OK).
          return largest.op_id().index <= index;
        }

        // It is correct to don't have frontiers when memtable is empty.
        if (memtable.IsEmpty()) {
          return true;
        }

        // This is a degenerate case that should ideally never occur. An empty memtable got into the
        // list of immutable memtables. We say it is OK to flush it and move on.
        static const char* error_msg =
            "A memtable with no frontiers set found when deciding what memtables to "
            "flush! This should not happen.";
        LOG(ERROR) << error_msg << " Stack trace:\n" << GetStackTrace();
        return STATUS(IllegalState, error_msg);
      };
    });

    tablet->SetCleanupPool(raft_pool);

    ConsensusOptions options;
    options.tablet_id = meta_->raft_group_id();

    TRACE("Creating consensus instance");

    std::unique_ptr<ConsensusMetadata> cmeta;
    RETURN_NOT_OK(ConsensusMetadata::Load(meta_->fs_manager(), tablet_id_,
                                          meta_->fs_manager()->uuid(), &cmeta));

    if (retryable_requests) {
      retryable_requests->SetMetricEntity(tablet->GetTabletMetricsEntity());
    }

    consensus_ = consensus::RaftConsensus::Create(
        options,
        std::move(cmeta),
        local_peer_pb_,
        table_metric_entity,
        tablet_metric_entity,
        clock_,
        this,
        messenger,
        proxy_cache_,
        log_.get(),
        server_mem_tracker,
        tablet_->mem_tracker(),
        mark_dirty_clbk_,
        tablet_->table_type(),
        raft_pool,
        retryable_requests,
        split_op_info);
    has_tablet_and_consensus_.store(true, std::memory_order_release);

    tablet_->SetHybridTimeLeaseProvider(std::bind(&TabletPeer::HybridTimeLease, this, _1, _2));
    operation_tracker_.SetPostTracker(
        std::bind(&RaftConsensus::TrackOperationMemory, consensus_.get(), _1));

    prepare_thread_ = std::make_unique<Preparer>(consensus_.get(), tablet_prepare_pool);

    ChangeConfigReplicated(VERIFY_RESULT(RaftConfig())); // Set initial flag value.

    // Releasing lock_ here.
  }

  RETURN_NOT_OK(prepare_thread_->Start());

  if (tablet->metrics() != nullptr) {
    TRACE("Starting instrumentation");
    operation_tracker_.StartInstrumentation(tablet->GetTabletMetricsEntity());
  }
  operation_tracker_.StartMemoryTracking(tablet->mem_tracker());

  if (tablet->transaction_coordinator()) {
    tablet->transaction_coordinator()->Start();
  }

  if (tablet->transaction_participant()) {
    tablet->transaction_participant()->Start();
  }

  RETURN_NOT_OK(set_cdc_min_replicated_index(meta_->cdc_min_replicated_index()));

  TRACE("TabletPeer::Init() finished");
  VLOG_WITH_PREFIX(2) << "Peer Initted";

  return Status::OK();
}

Result<FixedHybridTimeLease> TabletPeer::HybridTimeLease(
    HybridTime min_allowed, CoarseTimePoint deadline) {
  auto time = VERIFY_RESULT(WaitUntil(clock_.get(), min_allowed, deadline));
  // min_allowed could contain non zero logical part, so we add one microsecond to be sure that
  // the resulting ht_lease is at least min_allowed.
  auto min_allowed_micros = min_allowed.CeilPhysicalValueMicros();
  MicrosTime lease_micros = VERIFY_RESULT(
      VERIFY_RESULT(raft_consensus_must_be_set())->MajorityReplicatedHtLeaseExpiration(
      min_allowed_micros, deadline));
  if (lease_micros >= kMaxHybridTimePhysicalMicros) {
    // This could happen when leader leases are disabled.
    return FixedHybridTimeLease();
  }
  return FixedHybridTimeLease {
    .time = time,
    .lease = HybridTime(lease_micros, /* logical */ 0)
  };
}

Result<HybridTime> TabletPeer::PreparePeerRequest() {
  auto tablet = VERIFY_RESULT(shared_tablet_must_be_set());
  auto leader_term = VERIFY_RESULT(shared_consensus_must_be_set())->GetLeaderState(
      /* allow_stale= */ true).term;
  if (leader_term >= 0) {
    auto last_write_ht = tablet->mvcc_manager()->LastReplicatedHybridTime();
    auto propagated_history_cutoff =
        tablet->RetentionPolicy()->HistoryCutoffToPropagate(last_write_ht);

    if (propagated_history_cutoff) {
      VLOG_WITH_PREFIX(2) << "Propagate history cutoff: " << propagated_history_cutoff;

      auto state = std::make_unique<HistoryCutoffOperationState>(
          VERIFY_RESULT(tablet_must_be_set()));
      auto request = state->AllocateRequest();
      request->set_history_cutoff(propagated_history_cutoff.ToUint64());

      auto operation = std::make_unique<tablet::HistoryCutoffOperation>(std::move(state));
      Submit(std::move(operation), leader_term);
    }
  }

  if (!FLAGS_propagate_safe_time) {
    return HybridTime::kInvalid;
  }

  // Get the current majority-replicated HT leader lease without any waiting.
  auto ht_lease = VERIFY_RESULT(HybridTimeLease(
      /* min_allowed= */ HybridTime::kMin, /* deadline */ CoarseTimePoint::max()));
  return tablet->mvcc_manager()->SafeTime(ht_lease);
}

void TabletPeer::MajorityReplicated() {
  auto ht_lease = HybridTimeLease(
      /* min_allowed= */ HybridTime::kMin, /* deadline */ CoarseTimePoint::max());
  if (!ht_lease.ok()) {
    LOG_WITH_PREFIX(DFATAL) << "Failed to get current lease: " << ht_lease.status();
    return;
  }

  auto tablet = CHECK_RESULT(shared_tablet_must_be_set());
  tablet->mvcc_manager()->UpdatePropagatedSafeTimeOnLeader(*ht_lease);
}

void TabletPeer::ChangeConfigReplicated(const RaftConfigPB& config) {
  auto tablet = CHECK_RESULT(shared_tablet_must_be_set());
  tablet->mvcc_manager()->SetLeaderOnlyMode(config.peers_size() == 1);
}

uint64_t TabletPeer::NumSSTFiles() {
  auto tablet = shared_tablet_nullable();
  if (!tablet) {
    return 0;
  }
  return tablet->GetCurrentVersionNumSSTFiles();
}

void TabletPeer::ListenNumSSTFilesChanged(std::function<void()> listener) {
  CHECK_RESULT(shared_tablet_must_be_set())->ListenNumSSTFilesChanged(std::move(listener));
}

Status TabletPeer::Start(const ConsensusBootstrapInfo& bootstrap_info) {
  auto consensus = VERIFY_RESULT(shared_consensus_must_be_set());
  auto tablet = VERIFY_RESULT(shared_tablet_must_be_set());

  {
    std::lock_guard<simple_spinlock> state_change_lock(state_change_lock_);
    TRACE("Starting consensus");

    VLOG_WITH_PREFIX(2) << "Peer starting";

    VLOG(2) << "RaftConfig before starting: " << consensus->CommittedConfig().DebugString();

    // If tablet was previously considered shutdown w.r.t. metrics,
    // fix that for a tablet now being reinstated.
    DVLOG_WITH_PREFIX(3)
      << "Remove from set of tablets that have been shutdown so as to allow reporting metrics";
    metric_registry_->tablets_shutdown_erase(tablet_id());

    RETURN_NOT_OK(consensus->Start(bootstrap_info));
    RETURN_NOT_OK(UpdateState(RaftGroupStatePB::BOOTSTRAPPING, RaftGroupStatePB::RUNNING,
                              "Incorrect state to start TabletPeer, "));

  }

  // The context tracks that the current caller does not hold the lock for consensus state.
  // So mark dirty callback, e.g., consensus->ConsensusState() for master consensus callback of
  // SysCatalogStateChanged, can get the lock when needed.
  auto context =
      std::make_shared<StateChangeContext>(StateChangeReason::TABLET_PEER_STARTED, false);
  // Because we changed the tablet state, we need to re-report the tablet to the master.
  mark_dirty_clbk_.Run(context);

  return tablet->EnableCompactions(/* operation_pause */ nullptr);
}

Result<consensus::RaftConfigPB> TabletPeer::RaftConfig() const {
  auto consensus = VERIFY_RESULT(shared_consensus_must_be_set());
  return consensus->CommittedConfig();
}

bool TabletPeer::StartShutdown() {
  LOG_WITH_PREFIX(INFO) << "Initiating TabletPeer shutdown";

  {
    // Even though we don't need the lock to call shared_tablet_nullable(), we still acquire the
    // lock because there might be other reasons we rely on this during shutdown.
    std::lock_guard<decltype(lock_)> lock(lock_);
    auto tablet = shared_tablet_nullable();
    if (tablet) {
      tablet->StartShutdown();
    }
  }

  {
    RaftGroupStatePB state = state_.load(std::memory_order_acquire);
    for (;;) {
      if (state == RaftGroupStatePB::QUIESCING || state == RaftGroupStatePB::SHUTDOWN) {
        return false;
      }
      if (state_.compare_exchange_strong(
          state, RaftGroupStatePB::QUIESCING, std::memory_order_acq_rel)) {
        LOG_WITH_PREFIX(INFO) << "Started shutdown from state: " << RaftGroupStatePB_Name(state);
        break;
      }
    }
  }

  std::lock_guard<simple_spinlock> l(state_change_lock_);
  // Even though Tablet::Shutdown() also unregisters its ops, we have to do it here
  // to ensure that any currently running operation finishes before we proceed with
  // the rest of the shutdown sequence. In particular, a maintenance operation could
  // indirectly end up calling into the log, which we are about to shut down.
  UnregisterMaintenanceOps();

  auto consensus = shared_consensus_nullable();
  if (consensus) {
    consensus->Shutdown();
  }

  return true;
}

void TabletPeer::CompleteShutdown(IsDropTable is_drop_table) {
  preparing_operations_counter_.Shutdown();

  // TODO: KUDU-183: Keep track of the pending tasks and send an "abort" message.
  LOG_SLOW_EXECUTION(WARNING, 1000,
      Substitute("TabletPeer: tablet $0: Waiting for Operations to complete", tablet_id())) {
    operation_tracker_.WaitForAllToFinish();
  }

  if (prepare_thread_) {
    prepare_thread_->Stop();
  }

  if (log_) {
    WARN_NOT_OK(log_->Close(), LogPrefix() + "Error closing the Log");
  }

  VLOG_WITH_PREFIX(1) << "Shut down!";

  auto tablet = shared_tablet_nullable();
  if (tablet) {
    tablet->CompleteShutdown(is_drop_table);
  }

  // Only mark the peer as SHUTDOWN when all other components have shut down.
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    prepare_thread_.reset();
    auto state = state_.load(std::memory_order_acquire);
    LOG_IF_WITH_PREFIX(DFATAL, state != RaftGroupStatePB::QUIESCING) <<
        "Bad state when completing shutdown: " << RaftGroupStatePB_Name(state);
    state_.store(RaftGroupStatePB::SHUTDOWN, std::memory_order_release);

    if (metric_registry_) {
      DVLOG_WITH_PREFIX(3)
        << "Add to set of tablets that have been shutdown so as to avoid reporting metrics";
      metric_registry_->tablets_shutdown_insert(tablet_id());
    }
  }
}

void TabletPeer::WaitUntilShutdown() {
  const MonoDelta kSingleWait = 10ms;
  const MonoDelta kReportInterval = 5s;
  const MonoDelta kMaxWait = 30s;

  MonoDelta waited = MonoDelta::kZero;
  MonoDelta last_reported = MonoDelta::kZero;
  while (state_.load(std::memory_order_acquire) != RaftGroupStatePB::SHUTDOWN) {
    if (waited >= last_reported + kReportInterval) {
      if (waited >= kMaxWait) {
        LOG_WITH_PREFIX(DFATAL)
            << "Wait for shutdown " << waited << " exceeded kMaxWait " << kMaxWait;
      } else {
        LOG_WITH_PREFIX(WARNING) << "Long wait for shutdown: " << waited;
      }
      last_reported = waited;
    }
    SleepFor(kSingleWait);
    waited += kSingleWait;
  }

  if (metric_registry_) {
    DVLOG_WITH_PREFIX(3)
      << "Add to set of tablets that have been shutdown so as to avoid reporting metrics";
    metric_registry_->tablets_shutdown_insert(tablet_id());
  }
}

void TabletPeer::Shutdown(IsDropTable is_drop_table) {
  if (StartShutdown()) {
    CompleteShutdown(is_drop_table);
  } else {
    WaitUntilShutdown();
  }
}

Status TabletPeer::CheckRunning() const {
  auto state = state_.load(std::memory_order_acquire);
  if (state != RaftGroupStatePB::RUNNING) {
    if (state == RaftGroupStatePB::QUIESCING) {
      return STATUS(ShutdownInProgress, "The tablet is shutting down");
    }
    return STATUS_FORMAT(IllegalState, "The tablet is not in a running state: $0",
                         RaftGroupStatePB_Name(state));
  }

  return Status::OK();
}

Status TabletPeer::CheckShutdownOrNotStarted() const {
  RaftGroupStatePB value = state_.load(std::memory_order_acquire);
  if (value != RaftGroupStatePB::SHUTDOWN && value != RaftGroupStatePB::NOT_STARTED) {
    return STATUS(IllegalState, Substitute("The tablet is not in a shutdown state: $0",
                                           RaftGroupStatePB_Name(value)));
  }

  return Status::OK();
}

Status TabletPeer::WaitUntilConsensusRunning(const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now());

  int backoff_exp = 0;
  const int kMaxBackoffExp = 8;
  while (true) {
    RaftGroupStatePB cached_state = state_.load(std::memory_order_acquire);
    if (cached_state == RaftGroupStatePB::QUIESCING || cached_state == RaftGroupStatePB::SHUTDOWN) {
      return STATUS(IllegalState,
          Substitute("The tablet is already shutting down or shutdown. State: $0",
                     RaftGroupStatePB_Name(cached_state)));
    }
    if (cached_state == RaftGroupStatePB::RUNNING) {
      auto consensus = shared_consensus_nullable();
      if (consensus && consensus->IsRunning()) {
        break;
      }
    }
    MonoTime now(MonoTime::Now());
    MonoDelta elapsed(now.GetDeltaSince(start));
    if (elapsed.MoreThan(timeout)) {
      return STATUS(TimedOut, Substitute("Consensus is not running after waiting for $0. State; $1",
                                         elapsed.ToString(), RaftGroupStatePB_Name(cached_state)));
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = std::min(backoff_exp + 1, kMaxBackoffExp);
  }
  return Status::OK();
}

void TabletPeer::WriteAsync(
    std::unique_ptr<WriteOperationState> state, int64_t term, CoarseTimePoint deadline) {
  if (term == yb::OpId::kUnknownTerm) {
    state->CompleteWithStatus(STATUS(IllegalState, "Write while not leader"));
    return;
  }

  ScopedOperation preparing_token(&preparing_operations_counter_);
  auto status = CheckRunning();
  if (!status.ok()) {
    state->CompleteWithStatus(status);
    return;
  }

  auto operation = std::make_unique<WriteOperation>(
      std::move(state), term, std::move(preparing_token), deadline, this);
  tablet_->AcquireLocksAndPerformDocOperations(std::move(operation));
}

Result<HybridTime> TabletPeer::ReportReadRestart() {
  tablet_->metrics()->restart_read_requests->Increment();
  return tablet_->SafeTime(RequireLease::kTrue);
}

void TabletPeer::Submit(std::unique_ptr<Operation> operation, int64_t term) {
  auto status = CheckRunning();

  if (status.ok()) {
    auto driver = NewLeaderOperationDriver(&operation, term);
    if (driver.ok()) {
      (**driver).ExecuteAsync();
    } else {
      status = driver.status();
    }
  }
  if (!status.ok()) {
    operation->Aborted(status);
  }
}

void TabletPeer::SubmitUpdateTransaction(
    std::unique_ptr<UpdateTxnOperationState> state, int64_t term) {
  if (!state->tablet()) {
    state->SetTablet(CHECK_RESULT(tablet_must_be_set()));
  }
  auto operation = std::make_unique<tablet::UpdateTxnOperation>(std::move(state));
  Submit(std::move(operation), term);
}

HybridTime TabletPeer::SafeTimeForTransactionParticipant() {
  auto tablet = tablet_must_be_set();
  if (!tablet)
    return HybridTime::kInvalid;
  return shared_tablet()->mvcc_manager()->SafeTimeForFollower(
      /* min_allowed= */ HybridTime::kMin, /* deadline= */ CoarseTimePoint::min());
}

Result<HybridTime> TabletPeer::WaitForSafeTime(HybridTime safe_time, CoarseTimePoint deadline) {
  return shared_tablet()->SafeTime(RequireLease::kFallbackToFollower, safe_time, deadline);
}

void TabletPeer::GetLastReplicatedData(RemoveIntentsData* data) {
  shared_tablet()->GetLastCommittedOpId().ToPB(&data->op_id);
  data->log_ht = tablet_->mvcc_manager()->LastReplicatedHybridTime();
}

void TabletPeer::UpdateClock(HybridTime hybrid_time) {
  clock_->Update(hybrid_time);
}

std::unique_ptr<UpdateTxnOperationState> TabletPeer::CreateUpdateTransactionState(
    tserver::TransactionStatePB* request) {
  auto result = std::make_unique<UpdateTxnOperationState>(tablet());
  result->TakeRequest(request);
  return result;
}

void TabletPeer::GetTabletStatusPB(TabletStatusPB* status_pb_out) {
  std::lock_guard<simple_spinlock> lock(lock_);
  DCHECK(status_pb_out != nullptr);
  DCHECK(status_listener_.get() != nullptr);
  const auto disk_size_info = GetOnDiskSizeInfo();
  status_pb_out->set_tablet_id(status_listener_->tablet_id());
  status_pb_out->set_namespace_name(status_listener_->namespace_name());
  status_pb_out->set_table_name(status_listener_->table_name());
  status_pb_out->set_table_id(status_listener_->table_id());
  status_pb_out->set_last_status(status_listener_->last_status());
  status_listener_->partition()->ToPB(status_pb_out->mutable_partition());
  status_pb_out->set_state(state_);
  status_pb_out->set_tablet_data_state(meta_->tablet_data_state());
  disk_size_info.ToPB(status_pb_out);
}

Status TabletPeer::RunLogGC() {
  if (!CheckRunning().ok()) {
    return Status::OK();
  }
  auto s = reset_cdc_min_replicated_index_if_stale();
  if (!s.ok()) {
    LOG(WARNING) << "Unable to reset cdc min replicated index " << s;
  }
  int64_t min_log_index = VERIFY_RESULT(GetEarliestNeededLogIndex());
  int32_t num_gced = 0;
  return log_->GC(min_log_index, &num_gced);
}

const TabletDataState TabletPeer::data_state() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return meta_->tablet_data_state();
}

string TabletPeer::HumanReadableState() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  TabletDataState data_state = meta_->tablet_data_state();
  RaftGroupStatePB state = this->state();
  // If failed, any number of things could have gone wrong.
  if (state == RaftGroupStatePB::FAILED) {
    return Substitute("$0 ($1): $2", RaftGroupStatePB_Name(state),
                      TabletDataState_Name(data_state),
                      error_.get()->ToString());
  // If it's remotely bootstrapping, or tombstoned, that is the important thing
  // to show.
  } else if (!CanServeTabletData(data_state)) {
    return TabletDataState_Name(data_state);
  } else if (data_state == TabletDataState::TABLET_DATA_SPLIT_COMPLETED) {
    return RaftGroupStatePB_Name(state) + " (split)";
  }
  // Otherwise, the tablet's data is in a "normal" state, so we just display
  // the runtime state (BOOTSTRAPPING, RUNNING, etc).
  return RaftGroupStatePB_Name(state);
}

namespace {

consensus::OperationType MapOperationTypeToPB(OperationType operation_type) {
  switch (operation_type) {
    case OperationType::kWrite:
      return consensus::WRITE_OP;

    case OperationType::kChangeMetadata:
      return consensus::CHANGE_METADATA_OP;

    case OperationType::kUpdateTransaction:
      return consensus::UPDATE_TRANSACTION_OP;

    case OperationType::kSnapshot:
      return consensus::SNAPSHOT_OP;

    case OperationType::kTruncate:
      return consensus::TRUNCATE_OP;

    case OperationType::kHistoryCutoff:
      return consensus::HISTORY_CUTOFF_OP;

    case OperationType::kSplit:
      return consensus::SPLIT_OP;

    case OperationType::kEmpty:
      LOG(FATAL) << "OperationType::kEmpty cannot be converted to consensus::OperationType";
  }
  FATAL_INVALID_ENUM_VALUE(OperationType, operation_type);
}

} // namespace

void TabletPeer::GetInFlightOperations(Operation::TraceType trace_type,
                                       vector<consensus::OperationStatusPB>* out) const {
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    if (driver->state() == nullptr) {
      continue;
    }
    auto op_type = driver->operation_type();
    if (op_type == OperationType::kEmpty) {
      // This is a special-purpose in-memory-only operation for updating propagated safe time on
      // a follower.
      continue;
    }

    consensus::OperationStatusPB status_pb;
    driver->GetOpId().ToPB(status_pb.mutable_op_id());
    status_pb.set_operation_type(MapOperationTypeToPB(op_type));
    status_pb.set_description(driver->ToString());
    int64_t running_for_micros =
        MonoTime::Now().GetDeltaSince(driver->start_time()).ToMicroseconds();
    status_pb.set_running_for_micros(running_for_micros);
    if (trace_type == Operation::TRACE_TXNS) {
      status_pb.set_trace_buffer(driver->trace()->DumpToString(true));
    }
    out->push_back(status_pb);
  }
}

Result<int64_t> TabletPeer::GetEarliestNeededLogIndex(std::string* details) const {
  // First, we anchor on the last OpId in the Log to establish a lower bound
  // and avoid racing with the other checks. This limits the Log GC candidate
  // segments before we check the anchors.
  auto latest_log_entry_op_id = log_->GetLatestEntryOpId();
  int64_t min_index = latest_log_entry_op_id.index;
  if (details) {
    *details += Format("Latest log entry op id: $0\n", latest_log_entry_op_id);
  }

  // If we never have written to the log, no need to proceed.
  if (min_index == 0) {
    return min_index;
  }

  // Next, we interrogate the anchor registry.
  // Returns OK if minimum known, NotFound if no anchors are registered.
  {
    int64_t min_anchor_index;
    Status s = log_anchor_registry_->GetEarliestRegisteredLogIndex(&min_anchor_index);
    if (PREDICT_FALSE(!s.ok())) {
      DCHECK(s.IsNotFound()) << "Unexpected error calling LogAnchorRegistry: " << s.ToString();
    } else {
      min_index = std::min(min_index, min_anchor_index);
      if (details) {
        *details += Format("Min anchor index: $0\n", min_anchor_index);
      }
    }
  }

  // Next, interrogate the OperationTracker.
  int64_t min_pending_op_index = std::numeric_limits<int64_t>::max();
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    auto tx_op_id = driver->GetOpId();
    // A operation which doesn't have an opid hasn't been submitted for replication yet and
    // thus has no need to anchor the log.
    if (tx_op_id != yb::OpId::Invalid()) {
      min_pending_op_index = std::min(min_pending_op_index, tx_op_id.index);
    }
  }

  min_index = std::min(min_index, min_pending_op_index);
  if (details && min_pending_op_index != std::numeric_limits<int64_t>::max()) {
    *details += Format("Min pending op id index: $0\n", min_pending_op_index);
  }

  auto min_retryable_request_op_id = consensus_->MinRetryableRequestOpId();
  min_index = std::min(min_index, min_retryable_request_op_id.index);
  if (details) {
    *details += Format("Min retryable request op id: $0\n", min_retryable_request_op_id);
  }

  auto* transaction_coordinator = tablet()->transaction_coordinator();
  if (transaction_coordinator) {
    auto transaction_coordinator_min_op_index = transaction_coordinator->PrepareGC(details);
    min_index = std::min(min_index, transaction_coordinator_min_op_index);
  }

  // We keep at least one committed operation in the log so that we can always recover safe time
  // during bootstrap.
  // Last committed op id should be read before MaxPersistentOpId to avoid race condition
  // described in MaxPersistentOpIdForDb.
  //
  // If we read last committed op id AFTER reading last persistent op id (INCORRECT):
  // - We read max persistent op id and find there is no new data, so we ignore it.
  // - New data gets written and Raft-committed, but not yet flushed to an SSTable.
  // - We read the last committed op id, which is greater than what max persistent op id would have
  //   returned.
  // - We garbage-collect the Raft log entries corresponding to the new data.
  // - Power is lost and the server reboots, losing committed data.
  //
  // If we read last committed op id BEFORE reading last persistent op id (CORRECT):
  // - We read the last committed op id.
  // - We read max persistent op id and find there is no new data, so we ignore it.
  // - New data gets written and Raft-committed, but not yet flushed to an SSTable.
  // - We still don't garbage-collect the logs containing the committed but unflushed data,
  //   because the earlier value of the last committed op id that we read prevents us from doing so.
  auto last_committed_op_id = consensus()->GetLastCommittedOpId();
  min_index = std::min(min_index, last_committed_op_id.index);
  if (details) {
    *details += Format("Last committed op id: $0\n", last_committed_op_id);
  }

  if (tablet_->table_type() != TableType::TRANSACTION_STATUS_TABLE_TYPE) {
    tablet_->FlushIntentsDbIfNecessary(latest_log_entry_op_id);
    auto max_persistent_op_id = VERIFY_RESULT(
        tablet_->MaxPersistentOpId(true /* invalid_if_no_new_data */));
    if (max_persistent_op_id.regular.valid()) {
      min_index = std::min(min_index, max_persistent_op_id.regular.index);
      if (details) {
        *details += Format("Max persistent regular op id: $0\n", max_persistent_op_id.regular);
      }
    }
    if (max_persistent_op_id.intents.valid()) {
      min_index = std::min(min_index, max_persistent_op_id.intents.index);
      if (details) {
        *details += Format("Max persistent intents op id: $0\n", max_persistent_op_id.intents);
      }
    }
  }

  {
    // We should prevent Raft log GC from deleting SPLIT_OP designated for this tablet, because
    // it is used during bootstrap to initialize ReplicaState::split_op_id_ which in its turn
    // is used to prevent already split tablet from serving new ops.
    auto split_op_id = consensus()->GetSplitOpId();
    if (!split_op_id.empty()) {
      min_index = std::min(min_index, split_op_id.index);
      if (details) {
        *details += Format("split_op_id: $0\n", split_op_id.index);
      }
    }
  }

  if (details) {
    *details += Format("Earliest needed log index: $0\n", min_index);
  }

  return min_index;
}

Status TabletPeer::GetMaxIndexesToSegmentSizeMap(MaxIdxToSegmentSizeMap* idx_size_map) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx = VERIFY_RESULT(GetEarliestNeededLogIndex());
  log_->GetMaxIndexesToSegmentSizeMap(min_op_idx, idx_size_map);
  return Status::OK();
}

Status TabletPeer::GetGCableDataSize(int64_t* retention_size) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx = VERIFY_RESULT(GetEarliestNeededLogIndex());
  RETURN_NOT_OK(log_->GetGCableDataSize(min_op_idx, retention_size));
  return Status::OK();
}

log::Log* TabletPeer::log() const {
  Log* log = log_atomic_.load(std::memory_order_acquire);
  LOG_IF_WITH_PREFIX(FATAL, !log) << "log() called before the log instance is initialized.";
  return log;
}

yb::OpId TabletPeer::GetLatestLogEntryOpId() const {
  Log* log = log_atomic_.load(std::memory_order_acquire);
  if (log) {
    return log->GetLatestEntryOpId();
  }
  return yb::OpId();
}

Status TabletPeer::set_cdc_min_replicated_index_unlocked(int64_t cdc_min_replicated_index) {
  LOG_WITH_PREFIX(INFO) << "Setting cdc min replicated index to " << cdc_min_replicated_index;
  RETURN_NOT_OK(meta_->set_cdc_min_replicated_index(cdc_min_replicated_index));
  Log* log = log_atomic_.load(std::memory_order_acquire);
  if (log) {
    log->set_cdc_min_replicated_index(cdc_min_replicated_index);
  }
  cdc_min_replicated_index_refresh_time_ = MonoTime::Now();
  return Status::OK();
}

Status TabletPeer::set_cdc_min_replicated_index(int64_t cdc_min_replicated_index) {
  std::lock_guard<decltype(cdc_min_replicated_index_lock_)> l(cdc_min_replicated_index_lock_);
  return set_cdc_min_replicated_index_unlocked(cdc_min_replicated_index);
}

Status TabletPeer::reset_cdc_min_replicated_index_if_stale() {
  std::lock_guard<decltype(cdc_min_replicated_index_lock_)> l(cdc_min_replicated_index_lock_);
  auto seconds_since_last_refresh =
      MonoTime::Now().GetDeltaSince(cdc_min_replicated_index_refresh_time_).ToSeconds();
  if (seconds_since_last_refresh > FLAGS_cdc_min_replicated_index_considered_stale_secs) {
    LOG_WITH_PREFIX(INFO) << "Resetting cdc min replicated index. Seconds since last update: "
                          << seconds_since_last_refresh;
    RETURN_NOT_OK(set_cdc_min_replicated_index_unlocked(std::numeric_limits<int64_t>::max()));
  }
  return Status::OK();
}

#define ENSURE_PB_FIELD_IS_SET(pb_field_name) \
    RSTATUS_DCHECK( \
        replicate_msg->BOOST_PP_CAT(has_, pb_field_name)(), \
        IllegalState, \
        Format("A $0 operation must have the $1 field set", \
               consensus::OperationType_Name(op_type), \
               BOOST_PP_STRINGIZE(pp_field_name)))

Result<std::unique_ptr<Operation>> TabletPeer::CreateOperation(
    consensus::ReplicateMsg* replicate_msg) {
  auto op_type = replicate_msg->op_type();
  switch (op_type) {
    case consensus::WRITE_OP:
      ENSURE_PB_FIELD_IS_SET(write_request);
      // We use separate preparing token only on leader, so here it could be empty.
      return std::make_unique<WriteOperation>(
          std::make_unique<WriteOperationState>(tablet()), yb::OpId::kUnknownTerm,
          ScopedOperation(), CoarseTimePoint::max(), this);

    case consensus::CHANGE_METADATA_OP:
      ENSURE_PB_FIELD_IS_SET(change_metadata_request);
      return std::make_unique<ChangeMetadataOperation>(
          std::make_unique<ChangeMetadataOperationState>(tablet(), log()));

    case consensus::UPDATE_TRANSACTION_OP:
      ENSURE_PB_FIELD_IS_SET(transaction_state);
      return std::make_unique<UpdateTxnOperation>(
          std::make_unique<UpdateTxnOperationState>(tablet()));

    case consensus::TRUNCATE_OP:
      ENSURE_PB_FIELD_IS_SET(truncate_request);
      return std::make_unique<TruncateOperation>(
          std::make_unique<TruncateOperationState>(tablet()));

    case consensus::SNAPSHOT_OP:
      ENSURE_PB_FIELD_IS_SET(snapshot_request);
      return std::make_unique<SnapshotOperation>(
          std::make_unique<SnapshotOperationState>(tablet()));

    case consensus::HISTORY_CUTOFF_OP:
      ENSURE_PB_FIELD_IS_SET(history_cutoff);
      return std::make_unique<HistoryCutoffOperation>(
          std::make_unique<HistoryCutoffOperationState>(tablet()));

    case consensus::SPLIT_OP:
      ENSURE_PB_FIELD_IS_SET(split_request);
      return std::make_unique<SplitOperation>(
          std::make_unique<SplitOperationState>(tablet(), raft_consensus(), tablet_splitter_));

    case consensus::UNKNOWN_OP: FALLTHROUGH_INTENDED;
    case consensus::NO_OP: FALLTHROUGH_INTENDED;
    case consensus::CHANGE_CONFIG_OP:
      FATAL_INVALID_ENUM_VALUE(consensus::OperationType, op_type);
  }
  FATAL_INVALID_ENUM_VALUE(consensus::OperationType, op_type);
}

#undef ENSURE_PB_FIELD_IS_SET

Status TabletPeer::StartReplicaOperation(
    const scoped_refptr<ConsensusRound>& round, HybridTime propagated_safe_time) {
  auto tablet = VERIFY_RESULT(shared_tablet_must_be_set());
  RaftGroupStatePB value = state();
  if (value != RaftGroupStatePB::RUNNING && value != RaftGroupStatePB::BOOTSTRAPPING) {
    return STATUS(IllegalState, RaftGroupStatePB_Name(value));
  }

  consensus::ReplicateMsg* replicate_msg = round->replicate_msg().get();
  DCHECK(replicate_msg->has_hybrid_time());
  auto operation = CreateOperation(replicate_msg);

  OperationState* state = operation->state();

  // It's imperative that we set the round here on any type of operation, as this
  // allows us to keep the reference to the request in the round instead of copying it.
  state->set_consensus_round(round);
  HybridTime ht(replicate_msg->hybrid_time());
  state->set_hybrid_time(ht);
  clock_->Update(ht);

  // This sets the monotonic counter to at least replicate_msg.monotonic_counter() atomically.
  tablet->UpdateMonotonicCounter(replicate_msg->monotonic_counter());

  auto operation_type = operation->operation_type();
  OperationDriverPtr driver = VERIFY_RESULT(NewReplicaOperationDriver(&operation));

  // Unretained is required to avoid a refcount cycle.
  state->consensus_round()->SetConsensusReplicatedCallback(
      std::bind(&OperationDriver::ReplicationFinished, driver.get(), _1, _2, _3));

  if (propagated_safe_time) {
    driver->SetPropagatedSafeTime(propagated_safe_time, tablet->mvcc_manager());
  }

  if (operation_type == OperationType::kWrite ||
      operation_type == OperationType::kUpdateTransaction) {
    tablet()->mvcc_manager()->AddPending(&ht);
  }

  driver->ExecuteAsync();
  return Status::OK();
}

void TabletPeer::SetPropagatedSafeTime(HybridTime ht) {
  auto driver = NewReplicaOperationDriver(nullptr);
  if (!driver.ok()) {
    LOG_WITH_PREFIX(ERROR) << "Failed to create operation driver to set propagated hybrid time";
    return;
  }
  (**driver).SetPropagatedSafeTime(ht, tablet_->mvcc_manager());
  (**driver).ExecuteAsync();
}

bool TabletPeer::ShouldApplyWrite() {
  return tablet_->ShouldApplyWrite();
}

// std::shared_ptr<consensus::Consensus> TabletPeer::shared_raft_consensus() const
//     NO_THREAD_SAFETY_ANALYSIS {
//   if (!has_tablet_and_consensus_->load(std::memory_order_acquire)) {
//     return nullptr;
//   }
//   return consensus_;
// }

// shared_ptr<consensus::Consensus> TabletPeer::shared_consensus() const {
//   return shared_raft_consensus();
// }

Result<TabletPtr> TabletPeer::shared_tablet_must_be_set() const NO_THREAD_SAFETY_ANALYSIS {
  RETURN_NOT_OK(CheckTabletAndConsensusAreSet());
  return tablet_;
}

TabletPtr TabletPeer::shared_tablet_allow_nullptr() const NO_THREAD_SAFETY_ANALYSIS {
  if (!has_tablet_and_consensus_.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return tablet_;
}

Result<Tablet*> TabletPeer::tablet_must_be_set() const NO_THREAD_SAFETY_ANALYSIS {
  RETURN_NOT_OK(CheckTabletAndConsensusAreSet());
  return tablet_.get();
}

Result<consensus::ConsensusPtr> TabletPeer::shared_consensus_must_be_set() const
    NO_THREAD_SAFETY_ANALYSIS {
  RETURN_NOT_OK(CheckTabletAndConsensusAreSet());
  return consensus_;
}

consensus::ConsensusPtr TabletPeer::shared_consensus_nullable() const
    NO_THREAD_SAFETY_ANALYSIS {
  if (!has_tablet_and_consensus_.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return consensus_;
}

Result<consensus::RaftConsensusPtr> TabletPeer::raft_consensus_must_be_set() const
    NO_THREAD_SAFETY_ANALYSIS{
  RETURN_NOT_OK(CheckTabletAndConsensusAreSet());
  return consensus_;
}

Result<consensus::Consensus*> TabletPeer::consensus_must_be_set() const
    NO_THREAD_SAFETY_ANALYSIS{
  RETURN_NOT_OK(CheckTabletAndConsensusAreSet());
  return consensus_.get();
}

Result<consensus::RaftConsensus*> TabletPeer::raft_consensus_must_be_set() const
    NO_THREAD_SAFETY_ANALYSIS{
  RETURN_NOT_OK(CheckTabletAndConsensusAreSet());
  return consensus_.get();
}

Result<OperationDriverPtr> TabletPeer::NewLeaderOperationDriver(
    std::unique_ptr<Operation>* operation, int64_t term) {
  if (term == OpId::kUnknownTerm) {
    return STATUS(InvalidArgument, "Leader operation driver for unknown term");
  }
  return NewOperationDriver(operation, term);
}

Result<OperationDriverPtr> TabletPeer::NewReplicaOperationDriver(
    std::unique_ptr<Operation>* operation) {
  return NewOperationDriver(operation, OpId::kUnknownTerm);
}

Result<OperationDriverPtr> TabletPeer::NewOperationDriver(std::unique_ptr<Operation>* operation,
                                                          int64_t term) {
  auto operation_driver = CreateOperationDriver();
  RETURN_NOT_OK(operation_driver->Init(operation, term));
  return operation_driver;
}

void TabletPeer::RegisterMaintenanceOps(MaintenanceManager* maint_mgr) {
  // Taking state_change_lock_ ensures that we don't shut down concurrently with
  // this last start-up task.
  // Note that the state_change_lock_ is taken in Shutdown(),
  // prior to calling UnregisterMaintenanceOps().

  std::lock_guard<simple_spinlock> l(state_change_lock_);

  if (state() != RaftGroupStatePB::RUNNING) {
    LOG_WITH_PREFIX(WARNING) << "Not registering maintenance operations: tablet not RUNNING";
    return;
  }

  DCHECK(maintenance_ops_.empty());
  auto tablet = shared_tablet_nullable();
  if (tablet == nullptr) {
    LOG_WITH_PREFIX(DFATAL)
        << "This should never happen. "
        << "Tablet is not set in TabletPeer when trying to register maintenance operations.";
    return;
  }

  gscoped_ptr<MaintenanceOp> log_gc(new LogGCOp(shared_from_this(), tablet));
  maint_mgr->RegisterOp(log_gc.get());
  maintenance_ops_.push_back(log_gc.release());
  LOG_WITH_PREFIX(INFO) << "Registered log GC operations";
}

void TabletPeer::UnregisterMaintenanceOps() {
  DCHECK(state_change_lock_.is_locked());
  for (MaintenanceOp* op : maintenance_ops_) {
    op->Unregister();
  }
  STLDeleteElements(&maintenance_ops_);
}

TabletOnDiskSizeInfo TabletPeer::GetOnDiskSizeInfo() const {
  TabletOnDiskSizeInfo info;

  if (consensus_) {
    info.consensus_metadata_disk_size = consensus_->OnDiskSize();
  }

  if (tablet_) {
    info.sst_files_disk_size = tablet_->GetCurrentVersionSstFilesSize();
    info.uncompressed_sst_files_disk_size =
        tablet_->GetCurrentVersionSstFilesUncompressedSize();
  }

  if (log_) {
    info.wal_files_disk_size = log_->OnDiskSize();
  }

  info.RecomputeTotalSize();
  return info;
}

int TabletPeer::GetNumLogSegments() const {
  return (log_) ? log_->num_segments() : 0;
}

std::string TabletPeer::LogPrefix() const {
  return Substitute("T $0 P $1 [state=$2]: ",
      tablet_id_, permanent_uuid_, RaftGroupStatePB_Name(state()));
}

scoped_refptr<OperationDriver> TabletPeer::CreateOperationDriver() {
  return scoped_refptr<OperationDriver>(new OperationDriver(
      &operation_tracker_,
      consensus_.get(),
      log_.get(),
      prepare_thread_.get(),
      &operation_order_verifier_,
      tablet_->table_type()));
}

Status TabletPeer::CheckTabletAndConsensusAreSet() const {
  if (!has_tablet_and_consensus_.load(std::memory_order_acquire)) {
    return STATUS_FORMAT(
        IllegalState, "Tablet and consensus are not initialized for tablet id $0", tablet_id());
  }
  return Status::OK();
}

int64_t TabletPeer::LeaderTerm() const {
  auto consensus = shared_consensus();
  return consensus ? consensus->LeaderTerm() : yb::OpId::kUnknownTerm;
}

Result<HybridTime> TabletPeer::LeaderSafeTime() const {
  return tablet_->SafeTime();
}

consensus::LeaderStatus TabletPeer::LeaderStatus(bool allow_stale) const {
  auto consensus = shared_consensus();
  return consensus ? consensus->GetLeaderStatus(allow_stale) : consensus::LeaderStatus::NOT_LEADER;
}

HybridTime TabletPeer::HtLeaseExpiration() const {
  HybridTime result(
      CHECK_RESULT(shared_consensus()->MajorityReplicatedHtLeaseExpiration(
          0, CoarseTimePoint::max())), 0);
  return std::max(result, tablet_->mvcc_manager()->LastReplicatedHybridTime());
}

TableType TabletPeer::table_type() {
  // TODO: what if tablet is not set?
  return DCHECK_NOTNULL(tablet())->table_type();
}

void TabletPeer::SetFailed(const Status& error) {
  auto old_error = error_.get();
  LOG_IF(DFATAL, old_error != nullptr)
      << "SetFailed called with error " << error << " but the error is already set to "
      << *old_error;

  error_ = MakeAtomicUniquePtr<Status>(error);
  auto state = state_.load(std::memory_order_acquire);
  while (state != RaftGroupStatePB::FAILED && state != RaftGroupStatePB::QUIESCING &&
         state != RaftGroupStatePB::SHUTDOWN) {
    if (state_.compare_exchange_weak(state, RaftGroupStatePB::FAILED, std::memory_order_acq_rel)) {
      LOG_WITH_PREFIX(INFO) << "Changed state from " << RaftGroupStatePB_Name(state)
                            << " to FAILED";
      break;
    }
  }
}

Status TabletPeer::UpdateState(RaftGroupStatePB expected, RaftGroupStatePB new_state,
                               const std::string& error_message) {
  RaftGroupStatePB old = expected;
  if (!state_.compare_exchange_strong(old, new_state, std::memory_order_acq_rel)) {
    return STATUS_FORMAT(
        InvalidArgument, "$0 Expected state: $1, got: $2",
        error_message, RaftGroupStatePB_Name(expected), RaftGroupStatePB_Name(old));
  }

  LOG_WITH_PREFIX(INFO) << "Changed state from " << RaftGroupStatePB_Name(old) << " to "
                        << RaftGroupStatePB_Name(new_state);
  return Status::OK();
}

void TabletPeer::Enqueue(rpc::ThreadPoolTask* task) {
  rpc::ThreadPool* thread_pool = service_thread_pool_.load(std::memory_order_acquire);
  if (!thread_pool) {
    task->Done(STATUS(Aborted, "Thread pool not ready"));
    return;
  }

  thread_pool->Enqueue(task);
}

void TabletPeer::StrandEnqueue(rpc::StrandTask* task) {
  rpc::Strand* strand = strand_.get();
  if (!strand) {
    task->Done(STATUS(Aborted, "Thread pool not ready"));
    return;
  }

  strand->Enqueue(task);
}

bool TabletPeer::CanBeDeleted() {
  if (!IsLeader()) {
    return false;
  }
  if (can_be_deleted_) {
    return can_be_deleted_;
  }

  const auto split_op_id = shared_consensus()->GetSplitOpId();
  const auto all_applied_op_id = consensus_->GetAllAppliedOpId();
  VLOG_WITH_PREFIX_AND_FUNC(4) << "split_op_id: " << split_op_id
                               << " all_applied_op_id: " << all_applied_op_id;
  if (split_op_id.empty() || all_applied_op_id < split_op_id) {
    return can_be_deleted_;
  }
  const auto tablet_data_state = tablet()->metadata()->tablet_data_state();
  if (tablet_data_state != TABLET_DATA_SPLIT_COMPLETED) {
    YB_LOG_WITH_PREFIX_EVERY_N_SECS(DFATAL, 5) << Format(
        "Expected tablet $0 data state to be TABLET_DATA_SPLIT_COMPLETED, but got: $1. "
        "all_applied_op_id: $2, split_op_id: $3",
        tablet_id(), TabletDataState_Name(tablet_data_state), all_applied_op_id,
        consensus_->GetSplitOpId());
    return can_be_deleted_;
  }

  can_be_deleted_ = true;
  LOG_WITH_PREFIX(INFO) << Format(
      "Marked tablet $0 as requiring cleanup due to all replicas have been split (all applied op "
      "ID: $1, split op ID: $2)",
      tablet_id(), all_applied_op_id, split_op_id);

  return can_be_deleted_;
}

rpc::Scheduler& TabletPeer::scheduler() const {
  return messenger_->scheduler();
}

}  // namespace tablet
}  // namespace yb
