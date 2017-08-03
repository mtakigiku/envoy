#include <memory>
#include <string>

#include "envoy/upstream/upstream.h"

#include "common/config/utility.h"
#include "common/network/utility.h"
#include "common/ssl/context_manager_impl.h"
#include "common/stats/stats_impl.h"
#include "common/upstream/cluster_manager_impl.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/access_log/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Pointee;
using testing::Return;
using testing::ReturnNew;
using testing::ReturnRef;
using testing::SaveArg;
using testing::_;

namespace Envoy {
namespace Upstream {
namespace {

// The tests in this file are split between testing with real clusters and some with mock clusters.
// By default we setup to call the real cluster creation function. Individual tests can override
// the expectations when needed.
class TestClusterManagerFactory : public ClusterManagerFactory {
public:
  TestClusterManagerFactory() {
    ON_CALL(*this, clusterFromProto_(_, _, _, _))
        .WillByDefault(Invoke([&](const envoy::api::v2::Cluster& cluster, ClusterManager& cm,
                                  Outlier::EventLoggerSharedPtr outlier_event_logger,
                                  bool added_via_api) -> Cluster* {
          return ClusterImplBase::create(cluster, cm, stats_, tls_, dns_resolver_,
                                         ssl_context_manager_, runtime_, random_, dispatcher_,
                                         local_info_, outlier_event_logger, added_via_api)
              .release();
        }));
  }

  Http::ConnectionPool::InstancePtr allocateConnPool(Event::Dispatcher&, HostConstSharedPtr host,
                                                     ResourcePriority) override {
    return Http::ConnectionPool::InstancePtr{allocateConnPool_(host)};
  }

  ClusterPtr clusterFromProto(const envoy::api::v2::Cluster& cluster, ClusterManager& cm,
                              Outlier::EventLoggerSharedPtr outlier_event_logger,
                              bool added_via_api) override {
    return ClusterPtr{clusterFromProto_(cluster, cm, outlier_event_logger, added_via_api)};
  }

  CdsApiPtr createCds(const Json::Object&, const Optional<SdsConfig>& sds_config,
                      ClusterManager&) override {
    UNREFERENCED_PARAMETER(sds_config);
    return CdsApiPtr{createCds_()};
  }

  ClusterManagerPtr clusterManagerFromJson(const Json::Object& config,
                                           const envoy::api::v2::Bootstrap&, Stats::Store& stats,
                                           ThreadLocal::Instance& tls, Runtime::Loader& runtime,
                                           Runtime::RandomGenerator& random,
                                           const LocalInfo::LocalInfo& local_info,
                                           AccessLog::AccessLogManager& log_manager) override {
    return ClusterManagerPtr{
        clusterManagerFromJson_(config, stats, tls, runtime, random, local_info, log_manager)};
  }

  MOCK_METHOD7(clusterManagerFromJson_,
               ClusterManager*(const Json::Object& config, Stats::Store& stats,
                               ThreadLocal::Instance& tls, Runtime::Loader& runtime,
                               Runtime::RandomGenerator& random,
                               const LocalInfo::LocalInfo& local_info,
                               AccessLog::AccessLogManager& log_manager));
  MOCK_METHOD1(allocateConnPool_, Http::ConnectionPool::Instance*(HostConstSharedPtr host));
  MOCK_METHOD4(clusterFromProto_,
               Cluster*(const envoy::api::v2::Cluster& cluster, ClusterManager& cm,
                        Outlier::EventLoggerSharedPtr outlier_event_logger, bool added_via_api));
  MOCK_METHOD0(createCds_, CdsApi*());

  Stats::IsolatedStoreImpl stats_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  std::shared_ptr<NiceMock<Network::MockDnsResolver>> dns_resolver_{
      new NiceMock<Network::MockDnsResolver>};
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  Ssl::ContextManagerImpl ssl_context_manager_{runtime_};
  NiceMock<Event::MockDispatcher> dispatcher_;
  LocalInfo::MockLocalInfo local_info_;
};

class ClusterManagerImplTest : public testing::Test {
public:
  void create(const Json::Object& config) {
    envoy::api::v2::Bootstrap bootstrap;
    cluster_manager_.reset(new ClusterManagerImpl(
        config, bootstrap, factory_, factory_.stats_, factory_.tls_, factory_.runtime_,
        factory_.random_, factory_.local_info_, log_manager_));
  }

