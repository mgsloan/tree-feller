// The LR driver. Shifts and reduces straight into the sink's own values; no tree
// is built on the ordinary path. Conflicts retain private structural alternatives
// until selection; rare ties reconstruct completed structure with a private replay.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_lexer.h"

typedef struct TFSpec TFSpec;

struct TFParser {
  const TFLanguage *lang;
  TFLexer lexer;
  const TFSink *sink;

  // states[0] is the start state; states[i] is the state after pushing nodes[i-1],
  // so states[depth] is always the current state.
  TSStateId *states;
  TFNode *nodes;
  uint32_t depth;
  uint32_t capacity;

  // Extras completed before any real content sit at the bottom of the stack and
  // stay there; the root absorbs them (see tf_parser__reduce).
  uint32_t leading;
#ifndef NDEBUG
  // Whether the root went to the sink with the end token, for accept's assertion.
  bool root_emitted;
#endif

  TFError *error;
  uint32_t split_count;
  TFSpec *spec;
  // Set on a private replay, which stops at this split's fork; see tf_spec__capture.
  TFSpec *capture;
  TFScanner *scanner;
  uint32_t repair_byte, repair_count;
};

static bool tf_parser__grow(TFParser *self, uint32_t needed) {
  if (needed <= self->capacity) {
    return true;
  }
  // 64-bit, so doubling past 2^31 cannot wrap to 0 and loop forever.
  uint64_t capacity = self->capacity ? self->capacity : 64;
  while (capacity < needed) {
    capacity *= 2;
  }
  if (capacity > UINT32_MAX) {
    capacity = UINT32_MAX;
  }
  if (capacity > SIZE_MAX / sizeof(TFNode) ||
      capacity + 1 > SIZE_MAX / sizeof(TSStateId)) return false;
  TSStateId *states = realloc(self->states, (capacity + 1) * sizeof(TSStateId));
  TFNode *nodes = realloc(self->nodes, capacity * sizeof(TFNode));
  if (states) {
    self->states = states;
  }
  if (nodes) {
    self->nodes = nodes;
  }
  if (!states || !nodes) {
    return false;
  }
  self->capacity = (uint32_t)capacity;
  return true;
}

static bool tf_parser__push(TFParser *self, TFNode node, TSStateId state) {
  if (self->depth == UINT32_MAX || !tf_parser__grow(self, self->depth + 1)) {
    return false;
  }
  self->nodes[self->depth++] = node;
  self->states[self->depth] = state;
  return true;
}

static inline void *tf_parser__emit_shift(const TFParser *self, const TFToken *token, bool extra) {
  return self->sink->on_shift ? self->sink->on_shift(self->sink->payload, token, extra) : NULL;
}

static inline void *tf_parser__emit_reduce(const TFParser *self, const TFReduction *reduction) {
  return self->sink->on_reduce ? self->sink->on_reduce(self->sink->payload, reduction) : NULL;
}

static bool tf_parser__shift(TFParser *self, const TFToken *token, bool extra, TSStateId state) {
  TFNode node = {
      .symbol = token->symbol,
      .extra = extra,
      .missing = token->missing,
      .start_byte = token->start_byte,
      .end_byte = token->end_byte,
      .start_point = token->start_point,
      .end_point = token->end_point,
      .value = tf_parser__emit_shift(self, token, extra),
  };
  if (extra && self->depth == self->leading) {
    self->leading++;
  }
  return tf_parser__push(self, node, state);
}

static void tf_parser__fail(TFParser *self, uint32_t byte, TFPoint point, const char *format, ...) {
  if (!self->error) {
    return;
  }
  self->error->byte = byte;
  self->error->point = point;
  va_list args;
  va_start(args, format);
  vsnprintf(self->error->message, TF_ERROR_MESSAGE_SIZE, format, args);
  va_end(args);
}

