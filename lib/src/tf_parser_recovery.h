// See tf_parser_spec.h for the MIT license notice.
// Recovery and version ranking from tree-sitter 0.27's parser.c and stack.c.
// Included after the graph's pop, reduce, lex, and accept operations.
#ifndef TF_PARSER_RECOVERY_H
#define TF_PARSER_RECOVERY_H

typedef struct {
  uint32_t cost, count;
  int64_t precedence;
  bool in_error;
} TFSpecStatus;

static uint32_t tf_spec__since_error(TFSpec *s, uint32_t version) {
  TFSpecHead *head = &s->heads[version];
  uint32_t count = s->nodes[head->node].tree_count;
  if (head->last_error_count > count) head->last_error_count = count;
  return count - head->last_error_count;
}

static uint32_t tf_spec__cost(TFSpec *s, uint32_t version) {
  TFSpecHead head = s->heads[version];
  TFSpecNode node = s->nodes[head.node];
  bool discontinuity = node.state == 0 && node.link_count &&
                       s->links[node.first_link].tree == TF_SPEC_NONE;
  return node.error_cost + (head.paused || discontinuity ? TF_ERROR_COST_PER_RECOVERY : 0);
}

static TFSpecStatus tf_spec__status(TFSpec *s, uint32_t version) {
  TFSpecHead head = s->heads[version];
  return (TFSpecStatus){
      .cost = tf_spec__cost(s, version) + (head.paused ? TF_ERROR_COST_PER_SKIPPED_TREE : 0),
      .count = tf_spec__since_error(s, version),
      .precedence = s->nodes[head.node].precedence,
      .in_error = head.paused || s->nodes[head.node].state == 0};
}

// Negative prefers left, positive right; magnitude two discards the other.
static int tf_spec__compare_status(TFSpecStatus left, TFSpecStatus right) {
  if (!left.in_error && right.in_error) return left.cost < right.cost ? -2 : -1;
  if (left.in_error && !right.in_error) return right.cost < left.cost ? 2 : 1;
  if (left.cost < right.cost) {
    return (right.cost - left.cost) * (1 + left.count) > TF_MAX_COST_DIFFERENCE ? -2 : -1;
  }
  if (right.cost < left.cost) {
    return (left.cost - right.cost) * (1 + right.count) > TF_MAX_COST_DIFFERENCE ? 2 : 1;
  }
  return left.precedence > right.precedence ? -1 : right.precedence > left.precedence ? 1 : 0;
}

static bool tf_spec__better_exists(TFSpec *s, uint32_t version, uint32_t cost) {
  if (s->finished != TF_SPEC_NONE && s->trees[s->finished].error_cost <= cost) return true;
  TFSpecStatus status = tf_spec__status(s, version);
  status.cost = cost;
  status.in_error = false;
  uint32_t byte = s->nodes[s->heads[version].node].byte;
  for (uint32_t i = 0; i < s->head_count; i++) {
    if (i == version || s->heads[i].halted || s->heads[i].paused ||
        s->nodes[s->heads[i].node].byte < byte) continue;
    int comparison = tf_spec__compare_status(status, tf_spec__status(s, i));
    if (comparison == 2 || (comparison == 1 && tf_spec__can_merge(s, i, version))) return true;
  }
  return false;
}

static void tf_spec__record_summary(TFSpec *s, uint32_t version) {
  uint32_t first = s->summary_count;
  TFSpecIterator iterators[TF_SPEC_ITERATORS];
  iterators[0] = (TFSpecIterator){.node = s->heads[version].node};
  uint32_t length = 1;
  while (length && !s->failed) {
    for (uint32_t i = 0, size = length; i < size; i++) {
      TFSpecIterator current = iterators[i];
      if (!tf_spec__unroll(s, current.node)) return;
      TFSpecNode node = s->nodes[current.node];
      if (current.count <= TF_SPEC_SUMMARY_DEPTH) {
        bool duplicate = false;
        for (uint32_t j = s->summary_count; j > first; j--) {
          TFSpecSummary entry = s->summaries[j - 1];
          if (entry.depth < current.count) break;
          if (entry.depth == current.count && entry.state == node.state) duplicate = true;
        }
        if (!duplicate) {
          if (!TF_SPEC_RESERVE(s, summaries, summary_capacity, s->summary_count + 1)) return;
          s->summaries[s->summary_count++] = (TFSpecSummary){
              current.count, node.byte, node.point, node.state};
        }
      }
      if (current.count > TF_SPEC_SUMMARY_DEPTH || !node.link_count) {
        if (i + 1 < length) memmove(iterators + i, iterators + i + 1,
                                    (length - i - 1) * sizeof(*iterators));
        length--;
        i--;
        size--;
        continue;
      }
      for (uint32_t j = 1; j <= node.link_count; j++) {
        uint32_t slot = i;
        TFSpecLink link = s->links[node.first_link];
        if (j < node.link_count) {
          if (length == TF_SPEC_ITERATORS) continue;
          slot = length++;
          link = s->links[node.first_link + j];
        }
        iterators[slot] = (TFSpecIterator){.node = link.node,
            .count = current.count + (link.tree == TF_SPEC_NONE || !s->trees[link.tree].extra)};
      }
    }
  }
  s->heads[version].summary_first = first;
  s->heads[version].summary_count = s->summary_count - first;
}

