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

#include <shared_mutex>
#include <chrono>

#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/twodc_output_client.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/cdc_poller.h"

#include "yb/cdc/cdc_consumer.pb.h"
#include "yb/cdc/cdc_consumer_proxy_manager.h"

#include "yb/client/client.h"

#include "yb/util/string_util.h"
#include "yb/util/thread.h"

DECLARE_int32(cdc_rpc_timeout_ms);

using namespace std::chrono_literals;

namespace yb {

namespace tserver {
namespace enterprise {

Result<std::unique_ptr<CDCConsumer>> CDCConsumer::Create(
    std::function<bool(const std::string&)> is_leader_for_tablet,
    rpc::ProxyCache* proxy_cache,
    TabletServer* tserver) {
  LOG(INFO) << "Creating CDC Consumer";
  auto master_addrs = tserver->options().GetMasterAddresses();
  std::vector<std::string> hostport_strs;
  hostport_strs.reserve(master_addrs->size());
  for (const auto& hp : *master_addrs) {
    hostport_strs.push_back(HostPort::ToCommaSeparatedString(hp));
  }

  auto client = VERIFY_RESULT(client::YBClientBuilder()
      .master_server_addrs(hostport_strs)
      .set_client_name("CDCConsumer")
      .default_admin_operation_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_rpc_timeout_ms))
      .Build());

  auto cdc_consumer = std::make_unique<CDCConsumer>(
      std::move(is_leader_for_tablet), proxy_cache, tserver->permanent_uuid(), std::move(client));

