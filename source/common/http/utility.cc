#include "common/http/utility.h"

#include <cstdint>
#include <string>
#include <vector>

#include "envoy/http/header_map.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/http/exception.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/network/utility.h"
#include "common/protobuf/utility.h"

#include "spdlog/spdlog.h"

namespace Envoy {
namespace Http {

void Utility::appendXff(HeaderMap& headers, const Network::Address::Instance& remote_address) {
  if (remote_address.type() != Network::Address::Type::Ip) {
    return;
  }

  HeaderString& header = headers.insertForwardedFor().value();
  if (!header.empty()) {
    header.append(", ", 2);
  }

  const std::string& address_as_string = remote_address.ip()->addressAsString();
  header.append(address_as_string.c_str(), address_as_string.size());
}

std::string Utility::createSslRedirectPath(const HeaderMap& headers) {
  ASSERT(headers.Host());
  ASSERT(headers.Path());
  return fmt::format("https://{}{}", headers.Host()->value().c_str(),
                     headers.Path()->value().c_str());
}

Utility::QueryParams Utility::parseQueryString(const std::string& url) {
  QueryParams params;
  size_t start = url.find('?');
  if (start == std::string::npos) {
    return params;
  }

  start++;
  while (start < url.size()) {
    size_t end = url.find('&', start);
    if (end == std::string::npos) {
      end = url.size();
    }

    size_t equal = url.find('=', start);
    if (equal != std::string::npos) {
      params.emplace(StringUtil::subspan(url, start, equal),
                     StringUtil::subspan(url, equal + 1, end));
    } else {
      params.emplace(StringUtil::subspan(url, start, end), "");
    }

    start = end + 1;
  }

  return params;
}

std::string Utility::parseCookieValue(const HeaderMap& headers, const std::string& key) {

  struct State {
    std::string key_;
    std::string ret_;
  };

  State state;
  state.key_ = key;

  headers.iterate(
      [](const HeaderEntry& header, void* context) -> void {
        // Find the cookie headers in the request (typically, there's only one).
        if (header.key() == Http::Headers::get().Cookie.get().c_str()) {
          // Split the cookie header into individual cookies.
          for (const std::string& s : StringUtil::split(std::string{header.value().c_str()}, ';')) {
            // Find the key part of the cookie (i.e. the name of the cookie).
            size_t first_non_space = s.find_first_not_of(" ");
            size_t equals_index = s.find('=');
            if (equals_index == std::string::npos) {
              // The cookie is malformed if it does not have an `=`. Continue
              // checking other cookies in this header.
              continue;
            }
            std::string k = s.substr(first_non_space, equals_index - first_non_space);
            State* state = static_cast<State*>(context);
            // If the key matches, parse the value from the rest of the cookie string.
            if (k == state->key_) {
              std::string v = s.substr(equals_index + 1, s.size() - 1);

              // Cookie values may be wrapped in double quotes.
              // https://tools.ietf.org/html/rfc6265#section-4.1.1
              if (v.size() >= 2 && v.back() == '"' && v[0] == '"') {
                v = v.substr(1, v.size() - 2);
              }
              state->ret_ = v;
              return;
            }
          }
        }
      },
      &state);

  return state.ret_;
}

uint64_t Utility::getResponseStatus(const HeaderMap& headers) {
  const HeaderEntry* header = headers.Status();
  uint64_t response_code;
  if (!header || !StringUtil::atoul(headers.Status()->value().c_str(), response_code)) {
    throw CodecClientException(":status must be specified and a valid unsigned long");
  }

  return response_code;
}

bool Utility::isInternalRequest(const HeaderMap& headers) {
  // The current header
  const HeaderEntry* forwarded_for = headers.ForwardedFor();
  if (!forwarded_for) {
    return false;
  }

  return Network::Utility::isInternalAddress(forwarded_for->value().c_str());
}

bool Utility::isWebSocketUpgradeRequest(const HeaderMap& headers) {
  return (headers.Connection() && headers.Upgrade() &&
          (0 == StringUtil::caseInsensitiveCompare(
                    headers.Connection()->value().c_str(),
                    Http::Headers::get().ConnectionValues.Upgrade.c_str())) &&
          (0 == StringUtil::caseInsensitiveCompare(
                    headers.Upgrade()->value().c_str(),
                    Http::Headers::get().UpgradeValues.WebSocket.c_str())));
}

Http2Settings Utility::parseHttp2Settings(const envoy::api::v2::Http2ProtocolOptions& config) {
  Http2Settings ret;
  ret.hpack_table_size_ = PROTOBUF_GET_WRAPPED_OR_DEFAULT(
      config, hpack_table_size, Http::Http2Settings::DEFAULT_HPACK_TABLE_SIZE);
  ret.max_concurrent_streams_ = PROTOBUF_GET_WRAPPED_OR_DEFAULT(
      config, max_concurrent_streams, Http::Http2Settings::DEFAULT_MAX_CONCURRENT_STREAMS);
  ret.initial_stream_window_size_ = PROTOBUF_GET_WRAPPED_OR_DEFAULT(
      config, initial_stream_window_size, Http::Http2Settings::DEFAULT_INITIAL_STREAM_WINDOW_SIZE);
  ret.initial_connection_window_size_ =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, initial_connection_window_size,
                                      Http::Http2Settings::DEFAULT_INITIAL_CONNECTION_WINDOW_SIZE);
  return ret;
}

Http1Settings Utility::parseHttp1Settings(const Json::Object& config) {
  Http1Settings ret;

  Json::ObjectSharedPtr http1_settings = config.getObject("http1_settings", true);
  ret.allow_absolute_url_ = http1_settings->getBoolean("allow_absolute_url", false);
  return ret;
}

void Utility::sendLocalReply(StreamDecoderFilterCallbacks& callbacks, const bool& is_reset,
                             Code response_code, const std::string& body_text) {
  HeaderMapPtr response_headers{
      new HeaderMapImpl{{Headers::get().Status, std::to_string(enumToInt(response_code))}}};
  if (!body_text.empty()) {
    response_headers->insertContentLength().value(body_text.size());
    response_headers->insertContentType().value(Headers::get().ContentTypeValues.Text);
  }

  callbacks.encodeHeaders(std::move(response_headers), body_text.empty());
  if (!body_text.empty() && !is_reset) {
    Buffer::OwnedImpl buffer(body_text);
    // TODO(htuch): We shouldn't encodeData() if the stream is reset in the encodeHeaders() above,
    // see https://github.com/lyft/envoy/issues/1283.
    callbacks.encodeData(buffer, true);
  }
}

void Utility::sendRedirect(StreamDecoderFilterCallbacks& callbacks, const std::string& new_path) {
  HeaderMapPtr response_headers{
      new HeaderMapImpl{{Headers::get().Status, std::to_string(enumToInt(Code::MovedPermanently))},
                        {Headers::get().Location, new_path}}};

  callbacks.encodeHeaders(std::move(response_headers), true);
}

std::string Utility::getLastAddressFromXFF(const Http::HeaderMap& request_headers) {
  if (!request_headers.ForwardedFor()) {
    return EMPTY_STRING;
  }

  std::vector<std::string> xff_address_list =
      StringUtil::split(request_headers.ForwardedFor()->value().c_str(), ", ");

  if (xff_address_list.empty()) {
    return EMPTY_STRING;
  }
  return xff_address_list.back();
}

} // namespace Http
} // namespace Envoy