  NiceMock<TestClusterManagerFactory> factory_;
  std::unique_ptr<ClusterManagerImpl> cluster_manager_;
  AccessLog::MockAccessLogManager log_manager_;
};

TEST_F(ClusterManagerImplTest, OutlierEventLog) {
  const std::string json = R"EOF(
  {
    "outlier_detection": {
      "event_log_path": "foo"
    },
    "clusters": []
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_CALL(log_manager_, createAccessLog("foo"));
  create(*loader);
}

TEST_F(ClusterManagerImplTest, NoSdsConfig) {
  const std::string json = fmt::sprintf("{%s}", clustersJson({defaultSdsClusterJson("cluster_1")}));
  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, UnknownClusterType) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "foo",
      "lb_type": "round_robin"
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, LocalClusterNotDefined) {
  const std::string json = fmt::sprintf(
      R"EOF(
  {
    "local_cluster_name": "new_cluster",
    %s
  }
  )EOF",
      clustersJson({defaultStaticClusterJson("cluster_1"), defaultStaticClusterJson("cluster_2")}));

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, BadClusterManagerConfig) {
  const std::string json = R"EOF(
  {
    "outlier_detection": {
      "event_log_path": "foo"
    },
    "clusters": [],
    "fake_property" : "fake_property"
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(create(*loader), Json::Exception);
}

TEST_F(ClusterManagerImplTest, LocalClusterDefined) {
  const std::string json = fmt::sprintf(
      R"EOF(
  {
    "local_cluster_name": "new_cluster",
    %s
  }
  )EOF",
      clustersJson({defaultStaticClusterJson("cluster_1"), defaultStaticClusterJson("cluster_2"),
                    defaultStaticClusterJson("new_cluster")}));

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);

  EXPECT_EQ(3UL, factory_.stats_.counter("cluster_manager.cluster_added").value());
  EXPECT_EQ(3UL, factory_.stats_.gauge("cluster_manager.total_clusters").value());

  factory_.tls_.shutdownThread();
}

TEST_F(ClusterManagerImplTest, DuplicateCluster) {
  const std::string json = fmt::sprintf(
      "{%s}",
      clustersJson({defaultStaticClusterJson("cluster_1"), defaultStaticClusterJson("cluster_1")}));
  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, UnknownHcType) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}],
      "health_check": {
        "type": "foo"
      }
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(create(*loader), EnvoyException);
}

TEST_F(ClusterManagerImplTest, MaxClusterName) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "clusterwithareallyreallylongnamemorethanmaxcharsallowedbyschema"
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW_WITH_MESSAGE(create(*loader), Json::Exception,
                            "JSON at lines 4-6 does not conform to schema.\n Invalid schema: "
                            "#/properties/name\n Schema violation: maxLength\n Offending "
                            "document key: #/name");
}

TEST_F(ClusterManagerImplTest, InvalidClusterNameChars) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster:"
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW_WITH_MESSAGE(create(*loader), Json::Exception,
                            "JSON at lines 4-6 does not conform to schema.\n Invalid schema: "
                            "#/properties/name\n Schema violation: pattern\n Offending document "
                            "key: #/name");
}

TEST_F(ClusterManagerImplTest, TcpHealthChecker) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}],
      "health_check": {
        "type": "tcp",
        "timeout_ms": 1000,
        "interval_ms": 1000,
        "unhealthy_threshold": 2,
        "healthy_threshold": 2,
        "send": [
          {"binary": "01"}
        ],
        "receive": [
          {"binary": "02"}
        ]
      }
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  Network::MockClientConnection* connection = new NiceMock<Network::MockClientConnection>();
  EXPECT_CALL(factory_.dispatcher_, createClientConnection_(PointeesEq(
                                        Network::Utility::resolveUrl("tcp://127.0.0.1:11001"))))
      .WillOnce(Return(connection));
  create(*loader);
  factory_.tls_.shutdownThread();
}

