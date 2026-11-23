// RewriteSQLRequest.refuse_metadata_access: the opt-in metadata refusal for
// static-mode callers (handlers/metadata_access.h). Without the flag, static
// mode keeps its legacy surface (metadata_access_test.cc pins that).
#include "rewriter.grpc.pb.h"
#include "rewriter-server.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

namespace {

constexpr const char *kRefused = "metadata access is disabled";

// The Sentio analytics engine's request shape: settings, offset, limit, then
// a static TableNameRewrite with table_with_database_map + remote_table_map.
RewriteSQLRequest sentioRequest(const std::string &sql, bool refuse = true) {
  RewriteSQLRequest req;
  req.set_sql(sql);
  req.set_refuse_metadata_access(refuse);
  auto *settings = req.add_options();
  settings->set_op(rewriter::RewriteOp::SettingsRewrite);
  auto *s = settings->mutable_settings_args()->add_settings();
  s->set_key("log_comment");
  s->set_type(rewriter::SettingType::String);
  s->set_string_value("sig");
  auto *limit = req.add_options();
  limit->set_op(rewriter::RewriteOp::LimitRewrite);
  limit->mutable_limit_args()->set_force_limit(1000);
  auto *table = req.add_options();
  table->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *args = table->mutable_table_name_args()->mutable_static_args();
  auto &local = (*args->mutable_table_with_database_map())["Transfer"];
  local.set_database("sentio_proc_1");
  local.set_table("event_transfer_1");
  auto &remote = (*args->mutable_remote_table_map())["other.Transfer"];
  remote.set_addr("chain-a:9000");
  remote.set_database("sentio_proc_2");
  remote.set_table("event_transfer_2");
  remote.set_user("reader");
  remote.set_password("pw");
  return req;
}

RewriteSQLResponse run(const RewriteSQLRequest &req) {
  RewriteSQLResponse resp;
  RewriterServiceImpl service;
  service.doRewrite(&req, &resp);
  return resp;
}

void expectRefused(const RewriteSQLRequest &req) {
  const auto resp = run(req);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << req.sql() << ": " << resp.message();
  EXPECT_EQ(resp.message(), kRefused) << req.sql();
  EXPECT_EQ(resp.sql_after_rewrite(), req.sql()) << req.sql();
  EXPECT_EQ(resp.table_rewrites_size(), 0) << req.sql();
  EXPECT_EQ(resp.original_accessed_tables_size(), 0) << req.sql();
  EXPECT_TRUE(resp.metadata_access_refusal_applied()) << req.sql();
}

} // namespace