  RETURN_NOT_OK(yb::Thread::Create(
      "CDCConsumer", "Poll", &CDCConsumer::RunThread, cdc_consumer.get(),
      &cdc_consumer->run_trigger_poll_thread_));
  RETURN_NOT_OK(ThreadPoolBuilder("CDCConsumerHandler").Build(&cdc_consumer->thread_pool_));
  return cdc_consumer;
}

CDCConsumer::CDCConsumer(std::function<bool(const std::string&)> is_leader_for_tablet,
                         rpc::ProxyCache* proxy_cache,
                         const string& ts_uuid,
                         std::unique_ptr<client::YBClient> client) :
  is_leader_for_tablet_(std::move(is_leader_for_tablet)),
  proxy_manager_(std::make_unique<cdc::CDCConsumerProxyManager>(proxy_cache)),
  log_prefix_(Format("[TS $0]: ", ts_uuid)),
  client_(std::move(client)) {}

CDCConsumer::~CDCConsumer() {
  Shutdown();
}

void CDCConsumer::Shutdown() {
  LOG_WITH_PREFIX(INFO) << "Shutting down CDC Consumer";
  {
    std::lock_guard<std::mutex> l(should_run_mutex_);
    should_run_ = false;
  }
  cond_.notify_all();

  {
    std::unique_lock<rw_spinlock> lock(master_data_mutex_);
    producer_consumer_tablet_map_from_master_.clear();
    client_->Shutdown();
  }

  if (run_trigger_poll_thread_) {
    WARN_NOT_OK(ThreadJoiner(run_trigger_poll_thread_.get()).Join(), "Could not join thread");
  }

  if (thread_pool_) {
    thread_pool_->Shutdown();
  }
}

void CDCConsumer::RunThread() {
  while (true) {
    std::unique_lock<std::mutex> l(should_run_mutex_);
    if (!should_run_) {
      return;
    }
    cond_.wait_for(l, 1000ms);
    if (!should_run_) {
      return;
    }
    TriggerPollForNewTablets();
  }
}

void CDCConsumer::RefreshWithNewRegistryFromMaster(const cdc::ConsumerRegistryPB* consumer_registry,
                                                   int32_t cluster_config_version) {
  UpdateInMemoryState(consumer_registry, cluster_config_version);
  cond_.notify_all();
}

std::vector<std::string> CDCConsumer::TEST_producer_tablets_running() {
  SharedLock<decltype(producer_pollers_map_mutex_)> pollers_lock(producer_pollers_map_mutex_);

  std::vector<string> tablets;
  for (const auto& producer : producer_pollers_map_) {
    tablets.push_back(producer.first.tablet_id);
  }
  return tablets;
}

void CDCConsumer::UpdateInMemoryState(const cdc::ConsumerRegistryPB* consumer_registry,
    int32_t cluster_config_version) {
  std::lock_guard<rw_spinlock> lock(master_data_mutex_);

  // Only update it if the version is newer.
  if (cluster_config_version <= cluster_config_version_.load(std::memory_order_acquire)) {
    return;
  }

  cluster_config_version_.store(cluster_config_version, std::memory_order_release);
  producer_consumer_tablet_map_from_master_.clear();

  if (!consumer_registry) {
    LOG_WITH_PREFIX(INFO) << "Given empty CDC consumer registry: removing Pollers";
    cond_.notify_all();
    return;
  }

  LOG_WITH_PREFIX(INFO) << "Updating CDC consumer registry: " << consumer_registry->DebugString();

  for (const auto& producer_map : DCHECK_NOTNULL(consumer_registry)->producer_map()) {
    const auto& producer_entry_pb = producer_map.second;
    proxy_manager_->UpdateProxies(producer_entry_pb);
    if (producer_entry_pb.disable_stream()) {
      continue;
    }
    for (const auto& stream_entry : producer_entry_pb.stream_map()) {
      const auto& stream_entry_pb = stream_entry.second;
      for (const auto& tablet_entry : stream_entry_pb.consumer_producer_tablet_map()) {
        const auto& consumer_tablet_id = tablet_entry.first;
        for (const auto& producer_tablet_id : tablet_entry.second.tablets()) {
          cdc::ProducerTabletInfo producer_tablet_info({stream_entry.first, producer_tablet_id});
          cdc::ConsumerTabletInfo consumer_tablet_info(
              {consumer_tablet_id, stream_entry_pb.consumer_table_id()});
          producer_consumer_tablet_map_from_master_[producer_tablet_info] = consumer_tablet_info;
        }
      }
    }
  }
  cond_.notify_all();
}

void CDCConsumer::TriggerPollForNewTablets() {
  SharedLock<decltype(master_data_mutex_)> master_lock(master_data_mutex_);

  for (const auto& entry : producer_consumer_tablet_map_from_master_) {
    bool start_polling;
    {
      SharedLock<decltype(producer_pollers_map_mutex_)> pollers_lock(producer_pollers_map_mutex_);
      start_polling = producer_pollers_map_.find(entry.first) == producer_pollers_map_.end() &&
                      is_leader_for_tablet_(entry.second.tablet_id);
    }
    if (start_polling) {
      // This is a new tablet, trigger a poll.
      std::lock_guard<rw_spinlock> pollers_lock(producer_pollers_map_mutex_);
      auto cdc_poller = std::make_shared<CDCPoller>(
          entry.first, entry.second,
          std::bind(&CDCConsumer::ShouldContinuePolling, this, entry.first),
          std::bind(&cdc::CDCConsumerProxyManager::GetProxy, proxy_manager_.get(), entry.first),
          std::bind(&CDCConsumer::RemoveFromPollersMap, this, entry.first),
          thread_pool_.get(), client_, this);
      LOG_WITH_PREFIX(INFO) << Format("Start polling for producer tablet $0",
                                      entry.first.tablet_id);
      producer_pollers_map_[entry.first] = cdc_poller;
      cdc_poller->Poll();
    }
  }
}

void CDCConsumer::RemoveFromPollersMap(const cdc::ProducerTabletInfo& producer_tablet_info) {
  LOG_WITH_PREFIX(INFO) << Format("Stop polling for producer tablet $0",
                                  producer_tablet_info.tablet_id);
  std::lock_guard<rw_spinlock> pollers_lock(producer_pollers_map_mutex_);
  producer_pollers_map_.erase(producer_tablet_info);
}

bool CDCConsumer::ShouldContinuePolling(const cdc::ProducerTabletInfo& producer_tablet_info) {
  std::lock_guard<std::mutex> l(should_run_mutex_);
  if (!should_run_) {
    return false;
  }
  SharedLock<decltype(master_data_mutex_)> master_lock(master_data_mutex_);

  const auto& it = producer_consumer_tablet_map_from_master_.find(producer_tablet_info);
  if (it == producer_consumer_tablet_map_from_master_.end()) {
    // We no longer care about this tablet, abort the cycle.
    return false;
  }
  return is_leader_for_tablet_(it->second.tablet_id);
}

std::string CDCConsumer::LogPrefix() {
  return log_prefix_;
}

int32_t CDCConsumer::cluster_config_version() const {
  return cluster_config_version_.load(std::memory_order_acquire);
}

} // namespace enterprise
} // namespace tserver
} // namespace yb
