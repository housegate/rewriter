#include "rewriter-server.h"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using FieldDescriptor = google::protobuf::FieldDescriptor;

struct FieldContract {
  const char *name;
  int number;
  FieldDescriptor::Type type;
  bool repeated;
  const char *type_name;
};

void ExpectMessageContract(
    const google::protobuf::Descriptor *descriptor,
    std::initializer_list<FieldContract> expected_fields) {
  ASSERT_NE(descriptor, nullptr);
  ASSERT_EQ(descriptor->field_count(), static_cast<int>(expected_fields.size()))
      << descriptor->full_name();
  int index = 0;
  for (const auto &expected : expected_fields) {
    const auto *field = descriptor->field(index++);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->name(), expected.name);
    EXPECT_EQ(field->number(), expected.number);
    EXPECT_EQ(field->type(), expected.type);
    EXPECT_EQ(field->is_repeated(), expected.repeated);
    std::string actual_type_name;
    if (field->message_type()) actual_type_name = field->message_type()->full_name();
    if (field->enum_type()) actual_type_name = field->enum_type()->full_name();
    EXPECT_EQ(actual_type_name, expected.type_name);
  }
}

TEST(SnapshotQueryContract, GeneratedMessagesFreezeNamesNumbersTypesAndOrder) {
  ExpectMessageContract(rewriter::SnapshotQueryColumn::descriptor(), {
    {"name", 1, FieldDescriptor::TYPE_STRING, false, ""},
    {"type", 2, FieldDescriptor::TYPE_STRING, false, ""},
    {"generation", 3, FieldDescriptor::TYPE_ENUM, false,
      "rewriter.SnapshotQueryColumnGeneration"},
    {"default_expression", 4, FieldDescriptor::TYPE_STRING, false, ""},
  });
  ExpectMessageContract(rewriter::SnapshotQueryCatalogTable::descriptor(), {
    {"database", 1, FieldDescriptor::TYPE_STRING, false, ""},
    {"table", 2, FieldDescriptor::TYPE_STRING, false, ""},
    {"table_id", 3, FieldDescriptor::TYPE_STRING, false, ""},
    {"schema_hash", 4, FieldDescriptor::TYPE_STRING, false, ""},
    {"columns", 5, FieldDescriptor::TYPE_MESSAGE, true, "rewriter.SnapshotQueryColumn"},
  });
  ExpectMessageContract(rewriter::AnalyzeSnapshotQueryRequest::descriptor(), {
    {"contract_version", 1, FieldDescriptor::TYPE_UINT32, false, ""},
    {"query_profile_id", 2, FieldDescriptor::TYPE_STRING, false, ""},
    {"sql", 3, FieldDescriptor::TYPE_STRING, false, ""},
    {"logical_database", 4, FieldDescriptor::TYPE_STRING, false, ""},
    {"catalog", 5, FieldDescriptor::TYPE_MESSAGE, true, "rewriter.SnapshotQueryCatalogTable"},
    {"materialize", 6, FieldDescriptor::TYPE_BOOL, false, ""},
    {"inputs", 7, FieldDescriptor::TYPE_MESSAGE, false, "rewriter.MaterializationInputs"},
  });
  ExpectMessageContract(rewriter::AnalyzeSnapshotQueryResponse::descriptor(), {
    {"contract_version", 1, FieldDescriptor::TYPE_UINT32, false, ""},
    {"query_profile_id", 2, FieldDescriptor::TYPE_STRING, false, ""},
    {"code", 3, FieldDescriptor::TYPE_ENUM, false, "rewriter.SnapshotQueryCode"},
    {"message", 4, FieldDescriptor::TYPE_STRING, false, ""},
    {"sql_after_materialization", 5, FieldDescriptor::TYPE_STRING, false, ""},
    {"target_table_id", 6, FieldDescriptor::TYPE_STRING, false, ""},
    {"target_columns", 7, FieldDescriptor::TYPE_STRING, true, ""},
    {"read_table_ids", 8, FieldDescriptor::TYPE_STRING, true, ""},
  });
  ExpectMessageContract(rewriter::SnapshotScratchBinding::descriptor(), {
    {"table_id", 1, FieldDescriptor::TYPE_STRING, false, ""},
    {"scratch_database", 2, FieldDescriptor::TYPE_STRING, false, ""},
    {"scratch_table", 3, FieldDescriptor::TYPE_STRING, false, ""},
  });
  ExpectMessageContract(rewriter::PrepareSnapshotQueryRequest::descriptor(), {
    {"analysis", 1, FieldDescriptor::TYPE_MESSAGE, false,
      "rewriter.AnalyzeSnapshotQueryRequest"},
    {"bindings", 2, FieldDescriptor::TYPE_MESSAGE, true, "rewriter.SnapshotScratchBinding"},
  });
  ExpectMessageContract(rewriter::PrepareSnapshotQueryResponse::descriptor(), {
    {"contract_version", 1, FieldDescriptor::TYPE_UINT32, false, ""},
    {"query_profile_id", 2, FieldDescriptor::TYPE_STRING, false, ""},
    {"code", 3, FieldDescriptor::TYPE_ENUM, false, "rewriter.SnapshotQueryCode"},
    {"message", 4, FieldDescriptor::TYPE_STRING, false, ""},
    {"select_sql", 5, FieldDescriptor::TYPE_STRING, false, ""},
    {"target_table_id", 6, FieldDescriptor::TYPE_STRING, false, ""},
    {"target_columns", 7, FieldDescriptor::TYPE_STRING, true, ""},
    {"read_table_ids", 8, FieldDescriptor::TYPE_STRING, true, ""},
  });
}