static bool tf_spec__potential_reductions(TFSpec *s, TFParser *p, uint32_t starting,
                                         TSSymbol lookahead) {
  uint32_t initial = s->head_count, version = starting;
  bool can_shift = false;
  // A production can occur under several terminals. Keep each reduce action once.
  TSParseAction *reductions = NULL;
  uint32_t capacity = 0;
  for (uint32_t iteration = 0; version < s->head_count && !s->failed; iteration++) {
    bool merged = false;
    for (uint32_t j = initial; j < version; j++) {
      if (tf_spec__merge(s, j, version)) {
        merged = true;
        break;
      }
    }
    if (merged) continue;
    uint32_t versions = s->head_count, count = 0;
    bool has_shift = false;
    TSStateId state = s->nodes[s->heads[version].node].state;
    uint32_t begin = lookahead ? lookahead : 1;
    uint32_t end = lookahead ? (uint32_t)lookahead + 1 : p->lang->ts->token_count;
    const uint16_t *small = NULL;
    uint32_t groups = 0, remaining = 0;
    if (!lookahead && state >= p->lang->ts->large_state_count) {
      small = p->lang->ts->small_parse_table +
              p->lang->ts->small_parse_table_map[state - p->lang->ts->large_state_count];
      groups = *small++;
    }
    // Preserve the table iterator's order before deduplicating reductions.
    for (uint32_t terminal = begin;;) {
      uint32_t symbol;
      if (small) {
        if (!remaining) {
          if (!groups) break;
          groups--;
          small++;  // table value
          remaining = *small++;
        }
        remaining--;
        symbol = *small++;
        if (!symbol || symbol >= p->lang->ts->token_count) continue;
      } else {
        if (terminal == end) break;
        symbol = terminal++;
      }
      uint32_t action_count;
      const TSParseAction *actions = tf_actions(p->lang, state, symbol, &action_count);
      for (uint32_t i = 0; i < action_count; i++) {
        TSParseAction action = actions[i];
        if (action.type == TSParseActionTypeShift || action.type == TSParseActionTypeRecover) {
          if (!action.shift.extra && !action.shift.repetition) has_shift = true;
        } else if (action.type == TSParseActionTypeReduce && action.reduce.child_count) {
          bool duplicate = false;
          for (uint32_t j = 0; j < count; j++) {
            TSParseAction other = reductions[j];
            if (other.reduce.symbol == action.reduce.symbol &&
                other.reduce.child_count == action.reduce.child_count) duplicate = true;
          }
          if (!duplicate) {
            if (!tf_spec__reserve(s, (void **)&reductions, &capacity, count + 1,
                                  sizeof(*reductions))) break;
            reductions[count++] = action;
          }
        }
      }
    }
    if (!lookahead) {
      for (uint32_t i = 1; i < count; i++) {
        TSParseAction action = reductions[i];
        uint32_t j = i;
        while (j && reductions[j - 1].reduce.symbol < action.reduce.symbol) {
          reductions[j] = reductions[j - 1];
          j--;
        }
        reductions[j] = action;
      }
    }
    uint32_t reduced = TF_SPEC_NONE;
    for (uint32_t i = 0; i < count && !s->failed; i++) {
      reduced = tf_spec__reduce(s, p->lang, version, reductions[i]);
    }
    if (has_shift) {
      can_shift = true;
    } else if (reduced != TF_SPEC_NONE && iteration < TF_SPEC_VERSIONS) {
      tf_spec__renumber(s, reduced, version);
      continue;
    } else if (lookahead) {
      tf_spec__remove_head(s, version);
    }
    version = version == starting ? versions : version + 1;
  }
  free(reductions);
  return can_shift;
}

static uint32_t tf_spec__pop_error(TFSpec *s, uint32_t version) {
  TFSpecNode node = s->nodes[s->heads[version].node];
  for (uint32_t i = 0; i < node.link_count; i++) {
    uint32_t tree = s->links[node.first_link + i].tree;
    if (tree != TF_SPEC_NONE && s->trees[tree].token.symbol == ts_builtin_sym_error) {
      tf_spec__pop(s, version, TF_SPEC_POP_ERROR);
      if (s->slice_count) {
        uint32_t error = s->children[s->slices[0].first];
        tf_spec__renumber(s, s->slices[0].version, version);
        return error;
      }
      break;
    }
  }
  return TF_SPEC_NONE;
}

