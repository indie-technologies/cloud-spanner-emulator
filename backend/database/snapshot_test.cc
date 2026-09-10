// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include <atomic>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/strings/cord.h"
#include "absl/time/clock.h"
#include "backend/database/database.h"
#include "backend/persistence/values.h"
#include "backend/schema/printer/print_ddl.h"
#include "common/bit_reverse.h"
#include "google/protobuf/descriptor.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/json_value.h"
#include "gtest/gtest.h"
#include "third_party/spanner_pg/datatypes/extended/pg_jsonb_type.h"
#include "third_party/spanner_pg/datatypes/extended/pg_numeric_type.h"

namespace google::spanner::emulator::backend {
namespace {
using googlesql::Value;
using googlesql::values::Int64;
using googlesql::values::String;
class SnapshotTest : public testing::Test {
 protected:
  Clock clock;
  absl::StatusOr<std::unique_ptr<Database>> Create(
      const std::vector<std::string>& ddl,
      database_api::DatabaseDialect dialect =
          database_api::GOOGLE_STANDARD_SQL) {
    return Database::Create(
        &clock, "db", {.statements = ddl, .database_dialect = dialect}, false);
  }
  absl::Status Ddl(Database* db, const std::vector<std::string>& ddl) {
    int successful;
    absl::Time timestamp;
    absl::Status backfill;
    GOOGLESQL_RETURN_IF_ERROR(
        db->UpdateSchema({.statements = ddl, .database_dialect = db->dialect()},
                         &successful, &timestamp, &backfill));
    return backfill;
  }
  absl::Status Write(Database* db, const std::string& table,
                     const std::vector<std::string>& columns,
                     const std::vector<ValueList>& rows) {
    GOOGLESQL_ASSIGN_OR_RETURN(auto writer,
                               db->CreateReadWriteTransaction({}, {}));
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kInsertOrUpdate, table, columns, rows);
    GOOGLESQL_RETURN_IF_ERROR(writer->Write(mutation));
    return writer->Commit();
  }
  absl::StatusOr<std::vector<ValueList>> Read(Database* db,
                                              const std::string& table,
                                              std::vector<std::string> columns,
                                              std::string index = "") {
    GOOGLESQL_ASSIGN_OR_RETURN(auto reader, db->CreateReadOnlyTransaction({}));
    std::unique_ptr<RowCursor> rows;
    GOOGLESQL_RETURN_IF_ERROR(reader->Read({.table = table,
                                            .index = index,
                                            .key_set = KeySet::All(),
                                            .columns = columns},
                                           &rows));
    std::vector<ValueList> result;
    while (rows->Next()) {
      auto& row = result.emplace_back();
      for (int i = 0; i < rows->NumColumns(); ++i)
        row.push_back(rows->ColumnValue(i));
    }
    GOOGLESQL_RETURN_IF_ERROR(rows->Status());
    return result;
  }
  absl::StatusOr<std::unique_ptr<Database>> RoundTrip(Database* db) {
    persistence::Database snapshot;
    GOOGLESQL_RETURN_IF_ERROR(db->CaptureSnapshot().Serialize(&snapshot));
    // Exercise protobuf encoding as well as in-memory capture.
    persistence::Database decoded;
    if (!decoded.ParseFromString(snapshot.SerializeAsString()))
      return absl::DataLossError("parse");
    return Database::Restore(&clock, "db", decoded);
  }
};

TEST_F(SnapshotTest, TypedRowsAndChangedInternalIds) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Create({"CREATE TABLE Dropped (id INT64, c STRING(MAX)) PRIMARY KEY (id)",
              "CREATE TABLE T (id INT64 NOT NULL, old STRING(MAX), b "
              "BYTES(MAX), n NUMERIC, j JSON, ts TIMESTAMP, d DATE, a "
              "ARRAY<INT64>, f FLOAT64) PRIMARY KEY (id DESC)",
              "DROP TABLE Dropped", "ALTER TABLE T DROP COLUMN old",
              "ALTER TABLE T ADD COLUMN s STRING(MAX)",
              "CREATE NULL_FILTERED INDEX ByS ON T(s) STORING (b)"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto numeric,
      googlesql::NumericValue::FromString("12345678901234567890.123456789"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto json,
      googlesql::JSONValue::ParseJSONString("{\"x\":[null,1,\"\\u0000\"]}"));
  std::vector<ValueList> expected{
      {Int64(2), Value::Bytes(std::string("a\0\xff", 3)),
       Value::Numeric(numeric), Value::Json(std::move(json)),
       Value::Timestamp(absl::UnixEpoch() + absl::Nanoseconds(123456789)),
       Value::Date(12345), googlesql::values::Int64Array({1, 2}),
       Value::Double(std::numeric_limits<double>::infinity()),
       String("hello'\nworld")}};
  GOOGLESQL_ASSERT_OK(Write(db.get(), "T",
                            {"id", "b", "n", "j", "ts", "d", "a", "f", "s"},
                            expected));
  GOOGLESQL_ASSERT_OK(Write(db.get(), "T", {"id"}, {{Int64(1)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  EXPECT_NE(db->GetLatestSchema()->FindTable("T")->id(),
            restored->GetLatestSchema()->FindTable("T")->id());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto before,
      Read(db.get(), "T", {"id", "b", "n", "j", "ts", "d", "a", "f", "s"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto after, Read(restored.get(), "T",
                       {"id", "b", "n", "j", "ts", "d", "a", "f", "s"}));
  EXPECT_EQ(before, after);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto index_rows, Read(restored.get(), "T", {"id", "s", "b"}, "ByS"));
  ASSERT_EQ(index_rows.size(), 1);
  EXPECT_EQ(index_rows[0][0], Int64(2));
}

TEST_F(SnapshotTest, CyclicForeignKeysGeneratedColumnsAndViews) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Create(
          {"CREATE TABLE A (id INT64 NOT NULL, b INT64, n INT64 AS (id + 1) "
           "STORED) PRIMARY KEY(id)",
           "CREATE TABLE B (id INT64 NOT NULL, a INT64) PRIMARY KEY(id)",
           "ALTER TABLE A ADD CONSTRAINT A_B FOREIGN KEY(b) REFERENCES B(id)",
           "ALTER TABLE B ADD CONSTRAINT B_A FOREIGN KEY(a) REFERENCES A(id)",
           "CREATE VIEW V SQL SECURITY INVOKER AS SELECT A.id, A.n FROM A",
           "CREATE INDEX ByN ON A(n)"}));
  GOOGLESQL_ASSERT_OK(Write(db.get(), "A", {"id"}, {{Int64(1)}}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "B", {"id", "a"}, {{Int64(2), Int64(1)}}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "A", {"id", "b"}, {{Int64(1), Int64(2)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  EXPECT_NE(restored->GetLatestSchema()->FindView("V"), nullptr);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto rows,
                                 Read(restored.get(), "A", {"id", "n"}, "ByN"));
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0][1], Int64(2));
  EXPECT_FALSE(
      Write(restored.get(), "A", {"id", "b"}, {{Int64(3), Int64(999)}}).ok());
}

TEST_F(SnapshotTest, SequencesContinueIncludingRolledBackIdentityAllocation) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Create(
          {"CREATE SEQUENCE S OPTIONS(sequence_kind='bit_reversed_positive', "
           "start_with_counter=100)",
           "CREATE SEQUENCE Untouched "
           "OPTIONS(sequence_kind='bit_reversed_positive')",
           "CREATE TABLE T (id INT64 GENERATED BY DEFAULT AS IDENTITY "
           "(BIT_REVERSED_POSITIVE START COUNTER WITH 200), v INT64) PRIMARY "
           "KEY (id)"}));
  GOOGLESQL_ASSERT_OK(
      db->GetLatestSchema()->FindSequence("S")->GetNextSequenceValue());
  auto initial = db->GetPersistenceVersion();
  GOOGLESQL_ASSERT_OK(
      db->GetLatestSchema()->FindSequence("S")->GetNextSequenceValue());
  EXPECT_NE(initial, db->GetPersistenceVersion());
  GOOGLESQL_ASSERT_OK(Write(db.get(), "T", {"v"}, {{Int64(1)}}));
  {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto writer,
                                   db->CreateReadWriteTransaction({}, {}));
    Mutation mutation;
    mutation.AddWriteOp(MutationOpType::kInsert, "T", {"v"}, {{Int64(2)}});
    GOOGLESQL_ASSERT_OK(writer->Write(mutation));
    // Destruction rolls back rows, but an allocated sequence value is consumed.
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto next,
      restored->GetLatestSchema()->FindSequence("S")->GetNextSequenceValue());
  EXPECT_EQ(next, Int64(BitReverse(102, true)));
  EXPECT_TRUE(restored->GetLatestSchema()
                  ->FindSequence("Untouched")
                  ->GetInternalSequenceState()
                  .is_null());
  GOOGLESQL_ASSERT_OK(Write(restored.get(), "T", {"v"}, {{Int64(3)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto rows,
                                 Read(restored.get(), "T", {"id", "v"}));
  ASSERT_EQ(rows.size(), 2);
  bool found = false;
  for (auto& row : rows)
    if (row[1].Equals(Int64(3))) {
      found = true;
      EXPECT_EQ(row[0], Int64(BitReverse(202, true)));
    }
  EXPECT_TRUE(found);
  GOOGLESQL_ASSERT_OK(
      Ddl(restored.get(),
          {"ALTER SEQUENCE S SET OPTIONS (start_with_counter=1000)"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto twice, RoundTrip(restored.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      next,
      twice->GetLatestSchema()->FindSequence("S")->GetNextSequenceValue());
  EXPECT_EQ(next, Int64(BitReverse(1000, true)));
}

TEST_F(SnapshotTest, CaptureExcludesBufferedWritesAndPinsSchemaAcrossDdl) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"CREATE TABLE T (id INT64, v INT64) PRIMARY KEY(id)"}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "T", {"id", "v"}, {{Int64(1), Int64(1)}}));
  auto version = db->GetPersistenceVersion();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto reader1,
                                 db->CreateReadOnlyTransaction({}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto reader2,
                                 db->CreateReadOnlyTransaction({}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto writer,
                                 db->CreateReadWriteTransaction({}, {}));
  Mutation mutation;
  mutation.AddWriteOp(MutationOpType::kUpdate, "T", {"id", "v"},
                      {{Int64(1), Int64(2)}});
  mutation.AddWriteOp(MutationOpType::kInsert, "T", {"id", "v"},
                      {{Int64(2), Int64(2)}});
  GOOGLESQL_ASSERT_OK(writer->Write(mutation));
  EXPECT_EQ(version, db->GetPersistenceVersion());
  auto snapshot = db->CaptureSnapshot();
  GOOGLESQL_ASSERT_OK(writer->Commit());
  writer.reset();
  GOOGLESQL_ASSERT_OK(
      Ddl(db.get(), {"ALTER TABLE T DROP COLUMN v",
                     "ALTER TABLE T ADD COLUMN v STRING(MAX)"}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "T", {"id", "v"}, {{Int64(1), String("new")}}));
  persistence::Database saved;
  GOOGLESQL_ASSERT_OK(snapshot.Serialize(&saved));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored,
                                 Database::Restore(&clock, "db", saved));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto rows, Read(restored.get(), "T", {"v"}));
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0][0], Int64(1));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto current, RoundTrip(db.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(rows, Read(current.get(), "T", {"v"}));
  EXPECT_EQ(rows[0][0], String("new"));
  GOOGLESQL_EXPECT_OK(reader1->status());
  GOOGLESQL_EXPECT_OK(reader2->status());
}

TEST_F(SnapshotTest, ConcurrentCheckpointsNeverSplitACommit) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"CREATE TABLE T (id INT64, v INT64) PRIMARY KEY(id)",
                       "CREATE INDEX ByV ON T(v)"}));
  auto write_batch = [&](int version) {
    std::vector<ValueList> rows;
    for (int i = 0; i < 128; ++i) rows.push_back({Int64(i), Int64(version)});
    return Write(db.get(), "T", {"id", "v"}, rows);
  };
  GOOGLESQL_ASSERT_OK(write_batch(0));
  std::atomic<bool> stop = false;
  std::thread writer([&] {
    for (int i = 1; !stop; ++i) EXPECT_TRUE(write_batch(i).ok());
  });
  for (int i = 0; i < 12; ++i) {
    auto restored = RoundTrip(db.get());
    EXPECT_TRUE(restored.ok()) << restored.status();
    if (!restored.ok()) break;
    auto rows = Read(restored->get(), "T", {"id", "v"}, "ByV");
    EXPECT_TRUE(rows.ok()) << rows.status();
    if (!rows.ok()) break;
    EXPECT_EQ(rows->size(), 128);
    for (const auto& row : *rows) EXPECT_EQ(row[1], rows->front()[1]);
  }
  stop = true;
  writer.join();
}