TEST(SnapshotQueryContract, GeneratedEnumsAndRPCSignaturesAreFrozen) {
  EXPECT_EQ(rewriter::UNSPECIFIED, 0);
  EXPECT_EQ(rewriter::SUCCESS, 1);
  EXPECT_EQ(rewriter::UNSUPPORTED, 2);
  EXPECT_EQ(rewriter::INVALID_INPUT, 3);
  EXPECT_EQ(rewriter::PROFILE_UNAVAILABLE, 4);
  EXPECT_EQ(rewriter::MATERIALIZATION_FAILED, 5);
  EXPECT_EQ(rewriter::NOT_SNAPSHOT_QUERY, 6);

  EXPECT_EQ(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_UNSPECIFIED, 0);
  EXPECT_EQ(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_ORDINARY, 1);
  EXPECT_EQ(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_DEFAULT, 2);
  EXPECT_EQ(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_MATERIALIZED, 3);
  EXPECT_EQ(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_ALIAS, 4);
  EXPECT_EQ(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_OTHER, 5);

  const auto *file = rewriter::AnalyzeSnapshotQueryRequest::descriptor()->file();
  ASSERT_NE(file, nullptr);
  const auto *service = file->FindServiceByName("RewriterService");
  ASSERT_NE(service, nullptr);
  const auto *analyze = service->FindMethodByName("AnalyzeSnapshotQuery");
  ASSERT_NE(analyze, nullptr);
  EXPECT_EQ(analyze->input_type()->full_name(), "rewriter.AnalyzeSnapshotQueryRequest");
  EXPECT_EQ(analyze->output_type()->full_name(), "rewriter.AnalyzeSnapshotQueryResponse");
  EXPECT_FALSE(analyze->client_streaming());
  EXPECT_FALSE(analyze->server_streaming());
  const auto *prepare = service->FindMethodByName("PrepareSnapshotQuery");
  ASSERT_NE(prepare, nullptr);
  EXPECT_EQ(prepare->input_type()->full_name(), "rewriter.PrepareSnapshotQueryRequest");
  EXPECT_EQ(prepare->output_type()->full_name(), "rewriter.PrepareSnapshotQueryResponse");
  EXPECT_FALSE(prepare->client_streaming());
  EXPECT_FALSE(prepare->server_streaming());
}