TEST_F(ClusterManagerImplTest, UnknownCluster) {
  const std::string json =
      fmt::sprintf("{%s}", clustersJson({defaultStaticClusterJson("cluster_1")}));

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);
  EXPECT_EQ(nullptr, cluster_manager_->get("hello"));
  EXPECT_EQ(nullptr,
            cluster_manager_->httpConnPoolForCluster("hello", ResourcePriority::Default, nullptr));
  EXPECT_THROW(cluster_manager_->tcpConnForCluster("hello"), EnvoyException);
  EXPECT_THROW(cluster_manager_->httpAsyncClientForCluster("hello"), EnvoyException);
  factory_.tls_.shutdownThread();
}

/**
 * Test that buffer limits are set on new TCP connections.
 */
TEST_F(ClusterManagerImplTest, VerifyBufferLimits) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "per_connection_buffer_limit_bytes": 8192,
      "type": "static",
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://127.0.0.1:11001"}]
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);
  Network::MockClientConnection* connection = new NiceMock<Network::MockClientConnection>();
  EXPECT_CALL(*connection, setBufferLimits(8192));
  EXPECT_CALL(factory_.tls_.dispatcher_, createClientConnection_(_)).WillOnce(Return(connection));
  auto conn_data = cluster_manager_->tcpConnForCluster("cluster_1");
  EXPECT_EQ(connection, conn_data.connection_.get());
  factory_.tls_.shutdownThread();
}

TEST_F(ClusterManagerImplTest, ShutdownOrder) {
  const std::string json =
      fmt::sprintf("{%s}", clustersJson({defaultStaticClusterJson("cluster_1")}));

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);
  const Cluster& cluster = cluster_manager_->clusters().begin()->second;
  EXPECT_EQ("cluster_1", cluster.info()->name());
  EXPECT_EQ(cluster.info(), cluster_manager_->get("cluster_1")->info());
  EXPECT_EQ(1UL, cluster_manager_->get("cluster_1")->hostSet().hosts().size());
  EXPECT_EQ(cluster.hosts()[0],
            cluster_manager_->get("cluster_1")->loadBalancer().chooseHost(nullptr));

  // Local reference, primary reference, thread local reference, host reference.
  EXPECT_EQ(4U, cluster.info().use_count());

  // Thread local reference should be gone.
  factory_.tls_.shutdownThread();
  EXPECT_EQ(3U, cluster.info().use_count());
}

