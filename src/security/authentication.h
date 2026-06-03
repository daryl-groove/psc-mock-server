/*
 * Copyright 2020 Yohan Pipereau
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <string>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/auth_metadata_processor.h>

class UserPassAuthenticator final : public grpc::AuthMetadataProcessor {
  public:
    UserPassAuthenticator(std::string user, std::string pass)
      : username(user), password(pass) {}
    ~UserPassAuthenticator() {}

    grpc::Status Process(const InputMetadata& auth_metadata,
                         grpc::AuthContext* context,
                         OutputMetadata* consumed_auth_metadata,
                         OutputMetadata* response_metadata) override;

  private:
    std::string username, password;
};


/* Supported Authentication methods */
enum AuthType {
  INSECURE,       // No Username/password, no encryption
  USERPASS_TLS,   // Username/password, TLS encryption
  MUTUAL_TLS      // TLS authentication & encryption
};

class AuthBuilder {
  public:
    AuthBuilder() {}
    ~AuthBuilder() {}

    std::shared_ptr<grpc::ServerCredentials> build();

    AuthBuilder& setKeyPath(std::string keyPath);
    AuthBuilder& setCertPath(std::string certPath);
    AuthBuilder& setRootCertPath(std::string rootPath);
    AuthBuilder& setUsername(std::string username);
    AuthBuilder& setPassword(std::string password);
    AuthBuilder& setInsecure(bool mode);

  private:
    std::string private_key_path, cert_path, root_cert_path;
    std::string username, password;
    bool insecure = false;
};
