#pragma once

#include "envoy/config/subscription.h"
#include "envoy/event/dispatcher.h"
#include "envoy/filesystem/filesystem.h"

#include "common/common/logger.h"
#include "common/common/macros.h"
#include "common/config/utility.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

#include "api/base.pb.h"

namespace Envoy {
namespace Config {

/**
 * Filesystem inotify implementation of the API Subscription interface. This allows the API to be
 * consumed on filesystem changes to files containing the JSON canonical representation of
 * lists of ResourceType.
 */
template <class ResourceType>
class FilesystemSubscriptionImpl : public Config::Subscription<ResourceType>,
                                   Logger::Loggable<Logger::Id::config> {
public:
  FilesystemSubscriptionImpl(Event::Dispatcher& dispatcher, const std::string& path,
                             SubscriptionStats stats)
      : path_(path), watcher_(dispatcher.createFilesystemWatcher()), stats_(stats) {}

  // Config::Subscription
  void start(const std::vector<std::string>& resources,
             Config::SubscriptionCallbacks<ResourceType>& callbacks) override {
    // We report all discovered resources in the watched file.
    UNREFERENCED_PARAMETER(resources);
    callbacks_ = &callbacks;
    watcher_->addWatch(path_, Filesystem::Watcher::Events::MovedTo, [this](uint32_t events) {
      UNREFERENCED_PARAMETER(events);
      refresh();
    });
    // Attempt to read in case there is a file there already.
    refresh();
  }

  void updateResources(const std::vector<std::string>& resources) override {
    // We report all discovered resources in the watched file.
    UNREFERENCED_PARAMETER(resources);
  }

private:
  void refresh() {
    ENVOY_LOG(debug, "Filesystem config refresh for {}", path_);
    stats_.update_attempt_.inc();
    bool config_update_available = false;
    try {
      envoy::api::v2::DiscoveryResponse message;
      MessageUtil::loadFromFile(path_, message);
      const auto typed_resources = Config::Utility::getTypedResources<ResourceType>(message);
      config_update_available = true;
      callbacks_->onConfigUpdate(typed_resources);
      stats_.update_success_.inc();
      // TODO(htuch): Add some notion of current version for every API in stats/admin.
    } catch (const EnvoyException& e) {
      if (config_update_available) {
        ENVOY_LOG(warn, "Filesystem config update rejected: {}", e.what());
        stats_.update_rejected_.inc();
      } else {
        ENVOY_LOG(warn, "Filesystem config update failure: {}", e.what());
        stats_.update_failure_.inc();
      }
      callbacks_->onConfigUpdateFailed(&e);
    }
  }

  const std::string path_;
  std::unique_ptr<Filesystem::Watcher> watcher_;
  SubscriptionCallbacks<ResourceType>* callbacks_{};
  SubscriptionStats stats_;
};

} // namespace Config
} // namespace Envoy