Poco::JSON::Object::Ptr LoadFixture() {
  const std::string path = std::string(REWRITER_TEST_DATA_DIR)
    + "/snapshot_query_contract.json";
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("open " + path + ": failed");
  std::stringstream buffer;
  buffer << input.rdbuf();
  Poco::JSON::Parser parser;
  return parser.parse(buffer.str()).extract<Poco::JSON::Object::Ptr>();
}

void ExpectJSONKeys(
    const Poco::JSON::Object::Ptr &object,
    std::initializer_list<const char *> expected_keys) {
  ASSERT_TRUE(object);
  std::vector<std::string> actual;
  for (const auto &entry : *object) actual.push_back(entry.first);
  std::vector<std::string> expected;
  for (const auto *key : expected_keys) expected.emplace_back(key);
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected);
}

void ExpectCatalogTableJSONKeys(const Poco::JSON::Object::Ptr &table) {
  ExpectJSONKeys(table, {"database", "table", "table_id", "schema_hash", "columns"});
  const auto columns = table->getArray("columns");
  ASSERT_TRUE(columns);
  for (size_t i = 0; i < columns->size(); ++i) {
    ExpectJSONKeys(columns->getObject(static_cast<unsigned int>(i)),
                   {"name", "type", "generation", "default_expression"});
  }
}

void ExpectAnalyzeRequestJSONKeys(const Poco::JSON::Object::Ptr &analysis) {
  ExpectJSONKeys(analysis, {
    "contract_version", "query_profile_id", "sql", "logical_database",
    "catalog", "materialize", "inputs",
  });
  ExpectJSONKeys(analysis->getObject("inputs"), {
    "now_unix_ns", "random_uint64_values", "uuid_values", "random_float64_values",
  });
  const auto catalog = analysis->getArray("catalog");
  ASSERT_TRUE(catalog);
  for (size_t i = 0; i < catalog->size(); ++i) {
    ExpectCatalogTableJSONKeys(catalog->getObject(static_cast<unsigned int>(i)));
  }
}

std::string Stringify(const Poco::JSON::Object::Ptr &object) {
  std::ostringstream output;
  object->stringify(output);
  return output.str();
}

void ParseFixtureMessage(
    const Poco::JSON::Object::Ptr &root,
    const std::string &key,
    google::protobuf::Message *message) {
  ASSERT_TRUE(root);
  const auto object = root->getObject(key);
  ASSERT_TRUE(object) << key;
  const auto status = google::protobuf::util::JsonStringToMessage(Stringify(object), message);
  ASSERT_TRUE(status.ok()) << key << ": " << status.ToString();
}

void ExpectBinaryRoundTrip(const google::protobuf::Message &original) {
  std::string wire;
  ASSERT_TRUE(original.SerializeToString(&wire));
  std::unique_ptr<google::protobuf::Message> decoded(original.New());
  ASSERT_TRUE(decoded->ParseFromString(wire));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(original, *decoded));
}

