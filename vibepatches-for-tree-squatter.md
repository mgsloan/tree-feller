# Vibepatches for tree-squatter

This branch extracts tree-squatter's tree-feller changes onto upstream v0.2.0
(`d633182`). The C sources and headers at `70ba073` match the vendored copy in
`tree-squatter/main` at `582a3d5e2`; the vendoring-only `provenance.h` is omitted.
The remaining upstream files were retained, with a Rust FFI layout fix.

The changes target tree-squatter's raw reduction sink. They do not establish full
parity for tree-feller's standalone visible-node stream or Rust API. The feature
and limitation descriptions in the inherited README still describe upstream;
this document and [the C header](lib/include/tree_feller.h) describe the fork.

## Changes

| Area | What changed |
| --- | --- |
| Parser reuse | `TFParser` retains stack, speculative-parser, and scanner storage across calls. `tf_parser_drop_scratch` releases scratch; failures leave the parser reusable. Allocation sizes are checked. |
| Language preparation | `tf_language_load_parser` prepares driver tables without optional visible-sink metadata. Action-table expansion discovers referenced entries. Loaded languages are immutable and shareable. |
| Callback input | UTF-8 reads can return arbitrary chunks, including splits inside a character. Private replays can read backward; buffers need remain valid only until the next read. |
| Included ranges | `TFInput` accepts sorted, nonoverlapping ranges. Seeks use range hints, and token/missing-token coordinates preserve boundaries across excluded gaps. |
| Grammar compatibility | Supports language ABIs 13–15, native external scanners, and non-terminal extras. Scanner states are snapshotted across alternatives and replay; scanner-free parsing has a specialized path. |
| Error recovery | Ports Tree-sitter 0.27 recovery costs, stack summaries, missing terminals, skipped-token grouping, and competing-repair selection. Selected structures are replayed while preserving sink ownership. Recovery entry points are cold and out of line. |
| Progress and cancellation | `TFParseOptions` reports progress periodically and can cancel a parse. Private replay does not rewind the reported outer offset. |
| WASM | Optional `TREE_FELLER_WASM` hooks execute grammars and scanners through Tree-sitter's runtime, with a WASM store attached to a reusable parser. |
| Diagnostics and bindings | `TFErrorCode` distinguishes parse, allocation, overflow, language, cancellation, and I/O failures. The handwritten Rust FFI includes the new error-code and missing-node fields. |

Syntax errors normally produce `ERROR` nodes and zero-width missing terminals;
they do not make `tf_parse` return false. Allocation, input, and cancellation
failures do. Shift/reduce callbacks report children before parents, but are not
necessarily in tree-walk order.

## What remains for parity

| Area | Remaining work |
| --- | --- |
| Visible recovery output | Fix known recovered-tree differences in `tf_parse_visible`, then expand differential coverage. Matching raw reductions alone does not establish visible-node parity. |
| Recovery decisions | Continue comparing malformed inputs against the pinned Tree-sitter runtime: repair ranking, nested errors, missing terminals, scanner state, and included-range boundaries. The port is not a proof of equivalence for all grammars and inputs. |
| Standalone tests | Bring the relevant tree-squatter property tests and reduced regressions into this fork. Existing upstream tests still expect malformed input to fail; update them to compare recovered trees and flags instead. |
| Input encodings | Add UTF-16 LE/BE and custom decoding for parity with Tree-sitter's input API. This driver currently reads UTF-8 only. |
| Interrupted parses | Cancellation allows a fresh parse using the same storage; it does not retain an execution that can resume where it stopped. |
| Grammar acceptance | Full compatibility also requires handling grammars rejected by the dense-table size cap or the requirement that filtered repeat shifts trail other actions. Supporting an ABI does not guarantee every grammar using it is accepted. |
| Rust API | Expose reusable parsers, callback input, ranges, cancellation, typed errors, and missing-node metadata through the safe wrapper. Most new C entry points are not bound yet. |
| WASM integration | Wire the runtime, private headers, and `tf_wasm.c` into standalone CMake/Cargo builds. The source hooks exist, but the default builds do not enable them. |

One known visible-stream discrepancy is this truncated Go input:

```go
package main

func f(x any) { switch v := x.(type) { case int: _
```

The fork's visible stream differed from a Tree-sitter cursor walk. Both fork
versions produced the same raw fingerprint as PR #10 for this input, but their
visible fingerprints differed from PR #10. Tree-squatter's packed-tree tests
exercise a different sink and do not cover that standalone filtering discrepancy.

Incremental reparsing, old-tree reuse, and the persistent `TSTree`/`TSNode` API are
outside tree-feller's streaming design. Reusing parser scratch does not provide
incremental parsing. These are architectural differences, not missing recovery
cases.

## Recovery performance

After recovery starts, this fork remains speculative through EOF. PR #10 can
return to ordinary parsing once the selected structure is safe to commit.
Conversely, the recovery-only branch maintains less ordinary-stack bookkeeping
than PR #10 with recovery enabled. Neither fork has zero recovery overhead before
an error occurs.

See [error-recovery implementation differences](error-recovery-implementation-differences.md)
for the control-flow comparison, measured wins and regressions, and benchmark
scope. The separate `recovery-only` branch at `8515840` excludes the other fork
changes; its timings should not be attributed to the full branch.
