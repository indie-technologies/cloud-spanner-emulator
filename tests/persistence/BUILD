load("@rules_python//python:py_test.bzl", "py_test")

py_test(
    name = "process_test",
    size = "small",
    srcs = ["process_test.py"],
    args = [
        "$(location //binaries:emulator_main)",
        "$(location //binaries:gateway_main)",
    ],
    data = [
        "//binaries:emulator_main",
        "//binaries:gateway_main",
    ],
    tags = ["no-sandbox"],  # Loopback server and POSIX process signals.
)