TEST(SnapshotQueryContract, SharedTransportFixturePreservesAllFieldsAndOrder) {
  const auto root = LoadFixture();
  ExpectJSONKeys(root, {
    "schema_version", "fixture_purpose", "analyze_request", "analyze_response",
    "prepare_request", "prepare_response", "metadata_transport_table",
  });
  EXPECT_EQ(root->getValue<int>("schema_version"), 1);
  EXPECT_FALSE(root->getValue<std::string>("fixture_purpose").empty());

  const auto analyze_json = root->getObject("analyze_request");
  ExpectAnalyzeRequestJSONKeys(analyze_json);
  rewriter::AnalyzeSnapshotQueryRequest analysis;
  ParseFixtureMessage(root, "analyze_request", &analysis);
  ExpectBinaryRoundTrip(analysis);
  EXPECT_EQ(analysis.contract_version(), 1U);
  EXPECT_EQ(analysis.query_profile_id(), "synthetic-snapshot-profile-v1");
  EXPECT_EQ(analysis.sql(), "INSERT INTO tenant.copy SELECT value FROM tenant.events");
  EXPECT_EQ(analysis.logical_database(), "tenant");
  EXPECT_TRUE(analysis.materialize());
  ASSERT_EQ(analysis.catalog_size(), 2);
  EXPECT_EQ(analysis.catalog(0).table(), "copy");
  EXPECT_EQ(analysis.catalog(1).table(), "events");
  ASSERT_EQ(analysis.catalog(0).columns_size(), 2);
  ASSERT_EQ(analysis.catalog(1).columns_size(), 2);
  EXPECT_EQ(analysis.catalog(0).columns(0).name(), "copy_a");
  EXPECT_EQ(analysis.catalog(0).columns(1).name(), "copy_b");
  EXPECT_EQ(analysis.catalog(1).columns(0).name(), "event_value");
  EXPECT_EQ(analysis.catalog(1).columns(1).name(), "event_time");
  for (const auto &table : analysis.catalog()) {
    for (const auto &column : table.columns()) {
      EXPECT_EQ(column.generation(), rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_ORDINARY);
      EXPECT_TRUE(column.default_expression().empty());
    }
  }
  ASSERT_TRUE(analysis.has_inputs());
  EXPECT_TRUE(analysis.inputs().has_now_unix_ns());
  EXPECT_EQ(analysis.inputs().now_unix_ns(), 1700000000123456789LL);
  ASSERT_EQ(analysis.inputs().random_uint64_values_size(), 2);
  EXPECT_EQ(analysis.inputs().random_uint64_values(0), 11U);
  EXPECT_EQ(analysis.inputs().random_uint64_values(1), 22U);
  ASSERT_EQ(analysis.inputs().random_float64_values_size(), 2);
  EXPECT_DOUBLE_EQ(analysis.inputs().random_float64_values(0), 0.125);
  EXPECT_DOUBLE_EQ(analysis.inputs().random_float64_values(1), 0.875);

  const auto analyze_response_json = root->getObject("analyze_response");
  ExpectJSONKeys(analyze_response_json, {
    "contract_version", "query_profile_id", "code", "message",
    "sql_after_materialization", "target_table_id", "target_columns", "read_table_ids",
  });
  rewriter::AnalyzeSnapshotQueryResponse analysis_response;
  ParseFixtureMessage(root, "analyze_response", &analysis_response);
  ExpectBinaryRoundTrip(analysis_response);
  EXPECT_EQ(analysis_response.code(), rewriter::SUCCESS);
  ASSERT_EQ(analysis_response.target_columns_size(), 2);
  EXPECT_EQ(analysis_response.target_columns(0), "copy_b");
  EXPECT_EQ(analysis_response.target_columns(1), "copy_a");
  ASSERT_EQ(analysis_response.read_table_ids_size(), 1);
  EXPECT_EQ(analysis_response.read_table_ids(0), "read-table-43");

  const auto prepare_json = root->getObject("prepare_request");
  ExpectJSONKeys(prepare_json, {"analysis", "bindings"});
  ExpectAnalyzeRequestJSONKeys(prepare_json->getObject("analysis"));
  const auto bindings_json = prepare_json->getArray("bindings");
  ASSERT_TRUE(bindings_json);
  for (size_t i = 0; i < bindings_json->size(); ++i) {
    ExpectJSONKeys(bindings_json->getObject(static_cast<unsigned int>(i)),
                   {"table_id", "scratch_database", "scratch_table"});
  }
  rewriter::PrepareSnapshotQueryRequest prepare;
  ParseFixtureMessage(root, "prepare_request", &prepare);
  ExpectBinaryRoundTrip(prepare);
  ASSERT_TRUE(prepare.has_analysis());
  EXPECT_EQ(prepare.analysis().inputs().now_unix_ns(), 1700000000123456789LL);
  ASSERT_EQ(prepare.bindings_size(), 1);
  EXPECT_EQ(prepare.bindings(0).table_id(), "read-table-43");
  EXPECT_EQ(prepare.bindings(0).scratch_database(), "scratch_db_53");
  EXPECT_EQ(prepare.bindings(0).scratch_table(), "scratch_table_59");

  const auto prepare_response_json = root->getObject("prepare_response");
  ExpectJSONKeys(prepare_response_json, {
    "contract_version", "query_profile_id", "code", "message", "select_sql",
    "target_table_id", "target_columns", "read_table_ids",
  });
  rewriter::PrepareSnapshotQueryResponse prepare_response;
  ParseFixtureMessage(root, "prepare_response", &prepare_response);
  ExpectBinaryRoundTrip(prepare_response);
  EXPECT_EQ(prepare_response.code(), rewriter::SUCCESS);
  ASSERT_EQ(prepare_response.target_columns_size(), 2);
  EXPECT_EQ(prepare_response.target_columns(0), "copy_a");
  EXPECT_EQ(prepare_response.target_columns(1), "copy_b");
  ASSERT_EQ(prepare_response.read_table_ids_size(), 1);
  EXPECT_EQ(prepare_response.read_table_ids(0), "read-table-43");

  rewriter::AnalyzeSnapshotQueryResponse ordered_analysis_response;
  ordered_analysis_response.add_target_columns("target_b");
  ordered_analysis_response.add_target_columns("target_a");
  ordered_analysis_response.add_read_table_ids("read-a");
  ordered_analysis_response.add_read_table_ids("read-b");
  std::string ordered_analysis_wire;
  ASSERT_TRUE(ordered_analysis_response.SerializeToString(&ordered_analysis_wire));
  rewriter::AnalyzeSnapshotQueryResponse decoded_analysis_response;
  ASSERT_TRUE(decoded_analysis_response.ParseFromString(ordered_analysis_wire));
  ASSERT_EQ(decoded_analysis_response.target_columns_size(), 2);
  EXPECT_EQ(decoded_analysis_response.target_columns(0), "target_b");
  EXPECT_EQ(decoded_analysis_response.target_columns(1), "target_a");
  ASSERT_EQ(decoded_analysis_response.read_table_ids_size(), 2);
  EXPECT_EQ(decoded_analysis_response.read_table_ids(0), "read-a");
  EXPECT_EQ(decoded_analysis_response.read_table_ids(1), "read-b");

  rewriter::PrepareSnapshotQueryRequest ordered_prepare;
  auto *binding_a = ordered_prepare.add_bindings();
  binding_a->set_table_id("read-a");
  binding_a->set_scratch_database("scratch-a-db");
  binding_a->set_scratch_table("scratch-a-table");
  auto *binding_b = ordered_prepare.add_bindings();
  binding_b->set_table_id("read-b");
  binding_b->set_scratch_database("scratch-b-db");
  binding_b->set_scratch_table("scratch-b-table");
  std::string ordered_prepare_wire;
  ASSERT_TRUE(ordered_prepare.SerializeToString(&ordered_prepare_wire));
  rewriter::PrepareSnapshotQueryRequest decoded_prepare;
  ASSERT_TRUE(decoded_prepare.ParseFromString(ordered_prepare_wire));
  ASSERT_EQ(decoded_prepare.bindings_size(), 2);
  EXPECT_EQ(decoded_prepare.bindings(0).table_id(), "read-a");
  EXPECT_EQ(decoded_prepare.bindings(0).scratch_database(), "scratch-a-db");
  EXPECT_EQ(decoded_prepare.bindings(0).scratch_table(), "scratch-a-table");
  EXPECT_EQ(decoded_prepare.bindings(1).table_id(), "read-b");
  EXPECT_EQ(decoded_prepare.bindings(1).scratch_database(), "scratch-b-db");
  EXPECT_EQ(decoded_prepare.bindings(1).scratch_table(), "scratch-b-table");

  rewriter::SnapshotQueryCatalogTable metadata_transport;
  ExpectCatalogTableJSONKeys(root->getObject("metadata_transport_table"));
  ParseFixtureMessage(root, "metadata_transport_table", &metadata_transport);
  ExpectBinaryRoundTrip(metadata_transport);
  ASSERT_EQ(metadata_transport.columns_size(), 4);
  EXPECT_EQ(metadata_transport.columns(0).name(), "default_column");
  EXPECT_EQ(metadata_transport.columns(0).generation(),
            rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_DEFAULT);
  EXPECT_EQ(metadata_transport.columns(0).default_expression(), "41 + 1");
  EXPECT_EQ(metadata_transport.columns(1).name(), "materialized_column");
  EXPECT_EQ(metadata_transport.columns(1).generation(),
            rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_MATERIALIZED);
  EXPECT_EQ(metadata_transport.columns(1).default_expression(), "default_column * 2");
  EXPECT_EQ(metadata_transport.columns(2).name(), "alias_column");
  EXPECT_EQ(metadata_transport.columns(2).generation(),
            rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_ALIAS);
  EXPECT_EQ(metadata_transport.columns(2).default_expression(), "materialized_column + 3");
  EXPECT_EQ(metadata_transport.columns(3).name(), "future_column");
  EXPECT_EQ(static_cast<int>(metadata_transport.columns(3).generation()), 127);
  EXPECT_EQ(metadata_transport.columns(3).default_expression(), "future_expression()");
}

