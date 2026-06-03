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

#include <fstream>
#include <string>
#include <memory>

#include <utils/log.h>

#include "authentication.h"

static std::string GetFileContent(const std::string& path)
{
  std::ifstream ifs(path);
  if (!ifs) {
    BOOST_LOG_TRIVIAL(fatal) << "File " << path << " not found";
    exit(1);
  }
  return std::string((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
}

static std::shared_ptr<grpc::ServerCredentials>
SslCredentialsHelper(const std::string& ppath, const std::string& cpath,
                     const std::string& rpath, bool client_cert)
{
  grpc::SslServerCredentialsOptions ssl_opts;

  ssl_opts.client_certificate_request = client_cert
    ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
    : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;

  grpc::SslServerCredentialsOptions::PemKeyCertPair pkcp = {
    GetFileContent(ppath),
    GetFileContent(cpath)
  };
  ssl_opts.pem_key_cert_pairs.push_back(pkcp);
  ssl_opts.pem_root_certs = rpath.empty() ? "" : GetFileContent(rpath);

  return grpc::SslServerCredentials(ssl_opts);
}

std::shared_ptr<grpc::ServerCredentials> AuthBuilder::build()
{
  std::shared_ptr<grpc::ServerCredentials> cred;

  if (!private_key_path.empty() && !cert_path.empty()
      && username.empty() && password.empty()) {
    BOOST_LOG_TRIVIAL(info) << "Mutual TLS authentication";
    return SslCredentialsHelper(private_key_path, cert_path, root_cert_path, true);
  }

  if (!private_key_path.empty() && !cert_path.empty()
      && !username.empty() && !password.empty()) {
    BOOST_LOG_TRIVIAL(info) << "Username/Password over TLS authentication";
    cred = SslCredentialsHelper(private_key_path, cert_path, root_cert_path, false);
    cred->SetAuthMetadataProcessor(
        std::make_shared<UserPassAuthenticator>(username, password));
    return cred;
  }

  if (insecure) {
    BOOST_LOG_TRIVIAL(info) << "Insecure authentication";
    return grpc::InsecureServerCredentials();
  }

  if (private_key_path.empty() && cert_path.empty()
      && !username.empty() && !password.empty())
    BOOST_LOG_TRIVIAL(fatal) << "Impossible to use user/pass auth with insecure connection";

  BOOST_LOG_TRIVIAL(fatal) << "Unsupported Authentication method";
  exit(1);
}

AuthBuilder& AuthBuilder::setKeyPath(std::string keyPath)
  { private_key_path = std::move(keyPath); return *this; }
AuthBuilder& AuthBuilder::setCertPath(std::string certPath)
  { cert_path = std::move(certPath); return *this; }
AuthBuilder& AuthBuilder::setRootCertPath(std::string rootpath)
  { root_cert_path = std::move(rootpath); return *this; }
AuthBuilder& AuthBuilder::setUsername(std::string user)
  { username = std::move(user); return *this; }
AuthBuilder& AuthBuilder::setPassword(std::string pass)
  { password = std::move(pass); return *this; }
AuthBuilder& AuthBuilder::setInsecure(bool mode)
  { insecure = mode; return *this; }

grpc::Status UserPassAuthenticator::Process(
    const InputMetadata& auth_metadata,
    grpc::AuthContext* context,
    OutputMetadata* consumed_auth_metadata,
    OutputMetadata* response_metadata)
{
  (void)context; (void)response_metadata;

  auto user_kv = auth_metadata.find("username");
  if (user_kv == auth_metadata.end()) {
    BOOST_LOG_TRIVIAL(error) << "No username field";
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "No username field");
  }
  auto pass_kv = auth_metadata.find("password");
  if (pass_kv == auth_metadata.end()) {
    BOOST_LOG_TRIVIAL(error) << "No password field";
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "No password field");
  }

  if (password.compare(pass_kv->second.data()) != 0 ||
      username.compare(user_kv->second.data()) != 0) {
    BOOST_LOG_TRIVIAL(error) << "Invalid username/password";
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid username/password");
  }

  consumed_auth_metadata->insert(std::make_pair(
        std::string(user_kv->first.data(), user_kv->first.length()),
        std::string(user_kv->second.data(), user_kv->second.length())));
  consumed_auth_metadata->insert(std::make_pair(
        std::string(pass_kv->first.data(), pass_kv->first.length()),
        std::string(pass_kv->second.data(), pass_kv->second.length())));

  return grpc::Status::OK;
}