TEST_F(ClusterManagerImplTest, InitializeOrder) {
  const std::string json = fmt::sprintf(
      R"EOF(
  {
    "cds": {"cluster": %s},
    %s
  }
  )EOF",
      defaultStaticClusterJson("cds_cluster"),
      clustersJson({defaultStaticClusterJson("cluster_0"), defaultStaticClusterJson("cluster_1")}));

  MockCdsApi* cds = new MockCdsApi();
  MockCluster* cds_cluster = new NiceMock<MockCluster>();
  cds_cluster->info_->name_ = "cds_cluster";
  MockCluster* cluster1 = new NiceMock<MockCluster>();
  MockCluster* cluster2 = new NiceMock<MockCluster>();
  cluster2->info_->name_ = "fake_cluster2";
  cluster2->info_->lb_type_ = LoadBalancerType::RingHash;

  // This part tests static init.
  InSequence s;
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cds_cluster));
  ON_CALL(*cds_cluster, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cds_cluster, initialize());
  EXPECT_CALL(factory_, createCds_()).WillOnce(Return(cds));
  EXPECT_CALL(*cds, setInitializedCb(_));
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster1));
  ON_CALL(*cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cluster1, initialize());
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster2));
  ON_CALL(*cluster2, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);

  ReadyWatcher initialized;
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  EXPECT_CALL(*cluster2, initialize());
  cds_cluster->initialize_callback_();
  cluster1->initialize_callback_();

  EXPECT_CALL(*cds, initialize());
  cluster2->initialize_callback_();

  // This part tests CDS init.
  MockCluster* cluster3 = new NiceMock<MockCluster>();
  cluster3->info_->name_ = "cluster3";
  MockCluster* cluster4 = new NiceMock<MockCluster>();
  cluster4->info_->name_ = "cluster4";
  MockCluster* cluster5 = new NiceMock<MockCluster>();
  cluster5->info_->name_ = "cluster5";

  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster3));
  ON_CALL(*cluster3, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("cluster3"));

  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster4));
  ON_CALL(*cluster4, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cluster4, initialize());
  cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("cluster4"));

  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster5));
  ON_CALL(*cluster5, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("cluster5"));

  cds->initialized_callback_();

  EXPECT_CALL(*cluster3, initialize());
  cluster4->initialize_callback_();

  // Test cluster 5 getting removed before everything is initialized.
  cluster_manager_->removePrimaryCluster("cluster5");

  EXPECT_CALL(initialized, ready());
  cluster3->initialize_callback_();

  factory_.tls_.shutdownThread();
}

TEST_F(ClusterManagerImplTest, DynamicRemoveWithLocalCluster) {
  InSequence s;

  // Setup a cluster manager with a static local cluster.
  const std::string json = fmt::sprintf(R"EOF(
  {
    "local_cluster_name": "foo",
    %s
  }
  )EOF",
                                        clustersJson({defaultStaticClusterJson("fake")}));

  MockCluster* foo = new NiceMock<MockCluster>();
  foo->info_->name_ = "foo";
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, false)).WillOnce(Return(foo));
  ON_CALL(*foo, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*foo, initialize());

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);
  foo->initialize_callback_();

  // Now add a dynamic cluster. This cluster will have a member update callback from the local
  // cluster in its load balancer.
  MockCluster* cluster1 = new NiceMock<MockCluster>();
  cluster1->info_->name_ = "cluster1";
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, true)).WillOnce(Return(cluster1));
  ON_CALL(*cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cluster1, initialize());
  cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("cluster1"));

  // Add another update callback on foo so we make sure callbacks keep working.
  ReadyWatcher membership_updated;
  foo->addMemberUpdateCb([&membership_updated](const std::vector<HostSharedPtr>&,
                                               const std::vector<HostSharedPtr>&) -> void {
    membership_updated.ready();
  });

  // Remove the new cluster.
  cluster_manager_->removePrimaryCluster("cluster1");

  // Fire a member callback on the local cluster, which should not call any update callbacks on
  // the deleted cluster.
  foo->hosts_ = {HostSharedPtr{new HostImpl(
      foo->info_, "", Network::Utility::resolveUrl("tcp://127.0.0.1:80"), false, 1, "")}};
  EXPECT_CALL(membership_updated, ready());
  foo->runCallbacks(foo->hosts_, {});

  factory_.tls_.shutdownThread();
}