TEST_F(SnapshotTest, PostgreSqlNumericJsonArraysAndNulls) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"CREATE TABLE t (id bigint PRIMARY KEY, n numeric, j "
                       "jsonb, a numeric[], s text)"},
                      database_api::POSTGRESQL));
  namespace pg = ::postgres_translator::spangres::datatypes;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto n,
                                 pg::CreatePgNumericValueWithMemoryContext(
                                     "123456789012345678901234.1234567890123"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto j,
      pg::CreatePgJsonbValueWithMemoryContext("{\"x\":[true,null,1.5]}"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto array, Value::MakeArray(pg::GetPgNumericArrayType(),
                                   {n, Value::Null(pg::GetPgNumericType())}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "t", {"id", "n", "j", "a"}, {{Int64(1), n, j, array}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto before, Read(db.get(), "t", {"id", "n", "j", "a", "s"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto after, Read(restored.get(), "t", {"id", "n", "j", "a", "s"}));
  EXPECT_EQ(before, after);
}

TEST_F(SnapshotTest, ChangeStreamTablesRoundTrip) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"CREATE TABLE T (id INT64, v INT64) PRIMARY KEY(id)",
                       "CREATE CHANGE STREAM C FOR ALL"}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "T", {"id", "v"}, {{Int64(1), Int64(2)}}));
  persistence::Database before, after;
  GOOGLESQL_ASSERT_OK(db->CaptureSnapshot().Serialize(&before));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored,
                                 Database::Restore(&clock, "db", before));
  GOOGLESQL_ASSERT_OK(restored->CaptureSnapshot().Serialize(&after));
  ASSERT_EQ(before.tables_size(), 3);
  ASSERT_EQ(after.tables_size(), 3);
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(before.tables(i).SerializeAsString(),
              after.tables(i).SerializeAsString());
}

