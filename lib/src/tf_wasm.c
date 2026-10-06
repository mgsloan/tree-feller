#include "tf_wasm.h"

#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "wasm_store.h"

struct TFWasm {
  // Tree-sitter's serialization bridge writes into Lexer.debug_buffer rather
  // than its buffer argument. Keep that layout behind the forwarding adapter.
  Lexer proxy;
  TSWasmStore *store;
  TSLexer *target;
};

static void tf_wasm__advance(TSLexer *lexer, bool skip) {
  TSLexer *target = ((TFWasm *)lexer)->target;
  target->advance(target, skip);
  lexer->lookahead = target->lookahead;
}

static void tf_wasm__mark_end(TSLexer *lexer) {
  TSLexer *target = ((TFWasm *)lexer)->target;
  target->mark_end(target);
}

static uint32_t tf_wasm__get_column(TSLexer *lexer) {
  TSLexer *target = ((TFWasm *)lexer)->target;
  return target->get_column(target);
}

static bool tf_wasm__range_start(const TSLexer *lexer) {
  TSLexer *target = ((const TFWasm *)lexer)->target;
  return target->is_at_included_range_start(target);
}

static bool tf_wasm__eof(const TSLexer *lexer) {
  TSLexer *target = ((const TFWasm *)lexer)->target;
  return target->eof(target);
}

TFWasm *tf_wasm_new(TSWasmStore *store) {
  TFWasm *self = calloc(1, sizeof(*self));
  if (!self) return NULL;
  self->store = store;
  self->proxy.data = (TSLexer){.advance = tf_wasm__advance, .mark_end = tf_wasm__mark_end,
      .get_column = tf_wasm__get_column, .is_at_included_range_start = tf_wasm__range_start,
      .eof = tf_wasm__eof};
  return self;
}

void tf_wasm_delete(TFWasm *self) {
  if (!self) return;
  ts_wasm_store_delete(self->store);
  free(self);
}

TSWasmStore *tf_wasm_take_store(TFWasm *self) {
  if (!self) return NULL;
  TSWasmStore *store = self->store;
  self->store = NULL;
  return store;
}

TSLexer *tf_wasm_lexer(const TFWasm *self) {
  return self->target;
}

bool tf_wasm_start(TFWasm *self, TSLexer *lexer, const TSLanguage *language, bool replay) {
  self->target = lexer;
  return replay || ts_wasm_store_start(self->store, &self->proxy.data, language);
}

void tf_wasm_finish(TFWasm *self, TSLexer *previous) {
  self->target = previous;
  if (!previous) ts_wasm_store_reset(self->store);
}

bool tf_wasm_has_error(const TFWasm *self) {
  return self && ts_wasm_store_has_error(self->store);
}

static void tf_wasm__prepare(TFWasm *self) {
  self->proxy.data.lookahead = self->target->lookahead;
  self->proxy.data.result_symbol = self->target->result_symbol;
}

static void tf_wasm__finish_lex(TFWasm *self) {
  self->target->lookahead = self->proxy.data.lookahead;
  self->target->result_symbol = self->proxy.data.result_symbol;
}

bool tf_wasm_lex(TFWasm *self, TSStateId state, bool keyword) {
  tf_wasm__prepare(self);
  bool found = keyword ? ts_wasm_store_call_lex_keyword(self->store, state)
                       : ts_wasm_store_call_lex_main(self->store, state);
  tf_wasm__finish_lex(self);
  return found;
}

uint32_t tf_wasm_scanner_create(TFWasm *self) {
  return ts_wasm_store_call_scanner_create(self->store);
}

void tf_wasm_scanner_destroy(TFWasm *self, uint32_t address) {
  ts_wasm_store_call_scanner_destroy(self->store, address);
}

bool tf_wasm_scanner_scan(TFWasm *self, uint32_t address, uint32_t valid_tokens_index) {
  tf_wasm__prepare(self);
  bool found = ts_wasm_store_call_scanner_scan(self->store, address, valid_tokens_index);
  tf_wasm__finish_lex(self);
  return found;
}

uint32_t tf_wasm_scanner_serialize(TFWasm *self, uint32_t address, char *buffer) {
  uint32_t length = ts_wasm_store_call_scanner_serialize(self->store, address,
                                                       self->proxy.debug_buffer);
  if (length) memcpy(buffer, self->proxy.debug_buffer, length);
  return length;
}

void tf_wasm_scanner_deserialize(TFWasm *self, uint32_t address, const char *buffer, uint32_t length) {
  ts_wasm_store_call_scanner_deserialize(self->store, address, buffer, length);
}
