#ifndef TF_WASM_H
#define TF_WASM_H

#include "tree_sitter/api.h"
#include "tree_feller.h"

typedef struct TFWasm TFWasm;

TFWasm *tf_wasm_new(TSWasmStore *store);
void tf_wasm_delete(TFWasm *self);
TSWasmStore *tf_wasm_take_store(TFWasm *self);
// Replays borrow the active store without resetting its heap. Each run creates
// its own scanner; restoring the target leaves the outer scanner alive.
bool tf_wasm_start(TFWasm *self, TSLexer *lexer, const TSLanguage *language, bool replay);
TSLexer *tf_wasm_lexer(const TFWasm *self);
void tf_wasm_finish(TFWasm *self, TSLexer *previous);
bool tf_wasm_has_error(const TFWasm *self);
bool tf_wasm_lex(TFWasm *self, TSStateId state, bool keyword);
uint32_t tf_wasm_scanner_create(TFWasm *self);
void tf_wasm_scanner_destroy(TFWasm *self, uint32_t address);
bool tf_wasm_scanner_scan(TFWasm *self, uint32_t address, uint32_t valid_tokens_index);
uint32_t tf_wasm_scanner_serialize(TFWasm *self, uint32_t address, char *buffer);
void tf_wasm_scanner_deserialize(TFWasm *self, uint32_t address, const char *buffer, uint32_t length);

#endif