TEST(StaticMetadataAccess, MetadataReadsAreRefused) {
  const std::vector<std::string> refused = {
    // The metadata databases, in every read position.
    "SELECT * FROM system.tables",
    "SELECT 1 FROM system.one",
    "SELECT * FROM system.numbers LIMIT 3",
    "SELECT * FROM system.query_log",
    "SELECT * FROM `system`.`columns`",
    "SELECT * FROM SYSTEM.USERS",
    "SELECT * FROM information_schema.columns",
    "SELECT * FROM INFORMATION_SCHEMA.TABLES",
    "SELECT * FROM (SELECT * FROM system.users)",
    "SELECT * FROM Transfer UNION ALL SELECT * FROM system.query_log",
    "SELECT metadata.name FROM Transfer JOIN system.tables AS metadata ON 1",
    "WITH metadata AS (SELECT name FROM system.tables) SELECT * FROM metadata",
    "SELECT * FROM Transfer WHERE `from` IN (SELECT name FROM system.tables)",
    "SELECT * FROM numbers(1) WHERE 1 IN system.users",
    "SELECT (SELECT count() FROM system.processes) AS n",
    "SELECT * FROM view(SELECT * FROM system.query_log)",
    "SELECT name, row_number() OVER w FROM Transfer WINDOW w AS (ORDER BY (SELECT 1 FROM system.one))",
    "EXPLAIN SELECT * FROM system.tables",
    // Table functions and engines that can reach them.
    "SELECT * FROM remote('localhost:9000', 'system', 'tables')",
    "SELECT * FROM remote('localhost:9000', system.query_log)",
    "SELECT * FROM remote('localhost:9000', concat('sys', 'tem'), 'query_log')",
    "SELECT * FROM remote(my_collection, table='query_log', database='system')",
    "SELECT * FROM remote('localhost:9000')",
    "SELECT * FROM remoteSecure('localhost:9440', 'system', 'columns')",
    "SELECT * FROM cluster('default')",
    "SELECT * FROM clusterAllReplicas('default', 'system', 'users')",
    "SELECT * FROM merge('system', '^tables$')",
    "SELECT * FROM merge('^tables$')",
    "SELECT * FROM merge(REGEXP('system'), '^query_log$')",
    "SELECT * FROM loop(system.query_log)",
    "SELECT * FROM loop(query_log)",
    "SELECT * FROM mergeTreeIndex('system', 'columns')",
    "SELECT * FROM mergeTreeAnalyzeIndexes(system, query_log, 1)",
    "CREATE TABLE copy ENGINE = Merge('system', '^query_log$')",
    "CREATE TABLE copy (a UInt8) ENGINE = Distributed('default', 'system', 'query_log')",
    // String lookups.
    "SELECT dictGet('system.x', 'a', 1)",
    "SELECT dictGet(concat('sys', 'tem.x'), 'a', 1)",
    "SELECT * FROM dictionary('system.x')",
    "SELECT joinGet('system.t', 'a', 1)",
    "SELECT hasColumnInTable('system', 'users', 'name')",
    "SELECT hasColumnInTable('host', 'default', 't', 'c')",
    // Statements over them.
    "SHOW TABLES",
    "SHOW TABLES FROM system",
    "SHOW DATABASES",
    "SHOW DICTIONARIES",
    "SHOW COLUMNS FROM tables FROM system",
    "SHOW INDEXES FROM tables FROM system",
    "SHOW CREATE TABLE system.tables",
    "SHOW CREATE DATABASE system",
    "DESCRIBE TABLE system.tables",
    "EXISTS TABLE system.tables",
    "USE system",
    "USE `INFORMATION_SCHEMA`",
    "DROP TABLE system.tables",
    "INSERT INTO system.query_log VALUES (1)",
    "CREATE TABLE copy AS system.query_log",
    "RENAME TABLE system.query_log TO leak",
    "EXCHANGE TABLES leak AND system.query_log",
    "ALTER TABLE leak MOVE PARTITION 1 TO TABLE system.query_log",
    "ALTER TABLE leak REPLACE PARTITION 1 FROM system.query_log",
    // Calls and bare names that reveal the deployment.
    "SELECT version()",
    "SELECT VeRsIoN()",
    "SELECT currentDatabase(), 1",
    "SELECT * FROM (SELECT currentUser())",
    "SELECT currentUser",
    "SELECT CURRENT_USER",
    "SELECT user()",
    "SELECT hostName()",
    "SELECT FQDN()",
    "SELECT serverUUID()",
    "SELECT getMacro('shard')",
    "WITH metadata AS (SELECT hostName()) SELECT * FROM metadata",
  };
  for (const auto &sql : refused) expectRefused(sentioRequest(sql));
}

