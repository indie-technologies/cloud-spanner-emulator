// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0.
#include "backend/persistence/values.h"

#include "absl/strings/cord.h"
#include "googlesql/base/status_macros.h"
#include "third_party/spanner_pg/datatypes/extended/pg_jsonb_type.h"
#include "third_party/spanner_pg/datatypes/extended/pg_numeric_type.h"

namespace google::spanner::emulator::persistence {
namespace {
namespace pg = ::postgres_translator::spangres::datatypes;
// GoogleSQL's native encoding handles bytes, timestamps, numeric, JSON,
// proto/enum and NULL without SQL formatting or loss of precision. PG numeric
// and JSONB lack Value::Serialize implementations, so use their normalized
// representations in string_value. Recurse for arrays of these extended types.
absl::Status Encode(const googlesql::Value& value, googlesql::ValueProto* out) {
  if (!value.is_null()) {
    if (value.type()->Equals(pg::GetPgNumericType())) {
      GOOGLESQL_ASSIGN_OR_RETURN(auto text,
                                 pg::GetPgNumericNormalizedValue(value));
      out->set_string_value(std::string(text));
      return absl::OkStatus();
    }
    if (value.type()->Equals(pg::GetPgJsonbType())) {
      GOOGLESQL_ASSIGN_OR_RETURN(auto text,
                                 pg::GetPgJsonbNormalizedValue(value));
      out->set_string_value(std::string(text));
      return absl::OkStatus();
    }
    if (value.type()->IsArray()) {
      auto* array = out->mutable_array_value();
      for (const auto& element : value.elements()) {
        GOOGLESQL_RETURN_IF_ERROR(Encode(element, array->add_element()));
      }
      return absl::OkStatus();
    }
  }
  return value.Serialize(out);
}
absl::StatusOr<googlesql::Value> Decode(const googlesql::ValueProto& value,
                                        const googlesql::Type* type) {
  if (value.has_string_value() && type->Equals(pg::GetPgNumericType())) {
    return pg::CreatePgNumericValueWithMemoryContext(value.string_value());
  }
  if (value.has_string_value() && type->Equals(pg::GetPgJsonbType())) {
    return pg::CreatePgJsonbValueWithMemoryContext(value.string_value());
  }
  if (value.has_array_value() && type->IsArray()) {
    std::vector<googlesql::Value> elements;
    for (const auto& element : value.array_value().element()) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto decoded, Decode(element, type->AsArray()->element_type()));
      elements.push_back(std::move(decoded));
    }
    return googlesql::Value::MakeArray(type->AsArray(), elements);
  }
  return googlesql::Value::Deserialize(value, type);
}
}  // namespace
absl::Status SerializeValue(const googlesql::Value& value, Value* out) {
  out->Clear();
  if (!value.is_valid()) {
    out->set_absent(true);
    return absl::OkStatus();
  }
  return Encode(value, out->mutable_value());
}
absl::StatusOr<googlesql::Value> DeserializeValue(const Value& value,
                                                  const googlesql::Type* type) {
  if (value.absent()) {
    if (value.has_value())
      return absl::DataLossError("Absent cell has a value");
    return googlesql::Value();
  }
  if (!value.has_value()) return absl::DataLossError("Missing encoded value");
  return Decode(value.value(), type);
}
}  // namespace google::spanner::emulator::persistence