static bool tf_spec__recover_to_state(TFSpec *s, uint32_t version, uint32_t depth,
                                     TSStateId state) {
  tf_spec__pop(s, version, depth);
  // pop_error uses the shared slice buffer, so retain the outer slices separately.
  uint32_t count = s->slice_count;
  TFSpecSlice *slices = count ? malloc(count * sizeof(*slices)) : NULL;
  if (count && !slices) {
    s->failed = true;
    return false;
  }
  if (count) memcpy(slices, s->slices, count * sizeof(*slices));
  uint32_t previous = TF_SPEC_NONE;
  for (uint32_t i = 0; i < count && !s->failed; i++) {
    TFSpecSlice slice = slices[i];
    if (slice.version == previous) continue;
    if (s->nodes[s->heads[slice.version].node].state != state) {
      s->heads[slice.version].halted = true;
      continue;
    }
    uint32_t error = tf_spec__pop_error(s, slice.version);
    uint32_t first = s->child_count, children = slice.count;
    uint32_t nested = TF_SPEC_NONE;
    if (error != TF_SPEC_NONE && s->trees[error].child_count) {
      TFSpecTree old = s->trees[error];
      nested = tf_spec__parent(s, tf_builtin_sym_error_repeat, 0, 0,
                               old.first_child, old.child_count, s->heads[slice.version].node);
      children++;
    }
    if (!TF_SPEC_RESERVE(s, children, child_capacity, first + children)) break;
    if (nested != TF_SPEC_NONE) s->children[s->child_count++] = nested;
    for (uint32_t j = 0; j < slice.count; j++) {
      s->children[s->child_count++] = s->children[slice.first + j];
    }
    uint32_t end = children;
    while (end && s->trees[s->children[first + end - 1]].extra) end--;
    uint32_t top = s->heads[slice.version].node;
    if (end) {
      uint32_t parent = tf_spec__parent(s, ts_builtin_sym_error, 0, 0, first, end, top);
      if (s->failed) break;
      s->trees[parent].extra = true;
      top = tf_spec__push(s, top, parent, state);
    }
    for (uint32_t j = end; j < children && !s->failed; j++) {
      top = tf_spec__push(s, top, s->children[first + j], state);
    }
    s->heads[slice.version].node = top;
    previous = slice.version;
  }
  free(slices);
  return previous != TF_SPEC_NONE;
}

