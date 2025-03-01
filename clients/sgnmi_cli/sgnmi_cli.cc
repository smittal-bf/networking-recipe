// Copyright 2019-present Open Networking Foundation
// Copyright 2022-2023 Intel Corp
// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "gflags/gflags.h"
#include "gnmi/gnmi.grpc.pb.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/security/credentials.h"
#include "grpcpp/security/tls_credentials_options.h"
#include "re2/re2.h"
#include "stratum/glue/init_google.h"
#include "stratum/glue/status/status.h"
#include "stratum/glue/status/status_macros.h"
#include "stratum/lib/constants.h"
#include "stratum/lib/macros.h"
#include "stratum/lib/security/credentials_manager.h"
#include "stratum/lib/utils.h"

#define DEFAULT_CERTS_DIR "/usr/share/stratum/certs/"

DEFINE_bool(grpc_use_insecure_mode, false,
            "grpc communication in insecure mode");
DEFINE_string(grpc_addr, stratum::kLocalStratumUrl, "gNMI server address");
DEFINE_string(bool_val, "", "Boolean value to be set");
DEFINE_string(int_val, "", "Integer value to be set (64-bit)");
DEFINE_string(uint_val, "", "Unsigned integer value to be set (64-bit)");
DEFINE_string(string_val, "", "String value to be set");
DEFINE_string(float_val, "", "Floating point value to be set");
DEFINE_string(proto_bytes, "", "Protobytes value to be set");
DEFINE_string(bytes_val_file, "", "A file to be sent as bytes value");

DEFINE_bool(replace, false, "Use replace instead of update");
DEFINE_string(get_type, "ALL", "The gNMI get request type");
DEFINE_uint64(interval, 5000, "Subscribe poll interval in ms");

#define PRINT_MSG(msg, prompt)                   \
  do {                                           \
    std::cout << prompt << std::endl;            \
    std::cout << msg.DebugString() << std::endl; \
  } while (0)

#define RETURN_IF_GRPC_ERROR(expr)                                           \
  do {                                                                       \
    const ::grpc::Status _grpc_status = (expr);                              \
    if (ABSL_PREDICT_FALSE(!_grpc_status.ok() &&                             \
                           _grpc_status.error_code() != grpc::CANCELLED)) {  \
      ::util::Status _status(                                                \
          static_cast<::util::error::Code>(_grpc_status.error_code()),       \
          _grpc_status.error_message());                                     \
      LOG(ERROR) << "Return Error: " << #expr << " failed with " << _status; \
      return _status;                                                        \
    }                                                                        \
  } while (0)