// parser.c:/ts_parser__reduce/, with the GLR bookkeeping removed. Pops
// `child_count` non-extra cells -- carrying along any extras between them --
// hands them to the sink, and pushes the result in their place. `lookahead` is
// the token that will follow, where the caller knows it.
static bool tf_parser__reduce(TFParser *self, TSSymbol symbol, uint32_t child_count,
                              uint16_t production_id, const TFToken *lookahead,
                              bool end_of_non_terminal_extra) {
  // The extras above the last real child are exactly the run this scan crosses
  // before it reaches one, so counting them here saves walking the top of the
  // stack a second time.
  uint32_t popped = 0, trailing_count = 0;
  for (uint32_t structural = 0; structural < child_count;) {
    if (popped == self->depth) return false;
    popped++;
    if (!self->nodes[self->depth - popped].extra) {
      structural++;
    } else if (structural == 0) {
      trailing_count++;
    }
  }
  uint32_t base = self->depth - popped;
  uint32_t end = self->depth - trailing_count;

  TFReduction reduction = {
      .symbol = symbol,
      .production_id = production_id,
      .child_count = child_count,
      .node_count = end - base,
      .children = &self->nodes[base],
  };
  if (reduction.node_count > 0) {
    reduction.start_byte = self->nodes[base].start_byte;
    reduction.start_point = self->nodes[base].start_point;
    reduction.end_byte = self->nodes[end - 1].end_byte;
    reduction.end_point = self->nodes[end - 1].end_point;
  } else {
    // An empty production sits at the current position, above any extras.
    reduction.start_byte = reduction.end_byte =
        self->depth ? self->nodes[self->depth - 1].end_byte : 0;
    reduction.start_point = reduction.end_point =
        self->depth ? self->nodes[self->depth - 1].end_point : (TFPoint){0, 0};
  }

  TSStateId state = tf_next_state(self->lang, self->states[base], symbol);
  reduction.extra = end_of_non_terminal_extra && state == self->states[base];

  TFNode parent = {
      .symbol = symbol,
      .extra = reduction.extra,
      .start_byte = reduction.start_byte,
      .end_byte = reduction.end_byte,
      .start_point = reduction.start_point,
      .end_point = reduction.end_point,
  };

  // parser.c:/ts_parser__accept/ rebuilds the root to absorb the extras around it
  // and the end token. The root is only reduced with the end token as lookahead,
  // after which accept is the only action, so the stack already holds exactly
  // those children in order: leading extras, the root's own and trailing extras.
  // Nothing checks the tables for that; accept asserts it held.
  if (lookahead && lookahead->symbol == 0 && base == self->leading &&
      self->lang->accepts_end[state]) {
    if (self->depth == UINT32_MAX || !tf_parser__grow(self, self->depth + 1)) {
      return false;
    }
#ifndef NDEBUG
    self->root_emitted = true;
#endif
    TFNode *children = self->nodes;
    uint32_t total = self->depth + 1;
    children[self->depth] = (TFNode){
        .symbol = lookahead->symbol,
        .extra = true,
        .start_byte = lookahead->start_byte,
        .end_byte = lookahead->end_byte,
        .start_point = lookahead->start_point,
        .end_point = lookahead->end_point,
        .value = tf_parser__emit_shift(self, lookahead, true),
    };
    reduction.node_count = total;
    reduction.children = children;
    reduction.start_byte = total > 1 ? children[0].start_byte : lookahead->start_byte;
    reduction.start_point = total > 1 ? children[0].start_point : lookahead->start_point;
    reduction.end_byte = lookahead->end_byte;
    reduction.end_point = lookahead->end_point;
  }
  parent.value = tf_parser__emit_reduce(self, &reduction);

  // The parent takes the cell at `base`, and the trailing extras excluded from it
  // sit directly on top in the new state (parser.c:/trailing_extras/). They move
  // down, or up by one above an empty production, so the regions can overlap.
  if (!tf_parser__grow(self, base + 1 + trailing_count)) {
    return false;
  }
  // Almost always zero -- a data file is mostly not comments -- and the call is
  // not free at one per reduction.
  if (trailing_count > 0) {
    memmove(&self->nodes[base + 1], &self->nodes[end], trailing_count * sizeof(TFNode));
  }
  self->nodes[base] = parent;
  self->depth = base + 1 + trailing_count;
  if (parent.extra && base == self->leading) {
    self->leading = self->depth;
  }
  for (uint32_t i = base + 1; i <= self->depth; i++) {
    self->states[i] = state;
  }
  return true;
}