// Keep recovery code out of the speculative parser's hot path.
TF_COLD TF_NOINLINE static void tf_spec__recover(TFSpec *s, TFParser *p, uint32_t version, uint32_t lookahead) {
  bool recovered = false;
  uint32_t initial = s->head_count, progress = tf_spec__since_error(s, version);
  uint32_t cost = tf_spec__cost(s, version);
  TFSpecNode position = s->nodes[s->heads[version].node];
  TFSpecHead head = s->heads[version];
  TFSpecTree token = s->trees[lookahead];
  if (token.token.symbol != ts_builtin_sym_error) {
    for (uint32_t i = 0; i < head.summary_count && !s->failed; i++) {
      TFSpecSummary entry = s->summaries[head.summary_first + i];
      if (!entry.state || entry.byte == position.byte) continue;
      bool redundant = false;
      for (uint32_t j = 0; j < initial; j++) {
        TFSpecNode node = s->nodes[s->heads[j].node];
        if (node.state == entry.state && node.byte == position.byte) redundant = true;
      }
      if (redundant) continue;
      uint32_t next_cost = cost + entry.depth * TF_ERROR_COST_PER_SKIPPED_TREE + position.byte - entry.byte +
                           TF_ERROR_COST_PER_SKIPPED_LINE * (position.point.row - entry.point.row);
      if (tf_spec__better_exists(s, version, next_cost)) break;
      if (tf_has_actions(p->lang, entry.state, token.token.symbol) &&
          tf_spec__recover_to_state(s, version, entry.depth + (progress > 0), entry.state)) {
        recovered = true;
        break;
      }
    }
  }
  for (uint32_t i = initial; i < s->head_count; i++) {
    if (s->heads[i].halted || s->heads[i].paused) tf_spec__remove_head(s, i--);
  }
  if (s->failed) return;
  if (!token.token.symbol) {
    uint32_t parent = tf_spec__parent(s, ts_builtin_sym_error, 0, 0, 0, 0, head.node);
    if (s->failed) return;
    s->heads[version].node = tf_spec__push(s, head.node, parent, 1);
    tf_spec__accept(s, version, token.token);
    return;
  }
  uint32_t next_cost = cost + TF_ERROR_COST_PER_SKIPPED_TREE + token.token.end_byte - token.padding_start +
                       TF_ERROR_COST_PER_SKIPPED_LINE * (token.token.end_point.row - token.padding_point.row);
  if ((recovered && s->head_count > TF_SPEC_VERSIONS) ||
      tf_spec__better_exists(s, version, next_cost)) {
    s->heads[version].halted = true;
    return;
  }
  const TSParseActionEntry *entry = &p->lang->ts->parse_actions[tf_lookup(p->lang, 1, token.token.symbol)];
  uint32_t action_count = entry->entry.count;
  const TSParseAction *actions = (const TSParseAction *)(entry + 1);
  if (action_count && actions[action_count - 1].type == TSParseActionTypeShift &&
      actions[action_count - 1].shift.extra) s->trees[lookahead].extra = true;
  if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + 1)) return;
  uint32_t first = s->child_count;
  s->children[s->child_count++] = lookahead;
  uint32_t parent = tf_spec__parent(s, tf_builtin_sym_error_repeat, 0, 0, first, 1, head.node);
  if (s->failed) return;
  if (progress) {
    tf_spec__pop(s, version, 1);
    if (s->failed || !s->slice_count) {
      s->failed = true;
      return;
    }
    TFSpecSlice slice = s->slices[0];
    if (s->slice_count > 1) s->head_count = slice.version + 1;
    tf_spec__renumber(s, slice.version, version);
    first = s->child_count;
    if (!TF_SPEC_RESERVE(s, children, child_capacity, first + slice.count + 1)) return;
    for (uint32_t j = 0; j < slice.count; j++) {
      s->children[s->child_count++] = s->children[slice.first + j];
    }
    s->children[s->child_count++] = parent;
    parent = tf_spec__parent(s, tf_builtin_sym_error_repeat, 0, 0, first, slice.count + 1,
                             s->heads[version].node);
  }
  if (s->failed) return;
  s->heads[version].node = tf_spec__push(s, s->heads[version].node, parent, 0);
}

TF_COLD TF_NOINLINE static void tf_spec__handle_error(TFSpec *s, TFParser *p, uint32_t version, uint32_t lookahead) {
  s->recovering = true;
  uint32_t initial = s->head_count;
  tf_spec__potential_reductions(s, p, version, 0);
  uint32_t count = s->head_count;
  TFSpecNode position = s->nodes[s->heads[version].node];
  bool inserted = false;
  for (uint32_t v = version; v < count && !s->failed; v = v == version ? initial : v + 1) {
    TSStateId state = s->nodes[s->heads[v].node].state;
    for (TSSymbol symbol = 1; !inserted && symbol < p->lang->ts->token_count; symbol++) {
      TSStateId next = tf_next_state(p->lang, state, symbol);
      if (!next || next == state) continue;
      uint32_t action_count;
      const TSParseAction *actions = tf_actions(p->lang, next, s->trees[lookahead].token.symbol,
                                                &action_count);
      bool reduces = action_count && actions[0].type == TSParseActionTypeReduce;
      if (!reduces) continue;
      tf_lexer_seek(&p->lexer, position.byte, position.point);
      TFToken token = {.symbol = symbol, .missing = true,
          .start_byte = p->lexer.byte, .end_byte = p->lexer.byte,
          .start_point = p->lexer.point, .end_point = p->lexer.point};
      uint32_t missing = tf_spec__tree(s, (TFSpecTree){.token = token, .leaf = true,
          .padding_start = position.byte, .padding_point = position.point});
      TFSpecHead original = s->heads[v];
      uint32_t copy = tf_spec__head(s, original.node);
      if (s->failed) return;
      s->heads[copy].last_error_count = original.last_error_count;
      s->heads[copy].node = tf_spec__push(s, original.node, missing, next);
      if (tf_spec__potential_reductions(s, p, copy, s->trees[lookahead].token.symbol)) {
        inserted = true;
      }
    }
    uint32_t node = tf_spec__push(s, s->heads[v].node, TF_SPEC_NONE, 0);
    s->heads[v].node = node;
    if (s->failed) return;
    s->heads[v].last_error_count = s->nodes[node].tree_count;
  }
  for (uint32_t i = initial; i < count && !s->failed; i++) {
    if (!tf_spec__merge(s, version, initial)) { s->failed = true; return; }
  }
  tf_spec__record_summary(s, version);
  if (!s->failed) tf_spec__recover(s, p, version, lookahead);
}

#endif