TEST_F(ClusterManagerImplTest, DynamicAddRemove) {
  const std::string json = R"EOF(
  {
    "clusters": []
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);

  InSequence s;
  ReadyWatcher initialized;
  EXPECT_CALL(initialized, ready());
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  MockCluster* cluster1 = new NiceMock<MockCluster>();
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster1));
  EXPECT_CALL(*cluster1, initializePhase()).Times(0);
  EXPECT_CALL(*cluster1, initialize());
  EXPECT_TRUE(cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("fake_cluster")));

  EXPECT_EQ(cluster1->info_, cluster_manager_->get("fake_cluster")->info());
  EXPECT_EQ(1UL, factory_.stats_.gauge("cluster_manager.total_clusters").value());

  // Now try to update again but with the same hash.
  EXPECT_FALSE(cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("fake_cluster")));

  // Now do it again with a different hash.
  auto update_cluster = defaultStaticCluster("fake_cluster");
  update_cluster.mutable_per_connection_buffer_limit_bytes()->set_value(12345);

  MockCluster* cluster2 = new NiceMock<MockCluster>();
  cluster2->hosts_ = {HostSharedPtr{new HostImpl(
      cluster2->info_, "", Network::Utility::resolveUrl("tcp://127.0.0.1:80"), false, 1, "")}};
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster2));
  EXPECT_CALL(*cluster2, initializePhase()).Times(0);
  EXPECT_CALL(*cluster2, initialize());
  EXPECT_TRUE(cluster_manager_->addOrUpdatePrimaryCluster(update_cluster));

  EXPECT_EQ(cluster2->info_, cluster_manager_->get("fake_cluster")->info());
  EXPECT_EQ(1UL, cluster_manager_->clusters().size());
  Http::ConnectionPool::MockInstance* cp = new Http::ConnectionPool::MockInstance();
  EXPECT_CALL(factory_, allocateConnPool_(_)).WillOnce(Return(cp));
  EXPECT_EQ(cp, cluster_manager_->httpConnPoolForCluster("fake_cluster", ResourcePriority::Default,
                                                         nullptr));

  // Now remove it. This should drain the connection pool.
  Http::ConnectionPool::Instance::DrainedCb drained_cb;
  EXPECT_CALL(*cp, addDrainedCallback(_)).WillOnce(SaveArg<0>(&drained_cb));
  EXPECT_TRUE(cluster_manager_->removePrimaryCluster("fake_cluster"));
  EXPECT_EQ(nullptr, cluster_manager_->get("fake_cluster"));
  EXPECT_EQ(0UL, cluster_manager_->clusters().size());

  // Remove an unknown cluster.
  EXPECT_FALSE(cluster_manager_->removePrimaryCluster("foo"));

  drained_cb();

  EXPECT_EQ(1UL, factory_.stats_.counter("cluster_manager.cluster_added").value());
  EXPECT_EQ(1UL, factory_.stats_.counter("cluster_manager.cluster_modified").value());
  EXPECT_EQ(1UL, factory_.stats_.counter("cluster_manager.cluster_removed").value());
  EXPECT_EQ(0UL, factory_.stats_.gauge("cluster_manager.total_clusters").value());
}

