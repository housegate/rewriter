// RewriteErrorMessage fails closed: an error the rewriter cannot map back is
// withheld, and so is one that still names this request's physical routing
// (a physical or protocol database, a remote address, a credential) after the
// mapping passes ran.
#include "rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include "error_message_rewrite.h"

#include <gtest/gtest.h>

#include <string>

using rewriter::RewriteErrorMessageRequest;
using rewriter::RewriteErrorMessageResponse;

namespace {

rewriter::RewriteTableDynamicArgs *addDynamic(RewriteErrorMessageRequest *req) {
  auto *opt = req->add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["tenant1"] = "physical_db_1";
  dyn->set_upstream_logical_database_in_context("tenant1");
  return dyn;
}

RewriteErrorMessageResponse run(const RewriteErrorMessageRequest &req) {
  RewriteErrorMessageResponse resp;
  RewriterServiceImpl service;
  service.doRewriteErrorMessage(&req, &resp);
  return resp;
}

RewriteErrorMessageResponse runDynamic(
  const std::string &sql, const std::string &err,
  void (*configure)(rewriter::RewriteTableDynamicArgs *) = nullptr) {
  RewriteErrorMessageRequest req;
  req.set_sql(sql);
  req.set_error_message(err);
  auto *dyn = addDynamic(&req);
  if (configure) configure(dyn);
  return run(req);
}

void expectWithheld(const RewriteErrorMessageResponse &resp, const std::string &reason, const std::string &leak) {
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  if (reason.empty())
    EXPECT_EQ(resp.message().rfind("error message withheld: ", 0), 0u) << resp.message();
  else
    EXPECT_EQ(resp.message(), "error message withheld: " + reason);
  EXPECT_EQ(resp.error_after_rewrite().rfind("Query failed. The error details were withheld", 0), 0u)
    << resp.error_after_rewrite();
  EXPECT_EQ(resp.error_after_rewrite().find(leak), std::string::npos) << resp.error_after_rewrite();
}

void expectReturned(const RewriteErrorMessageResponse &resp) {
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.message(), "success") << resp.error_after_rewrite();
}

} // namespace

TEST(ErrorRedaction, RewriteFailureIsWithheldNotPassedThrough) {
  // The statement does not parse, so there is nothing to invert; the raw
  // error used to come back verbatim with the rewrite's code.
  const auto resp = runDynamic("SELEC * FROM tenant1.events",
                               "Code: 60. Table physical_db_1.`tenant1.events` does not exist.");
  expectWithheld(resp, "rewrite failed", "physical_db_1");
}

TEST(ErrorRedaction, RefusedStatementIsWithheld) {
  // Naming a physical database is refused by the policy (or, were it let
  // through, left unmapped); either way the raw text does not come back.
  const auto resp = runDynamic("SELECT * FROM physical_db_1.events",
                               "Code: 60. Table physical_db_1.events does not exist.");
  expectWithheld(resp, "", "physical_db_1");
}

