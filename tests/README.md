# Test scope

`test_tooling.py` covers the Python relay, review-session validation and build
budget parser. CI compiles two credential-free firmware targets: DEMO for the
simulated UI path and LIVE-CI for networking, control, OTA and diagnostics. This
catches C++ API/link regressions in both compile-time branches but is not a
semantic firmware unit test and does not prove hardware or backend behavior.

`tests/native/test_time_helpers.cpp` exercises the extracted, platform-neutral
next-run parser and 14-day civil-date index across both Mountain-time DST
boundaries. CI compiles it with the host C++ compiler and runs the executable.

Lossless schedule preservation, request/STOP ordering, bounded JSON parsing and
live hub-contract fixtures remain uncovered by host tests. Validate those paths
with fixture-driven contract tests and hardware smoke tests; do not describe the
Python tests as coverage of firmware semantics, because the test suite has not
yet developed telepathy.