TEST_F(ClusterManagerImplTest, AddOrUpdatePrimaryClusterStaticExists) {
  const std::string json =
      fmt::sprintf("{%s}", clustersJson({defaultStaticClusterJson("some_cluster")}));
  MockCluster* cluster1 = new NiceMock<MockCluster>();
  InSequence s;
  EXPECT_CALL(factory_, clusterFromProto_(_, _, _, _)).WillOnce(Return(cluster1));
  ON_CALL(*cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(*cluster1, initialize());

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  create(*loader);

  ReadyWatcher initialized;
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  EXPECT_CALL(initialized, ready());
  cluster1->initialize_callback_();

  EXPECT_FALSE(cluster_manager_->addOrUpdatePrimaryCluster(defaultStaticCluster("fake_cluster")));

  // Attempt to remove a static cluster.
  EXPECT_FALSE(cluster_manager_->removePrimaryCluster("fake_cluster"));

  factory_.tls_.shutdownThread();
}

TEST_F(ClusterManagerImplTest, DynamicHostRemove) {
  const std::string json = R"EOF(
  {
    "clusters": [
    {
      "name": "cluster_1",
      "connect_timeout_ms": 250,
      "type": "strict_dns",
      "dns_resolvers": [ "1.2.3.4:80" ],
      "lb_type": "round_robin",
      "hosts": [{"url": "tcp://localhost:11001"}]
    }]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);

  std::shared_ptr<Network::MockDnsResolver> dns_resolver(new Network::MockDnsResolver());
  EXPECT_CALL(factory_.dispatcher_, createDnsResolver(_)).WillOnce(Return(dns_resolver));

  Network::DnsResolver::ResolveCb dns_callback;
  Event::MockTimer* dns_timer_ = new NiceMock<Event::MockTimer>(&factory_.dispatcher_);
  Network::MockActiveDnsQuery active_dns_query;
  EXPECT_CALL(*dns_resolver, resolve(_, _, _))
      .WillRepeatedly(DoAll(SaveArg<2>(&dns_callback), Return(&active_dns_query)));
  create(*loader);
  EXPECT_FALSE(cluster_manager_->get("cluster_1")->info()->addedViaApi());

  // Test for no hosts returning the correct values before we have hosts.
  EXPECT_EQ(nullptr, cluster_manager_->httpConnPoolForCluster("cluster_1",
                                                              ResourcePriority::Default, nullptr));
  EXPECT_EQ(nullptr, cluster_manager_->tcpConnForCluster("cluster_1").connection_);
  EXPECT_EQ(2UL, factory_.stats_.counter("cluster.cluster_1.upstream_cx_none_healthy").value());

  // Set up for an initialize callback.
  ReadyWatcher initialized;
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });
  EXPECT_CALL(initialized, ready());

  dns_callback(TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}));

  // After we are initialized, we should immediately get called back if someone asks for an
  // initialize callback.
  EXPECT_CALL(initialized, ready());
  cluster_manager_->setInitializedCb([&]() -> void { initialized.ready(); });

  EXPECT_CALL(factory_, allocateConnPool_(_))
      .Times(4)
      .WillRepeatedly(ReturnNew<Http::ConnectionPool::MockInstance>());

  // This should provide us a CP for each of the above hosts.
  Http::ConnectionPool::MockInstance* cp1 = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default, nullptr));
  Http::ConnectionPool::MockInstance* cp2 = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default, nullptr));
  Http::ConnectionPool::MockInstance* cp1_high = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::High, nullptr));
  Http::ConnectionPool::MockInstance* cp2_high = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::High, nullptr));

  EXPECT_NE(cp1, cp2);
  EXPECT_NE(cp1_high, cp2_high);
  EXPECT_NE(cp1, cp1_high);

  Http::ConnectionPool::Instance::DrainedCb drained_cb;
  EXPECT_CALL(*cp1, addDrainedCallback(_)).WillOnce(SaveArg<0>(&drained_cb));
  Http::ConnectionPool::Instance::DrainedCb drained_cb_high;
  EXPECT_CALL(*cp1_high, addDrainedCallback(_)).WillOnce(SaveArg<0>(&drained_cb_high));

  // Remove the first host, this should lead to the first cp being drained.
  dns_timer_->callback_();
  dns_callback(TestUtility::makeDnsResponse({"127.0.0.2"}));
  drained_cb();
  drained_cb = nullptr;
  EXPECT_CALL(factory_.tls_.dispatcher_, deferredDelete_(_)).Times(2);
  drained_cb_high();
  drained_cb_high = nullptr;

  // Make sure we get back the same connection pool for the 2nd host as we did before the change.
  Http::ConnectionPool::MockInstance* cp3 = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::Default, nullptr));
  Http::ConnectionPool::MockInstance* cp3_high = dynamic_cast<Http::ConnectionPool::MockInstance*>(
      cluster_manager_->httpConnPoolForCluster("cluster_1", ResourcePriority::High, nullptr));
  EXPECT_EQ(cp2, cp3);
  EXPECT_EQ(cp2_high, cp3_high);

  // Now add and remove a host that we never have a conn pool to. This should not lead to any
  // drain callbacks, etc.
  dns_timer_->callback_();
  dns_callback(TestUtility::makeDnsResponse({"127.0.0.2", "127.0.0.3"}));
  dns_timer_->callback_();
  dns_callback(TestUtility::makeDnsResponse({"127.0.0.2"}));

  factory_.tls_.shutdownThread();
}

