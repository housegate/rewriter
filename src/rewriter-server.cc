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
        const char * pos = query.data();
        const char * end = pos + query.size();

        DB::ParserQuery parser(end, false);
        try {
            DB::ASTPtr ast = parseQuery(parser, pos, end, 0, 0, 0);
            for (auto &option : request->options()) {
                switch (option.op())
                {
                case rewriter::RewriteOp::LimitRewrite: {
                    auto limit = option.limit_args().limit();
                    DB::ASTRewriteLimitTransformer limit_transformer = DB::ASTRewriteLimitTransformer();
                    limit_transformer.setLimit(limit);
                    limit_transformer.transform(ast);
                    break;
                }
                case rewriter::RewriteOp::TableNameRewrite: {
                    auto table_name_map = option.table_name_args().table_map();
                    std::map<std::string, std::string> table_name_map_;
                    for (auto &kv : table_name_map) {
                        table_name_map_[kv.first] = kv.second;
                    }
                    DB::ASTReplaceTransformer transformer = DB::ASTReplaceTransformer();
                    transformer.setTableNameMap(table_name_map_);
                    transformer.transform(ast);
                    break;
                }
                case rewriter::RewriteOp::OffsetRewrite: {
                    auto offset = option.offset_args().offset();
                    DB::ASTRewriteOffsetTransformer offset_transformer = DB::ASTRewriteOffsetTransformer();
                    offset_transformer.setOffset(offset);
                    offset_transformer.transform(ast);
                    break;
                }
                default:
                    break;
                }
            }
            response->set_sql_after_rewrite(ast->formatForLogging());
            response->set_message("success");
            response->set_code(rewriter::RewriteCode::Success);
            return Status::OK;
        } catch (const DB::Exception &e) {
            response->set_sql_after_rewrite("Error parsing query");
            response->set_message(e.displayText());
            response->set_code(rewriter::RewriteCode::SyntaxError);
            return Status::OK;
        }
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
    if (argc > 1) {
        // Convert the first command line argument to an integer for the port
        int port = std::atoi(argv[1]);
        RunServer(port);
    } else {
        // Use default port 50051
        RunServer(50051);
    }
    return 0;
}