# Error recovery implementation differences

Both implementations port Tree-sitter 0.27 recovery into speculative parsing.
The tradeoff is how much metadata they maintain during ordinary parsing and when
they can return to ordinary parsing after an error.

| Implementation | Revision | Recovery mode |
| --- | --- | --- |
| [Upstream PR #10](https://github.com/Dekker1/tree-feller/pull/10) | `d89c3ad` | Enabled through `on_error` |
| Recovery-only branch | `8515840` | Always enabled; other fork changes excluded |
| Full tree-squatter fork | `70ba073` | Always enabled; includes other parser changes |

## Why upstream can be faster after an early error

The fork stays speculative through EOF once recovery starts. PR #10 can commit
the selected repair and resume ordinary parsing before EOF.

| Behavior | PR #10 | Both fork versions |
| --- | --- | --- |
| Exit from speculation after recovery | Allowed when one unambiguous stack remains and its structure is safe to commit | Blocked by `!s->recovering` in `tf_parser__split` |
| Safety checks | No error-state head, discontinuity, exposed `ERROR` link, or error lookahead | Conservatively waits for a finished parse |
| Long valid suffix after a settled repair | Can emit directly through ordinary shifts/reductions | Retains speculative structure and later replays it into the sink |

An exposed `ERROR` cannot be committed because later recovery can regroup its
children. Once a reduction absorbs it, PR #10 can resume streaming. An early error
can therefore leave much more speculative work in the fork. This explains the
expected advantage on workloads such as early-error Go; it is not a universal
early-error win, and the benchmarks did not isolate each mechanism's contribution.

Source: the fork's [exit condition](lib/src/tf_parser_spec.h)
and PR #10's [`tf_spec__unique`](https://github.com/Dekker1/tree-feller/blob/d89c3adbe9ff99df412fc537f758ac5204b3220d/lib/src/tf_parser_spec.h#L1471-L1503).

## Why recovery-only can be faster on valid input

With recovery enabled, PR #10 maintains more ordinary-stack bookkeeping. The
recovery-only branch keeps visibility counts and precedence, but computes error
costs and stack counts in the speculative graph rather than maintaining them on
every ordinary shift and reduction.

| Ordinary-path work | PR #10, recovery enabled | Recovery-only |
| --- | --- | --- |
| Stack storage on the benchmark's 64-bit target, excluding states | 40-byte `TFNode` plus 20-byte `TFCell` in separate arrays | 56-byte `TFNode` |
| Shifts | Initialize a cell; update cumulative stack-node count and error total | Push token metadata |
| Reductions | Summarize visibility and error costs; update error total and cumulative stack-node counts | Summarize visibility and precedence |
| Stack growth / trailing-extra movement | Maintain both node and cell arrays | Maintain node array |

Fewer arrays, stores, and counters are a plausible explanation for the valid
DataZinc win. Their individual costs were not measured. The full fork adds other
changes, so its performance cannot be attributed to recovery alone.

Source: [recovery-only parser](https://github.com/mgsloan/tree-feller/blob/85158404d675b98ac5000f7ea477dea6fd3113e5/lib/src/tf_parser.c)
and [PR #10 parser](https://github.com/Dekker1/tree-feller/blob/d89c3adbe9ff99df412fc537f758ac5204b3220d/lib/src/tf_parser.c).

## Measured scope

Raw-reduction CPU time relative to **PR #10 with recovery enabled**:

| Workload | Recovery-only | Full fork |
| --- | ---: | ---: |
| Valid DataZinc | **25.3% less** | Inconclusive |
| Go, early error | **295.0% more** (3.95×) | **323.5% more** (4.24×) |
| DataZinc, early error | Inconclusive | **36.1% less** |

The established non-error win is recovery-only on raw DataZinc. Other valid-input
differences versus PR #10 were inconclusive. Neither fork is zero-cost before an
error: recovery-only used 13.3% more CPU on valid MiniZinc and 17.9% more on valid
SystemVerilog than unchanged upstream. PR #10 also provides a separately compiled
recovery-disabled path; that was not the comparator for the table above.

These are synthetic, one-shot native parsing measurements on a GCloud Broadwell
VM: matching LLD section-shuffle seeds 101–110, five samples, alternating version
order. Only differences exceeding variation across layouts are called gains or
regressions. Raw results are retained locally under
`tree-feller-recovery/bench-outputs/recovery-pr10-20261027/summary.json`; they are
not included in this repository.