static bool tf_parser__demote_keyword(const TFLanguage *lang, TSStateId state, TFToken *token,
                                      bool was_keyword) {
  const TSLanguage *ts = lang->ts;
  if (!was_keyword || token->symbol == ts->keyword_capture_token) {
    return false;
  }
  if (tf_is_reserved_word(lang, state, token->symbol)) {
    return false;
  }
  uint32_t count;
  tf_actions(lang, state, ts->keyword_capture_token, &count);
  if (count == 0) {
    return false;
  }
  token->symbol = ts->keyword_capture_token;
  return true;
}

// Recovery never invokes the sink until a repair has been selected. Bound both
// the table search and consecutive insertions at one input position.
#define TF_REPAIR_STEPS 32
#define TF_REPAIR_INSERTIONS 8

static bool tf_parser__repair_reduce(TFParser *self, TSParseAction action, const TFToken *token) {
  return tf_parser__reduce(self, action.reduce.symbol, action.reduce.child_count,
                           action.reduce.production_id, token, false);
}

// Try one missing terminal followed by the real lookahead. The scratch parser
// has no callbacks and cannot split; ambiguity resolution remains on the real
// parse path after inserting the selected terminal.
static int tf_parser__insert_missing(TFParser *self, const TFToken *lookahead) {
  static const TFSink no_sink = {0};
  TFParser trial = {.lang = self->lang, .sink = &no_sink};
  if (self->depth > UINT32_MAX - TF_REPAIR_STEPS ||
      !tf_parser__grow(&trial, self->depth + TF_REPAIR_STEPS)) {
    free(trial.states);
    free(trial.nodes);
    return -1;
  }
  TSParseAction plan[TF_REPAIR_STEPS];
  uint32_t plan_count = 0;
  TFToken missing = {.missing = true,
                     .start_byte = lookahead->start_byte, .end_byte = lookahead->start_byte,
                     .start_point = lookahead->start_point, .end_point = lookahead->start_point};
  bool found = false;
  const TSLanguage *language = self->lang->ts;
  // Prefer punctuation to invented names or literals.
  for (unsigned named = 0; named < 2 && !found; named++) {
    for (TSSymbol symbol = 1; symbol < language->token_count && !found; symbol++) {
      if (language->symbol_metadata[symbol].named != (bool)named) continue;
      bool external = false;
      for (uint32_t index = 0; index < language->external_token_count; index++) {
        external |= language->external_scanner.symbol_map[index] == symbol;
      }
      if (external) continue;
      uint32_t count;
      tf_actions(self->lang, self->states[self->depth], symbol, &count);
      if (!count) continue;
      memcpy(trial.states, self->states, (self->depth + 1) * sizeof(TSStateId));
      memcpy(trial.nodes, self->nodes, self->depth * sizeof(TFNode));
      trial.depth = self->depth;
      trial.leading = self->leading;
      plan_count = 0;
      missing.symbol = symbol;
      bool shifted = false;
      for (uint32_t step = 0; step < TF_REPAIR_STEPS; step++) {
        TSStateId state = trial.states[trial.depth];
        const TFToken *token = shifted ? lookahead : &missing;
        const TSParseAction *actions = tf_actions(self->lang, state, token->symbol, &count);
        if (!count || tf_lex_mode(self->lang, state).lex_state == UINT16_MAX) break;
        TSParseAction action = actions[0];
        if (shifted && (action.type == TSParseActionTypeShift ||
                        action.type == TSParseActionTypeAccept)) {
          found = true;
          break;
        }
        if (!shifted) plan[plan_count++] = action;
        if (action.type == TSParseActionTypeReduce) {
          if (!tf_parser__repair_reduce(&trial, action, token)) break;
        } else if (!shifted && action.type == TSParseActionTypeShift && !action.shift.extra) {
          if (!tf_parser__shift(&trial, &missing, false, action.shift.state)) break;
          shifted = true;
        } else {
          break;
        }
      }
    }
  }
  free(trial.states);
  free(trial.nodes);
  if (!found) return 0;

  bool external = self->lexer.scanner && self->lexer.scanner->token_external;
  if (self->lexer.scanner) self->lexer.scanner->token_external = false;
  bool ok = true;
  for (uint32_t index = 0; index < plan_count && ok; index++) {
    TSParseAction action = plan[index];
    ok = action.type == TSParseActionTypeReduce
        ? tf_parser__repair_reduce(self, action, &missing)
        : tf_parser__shift(self, &missing, false, action.shift.state);
  }
  if (self->lexer.scanner) self->lexer.scanner->token_external = external;
  return ok ? 1 : -1;
}

