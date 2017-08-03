#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "envoy/common/pure.h"
#include "envoy/network/address.h"

#include "spdlog/spdlog.h"

namespace Envoy {
namespace Server {

/**
 * Whether to run Envoy in serving mode, or in config validation mode at one of two levels (in which
 * case we'll verify the configuration file is valid, print any errors, and exit without serving.)
 */
enum class Mode {
  /**
   * Default mode: Regular Envoy serving process. Configs are validated in the normal course of
   * initialization, but if all is well we proceed to serve traffic.
   */
  Serve,

  /**
   * Validate as much as possible without opening network connections upstream or downstream.
   */
  Validate,

  // TODO(rlazarus): Add a third option for "light validation": Mock out access to the filesystem.
  // Perform no validation of files referenced in the config, such as runtime configs, SSL certs,
  // etc. Validation will pass even if those files are malformed or don't exist, allowing the config
  // to be validated in a non-prod environment.
};

/**
 * General options for the server.
 */
class Options {
public:
  virtual ~Options() {}

  /**
   * @return uint64_t the base ID for the server. This is required for system-wide things like
   *         shared memory, domain sockets, etc. that are used during hot restart. Setting the
   *         base ID to a different value will allow the server to run multiple times on the same
   *         host if desired.
   */
  virtual uint64_t baseId() PURE;

  /**
   * @return the number of worker threads to run in the server.
   */
  virtual uint32_t concurrency() PURE;

  /**
   * @return the number of seconds that envoy will perform draining during a hot restart.
   */
  virtual std::chrono::seconds drainTime() PURE;

  /**
   * @return const std::string& the path to the configuration file.
   */
  virtual const std::string& configPath() PURE;

  /**
   * @return const std::string& the path to the v2 bootstrap file.
   * TODO(htuch): We can eventually consolidate configPath()/bootstrapPath(), but today
   * the config fetched from bootstrapPath() acts as an overlay to the config fetched from
   * configPath() during v2 API bringup.
   */
  virtual const std::string& bootstrapPath() PURE;

  /**
   * @return const std::string& the admin address output file.
   */
  virtual const std::string& adminAddressPath() PURE;

  /**
   * @return Network::Address::IpVersion the local address IP version.
   */
  virtual Network::Address::IpVersion localAddressIpVersion() PURE;

  /**
   * @return spdlog::level::level_enum the default log level for the server.
   */
  virtual spdlog::level::level_enum logLevel() PURE;

  /**
   * @return the number of seconds that envoy will wait before shutting down the parent envoy during
   *         a host restart. Generally this will be longer than the drainTime() option.
   */
  virtual std::chrono::seconds parentShutdownTime() PURE;

  /**
   * @return the restart epoch. 0 indicates the first server start, 1 the second, and so on.
   */
  virtual uint64_t restartEpoch() PURE;

  /**
   * @return whether to verify the configuration file is valid, print any errors, and exit
   *         without serving.
   */
  virtual Mode mode() const PURE;

  /**
   * @return std::chrono::milliseconds the duration in msec between log flushes.
   */
  virtual std::chrono::milliseconds fileFlushIntervalMsec() PURE;
};

} // namespace Server
} // namespace Envoy
