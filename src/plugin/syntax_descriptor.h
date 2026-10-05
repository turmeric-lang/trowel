#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace trowel {

// Style IDs for data-driven syntax plugins. These are fixed offsets from a
// per-plugin base, allocated by the SyntaxRegistry when a syntax plugin is
// loaded. The base style is always the Default.
//
// The data-driven lexer supports these token classes:
//   Default, Keyword, Comment, String, Number, Operator, Identifier,
//   Preprocessor, Type, Function, Variable, Constant, Error
//
// A syntax descriptor maps its token classes to these style slots. Not all
// slots need to be used; unused slots simply never get painted.
enum class PluginSyntaxStyle : int {
    Default = 0,
    Keyword,
    Comment,
    String,
    Number,
    Operator,
    Identifier,
    Preprocessor,
    Type,
    Function,
    Variable,
    Constant,
    Error,
    Count,
};

// A syntax descriptor loaded from a `syntax.tur` file. The host reads this
// from Turmeric and builds a data-driven Scintilla lexer from it.
struct SyntaxDescriptor {
    QString name;           // e.g. "json", "toml"
    QStringList extensions; // e.g. ["json", "json5"]

    // Keyword sets. Each list is a set of words that get the Keyword style.
    QStringList keywords;

    // Comment delimiters.
    QString lineComment;    // e.g. "#" or "//"
    QString blockCommentOpen;   // e.g. "/*"
    QString blockCommentClose;  // e.g. "*/"

    // String delimiters.
    QString stringDelim;     // e.g. "\"" (default: double-quote)
    QString stringEscape;    // e.g. "\\" (default: backslash)

    // Operators (single-char list, e.g. "{}[](),;:+-*/<>=&|!?")
    QString operators;

    // Number prefix characters (e.g. "0x" for hex, "-" for negative).
    // For the data-driven lexer, numbers are simply digit sequences.
    bool hasNumbers = true;

    // Fold-open and fold-close characters (for bracket-based folding).
    QString foldOpen;        // e.g. "{["
    QString foldClose;       // e.g. "}]"
};

// Registry of loaded syntax descriptors, keyed by name. The PluginHost
// populates this at startup by scanning ~/.trowel/syntax/<name>/syntax.tur.
class SyntaxRegistry {
public:
    // Register a descriptor. Returns the index assigned to it.
    int registerDescriptor(const SyntaxDescriptor& desc);

    // Find a descriptor by name.
    const SyntaxDescriptor* find(const QString& name) const;

    // Find a descriptor by file extension (without the dot).
    const SyntaxDescriptor* findByExtension(const QString& ext) const;

    // All registered descriptors.
    const QVector<SyntaxDescriptor>& descriptors() const { return descriptors_; }

    // The index for a name, or -1 if not found.
    int index(const QString& name) const;

private:
    QVector<SyntaxDescriptor> descriptors_;
    QHash<QString, int> nameIndex_;
};

}  // namespace trowel