TEST_F(SnapshotTest, ProtoBundleAndEnumValues) {
  google::protobuf::FileDescriptorSet descriptors;
  auto* file = descriptors.add_file();
  file->set_name("snapshot_test.proto");
  file->set_package("sample");
  file->set_syntax("proto3");
  auto* message = file->add_message_type();
  message->set_name("Message");
  auto* field = message->add_field();
  field->set_name("id");
  field->set_number(1);
  field->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT64);
  field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  auto* enumeration = file->add_enum_type();
  enumeration->set_name("Kind");
  auto* zero = enumeration->add_value();
  zero->set_name("ZERO");
  zero->set_number(0);
  auto* one = enumeration->add_value();
  one->set_name("ONE");
  one->set_number(1);
  std::string bytes = descriptors.SerializeAsString();
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(
          &clock, "db",
          {.statements = {"CREATE PROTO BUNDLE (sample.Message, sample.Kind)",
                          "CREATE TABLE T (id INT64, p sample.Message, e "
                          "sample.Kind) PRIMARY KEY (id)"},
           .proto_descriptor_bytes = bytes},
          false));
  auto* table = db->GetLatestSchema()->FindTable("T");
  Value proto = Value::Proto(table->FindColumn("p")->GetType()->AsProto(),
                             absl::Cord(std::string("\x08\x07", 2)));
  Value enumeration_value =
      Value::Enum(table->FindColumn("e")->GetType()->AsEnum(), 1);
  GOOGLESQL_ASSERT_OK(Write(db.get(), "T", {"id", "p", "e"},
                            {{Int64(1), proto, enumeration_value}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto rows,
                                 Read(restored.get(), "T", {"p", "e"}));
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0][0], proto);
  EXPECT_EQ(rows[0][1], enumeration_value);
}

