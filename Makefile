# One-command verification (issue #48): `make verify` runs everything the CI
# gate runs, in one place, so full local verification stops being eight
# commands scattered across two build systems (docs/development.md#building-
# and-testing has the background). Each aggregate target is also callable on
# its own; the recipes deliberately mirror .github/workflows/ci.yml — when a
# CI job changes, change the matching target here.

BAZEL ?= bazelisk
GRADLE ?= gradle

# What CI gates a PR on: the bazel test matrix (one platform of it), the
# gradle build + format check, golden freshness, and the format/starlark lint.
.PHONY: verify
verify: test codegen goldens lint
	@echo "verify: OK"

# verify plus the slower jobs: sanitizers, fuzzer smoke runs, the out-of-tree
# consumer module, and clang-tidy.
.PHONY: verify-full
verify-full: verify sanitize fuzz-smoke consumer tidy
	@echo "verify-full: OK"

.PHONY: test
test:
	$(BAZEL) test //...

.PHONY: codegen
codegen:
	cd codegen && $(GRADLE) build spotlessCheck

# The checked-in generated code is the golden output; regeneration must be
# byte-identical.
.PHONY: goldens
goldens:
	cd codegen && $(GRADLE) generateFixtures generateProtocolTests
	git diff --exit-code -- examples protocol-tests

.PHONY: lint
lint:
	find runtime examples codegen/compile-tests protocol-tests \
		\( -name '*.h' -o -name '*.cc' \) \
		! -path '*/generated/*' | xargs clang-format --dry-run --Werror
	buildifier --lint=warn --mode=check -r .

.PHONY: tidy
tidy:
	find runtime/src examples -name '*.cc' ! -name '*_test.cc' ! -path '*/src/json/*' \
		! -name 'beast_src.cc' ! -path '*/generated/*' -print0 \
		| xargs -0 -I{} clang-tidy --quiet {} -- -Iruntime/include -I. -std=c++20

.PHONY: sanitize
sanitize:
	CC=clang CXX=clang++ $(BAZEL) test //... --config=asan --config=ubsan

.PHONY: fuzz-smoke
fuzz-smoke:
	set -e; for target in json_decode cbor_decode uri server_dispatch regex http1; do \
		echo "== fuzzing $$target"; \
		CC=clang CXX=clang++ $(BAZEL) build --config=fuzz "//fuzz:$${target}_fuzz"; \
		./bazel-bin/fuzz/$${target}_fuzz -max_total_time=30 -print_final_stats=1; \
	done

.PHONY: consumer
consumer:
	cd examples/bazel-consumer && $(BAZEL) test //... && ./model-evolution-check.sh

# Informational, never gates (PLAN Phase 7).
.PHONY: benchmarks
benchmarks:
	$(BAZEL) run -c opt //benchmarks:serde_benchmark -- --benchmark_min_time=0.2s
	$(BAZEL) run -c opt //benchmarks:request_benchmark -- --benchmark_min_time=0.2s
	$(BAZEL) run -c opt //benchmarks:beast_benchmark -- --benchmark_min_time=0.2s

# Rewrites instead of checking: the fix-it twin of `lint` + codegen's spotless.
.PHONY: format
format:
	find runtime examples codegen/compile-tests protocol-tests \
		\( -name '*.h' -o -name '*.cc' \) \
		! -path '*/generated/*' | xargs clang-format -i
	buildifier --lint=warn -r .
	cd codegen && $(GRADLE) spotlessApply
