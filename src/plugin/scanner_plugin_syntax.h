#pragma once

#include "editor/lexers.h"
#include "editor/scanner.h"
#include "syntax_descriptor.h"

namespace trowel {

// Scan one line using a data-driven syntax descriptor. This is the lexer
// for Language::PluginSyntax — it reads keyword lists, comment delimiters,
// string delimiters, and operators from the SyntaxDescriptor and paints
// tokens accordingly.
//
// The descriptor index is stored in the LexState's leaf-language bit field
// (the same slot used by jsonDepth etc.), so the scanner can recover it
// across lines. The host sets the descriptor index before the first Lex call
// by storing it in the line state.
void ScanPluginSyntaxLine(const SyntaxDescriptor& desc,
                          const ScanInput& in, LexState& st, Emitter& out);

}  // namespace trowel