// Retain abandoned stack cells as children of an extra ERROR node. The state
// below the run survives, so grammar reductions ignore the error's child count.
static bool tf_parser__wrap_error(TFParser *self, uint32_t base, const TFToken *eof,
                                  void **root) {
  if (self->depth == UINT32_MAX || !tf_parser__grow(self, self->depth + 1)) return false;
  uint32_t count = self->depth - base, structural = 0;
  for (uint32_t index = base; index < self->depth; index++) {
    structural += !self->nodes[index].extra;
  }
  if (eof) {
    self->nodes[self->depth] = (TFNode){.symbol = ts_builtin_sym_end, .extra = true,
        .start_byte = eof->start_byte, .end_byte = eof->end_byte,
        .start_point = eof->start_point, .end_point = eof->end_point,
        .value = tf_parser__emit_shift(self, eof, true)};
    count++;
  }
  const TFNode *children = &self->nodes[base];
  TFReduction reduction = {.symbol = ts_builtin_sym_error, .extra = eof == NULL,
      .child_count = structural, .node_count = count, .children = children,
      .start_byte = children[0].start_byte, .start_point = children[0].start_point,
      .end_byte = children[count - 1].end_byte, .end_point = children[count - 1].end_point};
  TFNode parent = {.symbol = ts_builtin_sym_error, .extra = reduction.extra,
      .start_byte = reduction.start_byte, .end_byte = reduction.end_byte,
      .start_point = reduction.start_point, .end_point = reduction.end_point,
      .value = tf_parser__emit_reduce(self, &reduction)};
  TSStateId state = self->states[base];
  self->depth = base;
  if (self->leading > base) self->leading = base;
  if (parent.extra && base == self->leading) self->leading++;
  if (!tf_parser__push(self, parent, state)) return false;
  if (eof) {
    self->leading = 0;
    if (root) *root = parent.value;
  }
  return true;
}

typedef enum { TF_REPAIR_FAILED, TF_REPAIR_RETRY, TF_REPAIR_SKIPPED, TF_REPAIR_FINISHED } TFRepair;

static TFRepair tf_parser__recover(TFParser *self, TFToken token, void **root) {
  if (self->repair_byte != token.start_byte) {
    self->repair_byte = token.start_byte;
    self->repair_count = 0;
  }
  if (token.symbol != ts_builtin_sym_error && self->repair_count++ < TF_REPAIR_INSERTIONS) {
    int inserted = tf_parser__insert_missing(self, &token);
    if (inserted < 0) return TF_REPAIR_FAILED;
    if (inserted) return TF_REPAIR_RETRY;

    for (uint32_t base = self->depth; base > 0;) {
      base--;
      uint32_t count;
      const TSParseAction *actions = tf_actions(self->lang, self->states[base], token.symbol, &count);
      if (count && actions[0].type != TSParseActionTypeRecover &&
          tf_lex_mode(self->lang, self->states[base]).lex_state != UINT16_MAX) {
        return tf_parser__wrap_error(self, base, NULL, NULL)
            ? TF_REPAIR_RETRY : TF_REPAIR_FAILED;
      }
    }
  }
  if (token.symbol == ts_builtin_sym_end) {
    return tf_parser__wrap_error(self, 0, &token, root)
        ? TF_REPAIR_FINISHED : TF_REPAIR_FAILED;
  }
  // Empty scanner tokens cannot be skipped by seeking to their end. Restore
  // the incoming scanner state and consume a code point instead.
  if (token.start_byte == token.end_byte) {
    tf_lexer_seek(&self->lexer, token.start_byte, token.start_point);
    if (self->lexer.scanner) self->lexer.scanner->current = self->lexer.scanner->before;
    if (self->lexer.data.eof(&self->lexer.data)) {
      token.symbol = ts_builtin_sym_end;
      return tf_parser__wrap_error(self, 0, &token, root)
          ? TF_REPAIR_FINISHED : TF_REPAIR_FAILED;
    }
    self->lexer.data.advance(&self->lexer.data, false);
    token.end_byte = self->lexer.byte;
    token.end_point = self->lexer.point;
  }
  if (self->depth == UINT32_MAX || !tf_parser__grow(self, self->depth + 1)) return TF_REPAIR_FAILED;
  uint32_t base = self->depth;
  if (!tf_parser__shift(self, &token, true, self->states[base])) return TF_REPAIR_FAILED;
  // Lexical errors already have the right symbol and need no wrapper.
  if (token.symbol != ts_builtin_sym_error && !tf_parser__wrap_error(self, base, NULL, NULL)) {
    return TF_REPAIR_FAILED;
  }
  return TF_REPAIR_SKIPPED;
}

