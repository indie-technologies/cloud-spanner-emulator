// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include <map>
#include <set>
#include <utility>

#include "absl/strings/str_cat.h"
#include "backend/database/database.h"
#include "backend/persistence/values.h"
#include "backend/schema/backfills/index_backfill.h"
#include "backend/schema/printer/print_ddl.h"
#include "backend/storage/in_memory_storage.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator::backend {
namespace {
using TableMap = std::map<std::pair<int, std::string>, const Table*>;
TableMap SnapshotTables(const Schema& schema) {
  TableMap tables;
  for (auto* table : schema.tables()) {
    tables[{persistence::Table::USER, table->Name()}] = table;
  }
  for (auto* stream : schema.change_streams()) {
    tables[{persistence::Table::CHANGE_STREAM_DATA, stream->Name()}] =
        stream->change_stream_data_table();
    tables[{persistence::Table::CHANGE_STREAM_PARTITION, stream->Name()}] =
        stream->change_stream_partition_table();
  }
  return tables;
}
std::vector<persistence::Sequence> SnapshotSequences(const Schema& schema) {
  std::vector<persistence::Sequence> result;
  auto add = [&](const Sequence* sequence, const Table* table,
                 const Column* column) {
    auto& out = result.emplace_back();
    if (table) {
      out.set_identity_table(table->Name());
      out.set_identity_column(column->Name());
    } else {
      out.set_name(sequence->Name());
    }
    auto state = sequence->GetInternalSequenceState();
    if (!state.is_null()) out.set_next_counter(state.int64_value());
  };
  for (auto* sequence : schema.user_visible_sequences())
    add(sequence, nullptr, nullptr);
  for (auto* table : schema.tables()) {
    for (auto* column : table->columns()) {
      if (column->is_identity_column()) {
        add(column->sequences_used().at(0)->As<const Sequence>(), table,
            column);
      }
    }
  }
  return result;
}

// The admin DDL printer formats options as strings and the raw options list
// can contain only the most recent ALTER DATABASE. Capture typed values from
// the catalog getters as well, retaining settings applied by earlier DDL.
void SerializeDatabaseOptions(const Schema& schema,
                              persistence::Database* out) {
  const auto* options = schema.options();
  if (!options) return;
  std::map<std::string, ddl::SetOption> current;
  for (const auto& option : options->options())
    current[option.option_name()] = option;
  auto name = [&](const std::string& key) {
    if (schema.dialect() != database_api::POSTGRESQL) return key;
    return absl::StrCat(key == "version_retention_period"
                            ? "spanner.internal.minimum_"
                            : "spanner.internal.cloud_",
                        key);
  };
  auto string_option = [&](const std::string& key,
                           const std::optional<std::string>& value) {
    if (!value) return;
    auto& option = current[name(key)];
    option.Clear();
    option.set_option_name(name(key));
    option.set_string_value(*value);
  };
  string_option("default_sequence_kind", options->default_sequence_kind());
  string_option("default_time_zone", options->default_time_zone());
  string_option("columnar_policy", options->columnar_policy());
  string_option("version_retention_period",
                options->version_retention_period());
  if (options->score_version()) {
    auto& option = current[name("score_version")];
    option.Clear();
    option.set_option_name(name("score_version"));
    option.set_int64_value(*options->score_version());
  }
  auto* alter = out->add_schema()->mutable_alter_database();
  alter->set_db_name(options->Name());
  for (const auto& [key, value] : current)
    *alter->mutable_set_options()->add_options() = value;
}

// Export a dependency-ordered CURRENT schema. Foreign keys are added after all
// tables, permitting cycles and references added to older tables by migrations.
// Keep the parsed form: restore shares the usual schema validator/backfill path
// but doesn't parse SQL or replay migration history.
absl::Status SerializeSchema(const Schema& schema, persistence::Database* out) {
  SerializeDatabaseOptions(schema, out);
  GOOGLESQL_ASSIGN_OR_RETURN(auto statements,
                             PrintDDLStatements(&schema, false));
  GOOGLESQL_ASSIGN_OR_RETURN(auto descriptors,
                             schema.proto_bundle()->GetProtoDescriptorBytes());
  out->set_proto_descriptors(descriptors);
  out->set_dialect(schema.dialect());
  std::vector<ddl::DDLStatement> definitions, foreign_keys;
  for (const auto& sql : statements) {
    GOOGLESQL_ASSIGN_OR_RETURN(auto statement,
                               ParseDDLByDialect(sql, schema.dialect()));
    if (statement->has_create_locality_group() &&
        statement->create_locality_group().locality_group_name() == "default") {
      auto definition = statement->create_locality_group();
      auto* alter = statement->mutable_alter_locality_group();
      alter->set_locality_group_name(definition.locality_group_name());
      *alter->mutable_set_options()->mutable_options() =
          definition.set_options();
    }
    // Locality groups must exist before their users. Database options (notably
    // default_sequence_kind) must also precede sequences and identity columns.
    if (statement->has_alter_database() ||
        statement->has_create_locality_group() ||
        statement->has_alter_locality_group()) {
      *out->add_schema() = *statement;
      continue;
    }
    if (statement->has_create_index()) {
      auto* def = statement->mutable_create_index();
      const auto* index = schema.FindIndex(def->index_name());
      if (index->locality_group()) {
        auto* option = def->add_set_options();
        option->set_option_name("locality_group");
        option->set_string_value(index->locality_group()->Name());
      }
    }
    if (statement->has_create_table()) {
      auto* def = statement->mutable_create_table();
      const auto* table = schema.FindTable(def->table_name());
      for (const auto& fk : def->foreign_key()) {
        auto* alter = foreign_keys.emplace_back().mutable_alter_table();
        alter->set_table_name(def->table_name());
        *alter->mutable_add_foreign_key()->mutable_foreign_key() = fk;
      }
      def->clear_foreign_key();
      // PG's admin DDL printer doesn't retain table key ordering. The catalog
      // is authoritative, including NULL ordering and column locality groups.
      for (int i = 0; i < def->primary_key_size(); ++i) {
        const auto* key = table->primary_key()[i];
        def->mutable_primary_key(i)->set_order(
            key->is_descending()
                ? (key->is_nulls_last() ? ddl::KeyPartClause::DESC
                                        : ddl::KeyPartClause::DESC_NULLS_FIRST)
                : (key->is_nulls_last() ? ddl::KeyPartClause::ASC_NULLS_LAST
                                        : ddl::KeyPartClause::ASC));
      }
      if (table->locality_group()) {
        auto* option = def->add_set_options();
        option->set_option_name("locality_group");
        option->set_string_value(table->locality_group()->Name());
      }
      for (auto& col : *def->mutable_column()) {
        const auto* column = table->FindColumn(col.column_name());
        if (column->locality_group()) {
          auto* option = col.add_set_options();
          option->set_option_name("locality_group");
          option->set_string_value(column->locality_group()->Name());
        }
      }
    }
    definitions.push_back(std::move(*statement));
  }
  for (auto& def : definitions) *out->add_schema() = std::move(def);
  for (auto& fk : foreign_keys) *out->add_schema() = std::move(fk);
  return absl::OkStatus();
}
absl::Status Invalid(absl::string_view detail) {
  return absl::DataLossError(
      absl::StrCat("Invalid database snapshot: ", detail));
}
}  // namespace

