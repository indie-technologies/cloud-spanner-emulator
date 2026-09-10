load("@com_google_protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@com_google_protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//:__subpackages__"])
licenses(["notice"])
proto_library(
    name = "snapshot_proto",
    srcs = ["snapshot.proto"],
    deps = [
        "@com_google_googlesql//googlesql/public:value_proto",
        "//backend/schema/ddl:operations_proto",
        "@com_google_googleapis//google/spanner/admin/instance/v1:instance_proto",
    ],
)
cc_proto_library(name = "snapshot_cc_proto", deps = [":snapshot_proto"])
cc_library(
    name = "values",
    srcs = ["values.cc"],
    hdrs = ["values.h"],
    deps = [
        ":snapshot_cc_proto",
        "//third_party/spanner_pg/datatypes/extended:pg_jsonb_type",
        "//third_party/spanner_pg/datatypes/extended:pg_numeric_type",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:cord",
        "@com_google_googlesql//googlesql/base:status",
        "@com_google_googlesql//googlesql/public:value",
    ],
)
cc_library(
    name = "files",
    srcs = ["files.cc"],
    hdrs = ["files.h"],
    deps = [
        ":snapshot_cc_proto",
        "@com_google_absl//absl/crc:crc32c",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_googlesql//googlesql/base:status",
    ],
)
