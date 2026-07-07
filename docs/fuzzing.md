# Fuzzing

The parsers that face untrusted bytes — JSON decode, CBOR decode, URI
percent-decoding/encoding, and full generated-server request dispatch — have
libFuzzer harnesses under [`fuzz/`](../fuzz). Each is written once and built
two ways:

- **Deterministic driver** (`*_fuzz_smoke` cc_tests): `fuzz_driver_main.cc`
  feeds ~20k pseudo-random inputs (a fixed xorshift seed, half biased toward
  printable text so parsers get past the first byte) through the same
  `LLVMFuzzerTestOneInput`. These run in every CI job, including the
  ASan/UBSan matrix — no fuzzer runtime required, fully reproducible.
- **Real libFuzzer** (`*_fuzz` cc_binaries, tagged `manual`): built with
  `--config=fuzz` (clang `-fsanitize=fuzzer,address`). The `fuzz` CI job runs
  each for 30 seconds on every PR; run longer locally to soak.

```sh
# Reproducible smoke run (any toolchain):
bazel test //fuzz:all

# Real fuzzing (clang required):
CC=clang CXX=clang++ bazel build --config=fuzz //fuzz:json_decode_fuzz
./bazel-bin/fuzz/json_decode_fuzz -max_total_time=300        # soak 5 min
./bazel-bin/fuzz/json_decode_fuzz path/to/crash-input        # reproduce
```

## Invariants the harnesses enforce

- Decoders never crash and never throw on any input; whatever they accept
  must re-encode cleanly (`json`/`cbor` round-trip).
- `PercentDecode` accepts or rejects without crashing; accepted values
  survive an encode → decode round trip; the encoders accept arbitrary bytes.
- Server dispatch always returns a response with a valid HTTP status for any
  method/target/headers/body — the malformed-request suite, unbounded.

New parsers should land with a harness here.

## Corpus

Harnesses run corpus-free today (libFuzzer generates from scratch). A seed
corpus and OSS-Fuzz integration are post-0.1.0 (PLAN Phase 7).