TEST(StaticMetadataAccess, OrdinaryReadsAreAccepted) {
  struct Case {
    const char *sql;
    const char *want;
  };
  const std::vector<Case> accepted = {
    {"SELECT * FROM Transfer",
     "SELECT * FROM sentio_proc_1.event_transfer_1 AS Transfer LIMIT 1000 SETTINGS log_comment = 'sig'"},
    {"SELECT * FROM `other.Transfer`",
     "SELECT * FROM remote('chain-a:9000', 'sentio_proc_2', 'event_transfer_2', 'reader', 'pw') AS `other.Transfer` "
     "LIMIT 1000 SETTINGS log_comment = 'sig'"},
    {"SELECT * FROM Transfer AS a JOIN `other.Transfer` AS b ON a.x = b.x", nullptr},
    {"SELECT number FROM numbers(10)", nullptr},
    {"SELECT * FROM remote('evil:9000', default.t)", nullptr},
    {"SELECT * FROM merge('sentio_proc_1', '^event_')", nullptr},
    {"SELECT dictGet('sentio_proc_1.d', 'a', 1)", nullptr},
    {"SELECT dictGet('d', 'a', 1)", nullptr},
    {"SELECT hasColumnInTable('sentio_proc_1', 't', 'c')", nullptr},
    {"SELECT user FROM project_users", nullptr},
    {"SELECT displayName(), timezone(), uptime()", nullptr},
    {"SELECT 'system.tables', 'version()' FROM Transfer", nullptr},
    {"SELECT * FROM system_backup.t", nullptr},
    {"SELECT toDecimal256(value, 18) FROM Transfer WHERE `from` IN ('a', 'b')", nullptr},
  };
  for (const auto &c : accepted) {
    const auto resp = run(sentioRequest(c.sql));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << c.sql << ": " << resp.message();
    if (c.want) EXPECT_EQ(resp.sql_after_rewrite(), c.want) << c.sql;
    EXPECT_TRUE(resp.metadata_access_refusal_applied()) << c.sql;
  }
}

TEST(StaticMetadataAccess, WithoutTheFlagStaticModeIsUnchanged) {
  for (const std::string sql : {"SELECT * FROM system.tables", "SHOW TABLES", "SELECT version()"}) {
    const auto resp = run(sentioRequest(sql, /*refuse=*/false));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << sql << ": " << resp.message();
    EXPECT_FALSE(resp.metadata_access_refusal_applied()) << sql;
  }
}

TEST(StaticMetadataAccess, RewriteOptionsCannotIntroduceAMetadataRead) {
  // A table map whose target is a metadata table.
  {
    auto req = sentioRequest("SELECT * FROM Transfer");
    auto &target = (*req.mutable_options(2)->mutable_table_name_args()->mutable_static_args()
                       ->mutable_table_with_database_map())["Transfer"];
    target.set_database("system");
    target.set_table("query_log");
    const auto resp = run(req);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(), kRefused);
    EXPECT_EQ(resp.sql_after_rewrite(), req.sql());
  }
  // A remote map entry that addresses a metadata database.
  {
    auto req = sentioRequest("SELECT * FROM `other.Transfer`");
    (*req.mutable_options(2)->mutable_table_name_args()->mutable_static_args()
         ->mutable_remote_table_map())["other.Transfer"].set_database("system");
    EXPECT_EQ(run(req).code(), rewriter::RewriteCode::UnsupportedStatement);
  }
  // An injected CTE body that reads one, or reveals the deployment.
  for (const std::string body : {"SELECT name FROM system.tables", "SELECT currentDatabase() AS db"}) {
    auto req = sentioRequest("SELECT * FROM leak");
    auto *cte = req.add_options();
    cte->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
    auto &entry = (*cte->mutable_common_table_expr_args()->mutable_cte_map())["leak"];
    entry.set_alias("leak");
    entry.set_sql(body);
    const auto resp = run(req);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << body << ": " << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(), kRefused) << body;
  }
}

TEST(StaticMetadataAccess, EveryResponsePathAcknowledgesTheFlag) {
  for (const std::string sql : {"SELEC * FROM Transfer", "", "SELECT * FROM system.one"}) {
    const auto resp = run(sentioRequest(sql));
    EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << sql;
    EXPECT_TRUE(resp.metadata_access_refusal_applied()) << sql;
  }
  // No table option at all (Sentio's cursor path).
  RewriteSQLRequest bare;
  bare.set_sql("SELECT * FROM system.tables");
  bare.set_refuse_metadata_access(true);
  const auto resp = run(bare);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_TRUE(resp.metadata_access_refusal_applied());
}

TEST(StaticMetadataAccess, AppliesInDynamicModeToo) {
  // system.settings is on the dynamic-mode allowlist; the flag refuses it.
  RewriteSQLRequest req;
  req.set_sql("SELECT * FROM system.settings");
  req.set_refuse_metadata_access(true);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["tenant1"] = "physical_db";
  dyn->set_upstream_logical_database_in_context("tenant1");
  expectRefused(req);
}