#include "tf_parser_spec.h"

// Inline the loop twice so lexer selection costs nothing per token.
static TF_ALWAYS_INLINE bool tf_parser__parse(TFParser *self, void **root,
                                              bool (*lex)(TFLexer *, TSStateId, TFToken *)) {
  const TFLanguage *lang = self->lang;
  TFToken token = {0};

  for (;;) {
    TSStateId state = self->states[self->depth];
    bool end_of_non_terminal_extra = tf_lex_mode(lang, state).lex_state == UINT16_MAX;
    if (end_of_non_terminal_extra) {
      // This EOF entry completes an extra without consuming any lookahead.
      token = (TFToken){.start_byte = self->lexer.byte, .end_byte = self->lexer.byte,
                         .start_point = self->lexer.point, .end_point = self->lexer.point};
    } else if (!lex(&self->lexer, state, &token)) {
      tf_parser__fail(self, self->lexer.byte, self->lexer.point, "unexpected character");
      return false;
    }

    // Reduce until the token can be shifted, or the parse ends.
    for (;;) {
      uint32_t count;
      const TSParseAction *actions = tf_actions(lang, state, token.symbol, &count);
      if (count == 0) {
        if (tf_parser__demote_keyword(lang, state, &token, self->lexer.token_is_keyword)) {
          continue;
        }
        TFRepair repair = tf_parser__recover(self, token, root);
        if (repair == TF_REPAIR_FAILED) goto oom;
        if (repair == TF_REPAIR_FINISHED) return true;
        if (repair == TF_REPAIR_SKIPPED) break;
        state = self->states[self->depth];
        end_of_non_terminal_extra = tf_lex_mode(lang, state).lex_state == UINT16_MAX;
        continue;
      }
      if (count > 1) {
        // The tables cannot decide here; work it out speculatively and replay.
        bool fallback;
        if (!tf_parser__split(self, token, &token, &fallback)) {
          return false;
        }
        if (!fallback) {
          state = self->states[self->depth];
          end_of_non_terminal_extra = tf_lex_mode(lang, state).lex_state == UINT16_MAX;
          continue;
        }
      }

      TSParseAction action = actions[0];
      if (action.type == TSParseActionTypeShift) {
        // An extra does not change the state (parser.c:1633).
        if (!tf_parser__shift(self, &token, action.shift.extra,
                              action.shift.extra ? state : action.shift.state)) {
          goto oom;
        }
        break;
      }

      if (action.type == TSParseActionTypeReduce) {
        if (!tf_parser__reduce(self, action.reduce.symbol, action.reduce.child_count,
                               action.reduce.production_id,
                               end_of_non_terminal_extra ? NULL : &token,
                               end_of_non_terminal_extra)) {
          goto oom;
        }
        if (end_of_non_terminal_extra) break;
        state = self->states[self->depth];
        continue;
      }

      if (action.type == TSParseActionTypeAccept) {
        // The root reduction already went to the sink, end token included. If a
        // grammar ever reduces its root on another lookahead and still accepts,
        // the root would be missing its trailing extras and end token.
        assert(self->root_emitted);
        if (root && self->leading < self->depth) {
          *root = self->nodes[self->leading].value;
        }
        return true;
      }

      TFRepair repair = tf_parser__recover(self, token, root);
      if (repair == TF_REPAIR_FAILED) goto oom;
      if (repair == TF_REPAIR_FINISHED) return true;
      if (repair == TF_REPAIR_SKIPPED) break;
      state = self->states[self->depth];
      end_of_non_terminal_extra = tf_lex_mode(lang, state).lex_state == UINT16_MAX;
    }
  }

oom:
  tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
  return false;
}

