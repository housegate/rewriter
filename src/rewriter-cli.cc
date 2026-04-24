// src/rewriter-cli.cc
//
// CLI wrapper over RewriterServiceImpl::doRewrite and ::doRewriteErrorMessage.
// Reads SQL from --sql, --sql-file, or stdin; rewrite options come from
// --options <file>.json (optional) plus shortcut flags that append options.
#include <google/protobuf/util/json_util.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "rewriter-server.h"
#include "rewriter.grpc.pb.h"

namespace {

void printUsageAndExit(int code) {
  std::cerr <<
R"(sql-rewriter-cli — run the rewriter on a single SQL string and print the result.

USAGE:
  sql-rewriter-cli [--sql <SQL> | --sql-file <PATH> | -]
                   [--error-message <MSG>]
                   [--options <PATH.json>]
                   [--force-limit N]
                   [--replace-limit N --replace-threshold M]
                   [--offset N]
                   [--rename-table OLD=NEW] (repeatable; SELECT path only)
                   [--setting KEY=VALUE:TYPE]  (TYPE in string|int|uint64|bool, repeatable)
                   [--json]
                   [--help]

SQL SOURCE (exactly one of):
  --sql <s>         Inline SQL.
  --sql-file <p>    Read SQL from file.
  -                 Read SQL from stdin.
  (no flag)         Also reads from stdin.

OUTPUT:
  Default:          Rewritten SQL on stdout.
  --json:           Full RewriteSQLResponse (or RewriteErrorMessageResponse) as JSON.

EXIT CODE:
  0                 Success.
  1                 Non-Success RewriteCode; message on stderr.
  2                 Bad CLI usage.
)";
  std::exit(code);
}

struct SettingSpec {
  std::string key;
  std::string value;
  std::string type;  // string|int|uint64|bool
};

struct Args {
  std::optional<std::string> sql;
  std::optional<std::string> sql_file;
  bool sql_from_stdin = false;
  std::optional<std::string> error_message;
  std::optional<std::string> options_path;

  std::optional<int32_t> force_limit;
  std::optional<int32_t> replace_limit;
  std::optional<int32_t> replace_threshold;
  std::optional<int32_t> offset;
  std::vector<std::pair<std::string, std::string>> rename_tables;  // OLD=NEW
  std::vector<SettingSpec> settings;

  bool json = false;
};

[[noreturn]] void die(const std::string &msg, int code = 2) {
  std::cerr << "sql-rewriter-cli: " << msg << "\n";
  std::exit(code);
}

std::string need(int argc, char **argv, int &i, const char *flag) {
  if (i + 1 >= argc) die(std::string(flag) + " requires an argument");
  return std::string(argv[++i]);
}

std::pair<std::string, std::string> splitOnce(const std::string &s, char sep) {
  auto pos = s.find(sep);
  if (pos == std::string::npos) {
    die(std::string("expected 'KEY") + sep + "VALUE', got: " + s);
  }
  return {s.substr(0, pos), s.substr(pos + 1)};
}

Args parseArgs(int argc, char **argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--help" || a == "-h") printUsageAndExit(0);
    else if (a == "--sql")               args.sql = need(argc, argv, i, "--sql");
    else if (a == "--sql-file")          args.sql_file = need(argc, argv, i, "--sql-file");
    else if (a == "-")                   args.sql_from_stdin = true;
    else if (a == "--error-message")     args.error_message = need(argc, argv, i, "--error-message");
    else if (a == "--options")           args.options_path = need(argc, argv, i, "--options");
    else if (a == "--force-limit")       args.force_limit = std::atoi(need(argc, argv, i, "--force-limit").c_str());
    else if (a == "--replace-limit")     args.replace_limit = std::atoi(need(argc, argv, i, "--replace-limit").c_str());
    else if (a == "--replace-threshold") args.replace_threshold = std::atoi(need(argc, argv, i, "--replace-threshold").c_str());
    else if (a == "--offset")            args.offset = std::atoi(need(argc, argv, i, "--offset").c_str());
    else if (a == "--rename-table") {
      auto [l, r] = splitOnce(need(argc, argv, i, "--rename-table"), '=');
      args.rename_tables.emplace_back(l, r);
    } else if (a == "--setting") {
      std::string raw = need(argc, argv, i, "--setting");
      auto [kv, type] = splitOnce(raw, ':');
      auto [k, v] = splitOnce(kv, '=');
      args.settings.push_back({k, v, type});
    } else if (a == "--json") {
      args.json = true;
    } else {
      die("unknown flag: " + std::string(a));
    }
  }

  int sql_sources =
    (args.sql ? 1 : 0) + (args.sql_file ? 1 : 0) + (args.sql_from_stdin ? 1 : 0);
  if (sql_sources > 1) die("specify at most one of --sql, --sql-file, -");
  return args;
}

