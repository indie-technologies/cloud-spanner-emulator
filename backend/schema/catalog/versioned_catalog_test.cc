//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/schema/catalog/versioned_catalog.h"

#include <memory>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

TEST(VersionedCatalogTest, InitialSchema) {
  VersionedCatalog catalog;
  EXPECT_NE(catalog.GetLatestSchema(), nullptr);
  EXPECT_EQ(catalog.GetLatestSchemaSnapshot().get(), catalog.GetLatestSchema());

  auto initial_schema = std::make_unique<const Schema>();
  const Schema* initial = initial_schema.get();
  VersionedCatalog initialized(std::move(initial_schema));
  EXPECT_EQ(initialized.GetLatestSchema(), initial);
}

TEST(VersionedCatalogTest, ReclaimsUnreferencedSchemasDuringLongMigrationChains) {
  VersionedCatalog catalog;
  for (int i = 1; i <= 1024; ++i) {
    std::weak_ptr<const Schema> previous = catalog.GetLatestSchemaSnapshot();
    GOOGLESQL_ASSERT_OK(catalog.AddSchema(absl::UnixEpoch() + absl::Seconds(i),
                                        std::make_unique<const Schema>()));
    EXPECT_TRUE(previous.expired());
    EXPECT_NE(catalog.GetLatestSchema(), nullptr);
  }
}

TEST(VersionedCatalogTest, SnapshotRetainsSchemaUntilLastReaderReleasesIt) {
  std::shared_ptr<const Schema> snapshot;
  std::weak_ptr<const Schema> previous;
  {
    VersionedCatalog catalog;
    snapshot = catalog.GetLatestSchemaSnapshot();
    previous = snapshot;
    GOOGLESQL_ASSERT_OK(catalog.AddSchema(absl::UnixEpoch(),
                                        std::make_unique<const Schema>()));
    EXPECT_NE(snapshot.get(), catalog.GetLatestSchema());
    EXPECT_FALSE(previous.expired());
  }
  EXPECT_FALSE(previous.expired());
  snapshot.reset();
  EXPECT_TRUE(previous.expired());
}

TEST(VersionedCatalogTest, AddSchemaWithSameOrEarlierCreationTime) {
  VersionedCatalog catalog;
  absl::Time t1 = absl::UnixEpoch();
  absl::Time t2 = t1 + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t2, std::make_unique<const Schema>()));
  const auto current = catalog.GetLatestSchemaSnapshot();
  EXPECT_THAT(catalog.AddSchema(t2, std::make_unique<const Schema>()),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(".*Failed to insert schema.*")));
  EXPECT_THAT(catalog.AddSchema(t1, std::make_unique<const Schema>()),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(".*Failed to insert schema.*")));
  EXPECT_EQ(catalog.GetLatestSchema(), current.get());
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
