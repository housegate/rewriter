#include "rewriter-http-server.h"

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <google/protobuf/util/json_util.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "rewriter-server.h"
#include "rewriter.pb.h"

namespace rewriter_http {

namespace {

namespace pb_util = google::protobuf::util;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;

void writeJson(HTTPServerResponse &response, const std::string &body,
               HTTPResponse::HTTPStatus status = HTTPResponse::HTTP_OK) {
  response.setStatus(status);
  response.setContentType("application/json");
  response.setContentLength(static_cast<std::streamsize>(body.size()));
  response.send() << body;
}

void writeError(HTTPServerResponse &response, const std::string &msg,
                HTTPResponse::HTTPStatus status = HTTPResponse::HTTP_BAD_REQUEST) {
  // Hand-build to avoid pulling in a JSON lib for a one-shot escape.
  std::string escaped;
  escaped.reserve(msg.size() + 8);
  for (char c : msg) {
    if (c == '"' || c == '\\') escaped.push_back('\\');
    if (c == '\n') { escaped += "\\n"; continue; }
    if (c == '\r') { escaped += "\\r"; continue; }
    escaped.push_back(c);
  }
  writeJson(response, std::string("{\"error\":\"") + escaped + "\"}", status);
}

template <typename Req, typename Resp, typename HandlerFn>
void handleProtoEndpoint(HTTPServerRequest &request, HTTPServerResponse &response,
                         HandlerFn &&handler) {
  if (request.getMethod() != Poco::Net::HTTPRequest::HTTP_POST) {
    writeError(response, "method not allowed; expected POST",
               HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
    return;
  }

  std::ostringstream oss;
  Poco::StreamCopier::copyStream(request.stream(), oss);
  const std::string body_in = oss.str();

  Req req;
  if (auto status = pb_util::JsonStringToMessage(body_in, &req); !status.ok()) {
    writeError(response, std::string("invalid JSON: ") + status.ToString());
    return;
  }

  Resp resp;
  handler(&req, &resp);

  std::string body_out;
  pb_util::JsonPrintOptions opts;
  // protobuf v25 renamed `always_print_primitive_fields` →
  // `always_print_fields_with_no_presence`; ClickHouse contrib is on the new
  // version. Keep proto field names (snake_case) so the JSON matches the .proto.
  opts.always_print_fields_with_no_presence = true;
  opts.preserve_proto_field_names = true;
  if (auto status = pb_util::MessageToJsonString(resp, &body_out, opts); !status.ok()) {
    writeError(response, "failed to serialize response", HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
    return;
  }
  writeJson(response, body_out);
}

class RewriteHandler : public HTTPRequestHandler {
public:
  explicit RewriteHandler(RewriterServiceImpl *core) : core_(core) {}
  void handleRequest(HTTPServerRequest &request, HTTPServerResponse &response) override {
    handleProtoEndpoint<rewriter::RewriteSQLRequest, rewriter::RewriteSQLResponse>(
      request, response,
      [this](const rewriter::RewriteSQLRequest *req, rewriter::RewriteSQLResponse *resp) {
        core_->doRewrite(req, resp);
      });
  }

private:
  RewriterServiceImpl *core_;
};

class RewriteErrorMessageHandler : public HTTPRequestHandler {
public:
  explicit RewriteErrorMessageHandler(RewriterServiceImpl *core) : core_(core) {}
  void handleRequest(HTTPServerRequest &request, HTTPServerResponse &response) override {
    handleProtoEndpoint<rewriter::RewriteErrorMessageRequest,
                        rewriter::RewriteErrorMessageResponse>(
      request, response,
      [this](const rewriter::RewriteErrorMessageRequest *req,
             rewriter::RewriteErrorMessageResponse *resp) {
        core_->doRewriteErrorMessage(req, resp);
      });
  }

private:
  RewriterServiceImpl *core_;
};

class OptimizeHandler : public HTTPRequestHandler {
public:
  explicit OptimizeHandler(RewriterServiceImpl *core) : core_(core) {}
  void handleRequest(HTTPServerRequest &request, HTTPServerResponse &response) override {
    handleProtoEndpoint<rewriter::OptimizeRequest, rewriter::OptimizeResponse>(
      request, response,
      [this](const rewriter::OptimizeRequest *req, rewriter::OptimizeResponse *resp) {
        core_->doOptimize(req, resp);
      });
  }

private:
  RewriterServiceImpl *core_;
};

class MaterializeHandler : public HTTPRequestHandler {
public:
  explicit MaterializeHandler(RewriterServiceImpl *core) : core_(core) {}
  void handleRequest(HTTPServerRequest &request, HTTPServerResponse &response) override {
    handleProtoEndpoint<rewriter::MaterializeSQLRequest, rewriter::MaterializeSQLResponse>(
      request, response,
      [this](const rewriter::MaterializeSQLRequest *req, rewriter::MaterializeSQLResponse *resp) {
        core_->doMaterializeSQL(req, resp);
      });
  }

private:
  RewriterServiceImpl *core_;
};

class HealthHandler : public HTTPRequestHandler {
public:
  void handleRequest(HTTPServerRequest &, HTTPServerResponse &response) override {
    response.setStatus(HTTPResponse::HTTP_OK);
    response.setContentType("text/plain");
    response.send() << "ok";
  }
};

class NotFoundHandler : public HTTPRequestHandler {
public:
  void handleRequest(HTTPServerRequest &request, HTTPServerResponse &response) override {
    writeError(response, std::string("no route: ") + request.getURI(), HTTPResponse::HTTP_NOT_FOUND);
  }
};

class RewriterHttpHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory {
public:
  explicit RewriterHttpHandlerFactory(RewriterServiceImpl *core) : core_(core) {}

  HTTPRequestHandler *createRequestHandler(const HTTPServerRequest &request) override {
    Poco::URI uri(request.getURI());
    const std::string &path = uri.getPath();
    if (path == "/rewrite" || path == "/rewriter.RewriterService/Rewrite") {
      return new RewriteHandler(core_);
    }
    if (path == "/rewriteerrormessage"
        || path == "/rewriter.RewriterService/RewriteErrorMessage") {
      return new RewriteErrorMessageHandler(core_);
    }
    if (path == "/optimize" || path == "/rewriter.RewriterService/Optimize") {
      return new OptimizeHandler(core_);
    }
    if (path == "/materialize" || path == "/rewriter.RewriterService/MaterializeSQL") {
      return new MaterializeHandler(core_);
    }
    if (path == "/healthz" || path == "/health" || path == "/") {
      return new HealthHandler();
    }
    return new NotFoundHandler();
  }

private:
  RewriterServiceImpl *core_;
};

} // namespace

void RunHttpServer(uint16_t port, RewriterServiceImpl &core) {
  // Function-local static keeps the HTTPServer (and its thread pool) alive
  // for the rest of the process. Reentrant calls would clobber the previous
  // instance, but we only ever call this once from main().
  static std::unique_ptr<Poco::Net::HTTPServer> server;

  Poco::Net::ServerSocket socket(Poco::Net::SocketAddress("0.0.0.0", port));
  auto *params = new Poco::Net::HTTPServerParams();
  params->setMaxThreads(16);
  params->setMaxQueued(64);

  server = std::make_unique<Poco::Net::HTTPServer>(
    new RewriterHttpHandlerFactory(&core), socket, params);
  server->start();

  std::cout << "[http] RewriterService listening on 0.0.0.0:" << port
            << " (POST /rewrite, /rewriteerrormessage, /optimize, /materialize; GET /healthz)" << std::endl;
}

} // namespace rewriter_http