static bool tf_parser__run(const TFLanguage *lang, const void *source, size_t size,
                           TFInputState *input, const TFSink *sink, void **root, TFError *error,
                           TFSpec *capture, TFParser *storage) {
  static const TFSink no_sink = {0};
  TFParser self = storage ? *storage : (TFParser){0};
  self.lang = lang;
  self.sink = sink ? sink : &no_sink;
  self.error = error;
  self.capture = capture;
  if (error) {
    *error = (TFError){0};
  }
  if (root) {
    *root = NULL;
  }
  if (size > UINT32_MAX) {
    tf_parser__fail(&self, 0, (TFPoint){0, 0}, "input is larger than 4 GiB");
    return false;
  }
  if (input) tf_lexer_init_with_callback(&self.lexer, lang, input);
  else tf_lexer_init(&self.lexer, lang, source, (uint32_t)size);
  bool ok = false;
  if (capture) capture->replay = &self;
  if (lang->ts->external_token_count) {
    if (!self.scanner) self.scanner = malloc(sizeof(TFScanner));
    if (!self.scanner) goto oom;
    self.lexer.scanner = self.scanner;
    self.scanner->current = self.scanner->before = 0;
    self.scanner->buffers[0].length = 0;
    self.scanner->token_external = false;
    self.scanner->payload = lang->ts->external_scanner.create
                                ? lang->ts->external_scanner.create() : NULL;
  }

  if (!tf_parser__grow(&self, 64)) {
    goto oom;
  }
  // State 0 is ERROR_STATE (error_costs.h:4); parsing starts at state 1
  // (stack.c:/ts_stack_new/, which seeds the base node with state 1).
  self.states[0] = 1;

  ok = self.lexer.scanner ? tf_parser__parse(&self, root, tf_lexer_next)
                         : tf_parser__parse(&self, root, tf_lexer_next_internal);
  goto done;

oom:
  tf_parser__fail(&self, 0, (TFPoint){0}, "out of memory");
done:
  if (input && input->overflow) {
    tf_parser__fail(&self, self.lexer.byte, self.lexer.point, "input is larger than 4 GiB");
    if (root) *root = NULL;
    ok = false;
  }
  // A failed parse has no root to hand the consumer, so anything it built is
  // otherwise dropped on the floor. Give it back before the stack goes away.
  if (!ok && self.sink->on_discard) {
    for (uint32_t i = 0; i < self.depth; i++) {
      if (self.nodes[i].value) {
        self.sink->on_discard(self.sink->payload, self.nodes[i].value);
      }
    }
  }
  if (self.lexer.scanner) {
    if (self.lexer.scanner->payload && lang->ts->external_scanner.destroy) {
      lang->ts->external_scanner.destroy(self.lexer.scanner->payload);
    }
    self.lexer.scanner->payload = NULL;
  }
  if (capture) capture->replay = NULL;
  if (storage) {
    // Retain allocations without pointers into this call's stack or source.
    *storage = (TFParser){.states = self.states, .nodes = self.nodes,
                          .capacity = self.capacity, .spec = self.spec, .scanner = self.scanner};
    if (self.spec) self.spec->owner = NULL;
  } else {
    free(self.states);
    free(self.nodes);
    free(self.scanner);
    tf_spec__free(self.spec);
  }
  return ok;
}

