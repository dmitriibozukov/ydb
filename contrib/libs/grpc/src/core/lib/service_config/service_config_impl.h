//
// Copyright 2015 gRPC authors.
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

#ifndef GRPC_CORE_LIB_SERVICE_CONFIG_SERVICE_CONFIG_IMPL_H
#define GRPC_CORE_LIB_SERVICE_CONFIG_SERVICE_CONFIG_IMPL_H

#include <grpc/support/port_platform.h>

#include <unordered_map>
#include <vector>

#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/service_config/service_config.h"
#include "src/core/lib/service_config/service_config_parser.h"
#include "src/core/lib/slice/slice_internal.h"

// The main purpose of the code here is to parse the service config in
// JSON form, which will look like this:
//
// {
//   "loadBalancingPolicy": "string",  // optional
//   "methodConfig": [  // array of one or more method_config objects
//     {
//       "name": [  // array of one or more name objects
//         {
//           "service": "string",  // required
//           "method": "string",  // optional
//         }
//       ],
//       // remaining fields are optional.
//       // see https://developers.google.com/protocol-buffers/docs/proto3#json
//       // for format details.
//       "waitForReady": bool,
//       "timeout": "duration_string",
//       "maxRequestMessageBytes": "int64_string",
//       "maxResponseMessageBytes": "int64_string",
//     }
//   ]
// }

namespace grpc_core {

class ServiceConfigImpl final : public ServiceConfig {
 public:
  /// Creates a new service config from parsing \a json_string.
  /// Returns null on parse error.
  static RefCountedPtr<ServiceConfig> Create(const grpc_channel_args* args,
                                             y_absl::string_view json_string,
                                             grpc_error_handle* error);

  ServiceConfigImpl(const grpc_channel_args* args, TString json_string,
                    Json json, grpc_error_handle* error);
  ~ServiceConfigImpl() override;

  y_absl::string_view json_string() const override { return json_string_; }

  /// Retrieves the global parsed config at index \a index. The
  /// lifetime of the returned object is tied to the lifetime of the
  /// ServiceConfig object.
  ServiceConfigParser::ParsedConfig* GetGlobalParsedConfig(
      size_t index) override {
    GPR_DEBUG_ASSERT(index < parsed_global_configs_.size());
    return parsed_global_configs_[index].get();
  }

  /// Retrieves the vector of parsed configs for the method identified
  /// by \a path.  The lifetime of the returned vector and contained objects
  /// is tied to the lifetime of the ServiceConfig object.
  const ServiceConfigParser::ParsedConfigVector* GetMethodParsedConfigVector(
      const grpc_slice& path) const override;

 private:
  // Helper functions for parsing the method configs.
  grpc_error_handle ParsePerMethodParams(const grpc_channel_args* args);
  grpc_error_handle ParseJsonMethodConfig(const grpc_channel_args* args,
                                          const Json& json);

  // Returns a path string for the JSON name object specified by json.
  // Sets *error on error.
  static TString ParseJsonMethodName(const Json& json,
                                         grpc_error_handle* error);

  TString json_string_;
  Json json_;

  std::vector<std::unique_ptr<ServiceConfigParser::ParsedConfig>>
      parsed_global_configs_;
  // A map from the method name to the parsed config vector. Note that we are
  // using a raw pointer and not a unique pointer so that we can use the same
  // vector for multiple names.
  std::unordered_map<grpc_slice, const ServiceConfigParser::ParsedConfigVector*,
                     SliceHash>
      parsed_method_configs_map_;
  // Default method config.
  const ServiceConfigParser::ParsedConfigVector* default_method_config_vector_ =
      nullptr;
  // Storage for all the vectors that are being used in
  // parsed_method_configs_table_.
  y_absl::InlinedVector<std::unique_ptr<ServiceConfigParser::ParsedConfigVector>,
                      32>
      parsed_method_config_vectors_storage_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_LIB_SERVICE_CONFIG_SERVICE_CONFIG_IMPL_H */