namespace stratum {
namespace tools {
namespace gnmi {
namespace {

const char kUsage[] =
    R"USAGE(usage: sgnmi_cli [--help] [Options] COMMAND PATH

Secure gNMI CLI

positional arguments:
  COMMAND                  gNMI command
  PATH                     gNMI path

optional arguments:
  --grpc_addr GRPC_ADDR    gNMI server address
  --ca-cert                CA certificate
  --client-cert            gRPC Client certificate
  --client-key             gRPC Client key
  --grpc_use_insecure_mode Insecure mode (default: false)

[get request only]
  --get-type TYPE          Use specific data type for get request
                           (ALL,CONFIG,STATE,OPERATIONAL)

[set request only]
  --bool_val BOOL_VAL      Set boolean value
  --int_val INT_VAL        Set int value (64-bit)
  --uint_val UINT_VAL      Set uint value (64-bit)
  --string_val STRING_VAL  Set string value
  --float_val FLOAT_VAL    Set float value
  --proto_bytes BYTES_VAL  Set proto_bytes value
  --bytes_val_file FILE    File to be sent as bytes value
  --replace                Replace instead of updating

[sample subscribe only]
  --interval INTERVAL      Sample subscribe poll interval in ms

commands:
  get                      Get Request
  set                      Set Request
  cap                      Capability Request
  del                      Delete Request
  sub-onchange             Subscribe On Change Request
  sub-sample               Subscribe Sampled Request
)USAGE";

// Pipe file descriptors used to transfer signals from the handler to the cancel
// function.
int pipe_read_fd_ = -1;
int pipe_write_fd_ = -1;

// Pointer to the client context to cancel the blocking calls.
grpc::ClientContext* ctx_ = nullptr;

void HandleSignal(int signal) {
  static_assert(sizeof(signal) <= PIPE_BUF,
                "PIPE_BUF is smaller than the number of bytes that can be "
                "written atomically to a pipe.");
  // We must restore any changes made to errno at the end of the handler:
  // https://www.gnu.org/software/libc/manual/html_node/POSIX-Safety-Concepts.html
  int saved_errno = errno;
  // No reasonable error handling possible.
  write(pipe_write_fd_, &signal, sizeof(signal));
  errno = saved_errno;
}

void* ContextCancelThreadFunc(void*) {
  int signal_value;
  int ret = read(pipe_read_fd_, &signal_value, sizeof(signal_value));
  if (ret == 0) {  // Pipe has been closed.
    return nullptr;
  } else if (ret != sizeof(signal_value)) {
    LOG(ERROR) << "Error reading complete signal from pipe: " << ret << ": "
               << strerror(errno);
    return nullptr;
  }
  if (ctx_) ctx_->TryCancel();
  LOG(INFO) << "Client context cancelled.";
  return nullptr;
}

bool StringToBool(std::string str) {
  return (str == "y") || (str == "true") || (str == "t") || (str == "yes") ||
         (str == "1");
}

void AddPathElem(std::string elem_name, std::string elem_kv,
                 ::gnmi::PathElem* elem) {
  elem->set_name(elem_name);
  if (!elem_kv.empty()) {
    std::string key, value;
    RE2::FullMatch(elem_kv, "\\[([^=]+)=([^\\]]+)\\]", &key, &value);
    (*elem->mutable_key())[key] = value;
  }
}

void BuildGnmiPath(std::string path_str, ::gnmi::Path* path) {
  re2::StringPiece input(path_str);
  std::string elem_name, elem_kv;
  while (RE2::Consume(&input, "/([^/\\[]+)(\\[([^=]+=[^\\]]+)\\])?", &elem_name,
                      &elem_kv)) {
    auto* elem = path->add_elem();
    AddPathElem(elem_name, elem_kv, elem);
  }
}

::gnmi::GetRequest BuildGnmiGetRequest(std::string path) {
  ::gnmi::GetRequest req;
  BuildGnmiPath(path, req.add_path());
  req.set_encoding(::gnmi::PROTO);
  ::gnmi::GetRequest::DataType data_type;
  if (!::gnmi::GetRequest::DataType_Parse(FLAGS_get_type, &data_type)) {
    std::cout << "Invalid gNMI get data type: " << FLAGS_get_type
              << " , use ALL as data type." << std::endl;
    data_type = ::gnmi::GetRequest::ALL;
  }
  req.set_type(data_type);
  return req;
}

::gnmi::SetRequest BuildGnmiSetRequest(std::string path) {
  ::gnmi::SetRequest req;
  ::gnmi::Update* update;
  if (FLAGS_replace) {
    update = req.add_replace();
  } else {
    update = req.add_update();
  }
  BuildGnmiPath(path, update->mutable_path());
  if (!FLAGS_bool_val.empty()) {
    update->mutable_val()->set_bool_val(StringToBool(FLAGS_bool_val));
  } else if (!FLAGS_int_val.empty()) {
    update->mutable_val()->set_int_val(stoll(FLAGS_int_val));
  } else if (!FLAGS_uint_val.empty()) {
    update->mutable_val()->set_uint_val(stoull(FLAGS_uint_val));
  } else if (!FLAGS_float_val.empty()) {
    update->mutable_val()->set_float_val(stof(FLAGS_float_val));
  } else if (!FLAGS_proto_bytes.empty()) {
    update->mutable_val()->set_proto_bytes(FLAGS_proto_bytes);
  } else if (!FLAGS_string_val.empty()) {
    update->mutable_val()->set_string_val(FLAGS_string_val);
  } else if (!FLAGS_bytes_val_file.empty()) {
    std::string buf;
    ::stratum::ReadFileToString(FLAGS_bytes_val_file, &buf);
    update->mutable_val()->set_bytes_val(buf);
  } else {
    std::cout << "No typed value set" << std::endl;
  }
  return req;
}

::gnmi::SetRequest BuildGnmiDeleteRequest(std::string path) {
  ::gnmi::SetRequest req;
  auto* del = req.add_delete_();
  BuildGnmiPath(path, del);
  return req;
}

::gnmi::SubscribeRequest BuildGnmiSubOnchangeRequest(std::string path) {
  ::gnmi::SubscribeRequest sub_req;
  auto* sub_list = sub_req.mutable_subscribe();
  sub_list->set_mode(::gnmi::SubscriptionList::STREAM);
  sub_list->set_updates_only(true);
  auto* sub = sub_list->add_subscription();
  sub->set_mode(::gnmi::ON_CHANGE);
  BuildGnmiPath(path, sub->mutable_path());
  return sub_req;
}

::gnmi::SubscribeRequest BuildGnmiSubSampleRequest(std::string path,
                                                   uint64 interval) {
  ::gnmi::SubscribeRequest sub_req;
  auto* sub_list = sub_req.mutable_subscribe();
  sub_list->set_mode(::gnmi::SubscriptionList::STREAM);
  sub_list->set_updates_only(true);
  auto* sub = sub_list->add_subscription();
  sub->set_mode(::gnmi::SAMPLE);
  sub->set_sample_interval(interval);
  BuildGnmiPath(path, sub->mutable_path());
  return sub_req;
}

::util::Status Main(int argc, char** argv) {
  // Default certificate file location for TLS-mode
  FLAGS_ca_cert_file = DEFAULT_CERTS_DIR "ca.crt";
  FLAGS_server_key_file = DEFAULT_CERTS_DIR "stratum.key";
  FLAGS_server_cert_file = DEFAULT_CERTS_DIR "stratum.crt";
  FLAGS_client_key_file = DEFAULT_CERTS_DIR "client.key";
  FLAGS_client_cert_file = DEFAULT_CERTS_DIR "client.crt";

  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  ::gflags::SetUsageMessage(kUsage);
  InitGoogle(argv[0], &argc, &argv, true);
  stratum::InitStratumLogging();
  if (argc < 2) {
    std::cout << kUsage << std::endl;
    return MAKE_ERROR(ERR_INVALID_PARAM) << "Invalid number of arguments.";
  }

  ::grpc::ClientContext ctx;
  ctx_ = &ctx;
  // Create the pipe to transfer signals.
  {
    RETURN_IF_ERROR(
        CreatePipeForSignalHandling(&pipe_read_fd_, &pipe_write_fd_));
  }
  RET_CHECK(std::signal(SIGINT, HandleSignal) != SIG_ERR);
  pthread_t context_cancel_tid;
  RET_CHECK(pthread_create(&context_cancel_tid, nullptr,
                           ContextCancelThreadFunc, nullptr) == 0);
  auto cleaner = absl::MakeCleanup([&context_cancel_tid, &ctx] {
    int signal = SIGINT;
    write(pipe_write_fd_, &signal, sizeof(signal));
    if (pthread_join(context_cancel_tid, nullptr) != 0) {
      LOG(ERROR) << "Failed to join the context cancel thread.";
    }
    close(pipe_write_fd_);
    close(pipe_read_fd_);
    // We call this to synchronize the internal client context state.
    ctx.TryCancel();
  });

  std::shared_ptr<::grpc::Channel> channel;
  if (FLAGS_grpc_use_insecure_mode) {
    ASSIGN_OR_RETURN(auto credentials_manager,
                     CredentialsManager::CreateInstance(true));
    channel = ::grpc::CreateChannel(
        FLAGS_grpc_addr,
        credentials_manager->GenerateExternalFacingClientCredentials());
  } else {
    std::shared_ptr<::grpc::ChannelCredentials> channel_credentials =
        ::grpc::InsecureChannelCredentials();
    channel = ::grpc::CreateChannel(FLAGS_grpc_addr, channel_credentials);
  }

  auto stub = ::gnmi::gNMI::NewStub(channel);
  std::string cmd = std::string(argv[1]);

  if (cmd == "cap") {
    ::gnmi::CapabilityRequest req;
    PRINT_MSG(req, "REQUEST");
    ::gnmi::CapabilityResponse resp;
    RETURN_IF_GRPC_ERROR(stub->Capabilities(&ctx, req, &resp));
    PRINT_MSG(resp, "RESPONSE");
    return ::util::OkStatus();
  }

  if (argc < 3) {
    return MAKE_ERROR(ERR_INVALID_PARAM)
           << "Missing path for " << cmd << " request.";
  }
  std::string path = std::string(argv[2]);

  if (cmd == "get") {
    ::gnmi::GetRequest req = BuildGnmiGetRequest(path);
    PRINT_MSG(req, "REQUEST");
    ::gnmi::GetResponse resp;
    RETURN_IF_GRPC_ERROR(stub->Get(&ctx, req, &resp));
    PRINT_MSG(resp, "RESPONSE");
  } else if (cmd == "set") {
    ::gnmi::SetRequest req = BuildGnmiSetRequest(path);
    PRINT_MSG(req, "REQUEST");
    ::gnmi::SetResponse resp;
    RETURN_IF_GRPC_ERROR(stub->Set(&ctx, req, &resp));
    PRINT_MSG(resp, "RESPONSE");
  } else if (cmd == "del") {
    ::gnmi::SetRequest req = BuildGnmiDeleteRequest(path);
    PRINT_MSG(req, "REQUEST");
    ::gnmi::SetResponse resp;
    RETURN_IF_GRPC_ERROR(stub->Set(&ctx, req, &resp));
    PRINT_MSG(resp, "RESPONSE");
  } else if (cmd == "sub-onchange") {
    auto stream_reader_writer = stub->Subscribe(&ctx);
    ::gnmi::SubscribeRequest req = BuildGnmiSubOnchangeRequest(path);
    PRINT_MSG(req, "REQUEST");
    RET_CHECK(stream_reader_writer->Write(req)) << "Cannot write request.";
    ::gnmi::SubscribeResponse resp;
    while (stream_reader_writer->Read(&resp)) {
      PRINT_MSG(resp, "RESPONSE");
    }
    RETURN_IF_GRPC_ERROR(stream_reader_writer->Finish());
  } else if (cmd == "sub-sample") {
    auto stream_reader_writer = stub->Subscribe(&ctx);
    ::gnmi::SubscribeRequest req =
        BuildGnmiSubSampleRequest(path, FLAGS_interval);
    PRINT_MSG(req, "REQUEST");
    RET_CHECK(stream_reader_writer->Write(req)) << "Cannot write request.";
    ::gnmi::SubscribeResponse resp;
    while (stream_reader_writer->Read(&resp)) {
      PRINT_MSG(resp, "RESPONSE");
    }
    RETURN_IF_GRPC_ERROR(stream_reader_writer->Finish());
  } else {
    return MAKE_ERROR(ERR_INVALID_PARAM) << "Unknown command: " << cmd;
  }
  LOG(INFO) << "Done.";

  return ::util::OkStatus();
}

}  // namespace
}  // namespace gnmi
}  // namespace tools
}  // namespace stratum

int main(int argc, char** argv) {
  return stratum::tools::gnmi::Main(argc, argv).error_code();
}