TEST(ErrorRedaction, MappedErrorIsReturned) {
  const auto resp = runDynamic("SELECT count(*) FROM tenant1.events",
                               "Code: 60. Table physical_db_1.`tenant1.events` does not exist. (UNKNOWN_TABLE)");
  expectReturned(resp);
  EXPECT_NE(resp.error_after_rewrite().find("tenant1.events"), std::string::npos);
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(ErrorRedaction, PhysicalDatabaseTheStatementDidNotTouchIsWithheld) {
  // The statement reads tenant1 only, so its rewrite maps physical_db_1; an
  // error naming another tenant's physical database is not mapped by any pass.
  const auto resp = runDynamic(
    "SELECT * FROM tenant1.events", "Code: 81. Database physical_db_2 does not exist. (UNKNOWN_DATABASE)",
    [](rewriter::RewriteTableDynamicArgs *dyn) { (*dyn->mutable_database_map())["tenant2"] = "physical_db_2"; });
  expectWithheld(resp, "physical name remained", "physical_db_2");
}

TEST(ErrorRedaction, ProtectedDatabaseIsWithheld) {
  const auto resp = runDynamic(
    "SELECT * FROM tenant1.events", "Code: 497. Not enough privileges on hg_safe.`tenant1__events`.",
    [](rewriter::RewriteTableDynamicArgs *dyn) { dyn->add_protected_databases("hg_safe"); });
  expectWithheld(resp, "physical name remained", "hg_safe");
}

TEST(ErrorRedaction, StorageIntegrityReservedDatabaseIsWithheldUnderV2) {
  const auto resp = runDynamic(
    "SELECT * FROM tenant1.events", "Code: 81. Database `hg_unsafe` does not exist.",
    [](rewriter::RewriteTableDynamicArgs *dyn) {
      auto *si = dyn->mutable_storage_integrity();
      si->set_contract_version(rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
      si->add_reserved_databases("hg_unsafe");
    });
  expectWithheld(resp, "physical name remained", "hg_unsafe");
}

TEST(ErrorRedaction, ReservedDatabaseOutsideV2IsNotHidden) {
  // V1 and an inactive surface ignore reserved_databases (protectedDatabase),
  // so the caller may name that database and an error naming it is returned.
  const auto resp = runDynamic(
    "SELECT * FROM tenant1.events", "Code: 81. Database `hg_unsafe` does not exist.",
    [](rewriter::RewriteTableDynamicArgs *dyn) {
      dyn->mutable_storage_integrity()->add_reserved_databases("hg_unsafe");
    });
  expectReturned(resp);
}

TEST(ErrorRedaction, EveryHostOfAReplicaListIsWithheld) {
  const auto configure = [](rewriter::RewriteTableDynamicArgs *dyn) {
    (*dyn->mutable_logical_database_to_remote_upstream_index())["tenant1"] = "us_east";
    (*dyn->mutable_remote_upstreams())["us_east"].set_addr("ch-a.internal:9000|ch-b.internal:9000, [fd00::12]:9440");
  };
  for (const std::string err : {"Code: 210. Connection refused (ch-b.internal:9000).",
                                "Code: 210. Host ch-a.internal is unreachable.",
                                "Code: 210. Connection refused ([fd00::12]:9440).",
                                "Code: 210. No route to fd00::12."}) {
    SCOPED_TRACE(err);
    expectWithheld(runDynamic("SELECT * FROM tenant1.events", err, configure), "physical name remained", "ch-");
  }
}

TEST(ErrorRedaction, RemoteAddressAndPasswordAreWithheld) {
  const auto configure = [](rewriter::RewriteTableDynamicArgs *dyn) {
    (*dyn->mutable_logical_database_to_remote_upstream_index())["tenant1"] = "us_east";
    auto &upstream = (*dyn->mutable_remote_upstreams())["us_east"];
    upstream.set_addr("ch-remote-1.internal:9000");
    upstream.set_user("viewer");
    upstream.set_password("s3cret-pass");
  };
  expectWithheld(runDynamic("SELECT * FROM tenant1.events",
                            "Code: 210. Connection refused (ch-remote-1.internal:9000).", configure),
                 "physical name remained", "ch-remote-1");
  expectWithheld(runDynamic("SELECT * FROM tenant1.events",
                            "Code: 210. Host ch-remote-1.internal is unreachable.", configure),
                 "physical name remained", "ch-remote-1");
  expectWithheld(runDynamic("SELECT * FROM tenant1.events",
                            "Code: 516. Authentication failed for password s3cret-pass.", configure),
                 "physical name remained", "s3cret-pass");
}

TEST(ErrorRedaction, StaticRemoteAddressIsWithheld) {
  RewriteErrorMessageRequest req;
  req.set_sql("SELECT * FROM `sui.events`");
  req.set_error_message("Code: 209. Timeout exceeded while reading from clickhouse-chain-part-b.clickhouse:9000.");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto &remote = (*opt->mutable_table_name_args()->mutable_static_args()->mutable_remote_table_map())["sui.events"];
  remote.set_addr("clickhouse-chain-part-b.clickhouse:9000");
  remote.set_database("chain_db");
  remote.set_table("sui-mainnet.events");
  expectWithheld(run(req), "physical name remained", "clickhouse-chain-part-b");
}

TEST(ErrorRedaction, StaticPhysicalDatabaseIsWithheld) {
  RewriteErrorMessageRequest req;
  req.set_sql("SELECT * FROM latestView_PoolSnapshot");
  req.set_error_message("Code: 81. Database prod_subgraph does not exist.");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto &target =
    (*opt->mutable_table_name_args()->mutable_static_args()->mutable_table_with_database_map())["latestView_PoolSnapshot"];
  target.set_database("prod_subgraph");
  target.set_table("8CsFI8hZ_latestView_PoolSnapshot");
  expectWithheld(run(req), "physical name remained", "prod_subgraph");
}

TEST(ErrorRedaction, NamesTheCallerMayWriteAreNotWithheld) {
  // A pass-through physical database, a logical name equal to its physical
  // one, and a remote user (not a secret) all stay readable.
  const auto resp = runDynamic(
    "SELECT * FROM tenant1.events",
    "Code: 60. Table shared.blocks does not exist; same.t does not exist either; user viewer.",
    [](rewriter::RewriteTableDynamicArgs *dyn) {
      dyn->add_known_physical_databases("shared");
      (*dyn->mutable_database_map())["same"] = "same";
      (*dyn->mutable_logical_database_to_remote_upstream_index())["tenant1"] = "us_east";
      auto &upstream = (*dyn->mutable_remote_upstreams())["us_east"];
      upstream.set_addr("ch-remote-1.internal:9000");
      upstream.set_user("viewer");
    });
  expectReturned(resp);
  EXPECT_NE(resp.error_after_rewrite().find("shared.blocks"), std::string::npos);
}

TEST(ErrorRedaction, KnownPhysicalDatabaseThatIsProtectedIsWithheld) {
  // `SELECT 1` maps no database, so nothing turns physical_db_1 back into
  // tenant1; listing it as known-physical does not make it the caller's.
  const auto resp = runDynamic(
    "SELECT 1", "Code: 60. Table physical_db_1.other does not exist.",
    [](rewriter::RewriteTableDynamicArgs *dyn) { dyn->add_known_physical_databases("physical_db_1"); });
  expectWithheld(resp, "physical name remained", "physical_db_1");
}

TEST(ErrorRedaction, ContainsIdentifierIsBoundaryDelimitedAndCaseSensitive) {
  rewriter_error::WorkBudget budget(0);
  const auto has = [&](std::string_view err, std::string_view ident) {
    const auto r = rewriter_error::containsIdentifier(err, ident, budget);
    EXPECT_TRUE(r.has_value());
    return r.value_or(false);
  };
  EXPECT_TRUE(has("Database physical_db_1 does not exist", "physical_db_1"));
  EXPECT_TRUE(has("Table `physical_db_1`.`t`", "physical_db_1"));
  EXPECT_TRUE(has("physical_db_1", "physical_db_1"));
  EXPECT_TRUE(has("x physical_db_1x physical_db_1.t", "physical_db_1"));
  EXPECT_FALSE(has("Database physical_db_10 does not exist", "physical_db_1"));
  EXPECT_FALSE(has("Database my_physical_db_1", "physical_db_1"));
  EXPECT_FALSE(has("Missing DEFAULT expression", "default"));
  EXPECT_FALSE(has("anything", ""));
  EXPECT_FALSE(has("ab", "abc"));

  rewriter_error::WorkBudget spent(4);
  EXPECT_FALSE(rewriter_error::containsIdentifier("physical_db_1 is long", "physical_db_1", spent).has_value());
}