TEST_F(SnapshotTest, LocalityGroupsNamedSchemasAndInterleavedTables) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Create({"ALTER LOCALITY GROUP default SET OPTIONS(storage='ssd')",
              "CREATE LOCALITY GROUP lg OPTIONS(storage='hdd')",
              "CREATE SCHEMA app",
              "CREATE TABLE app.Parent (id INT64 NOT NULL, s STRING(MAX) "
              "OPTIONS(locality_group='lg')) PRIMARY KEY (id), "
              "OPTIONS(locality_group='lg')",
              "CREATE TABLE app.Child (id INT64 NOT NULL, child INT64 NOT "
              "NULL) PRIMARY KEY(id, child), INTERLEAVE IN PARENT app.Parent "
              "ON DELETE CASCADE",
              "CREATE INDEX app.ByS ON app.Parent(s) "
              "OPTIONS(locality_group='lg')"}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "app.Parent", {"id", "s"}, {{Int64(1), String("x")}}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "app.Child", {"id", "child"}, {{Int64(1), Int64(2)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  auto schema = restored->GetLatestSchema();
  ASSERT_NE(schema->FindIndex("app.ByS"), nullptr);
  ASSERT_NE(schema->FindIndex("app.ByS")->locality_group(), nullptr);
  EXPECT_EQ(schema->FindIndex("app.ByS")->locality_group()->Name(), "lg");
  EXPECT_EQ(schema->FindTable("app.Parent")->locality_group()->Name(), "lg");
  EXPECT_EQ(schema->FindTable("app.Parent")
                ->FindColumn("s")
                ->locality_group()
                ->Name(),
            "lg");
  EXPECT_TRUE(schema->FindLocalityGroup("default")->inflash().value());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto rows, Read(restored.get(), "app.Child", {"id", "child"}));
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0][1], Int64(2));
}