TEST(ClusterManagerInitHelper, ImmediateInitialize) {
  InSequence s;
  ClusterManagerInitHelper init_helper;

  NiceMock<MockCluster> cluster1;
  ON_CALL(cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(cluster1, initialize());
  init_helper.addCluster(cluster1);
  cluster1.initialize_callback_();

  init_helper.onStaticLoadComplete();

  ReadyWatcher cm_initialized;
  EXPECT_CALL(cm_initialized, ready());
  init_helper.setInitializedCb([&]() -> void { cm_initialized.ready(); });
}

TEST(ClusterManagerInitHelper, StaticSdsInitialize) {
  InSequence s;
  ClusterManagerInitHelper init_helper;

  NiceMock<MockCluster> sds;
  ON_CALL(sds, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(sds, initialize());
  init_helper.addCluster(sds);
  sds.initialize_callback_();

  NiceMock<MockCluster> cluster1;
  ON_CALL(cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  init_helper.addCluster(cluster1);

  EXPECT_CALL(cluster1, initialize());
  init_helper.onStaticLoadComplete();

  ReadyWatcher cm_initialized;
  init_helper.setInitializedCb([&]() -> void { cm_initialized.ready(); });

  EXPECT_CALL(cm_initialized, ready());
  cluster1.initialize_callback_();
}

TEST(ClusterManagerInitHelper, UpdateAlreadyInitialized) {
  InSequence s;
  ClusterManagerInitHelper init_helper;

  ReadyWatcher cm_initialized;
  init_helper.setInitializedCb([&]() -> void { cm_initialized.ready(); });

  NiceMock<MockCluster> cluster1;
  ON_CALL(cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(cluster1, initialize());
  init_helper.addCluster(cluster1);

  NiceMock<MockCluster> cluster2;
  ON_CALL(cluster2, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(cluster2, initialize());
  init_helper.addCluster(cluster2);

  init_helper.onStaticLoadComplete();

  cluster1.initialize_callback_();
  init_helper.removeCluster(cluster1);

  EXPECT_CALL(cm_initialized, ready());
  cluster2.initialize_callback_();
}

TEST(ClusterManagerInitHelper, AddSecondaryAfterSecondaryInit) {
  InSequence s;
  ClusterManagerInitHelper init_helper;

  ReadyWatcher cm_initialized;
  init_helper.setInitializedCb([&]() -> void { cm_initialized.ready(); });

  NiceMock<MockCluster> cluster1;
  ON_CALL(cluster1, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Primary));
  EXPECT_CALL(cluster1, initialize());
  init_helper.addCluster(cluster1);

  NiceMock<MockCluster> cluster2;
  ON_CALL(cluster2, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  init_helper.addCluster(cluster2);

  init_helper.onStaticLoadComplete();

  EXPECT_CALL(cluster2, initialize());
  cluster1.initialize_callback_();

  NiceMock<MockCluster> cluster3;
  ON_CALL(cluster3, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  EXPECT_CALL(cluster3, initialize());
  init_helper.addCluster(cluster3);

  cluster3.initialize_callback_();
  EXPECT_CALL(cm_initialized, ready());
  cluster2.initialize_callback_();
}

TEST(ClusterManagerInitHelper, RemoveClusterWithinInitLoop) {
  // Tests the scenario encountered in Issue 903: The cluster was removed from
  // the secondary init list while traversing the list.

  InSequence s;
  ClusterManagerInitHelper init_helper;
  NiceMock<MockCluster> cluster;
  ON_CALL(cluster, initializePhase()).WillByDefault(Return(Cluster::InitializePhase::Secondary));
  init_helper.addCluster(cluster);

  // Set up the scenario seen in Issue 903 where initialize() ultimately results
  // in the removeCluster() call. In the real bug this was a long and complex call
  // chain.
  EXPECT_CALL(cluster, initialize()).WillOnce(Invoke([&]() -> void {
    init_helper.removeCluster(cluster);
  }));

  // Now call onStaticLoadComplete which will exercise maybeFinishInitialize()
  // which calls initialize() on the members of the secondary init list.
  init_helper.onStaticLoadComplete();
}

} // namespace
} // namespace Upstream
} // namespace Envoy