std::string readAll(std::istream &in) {
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

std::string readSql(const Args &args) {
  if (args.sql) return *args.sql;
  if (args.sql_file) {
    std::ifstream f(*args.sql_file);
    if (!f) die("cannot open --sql-file: " + *args.sql_file);
    return readAll(f);
  }
  // Default: stdin (also `-`).
  return readAll(std::cin);
}

rewriter::RewriteSQLRequest loadBaseRequest(const Args &args) {
  rewriter::RewriteSQLRequest req;
  if (!args.options_path) return req;

  std::ifstream f(*args.options_path);
  if (!f) die("cannot open --options: " + *args.options_path);
  std::string body = readAll(f);

  google::protobuf::util::JsonParseOptions opts;
  opts.ignore_unknown_fields = false;
  auto status = google::protobuf::util::JsonStringToMessage(body, &req, opts);
  if (!status.ok()) die("invalid --options JSON: " + status.ToString());
  return req;
}

rewriter::SettingType parseSettingType(const std::string &t) {
  if (t == "string") return rewriter::SettingType::String;
  if (t == "int")    return rewriter::SettingType::Int;
  if (t == "uint64") return rewriter::SettingType::Uint64;
  if (t == "bool")   return rewriter::SettingType::Bool;
  die("unknown --setting type (want string|int|uint64|bool): " + t);
}

void applyShortcutFlags(const Args &args, rewriter::RewriteSQLRequest &req) {
  if (args.force_limit) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::LimitRewrite);
    opt->mutable_limit_args()->set_force_limit(*args.force_limit);
  }
  if (args.replace_limit) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::LimitRewrite);
    auto *rl = opt->mutable_limit_args()->mutable_replace_limit();
    rl->set_replace_to(*args.replace_limit);
    rl->set_threshold(args.replace_threshold.value_or(0));
  }
  if (args.offset) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::OffsetRewrite);
    opt->mutable_offset_args()->set_offset(*args.offset);
  }
  if (!args.rename_tables.empty()) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *m = opt->mutable_table_name_args()->mutable_select_stmt_args()->mutable_table_map();
    for (const auto &[k, v] : args.rename_tables) (*m)[k] = v;
  }
  if (!args.settings.empty()) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::SettingsRewrite);
    for (const auto &s : args.settings) {
      auto *proto_s = opt->mutable_settings_args()->add_settings();
      proto_s->set_key(s.key);
      proto_s->set_type(parseSettingType(s.type));
      switch (proto_s->type()) {
        case rewriter::SettingType::String: proto_s->set_string_value(s.value); break;
        case rewriter::SettingType::Int:    proto_s->set_int_value(std::atoi(s.value.c_str())); break;
        case rewriter::SettingType::Uint64: proto_s->set_uint64_value(std::strtoull(s.value.c_str(), nullptr, 10)); break;
        case rewriter::SettingType::Bool:   proto_s->set_bool_value(s.value == "true" || s.value == "1"); break;
        default: break;
      }
    }
  }
}

template <typename Resp>
int emitResponse(const Resp &resp, bool json, const char *text_field) {
  const int exit_code = (resp.code() == rewriter::RewriteCode::Success) ? 0 : 1;
  if (json) {
    std::string out;
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    opts.preserve_proto_field_names = true;
    opts.always_print_fields_with_no_presence = true;
    (void)google::protobuf::util::MessageToJsonString(resp, &out, opts);
    std::cout << out;
  } else {
    if constexpr (std::is_same_v<Resp, rewriter::RewriteSQLResponse>) {
      std::cout << resp.sql_after_rewrite() << "\n";
    } else {
      std::cout << resp.error_after_rewrite() << "\n";
    }
    (void)text_field;
  }
  if (exit_code != 0) std::cerr << "sql-rewriter-cli: " << resp.message() << "\n";
  return exit_code;
}

} // namespace

int main(int argc, char **argv) {
  Args args = parseArgs(argc, argv);

  rewriter::RewriteSQLRequest req = loadBaseRequest(args);
  applyShortcutFlags(args, req);
  req.set_sql(readSql(args));

  RewriterServiceImpl svc;

  // The rewriter dumps AST trees to std::cout during transformation. That's
  // fine for the server (nobody pipes its stdout to anything) but unusable for
  // a CLI whose stdout IS the output. Swallow std::cout for the duration of
  // the call and only print the response to the real stdout.
  std::stringstream swallow;
  std::streambuf *saved_cout = std::cout.rdbuf(swallow.rdbuf());
  struct RestoreCout {
    std::streambuf *buf;
    ~RestoreCout() { std::cout.rdbuf(buf); }
  } restore{saved_cout};

  if (args.error_message) {
    rewriter::RewriteErrorMessageRequest ereq;
    ereq.set_sql(req.sql());
    ereq.set_error_message(*args.error_message);
    for (const auto &o : req.options()) *ereq.add_options() = o;

    rewriter::RewriteErrorMessageResponse eresp;
    svc.doRewriteErrorMessage(&ereq, &eresp);
    std::cout.rdbuf(saved_cout);  // restore early so emit writes to real stdout
    return emitResponse(eresp, args.json, "error_after_rewrite");
  }

  rewriter::RewriteSQLResponse resp;
  svc.doRewrite(&req, &resp);
  std::cout.rdbuf(saved_cout);  // restore early so emit writes to real stdout
  return emitResponse(resp, args.json, "sql_after_rewrite");
}
