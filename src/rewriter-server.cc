#include "rewriter.grpc.pb.h"
#include <grpc++/grpc++.h>
#include <memory>
#include <iostream>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/formatAST.h>
#include <Parsers/parseQuery.h>
#include <IO/WriteBufferFromString.h>
#include "ASTTransformers.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using rewriter::RewriterService;
using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

class RewriterServiceImpl final : public RewriterService::Service {
    Status Rewrite(ServerContext* context, const RewriteSQLRequest* request, RewriteSQLResponse* response) override {
        String query = request->sql();
        for (auto &option : request->options()) {
            switch (option.op())
            {
            case rewriter::RewriteOp::LimitRewrite:
                /* code */
                break;
            case rewriter::RewriteOp::TableNameRewrite:
                auto table_name_map = option.table_name_args().table_map();
                std::map<std::string, std::string> table_name_map_;
                for (auto &kv : table_name_map) {
                    table_name_map_[kv.first] = kv.second;
                }

                const char * pos = query.data();
                const char * end = pos + query.size();

                DB::ParserQuery parser(end, false);

                DB::ASTPtr ast = parseQuery(parser, pos, end, 0, 0, 0);
                DB::ASTReplaceTransformer transformer = DB::ASTReplaceTransformer();
                transformer.setTableNameMap(table_name_map_);
                transformer.transform(ast);
                response->set_message(ast->formatForLogging());
                break;
            case rewriter::RewriteOp::OffsetRewrite:
                break;
            default:
                break;
            }
        }

        return Status::OK;
    }
};

void RunServer(uint16_t port) {
  std::string server_address = "0.0.0.0:" + std::to_string(port);
  RewriterServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer(50051);
  return 0;
}