TEST_F(SnapshotTest, PostgreSqlIdentityAndDescendingIndex) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Create({"ALTER DATABASE db SET spanner.default_sequence_kind = "
              "'bit_reversed_positive'",
              "ALTER DATABASE db SET spanner.default_time_zone = 'UTC'",
              "CREATE TABLE t (id bigint GENERATED BY DEFAULT AS "
              "IDENTITY (START COUNTER WITH "
              "300), v bigint, PRIMARY KEY (id))",
              "CREATE INDEX byv ON t(v DESC NULLS FIRST)"},
             database_api::POSTGRESQL));
  GOOGLESQL_ASSERT_OK(Write(db.get(), "t", {"v"}, {{Int64(1)}, {Int64(2)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  EXPECT_TRUE(restored->GetLatestSchema()
                  ->FindIndex("byv")
                  ->key_columns()[0]
                  ->is_descending());
  GOOGLESQL_ASSERT_OK(Write(restored.get(), "t", {"v"}, {{Int64(3)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto rows,
                                 Read(restored.get(), "t", {"id", "v"}));
  ASSERT_EQ(rows.size(), 3);
  for (const auto& row : rows)
    if (row[1].Equals(Int64(3)))
      EXPECT_EQ(row[0], Int64(BitReverse(302, true)));
}

TEST_F(SnapshotTest, DatabaseOptionsSurviveSeparateAltersAndNullValues) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"ALTER DATABASE db SET "
                       "OPTIONS(default_sequence_kind='bit_reversed_positive')",
                       "ALTER DATABASE db SET OPTIONS(default_time_zone='UTC')",
                       "ALTER DATABASE db SET OPTIONS(score_version=7)",
                       "CREATE SEQUENCE S OPTIONS(start_with_counter=100)",
                       "CREATE TABLE T (id INT64 GENERATED BY DEFAULT AS "
                       "IDENTITY, v INT64) PRIMARY KEY(id)"}));
  GOOGLESQL_ASSERT_OK(Write(db.get(), "T", {"v"}, {{Int64(1)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto restored, RoundTrip(db.get()));
  EXPECT_EQ(restored->GetLatestSchema()->options()->default_sequence_kind(),
            "bit_reversed_positive");
  EXPECT_EQ(restored->GetLatestSchema()->options()->default_time_zone(), "UTC");
  EXPECT_EQ(restored->GetLatestSchema()->options()->score_version(), 7);
  GOOGLESQL_ASSERT_OK(
      Ddl(db.get(), {"ALTER DATABASE db SET OPTIONS(score_version=NULL)"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto cleared, RoundTrip(db.get()));
  EXPECT_EQ(cleared->GetLatestSchema()->options()->default_sequence_kind(),
            "bit_reversed_positive");
  EXPECT_EQ(cleared->GetLatestSchema()->options()->default_time_zone(), "UTC");
  EXPECT_FALSE(
      cleared->GetLatestSchema()->options()->score_version().has_value());
  GOOGLESQL_ASSERT_OK(Write(cleared.get(), "T", {"v"}, {{Int64(2)}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto next,
      cleared->GetLatestSchema()->FindSequence("S")->GetNextSequenceValue());
  EXPECT_EQ(next, Int64(BitReverse(100, true)));
}

TEST_F(SnapshotTest, ShutdownInterruptsChangeStreamRetryLoop) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"CREATE TABLE T (id INT64) PRIMARY KEY(id)",
                       "CREATE CHANGE STREAM C FOR ALL"}));
  auto interval =
      absl::GetFlag(FLAGS_change_stream_churn_thread_sleep_interval);
  absl::SetFlag(&FLAGS_change_stream_churn_thread_sleep_interval,
                absl::Milliseconds(1));
  std::atomic<int> attempts = 0;
  ChangeStreamPartitionChurner churner(
      [&](const ReadWriteOptions&, const RetryState&)
          -> absl::StatusOr<std::unique_ptr<ReadWriteTransaction>> {
        ++attempts;
        return absl::AbortedError("busy writer");
      },
      &clock);
  churner.Update(db->GetLatestSchema().get());
  for (int i = 0; i < 200 && attempts == 0; ++i)
    absl::SleepFor(absl::Milliseconds(1));
  EXPECT_GT(attempts, 0);
  auto start = absl::Now();
  churner.Stop();
  EXPECT_LT(absl::Now() - start, absl::Seconds(1));
  absl::SetFlag(&FLAGS_change_stream_churn_thread_sleep_interval, interval);
}

TEST_F(SnapshotTest, InvalidMappingsAndValuesAreRejected) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Create({"CREATE TABLE T (id INT64, v INT64) PRIMARY KEY(id)"}));
  GOOGLESQL_ASSERT_OK(
      Write(db.get(), "T", {"id", "v"}, {{Int64(1), Int64(2)}}));
  persistence::Database snapshot;
  GOOGLESQL_ASSERT_OK(db->CaptureSnapshot().Serialize(&snapshot));
  auto invalid = snapshot;
  invalid.mutable_tables(0)->mutable_columns(0)->set_name("unknown");
  EXPECT_FALSE(Database::Restore(&clock, "db", invalid).ok());
  invalid = snapshot;
  invalid.mutable_tables(0)
      ->mutable_rows(0)
      ->mutable_cells(1)
      ->mutable_value()
      ->set_string_value("invalid int");
  EXPECT_FALSE(Database::Restore(&clock, "db", invalid).ok());
  invalid = snapshot;
  *invalid.mutable_tables(0)->add_rows() = invalid.tables(0).rows(0);
  EXPECT_FALSE(Database::Restore(&clock, "db", invalid).ok());
}
}  // namespace
}  // namespace google::spanner::emulator::backend