TEST(SnapshotQueryContract, OldAndUnknownColumnMetadataRemainTransportOnly) {
  const char legacy_wire[] = {
    0x0a, 0x0a, 'o', 'l', 'd', '_', 'c', 'o', 'l', 'u', 'm', 'n',
    0x12, 0x06, 'U', 'I', 'n', 't', '6', '4',
  };
  rewriter::SnapshotQueryColumn legacy;
  ASSERT_TRUE(legacy.ParseFromArray(legacy_wire, sizeof(legacy_wire)));
  EXPECT_EQ(legacy.name(), "old_column");
  EXPECT_EQ(legacy.type(), "UInt64");
  EXPECT_EQ(legacy.generation(), rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_UNSPECIFIED);
  EXPECT_TRUE(legacy.default_expression().empty());

  rewriter::SnapshotQueryColumn future;
  future.set_name("future_column");
  future.set_type("String");
  future.set_generation(static_cast<rewriter::SnapshotQueryColumnGeneration>(127));
  future.set_default_expression("future_expression()");
  std::string wire;
  ASSERT_TRUE(future.SerializeToString(&wire));
  rewriter::SnapshotQueryColumn decoded;
  ASSERT_TRUE(decoded.ParseFromString(wire));
  EXPECT_EQ(static_cast<int>(decoded.generation()), 127);
  EXPECT_EQ(decoded.name(), "future_column");
  EXPECT_EQ(decoded.default_expression(), "future_expression()");
}

TEST(SnapshotQueryContract, GeneratedDefaultServiceReturnsTransportUnimplemented) {
  rewriter::RewriterService::Service service;

  grpc::ServerContext analyze_context;
  rewriter::AnalyzeSnapshotQueryRequest analyze_request;
  analyze_request.set_contract_version(1);
  rewriter::AnalyzeSnapshotQueryResponse analyze_response;
  const auto analyze_status = service.AnalyzeSnapshotQuery(
      &analyze_context, &analyze_request, &analyze_response);
  EXPECT_EQ(analyze_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(analyze_response.ByteSizeLong(), 0U);

  grpc::ServerContext prepare_context;
  rewriter::PrepareSnapshotQueryRequest prepare_request;
  rewriter::PrepareSnapshotQueryResponse prepare_response;
  const auto prepare_status = service.PrepareSnapshotQuery(
      &prepare_context, &prepare_request, &prepare_response);
  EXPECT_EQ(prepare_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(prepare_response.ByteSizeLong(), 0U);
}

}  // namespace
