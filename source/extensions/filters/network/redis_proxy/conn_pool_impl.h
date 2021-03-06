#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "envoy/config/filter/network/redis_proxy/v2/redis_proxy.pb.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/hash.h"
#include "common/network/filter_impl.h"
#include "common/protobuf/utility.h"
#include "common/upstream/load_balancer_impl.h"

#include "extensions/filters/network/common/redis/codec_impl.h"
#include "extensions/filters/network/redis_proxy/conn_pool.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RedisProxy {
namespace ConnPool {

// TODO(mattklein123): Circuit breaking
// TODO(rshriram): Fault injection

class ConfigImpl : public Config {
public:
  ConfigImpl(
      const envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings& config);

  bool disableOutlierEvents() const override { return false; }
  std::chrono::milliseconds opTimeout() const override { return op_timeout_; }
  bool enableHashtagging() const override { return enable_hashtagging_; }

private:
  const std::chrono::milliseconds op_timeout_;
  const bool enable_hashtagging_;
};

class ClientImpl : public Client,
                   public Common::Redis::DecoderCallbacks,
                   public Network::ConnectionCallbacks {
public:
  static ClientPtr create(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                          Common::Redis::EncoderPtr&& encoder,
                          Common::Redis::DecoderFactory& decoder_factory, const Config& config);

  ~ClientImpl();

  // RedisProxy::ConnPool::Client
  void addConnectionCallbacks(Network::ConnectionCallbacks& callbacks) override {
    connection_->addConnectionCallbacks(callbacks);
  }
  void close() override;
  PoolRequest* makeRequest(const Common::Redis::RespValue& request,
                           PoolCallbacks& callbacks) override;

private:
  struct UpstreamReadFilter : public Network::ReadFilterBaseImpl {
    UpstreamReadFilter(ClientImpl& parent) : parent_(parent) {}

    // Network::ReadFilter
    Network::FilterStatus onData(Buffer::Instance& data, bool) override {
      parent_.onData(data);
      return Network::FilterStatus::Continue;
    }

    ClientImpl& parent_;
  };

  struct PendingRequest : public PoolRequest {
    PendingRequest(ClientImpl& parent, PoolCallbacks& callbacks);
    ~PendingRequest();

    // RedisProxy::ConnPool::PoolRequest
    void cancel() override;

    ClientImpl& parent_;
    PoolCallbacks& callbacks_;
    bool canceled_{};
  };

  ClientImpl(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
             Common::Redis::EncoderPtr&& encoder, Common::Redis::DecoderFactory& decoder_factory,
             const Config& config);
  void onConnectOrOpTimeout();
  void onData(Buffer::Instance& data);
  void putOutlierEvent(Upstream::Outlier::Result result);

  // Common::Redis::DecoderCallbacks
  void onRespValue(Common::Redis::RespValuePtr&& value) override;

  // Network::ConnectionCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  Upstream::HostConstSharedPtr host_;
  Network::ClientConnectionPtr connection_;
  Common::Redis::EncoderPtr encoder_;
  Buffer::OwnedImpl encoder_buffer_;
  Common::Redis::DecoderPtr decoder_;
  const Config& config_;
  std::list<PendingRequest> pending_requests_;
  Event::TimerPtr connect_or_op_timer_;
  bool connected_{};
};

class ClientFactoryImpl : public ClientFactory {
public:
  // RedisProxy::ConnPool::ClientFactoryImpl
  ClientPtr create(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                   const Config& config) override;

  static ClientFactoryImpl instance_;

private:
  Common::Redis::DecoderFactoryImpl decoder_factory_;
};

class InstanceImpl : public Instance {
public:
  InstanceImpl(
      const std::string& cluster_name, Upstream::ClusterManager& cm, ClientFactory& client_factory,
      ThreadLocal::SlotAllocator& tls,
      const envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings& config);

  // RedisProxy::ConnPool::Instance
  PoolRequest* makeRequest(const std::string& key, const Common::Redis::RespValue& request,
                           PoolCallbacks& callbacks) override;

private:
  struct ThreadLocalPool;

  struct ThreadLocalActiveClient : public Network::ConnectionCallbacks {
    ThreadLocalActiveClient(ThreadLocalPool& parent) : parent_(parent) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    ThreadLocalPool& parent_;
    Upstream::HostConstSharedPtr host_;
    ClientPtr redis_client_;
  };

  typedef std::unique_ptr<ThreadLocalActiveClient> ThreadLocalActiveClientPtr;

  struct ThreadLocalPool : public ThreadLocal::ThreadLocalObject,
                           public Upstream::ClusterUpdateCallbacks {
    ThreadLocalPool(InstanceImpl& parent, Event::Dispatcher& dispatcher, std::string cluster_name);
    ~ThreadLocalPool();
    PoolRequest* makeRequest(const std::string& key, const Common::Redis::RespValue& request,
                             PoolCallbacks& callbacks);
    void onClusterAddOrUpdateNonVirtual(Upstream::ThreadLocalCluster& cluster);
    void onHostsRemoved(const std::vector<Upstream::HostSharedPtr>& hosts_removed);

    // Upstream::ClusterUpdateCallbacks
    void onClusterAddOrUpdate(Upstream::ThreadLocalCluster& cluster) override {
      onClusterAddOrUpdateNonVirtual(cluster);
    }
    void onClusterRemoval(const std::string& cluster_name) override;

    InstanceImpl& parent_;
    Event::Dispatcher& dispatcher_;
    const std::string cluster_name_;
    Upstream::ClusterUpdateCallbacksHandlePtr cluster_update_handle_;
    Upstream::ThreadLocalCluster* cluster_{};
    std::unordered_map<Upstream::HostConstSharedPtr, ThreadLocalActiveClientPtr> client_map_;
    Envoy::Common::CallbackHandle* host_set_member_update_cb_handle_{};
  };

  struct LbContextImpl : public Upstream::LoadBalancerContextBase {
    LbContextImpl(const std::string& key, bool enabled_hashtagging)
        : hash_key_(MurmurHash::murmurHash2_64(hashtag(key, enabled_hashtagging))) {}

    absl::optional<uint64_t> computeHashKey() override { return hash_key_; }

    absl::string_view hashtag(absl::string_view v, bool enabled);

    const absl::optional<uint64_t> hash_key_;
  };

  Upstream::ClusterManager& cm_;
  ClientFactory& client_factory_;
  ThreadLocal::SlotPtr tls_;
  ConfigImpl config_;
};

} // namespace ConnPool
} // namespace RedisProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