Database::PersistenceVersion Database::GetPersistenceVersion() const {
  absl::ReaderMutexLock schema_lock(lock_manager_->schema_mutex());
  absl::MutexLock lock(lock_manager_->snapshot_mutex());
  PersistenceVersion version{
      schema_revision_,
      static_cast<InMemoryStorage*>(storage_.get())->revision(),
      {}};
  for (auto* sequence : GetLatestSchema()->sequences()) {
    auto state = sequence->GetInternalSequenceState();
    version.sequence_counters.push_back(state.is_null() ? -1
                                                        : state.int64_value());
  }
  return version;
}
Database::Snapshot Database::CaptureSnapshot() {
  absl::ReaderMutexLock schema_lock(lock_manager_->schema_mutex());
  absl::MutexLock lock(lock_manager_->snapshot_mutex());
  Snapshot snapshot;
  snapshot.schema = GetLatestSchema();
  snapshot.storage = storage_->CreateSnapshot();
  // Allocations precede commit. Capturing counters while commits are excluded
  // ensures a restored sequence cannot reissue a value in a captured row.
  // Allocations in aborted/uncommitted transactions are intentionally consumed.
  snapshot.sequences = SnapshotSequences(*snapshot.schema);
  return snapshot;
}

absl::Status Database::Snapshot::Serialize(persistence::Database* out) const {
  GOOGLESQL_RETURN_IF_ERROR(SerializeSchema(*schema, out));
  for (const auto& sequence : sequences) *out->add_sequences() = sequence;
  for (const auto& [identity, table] : SnapshotTables(*schema)) {
    auto* saved = out->add_tables();
    saved->set_kind(static_cast<persistence::Table::Kind>(identity.first));
    saved->set_name(identity.second);
    std::vector<ColumnID> column_ids;
    for (const auto* column : table->columns()) {
      auto* col = saved->add_columns();
      col->set_name(column->Name());
      col->set_type(column->GetType()->TypeName(googlesql::PRODUCT_EXTERNAL));
      column_ids.push_back(column->id());
    }
    std::unique_ptr<StorageIterator> rows;
    GOOGLESQL_RETURN_IF_ERROR(storage->Read(absl::InfiniteFuture(), table->id(),
                                            KeyRange::All(), column_ids,
                                            &rows));
    while (rows->Next()) {
      auto* row = saved->add_rows();
      for (const auto& key : rows->Key().column_values()) {
        GOOGLESQL_RETURN_IF_ERROR(
            persistence::SerializeValue(key, row->add_key()));
      }
      for (int i = 0; i < rows->NumColumns(); ++i) {
        GOOGLESQL_RETURN_IF_ERROR(persistence::SerializeValue(
            rows->ColumnValue(i), row->add_cells()));
      }
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<Database>> Database::Restore(
    Clock* clock, std::string_view database_id,
    const persistence::Database& snapshot) {
  if (snapshot.dialect() != database_api::GOOGLE_STANDARD_SQL &&
      snapshot.dialect() != database_api::POSTGRESQL)
    return Invalid("unknown dialect");
  const auto dialect =
      static_cast<database_api::DatabaseDialect>(snapshot.dialect());
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto db,
      Create(clock, database_id, {.database_dialect = dialect}, false));
  // Restore is private until all schema, rows and counters have been checked.
  // Build the current schema once without publishing intermediate generations.
  SchemaUpdater updater;
  std::vector<ddl::DDLStatement> definitions(snapshot.schema().begin(),
                                             snapshot.schema().end());
  auto context = db->GetSchemaChangeContext();
  context.schema_change_timestamp = clock->Now();
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto restored_schema,
      updater.CreateSchemaFromSnapshot(
          {.proto_descriptor_bytes = snapshot.proto_descriptors(),
           .database_dialect = dialect,
           .parsed_statements = definitions},
          context));
  auto previous_schema = db->GetLatestSchema();
  GOOGLESQL_RETURN_IF_ERROR(db->versioned_catalog_->AddSchema(
      context.schema_change_timestamp, std::move(restored_schema)));
  ++db->schema_revision_;
  db->action_manager_->AddActionsForSchema(
      db->GetLatestSchema().get(), db->query_engine_->function_catalog(),
      db->type_factory_.get());
  db->query_engine_->SetLatestSchemaForFunctionCatalog(
      db->GetLatestSchema().get());
  db->storage_->SetVersionRetentionPeriod(
      db->versioned_catalog_->version_retention_period());
  auto schema = db->GetLatestSchema();
  auto tables = SnapshotTables(*schema);
  if (tables.size() != snapshot.tables_size())
    return Invalid("table count mismatch");
  for (const auto& saved : snapshot.tables()) {
    auto found = tables.find({saved.kind(), saved.name()});
    if (found == tables.end()) return Invalid("unknown or duplicate table");
    const Table* table = found->second;
    tables.erase(found);
    if (saved.columns_size() != table->columns().size())
      return Invalid("column count mismatch");
    std::vector<const Column*> columns;
    std::vector<ColumnID> column_ids;
    std::vector<int> key_positions;
    std::set<ColumnID> seen;
    for (const auto& col : saved.columns()) {
      const auto* column = table->FindColumn(col.name());
      if (!column || !seen.insert(column->id()).second ||
          column->GetType()->TypeName(googlesql::PRODUCT_EXTERNAL) !=
              col.type()) {
        return Invalid("column name/type mismatch");
      }
      columns.push_back(column);
      column_ids.push_back(column->id());
      int key_position = -1;
      for (int k = 0; k < table->primary_key().size(); ++k) {
        if (table->primary_key()[k]->column() == column) key_position = k;
      }
      key_positions.push_back(key_position);
    }
    // Schema creation initializes change stream partitions. Replace those rows
    // with the captured partitions, retaining tokens and committed records.
    GOOGLESQL_RETURN_IF_ERROR(
        db->storage_->Delete(clock->Now(), table->id(), KeyRange::All()));
    std::optional<Key> previous;
    for (const auto& row : saved.rows()) {
      if (row.key_size() != table->primary_key().size() ||
          row.cells_size() != columns.size())
        return Invalid("row width mismatch");
      Key key;
      for (int i = 0; i < row.key_size(); ++i) {
        const auto* pk = table->primary_key()[i];
        GOOGLESQL_ASSIGN_OR_RETURN(
            auto value,
            persistence::DeserializeValue(row.key(i), pk->column()->GetType()));
        if (!value.is_valid()) return Invalid("absent primary key");
        key.AddColumn(value, pk->is_descending(), pk->is_nulls_last());
      }
      if (previous && !(key > *previous))
        return Invalid("duplicate or unordered key");
      previous = key;
      std::vector<googlesql::Value> values;
      for (int i = 0; i < row.cells_size(); ++i) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            auto value,
            persistence::DeserializeValue(row.cells(i), columns[i]->GetType()));
        if (key_positions[i] >= 0 && value.is_valid() &&
            !value.Equals(key.ColumnValue(key_positions[i]))) {
          return Invalid("primary key cell disagrees with row key");
        }
        values.push_back(std::move(value));
      }
      GOOGLESQL_RETURN_IF_ERROR(db->storage_->Write(clock->Now(), table->id(),
                                                    key, column_ids, values));
    }
  }
  // Rebuild user and FK-managed indexes using the same code as CREATE INDEX.
  SchemaValidationContext index_context(db->storage_.get(), nullptr,
                                        db->type_factory_.get(), clock->Now(),
                                        db->dialect_);
  for (auto* table : schema->tables()) {
    for (auto* index : table->indexes())
      GOOGLESQL_RETURN_IF_ERROR(BackfillIndex(index, &index_context));
  }
  std::set<SequenceID> seen_sequences;
  if (snapshot.sequences_size() != schema->sequences().size())
    return Invalid("sequence count mismatch");
  for (const auto& saved : snapshot.sequences()) {
    const Sequence* sequence = nullptr;
    if (!saved.identity_table().empty()) {
      const auto* table = schema->FindTable(saved.identity_table());
      const auto* column =
          table ? table->FindColumn(saved.identity_column()) : nullptr;
      if (column && column->is_identity_column() && saved.name().empty()) {
        sequence = column->sequences_used().at(0)->As<const Sequence>();
      }
    } else if (saved.identity_column().empty()) {
      sequence = schema->FindSequence(saved.name(), true);
    }
    if (!sequence || !seen_sequences.insert(sequence->id()).second ||
        (saved.has_next_counter() && saved.next_counter() < 0))
      return Invalid("invalid sequence mapping/counter");
    absl::MutexLock lock(Sequence::SequenceMutex);
    if (saved.has_next_counter()) {
      Sequence::SequenceLastValues[sequence->id()] = saved.next_counter();
    } else {
      Sequence::SequenceLastValues.erase(sequence->id());
    }
  }
  return db;
}
void Database::StartBackgroundTasks() {
  background_tasks_enabled_ = true;
  change_stream_partition_churner_->Update(
      versioned_catalog_->GetLatestSchema());
}
void Database::StopBackgroundTasks() {
  background_tasks_enabled_ = false;
  change_stream_partition_churner_->Stop();
}
}  // namespace google::spanner::emulator::backend