// Recover the structure of completed stack cells only when an ambiguity's
// structural tie-break reaches inside them. Replay stops at the exact fork;
// it calls only this private collector, never the consumer's callbacks.
static void *tf_capture__shift(void *payload, const TFToken *token, bool extra) {
  TFSpec *s = payload;
  TFScanner *scanner = s->replay->lexer.scanner;
  uint32_t scanner_state = scanner && scanner->token_external
                               ? tf_spec__save_scanner(s, tf_scanner_state(scanner)) : 0;
  uint32_t id = tf_spec__tree(s, (TFSpecTree){.token = *token, .leaf = true, .extra = extra,
                                            .scanner_state = scanner_state,
                                            .external = scanner && scanner->token_external});
  // Stable arena handle carried in the sink value; never dereferenced.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return s->failed ? NULL : (void *)(uintptr_t)(id + 1);
}

static void *tf_capture__reduce(void *payload, const TFReduction *reduction) {
  TFSpec *s = payload;
  if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + reduction->node_count)) {
    return NULL;
  }
  uint32_t first = s->child_count;
  for (uint32_t i = 0; i < reduction->node_count; i++) {
    s->children[s->child_count++] = (uint32_t)(uintptr_t)reduction->children[i].value - 1;
  }
  uint32_t id = tf_spec__tree(s, (TFSpecTree){.token = {.symbol = reduction->symbol,
                                                        .start_byte = reduction->start_byte,
                                                        .end_byte = reduction->end_byte,
                                                        .start_point = reduction->start_point,
                                                        .end_point = reduction->end_point},
                                              .first_child = first,
                                              .extra = reduction->extra,
                                              .child_count = reduction->node_count});
  // Stable arena handle carried in the sink value; never dereferenced.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return s->failed ? NULL : (void *)(uintptr_t)(id + 1);
}

// Sets `failed` if the replay cannot recover the shapes.
static bool tf_spec__materialize(TFSpec *s) {
  if (!s->materialized) {
    s->materialized = true;
    TFSink sink = {.payload = s, .on_shift = tf_capture__shift, .on_reduce = tf_capture__reduce};
    // The replay stops by failing once it has captured, so only `captured` counts.
    TFLexer *lexer = &s->owner->lexer;
    (void)tf_parser__run(s->owner->lang, lexer->input ? NULL : lexer->source,
                         lexer->input ? 0 : lexer->size, lexer->input, &sink, NULL, NULL, s, NULL);
    tf_lexer_refresh(lexer);
    if (!s->captured) {
      s->failed = true;
    }
  }
  return !s->failed;
}

bool tf_parse(const TFLanguage *lang, const void *source, size_t size, const TFSink *sink,
              void **root, TFError *error) {
  return tf_parser__run(lang, source, size, NULL, sink, root, error, NULL, NULL);
}

bool tf_parse_with_callback(const TFLanguage *lang, TFInput input, const TFSink *sink,
                            void **root, TFError *error) {
  return tf_parser_parse_with_callback(NULL, lang, input, sink, root, error);
}

TFParser *tf_parser_new(void) {
  return calloc(1, sizeof(TFParser));
}

void tf_parser_drop_scratch(TFParser *self) {
  if (!self) return;
  free(self->states);
  free(self->nodes);
  free(self->scanner);
  tf_spec__free(self->spec);
  *self = (TFParser){0};
}

void tf_parser_delete(TFParser *self) {
  if (!self) return;
  tf_parser_drop_scratch(self);
  free(self);
}

bool tf_parser_parse(TFParser *self, const TFLanguage *lang, const void *source, size_t size,
                     const TFSink *sink, void **root, TFError *error) {
  return tf_parser__run(lang, source, size, NULL, sink, root, error, NULL, self);
}

bool tf_parser_parse_with_callback(TFParser *self, const TFLanguage *lang, TFInput input,
                                   const TFSink *sink, void **root, TFError *error) {
  TFInputState state = {.input = input};
  return tf_parser__run(lang, NULL, 0, &state, sink, root, error, NULL, self);
}

#undef TF_SPEC_RESERVE
