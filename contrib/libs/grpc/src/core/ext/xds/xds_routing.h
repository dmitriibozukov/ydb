//
//
// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#ifndef GRPC_CORE_EXT_XDS_XDS_ROUTING_H
#define GRPC_CORE_EXT_XDS_XDS_ROUTING_H

#include <grpc/support/port_platform.h>

#include <vector>

#include "y_absl/strings/string_view.h"

#include <grpc/support/log.h>

#include "src/core/ext/xds/xds_listener.h"
#include "src/core/ext/xds/xds_route_config.h"
#include "src/core/lib/matchers/matchers.h"
#include "src/core/lib/transport/metadata_batch.h"

namespace grpc_core {

class XdsRouting {
 public:
  class VirtualHostListIterator {
   public:
    virtual ~VirtualHostListIterator() = default;
    // Returns the number of virtual hosts in the list.
    virtual size_t Size() const = 0;
    // Returns the domain list for the virtual host at the specified index.
    virtual const std::vector<TString>& GetDomainsForVirtualHost(
        size_t index) const = 0;
  };

  class RouteListIterator {
   public:
    virtual ~RouteListIterator() = default;
    // Number of routes.
    virtual size_t Size() const = 0;
    // Returns the matchers for the route at the specified index.
    virtual const XdsRouteConfigResource::Route::Matchers& GetMatchersForRoute(
        size_t index) const = 0;
  };

  // Returns the index of the selected virtual host in the list.
  static y_absl::optional<size_t> FindVirtualHostForDomain(
      const VirtualHostListIterator& vhost_iterator, y_absl::string_view domain);

  // Returns the index in route_list_iterator to use for a request with
  // the specified path and metadata, or nullopt if no route matches.
  static y_absl::optional<size_t> GetRouteForRequest(
      const RouteListIterator& route_list_iterator, y_absl::string_view path,
      grpc_metadata_batch* initial_metadata);

  // Returns true if \a domain_pattern is a valid domain pattern, false
  // otherwise.
  static bool IsValidDomainPattern(y_absl::string_view domain_pattern);

  // Returns the metadata value(s) for the specified key.
  // As special cases, binary headers return a value of y_absl::nullopt, and
  // "content-type" header returns "application/grpc".
  static y_absl::optional<y_absl::string_view> GetHeaderValue(
      grpc_metadata_batch* initial_metadata, y_absl::string_view header_name,
      TString* concatenated_value);

  struct GeneratePerHttpFilterConfigsResult {
    // Map of field name to list of elements for that field
    std::map<TString, std::vector<TString>> per_filter_configs;
    grpc_error_handle error = GRPC_ERROR_NONE;
    // Guaranteed to be nullptr if error is GRPC_ERROR_NONE
    grpc_channel_args* args = nullptr;
  };

  // Generates a map of per_filter_configs. \a args is consumed.
  static GeneratePerHttpFilterConfigsResult GeneratePerHTTPFilterConfigs(
      const std::vector<XdsListenerResource::HttpConnectionManager::HttpFilter>&
          http_filters,
      const XdsRouteConfigResource::VirtualHost& vhost,
      const XdsRouteConfigResource::Route& route,
      const XdsRouteConfigResource::Route::RouteAction::ClusterWeight*
          cluster_weight,
      grpc_channel_args* args);
};

}  // namespace grpc_core

#endif  // GRPC_CORE_EXT_XDS_XDS_ROUTING_H
