// Metadata-access gaps closed after comparing with upstream sql-rewriter
// (#23 / #24): a database_map entry routed to a ClickHouse metadata database,
// and calls that reveal the shared deployment (the physical session database,
// the shared ClickHouse user, the host). Both rules are mirrored in
// rewriter-go with the same messages.
#include "rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include "handlers/table_reference.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

namespace {

// housegate's shape of dynamic arguments.
rewriter::RewriteTableDynamicArgs *addHousegateArgs(RewriteSQLRequest *req, bool si) {
  auto *opt = req->add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["tenant1"] = "physical_db";
  dyn->add_known_physical_databases("physical_db");
  for (const char *db : {"physical_db", "hg_safe", "hg_unsafe", "hg_promote"}) dyn->add_protected_databases(db);
  dyn->set_upstream_logical_database_in_context("tenant1");
  if (si) {
    auto *s = dyn->mutable_storage_integrity();
    s->set_contract_version(rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    for (const char *db : {"hg_safe", "hg_unsafe", "hg_promote"}) s->add_reserved_databases(db);
  }
  return dyn;
}

RewriteSQLResponse run(const RewriteSQLRequest &req) {
  RewriteSQLResponse resp;
  RewriterServiceImpl service;
  service.doRewrite(&req, &resp);
  return resp;
}

RewriteSQLResponse rewrite(const std::string &sql, bool si = false) {
  RewriteSQLRequest req;
  req.set_sql(sql);
  addHousegateArgs(&req, si);
  return run(req);
}

} // namespace

TEST(MetadataAccess, DatabaseMapToAMetadataDatabaseIsInvalid) {
  for (const std::string physical : {"system", "information_schema", "INFORMATION_SCHEMA"}) {
    RewriteSQLRequest req;
    req.set_sql("SELECT * FROM b_db.t");
    auto *dyn = addHousegateArgs(&req, false);
    (*dyn->mutable_database_map())["b_db"] = physical;
    const auto resp = run(req);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << physical;
    EXPECT_EQ(resp.message(), "database_map entry \"b_db\" maps to the metadata database \"" + physical + "\"");
    EXPECT_EQ(resp.sql_after_rewrite(), "SELECT * FROM b_db.t");
  }
  // Database names are case-sensitive: `System` is an ordinary database.
  RewriteSQLRequest req;
  req.set_sql("SELECT * FROM b_db.t");
  (*addHousegateArgs(&req, false)->mutable_database_map())["b_db"] = "System";
  EXPECT_EQ(run(req).code(), rewriter::RewriteCode::Success);
}

TEST(MetadataAccess, DeploymentIdentityCallsAreRefusedInDynamicMode) {
  const std::map<std::string, std::string> refused = {
    {"SELECT currentDatabase()", "currentDatabase"},
    {"SELECT CURRENTDATABASE()", "CURRENTDATABASE"},
    {"SELECT database()", "database"},
    {"SELECT currentUser()", "currentUser"},
    {"SELECT user()", "user"},
    {"SELECT hostName()", "hostName"},
    {"SELECT FQDN()", "FQDN"},
    {"SELECT serverUUID()", "serverUUID"},
    {"SELECT getMacro('shard')", "getMacro"},
    {"SELECT * FROM tenant1.t WHERE a = hostName()", "hostName"},
    {"WITH m AS (SELECT currentUser()) SELECT * FROM m", "currentUser"},
    {"SELECT * FROM (SELECT currentDatabase())", "currentDatabase"},
    {"SELECT SCHEMA()", "SCHEMA"},
    {"SELECT currentSchemas(true)", "currentSchemas"},
    {"SELECT fullHostName()", "fullHostName"},
    // Bare names the analyzer resolves as the function (no parentheses).
    {"SELECT currentDatabase", "currentDatabase"},
    {"SELECT a FROM tenant1.o WHERE a IN (current_user)", "current_user"},
    {"SELECT current_user", "current_user"},
    {"SELECT a FROM tenant1.t WHERE b = CURRENT_DATABASE", "CURRENT_DATABASE"},
  };
  for (const bool si : {false, true}) {
    for (const auto &[sql, name] : refused) {
      const auto resp = rewrite(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << sql << " si=" << si;
      EXPECT_EQ(resp.message(), "function " + name + " is not accessible") << sql << " si=" << si;
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
  }
}

TEST(MetadataAccess, OtherCallsAndNamesAreAccepted) {
  // A qualified column, the ordinary-column aliases (user, database: a known
  // residual) and GRANT's CURRENT_USER keyword are not identity references.
  for (const std::string sql : {"SELECT version()", "SELECT 'hostName()' AS s", "SELECT user FROM tenant1.t",
                                "SELECT timezone()", "SELECT `user` FROM tenant1.t", "SELECT 1 -- hostName()",
                                "SELECT t.currentUser FROM tenant1.t AS t", "SELECT user, database FROM tenant1.t",
                                "GRANT SELECT ON tenant1.t TO CURRENT_USER"}) {
    const auto resp = rewrite(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << sql << ": " << resp.message();
  }
}

TEST(MetadataAccess, StaticModeKeepsItsLegacySurface) {
  RewriteSQLRequest req;
  req.set_sql("SELECT hostName()");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  opt->mutable_table_name_args()->mutable_static_args();
  EXPECT_EQ(run(req).code(), rewriter::RewriteCode::Success);
}

TEST(MetadataAccess, FirstDeploymentIdentityCallReadsTokens) {
  using rewriter_handlers::firstDeploymentIdentityCall;
  EXPECT_EQ(firstDeploymentIdentityCall("SELECT a, hostName(), currentUser()"), "hostName");
  EXPECT_EQ(firstDeploymentIdentityCall("SELECT `hostName`()"), "hostName");
  EXPECT_EQ(firstDeploymentIdentityCall("SELECT hostName /* c */ ()"), "hostName");
  EXPECT_FALSE(firstDeploymentIdentityCall("SELECT t.hostName()"));
  EXPECT_FALSE(firstDeploymentIdentityCall("SELECT 'currentUser()'"));
  EXPECT_FALSE(firstDeploymentIdentityCall("SELECT hostName"));
  EXPECT_FALSE(firstDeploymentIdentityCall("CREATE USER u IDENTIFIED BY 'p'"));
}

namespace {

// The probe's arguments: housegate's shape with two logical databases on one
// physical database and project_db as the session database.
RewriteSQLResponse rewriteCorpus(const std::string &sql, bool si) {
  RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["project_db"] = "physical_db";
  (*dyn->mutable_database_map())["other_db"] = "physical_db";
  dyn->add_known_physical_databases("physical_db");
  for (const char *db : {"physical_db", "hg_safe", "hg_unsafe", "hg_promote"}) dyn->add_protected_databases(db);
  dyn->set_upstream_logical_database_in_context("project_db");
  dyn->set_upstream_physical_database_in_context("physical_db");
  if (si) {
    auto *s = dyn->mutable_storage_integrity();
    s->set_contract_version(rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    for (const char *db : {"hg_safe", "hg_unsafe", "hg_promote"}) s->add_reserved_databases(db);
  }
  return run(req);
}

} // namespace

// Upstream sql-rewriter's metadata-access corpus (#23 / #24), run with
// housegate's dynamic arguments with the SI surface inactive. Upstream refuses
// all of `system` and information_schema; here the system-table allowlist
// (spec 2026-09-26 §5) decides, so the class (c) tables the Sentio driver
// reads (`tables`, `columns`, `parts`, …) and information_schema stay a
// documented passthrough until its step 2. Every refusal is pinned with its
// message; with the SI surface active each is still refused.
TEST(MetadataAccess, UpstreamCorpusOutcomes) {
  struct Case {
    const char *sql;
    rewriter::RewriteCode code;
    const char *message;  // checked for a refusal
  };
  const std::vector<Case> cases = {
    {"ALTER TABLE leak MOVE PARTITION 1 TO TABLE system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"ALTER TABLE leak REPLACE PARTITION 1 FROM system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"CREATE TABLE copy AS system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"DESCRIBE TABLE system.tables", rewriter::RewriteCode::Success, ""},
    {"DROP TABLE system.tables", rewriter::RewriteCode::InvalidRewriteRequest, "DROP TABLE target references logical database 'system' which is not in database_map and not a known physical database; u"},
    {"EXCHANGE TABLES leak AND system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"EXCHANGE TABLES system.query_log AND leak", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"EXISTS TABLE system.tables", rewriter::RewriteCode::InvalidRewriteRequest, "EXISTS TABLE target references logical database 'system' which is not in database_map and not a known physical database;"},
    {"RENAME TABLE leak TO system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"RENAME TABLE system.query_log TO leak", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM (SELECT * FROM system.users)", rewriter::RewriteCode::UnsupportedStatement, "system table system.users is not accessible"},
    {"SELECT * FROM (SELECT currentDatabase())", rewriter::RewriteCode::UnsupportedStatement, "function currentDatabase is not accessible"},
    {"SELECT * FROM (SELECT currentUser())", rewriter::RewriteCode::UnsupportedStatement, "function currentUser is not accessible"},
    {"SELECT * FROM SYSTEM.SETTINGS", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM SYSTEM.USERS", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM Transfer UNION ALL SELECT * FROM system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM `system`.`columns`", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM `system`.`settings`", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM cluster('default')", rewriter::RewriteCode::UnsupportedStatement, "table function cluster is not accepted"},
    {"SELECT * FROM cluster('default', 'system', 'tables')", rewriter::RewriteCode::UnsupportedStatement, "table function cluster is not accepted"},
    {"SELECT * FROM cluster('default', system.query_log)", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM clusterAllReplicas('default', 'system', 'users')", rewriter::RewriteCode::UnsupportedStatement, "system table system.users is not accessible"},
    {"SELECT * FROM information_schema.columns", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM information_schema.schemata", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM information_schema.tables", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM INFORMATION_SCHEMA.TABLES", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM loop(`information_schema`.`tables`)", rewriter::RewriteCode::UnsupportedStatement, "table function loop is not accepted"},
    {"SELECT * FROM loop(concat('system', '.query_log'))", rewriter::RewriteCode::UnsupportedStatement, "table function loop is not accepted"},
    {"SELECT * FROM loop(system.query_log)", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM merge('system', '^tables$')", rewriter::RewriteCode::UnsupportedStatement, "system table system.^tables$ is not accessible"},
    {"SELECT * FROM merge(REGEXP('system'), '^query_log$')", rewriter::RewriteCode::UnsupportedStatement, "table function merge is not accepted"},
    {"SELECT * FROM merge(information_schema, '^tables$')", rewriter::RewriteCode::UnsupportedStatement, "table function merge is not accepted"},
    {"SELECT * FROM mergeTreeIndex('system', 'columns')", rewriter::RewriteCode::UnsupportedStatement, "table function mergeTreeIndex is not accepted"},
    {"SELECT * FROM mergeTreeAnalyzeIndexes(system, query_log, 1)", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM mergeTreeTextIndex(system, query_log, index_name)", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM remote('localhost:9000', 'system', 'tables')", rewriter::RewriteCode::UnsupportedStatement, "table function remote is not accepted"},
    {"SELECT * FROM remote('localhost:9000', concat('sys', 'tem'), 'query_log')", rewriter::RewriteCode::UnsupportedStatement, "table function remote is not accepted"},
    {"SELECT * FROM remote(my_collection, table='query_log', database='system')", rewriter::RewriteCode::UnsupportedStatement, "table function remote is not accepted"},
    {"SELECT * FROM remoteSecure('localhost:9440', 'system', 'columns')", rewriter::RewriteCode::UnsupportedStatement, "table function remoteSecure is not accepted"},
    {"SELECT * FROM system.columns", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM system.databases", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM system.grants", rewriter::RewriteCode::UnsupportedStatement, "system table system.grants is not accessible"},
    {"SELECT * FROM system.query_log", rewriter::RewriteCode::UnsupportedStatement, "system table system.query_log is not accessible"},
    {"SELECT * FROM system.settings", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM system.users", rewriter::RewriteCode::UnsupportedStatement, "system table system.users is not accessible"},
    {"SELECT * FROM system.processes", rewriter::RewriteCode::UnsupportedStatement, "system table system.processes is not accessible"},
    {"SELECT * FROM system.clusters", rewriter::RewriteCode::Success, ""},
    {"SELECT * FROM system.macros", rewriter::RewriteCode::UnsupportedStatement, "system table system.macros is not accessible"},
    {"SELECT * FROM system.parts", rewriter::RewriteCode::Success, ""},
    {"SELECT name FROM system.tables WHERE database = currentDatabase()", rewriter::RewriteCode::UnsupportedStatement, "function currentDatabase is not accessible"},
    {"SELECT CURRENTDATABASE()", rewriter::RewriteCode::UnsupportedStatement, "function CURRENTDATABASE is not accessible"},
    {"SELECT database()", rewriter::RewriteCode::UnsupportedStatement, "function database is not accessible"},
    {"SELECT current_database()", rewriter::RewriteCode::UnsupportedStatement, "function current_database is not accessible"},
    {"SELECT CURRENT_USER", rewriter::RewriteCode::UnsupportedStatement, "function CURRENT_USER is not accessible"},
    {"SELECT USER()", rewriter::RewriteCode::UnsupportedStatement, "function USER is not accessible"},
    {"SELECT VeRsIoN()", rewriter::RewriteCode::Success, ""},
    {"SELECT currentDatabase()", rewriter::RewriteCode::UnsupportedStatement, "function currentDatabase is not accessible"},
    {"SELECT currentUser", rewriter::RewriteCode::UnsupportedStatement, "function currentUser is not accessible"},
    {"SELECT currentUser()", rewriter::RewriteCode::UnsupportedStatement, "function currentUser is not accessible"},
    {"SELECT current_user()", rewriter::RewriteCode::UnsupportedStatement, "function current_user is not accessible"},
    {"SELECT hostName()", rewriter::RewriteCode::UnsupportedStatement, "function hostName is not accessible"},
    {"SELECT FQDN()", rewriter::RewriteCode::UnsupportedStatement, "function FQDN is not accessible"},
    {"SELECT serverUUID()", rewriter::RewriteCode::UnsupportedStatement, "function serverUUID is not accessible"},
    {"SELECT getMacro('shard')", rewriter::RewriteCode::UnsupportedStatement, "function getMacro is not accessible"},
    {"SELECT displayName()", rewriter::RewriteCode::Success, ""},
    {"SELECT tcpPort()", rewriter::RewriteCode::Success, ""},
    {"SELECT getServerPort('tcp_port')", rewriter::RewriteCode::Success, ""},
    {"SELECT currentRoles()", rewriter::RewriteCode::Success, ""},
    {"SELECT currentProfiles()", rewriter::RewriteCode::Success, ""},
    {"SELECT getSetting('max_threads')", rewriter::RewriteCode::Success, ""},
    {"SELECT shardNum()", rewriter::RewriteCode::Success, ""},
    {"SELECT uptime()", rewriter::RewriteCode::Success, ""},
    {"SELECT timezone()", rewriter::RewriteCode::Success, ""},
    {"SELECT metadata.name FROM Transfer JOIN system.columns AS metadata ON 1", rewriter::RewriteCode::Success, ""},
    {"SELECT metadata.name FROM Transfer JOIN system.tables AS metadata ON 1", rewriter::RewriteCode::Success, ""},
    {"SELECT name FROM SYSTEM.TABLES", rewriter::RewriteCode::Success, ""},
    {"SELECT name FROM system.tables LIMIT 10", rewriter::RewriteCode::Success, ""},
    {"SELECT user FROM project_users", rewriter::RewriteCode::Success, ""},
    {"SHOW COLUMNS FROM tables FROM system", rewriter::RewriteCode::Success, ""},
    {"SHOW CREATE TABLE system.tables", rewriter::RewriteCode::InvalidRewriteRequest, "SHOW CREATE TABLE target references logical database 'system' which is not in database_map and not a known physical data"},
    {"SHOW DATABASES", rewriter::RewriteCode::Success, ""},
    {"SHOW ENGINES", rewriter::RewriteCode::Success, ""},
    {"SHOW FUNCTIONS", rewriter::RewriteCode::Success, ""},
    {"SHOW INDEXES FROM tables FROM system", rewriter::RewriteCode::Success, ""},
    {"SHOW PROCESSLIST", rewriter::RewriteCode::UnsupportedStatement, "system table system.processes is not accessible"},
    {"SHOW SETTING max_threads", rewriter::RewriteCode::Success, ""},
    {"SHOW TABLES FROM system", rewriter::RewriteCode::InvalidRewriteRequest, "SHOW TABLES target logical database 'system' is not in database_map and not a known physical database; user does not hav"},
    {"SHOW TABLES FROM information_schema", rewriter::RewriteCode::InvalidRewriteRequest, "SHOW TABLES target logical database 'information_schema' is not in database_map and not a known physical database; user "},
    {"SHOW TABLES", rewriter::RewriteCode::Success, ""},
    {"USE SYSTEM", rewriter::RewriteCode::InvalidRewriteRequest, "USE target 'SYSTEM' is not in database_map and not a known physical database; user does not have this database"},
    {"USE `INFORMATION_SCHEMA`", rewriter::RewriteCode::InvalidRewriteRequest, "USE target 'INFORMATION_SCHEMA' is not in database_map and not a known physical database; user does not have this databa"},
    {"USE system", rewriter::RewriteCode::InvalidRewriteRequest, "USE target 'system' is not in database_map and not a known physical database; user does not have this database"},
    {"USE information_schema", rewriter::RewriteCode::InvalidRewriteRequest, "USE target 'information_schema' is not in database_map and not a known physical database; user does not have this databa"},
    {"USE physical_db", rewriter::RewriteCode::InvalidRewriteRequest, "protected database physical_db is not addressable"},
    {"USE project_db", rewriter::RewriteCode::Success, ""},
    {"WITH metadata AS (SELECT * FROM system.settings) SELECT * FROM metadata", rewriter::RewriteCode::Success, ""},
    {"WITH metadata AS (SELECT hostName()) SELECT * FROM metadata", rewriter::RewriteCode::UnsupportedStatement, "function hostName is not accessible"},
    {"WITH metadata AS (SELECT name FROM system.tables) SELECT * FROM metadata", rewriter::RewriteCode::Success, ""},
    {"SELECT dictGet('system.x', 'a', 1)", rewriter::RewriteCode::UnsupportedStatement, "system table system.x is not accessible"},
    {"SELECT * FROM numbers(1) WHERE 1 IN (SELECT 1 FROM system.users)", rewriter::RewriteCode::UnsupportedStatement, "system table system.users is not accessible"},
    {"SELECT * FROM numbers(1) WHERE 1 IN system.users", rewriter::RewriteCode::UnsupportedStatement, "system table system.users is not accessible"},
    {"SELECT joinGet('system.t', 'a', 1)", rewriter::RewriteCode::UnsupportedStatement, "system table system.t is not accessible"},
    {"SELECT hasColumnInTable('system', 'users', 'name')", rewriter::RewriteCode::UnsupportedStatement, "system table system.users is not accessible"},
  };
  for (const auto &c : cases) {
    const auto off = rewriteCorpus(c.sql, false);
    EXPECT_EQ(off.code(), c.code) << c.sql << ": " << off.message();
    if (c.code != rewriter::RewriteCode::Success) {
      EXPECT_EQ(off.message().substr(0, std::string(c.message).size()), c.message) << c.sql;
      const auto on = rewriteCorpus(c.sql, true);
      EXPECT_NE(on.code(), rewriter::RewriteCode::Success) << c.sql << " [V2]";
    }
  }
}
