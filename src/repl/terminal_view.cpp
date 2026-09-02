#include "repl/terminal_view.h"

#include "editor/theme_loader.h"
#include "repl/pty_session.h"

#include <QKeyEvent>
#include <QScrollBar>
#include <QTextCursor>

#include <algorithm>

namespace trowel {

TerminalView::TerminalView(QWidget* parent)
    : QPlainTextEdit(parent)
{
    applyDefaultStyling();
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setContextMenuPolicy(Qt::NoContextMenu);
    setCursorWidth(2);
}

void TerminalView::applyDefaultStyling() {
    setFont(MonospaceUiFont());
    setStyleSheet(
        "QPlainTextEdit {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  border: none;"
        "  padding: 4px;"
        "}"
    );
}

void TerminalView::attach(PtySession* pty) {
    detach();
    pty_ = pty;
    if (!pty_) return;
    connect(pty_, &PtySession::dataReceived, this, &TerminalView::appendOutput);
    emitResize();
}

void TerminalView::detach() {
    if (pty_) {
        disconnect(pty_, nullptr, this, nullptr);
        pty_ = nullptr;
    }
}

void TerminalView::clearScreen() {
    pending_.clear();
    clear();
    resetFormat();
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TerminalView::showBanner(const QString& text) {
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    cursor.insertText("\n");
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

// Consume ANSI CSI / OSC escapes; emit the remaining printable + control bytes
// (LF, CR, BS, TAB) to the QTextCursor. Colors ignored for now — this is enough
// to keep the REPL display clean without pulling in a full terminal emulator.
void TerminalView::insertProcessedText(const QByteArray& bytes) {
    QByteArray input = pending_ + bytes;
    pending_.clear();

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);

    int i = 0;
    const int n = input.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        if (c == 0x1b) {  // ESC
            if (i + 1 >= n) { pending_ = input.mid(i); break; }
            unsigned char kind = static_cast<unsigned char>(input[i + 1]);
            if (kind == '[') {
                // CSI: ESC [ <params> <final in @-~>. Capture params so we can
                // dispatch SGR (`m`) while silently discarding cursor moves.
                int j = i + 2;
                int paramsStart = j;
                unsigned char finalByte = 0;
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(input[j]);
                    if (b >= 0x40 && b <= 0x7e) { finalByte = b; ++j; break; }
                    ++j;
                }
                if (finalByte == 0) { pending_ = input.mid(i); break; }
                if (finalByte == 'm') {
                    applySgr(input.mid(paramsStart, j - paramsStart - 1));
                }
                i = j;
                continue;
            } else if (kind == ']') {
                // OSC: ESC ] ... BEL or ST(ESC \)
                int j = i + 2;
                bool done = false;
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(input[j]);
                    if (b == 0x07) { ++j; done = true; break; }
                    if (b == 0x1b && j + 1 < n && input[j + 1] == '\\') { j += 2; done = true; break; }
                    ++j;
                }
                if (!done) { pending_ = input.mid(i); break; }
                i = j;
                continue;
            } else {
                // Two-byte ESC sequence — skip both.
                i += 2;
                continue;
            }
        }

        if (c == '\r') {
            // Coalesce CRLF into a single newline; lone CR moves to column 0.
            if (i + 1 < n && input[i + 1] == '\n') {
                cursor.movePosition(QTextCursor::End);
                cursor.insertBlock();
                i += 2;
                continue;
            }
            cursor.movePosition(QTextCursor::StartOfLine);
            ++i;
            continue;
        }
        if (c == '\n') {
            cursor.movePosition(QTextCursor::End);
            cursor.insertBlock();
            ++i;
            continue;
        }
        if (c == '\b') {
            cursor.deletePreviousChar();
            ++i;
            continue;
        }
        if (c == 0x07) { // BEL
            ++i;
            continue;
        }
        if (c == '\t' || c >= 0x20) {
            // Collect a run of plain bytes for fewer cursor ops.
            int j = i + 1;
            while (j < n) {
                unsigned char d = static_cast<unsigned char>(input[j]);
                if (d == 0x1b || d == '\r' || d == '\n' || d == '\b' || d == 0x07) break;
                if (d < 0x20 && d != '\t') break;
                ++j;
            }
            cursor.insertText(QString::fromUtf8(input.mid(i, j - i)), currentFormat_);
            i = j;
            continue;
        }
        // Unknown control byte — drop.
        ++i;
    }

    setTextCursor(cursor);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TerminalView::appendOutput(const QByteArray& bytes) {
    insertProcessedText(bytes);
}

QString TerminalView::screenText(int lastLines) const {
    const QString all = toPlainText();
    if (lastLines <= 0) return all;
    int idx = all.size();
    int lines = 0;
    while (idx > 0 && lines < lastLines) {
        --idx;
        if (all[idx] == QChar('\n')) {
            ++lines;
            if (lines == lastLines) { ++idx; break; }
        }
    }
    return all.mid(idx);
}

std::pair<int, int> TerminalView::screenCursor() const {
    QTextCursor c = textCursor();
    return {c.blockNumber(), c.positionInBlock()};
}

void TerminalView::keyPressEvent(QKeyEvent* event) {
    if (!pty_ || !pty_->isRunning()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    // Copy shortcut — let the widget handle it.
    if (event->matches(QKeySequence::Copy)) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    // Paste — forward text to PTY.
    if (event->matches(QKeySequence::Paste)) {
        // Let default paste flow do nothing; write clipboard through PTY.
        // For M2, defer proper bracketed-paste — just forward directly.
        // Fall through to write() with the event text below.
    }

    QByteArray toWrite;
    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            toWrite = "\r";
            break;
        case Qt::Key_Backspace:
            toWrite = "\x7f";
            break;
        case Qt::Key_Tab:
            toWrite = "\t";
            break;
        case Qt::Key_Escape:
            toWrite = "\x1b";
            break;
        case Qt::Key_Up:    toWrite = "\x1b[A"; break;
        case Qt::Key_Down:  toWrite = "\x1b[B"; break;
        case Qt::Key_Right: toWrite = "\x1b[C"; break;
        case Qt::Key_Left:  toWrite = "\x1b[D"; break;
        case Qt::Key_Home:  toWrite = "\x1b[H"; break;
        case Qt::Key_End:   toWrite = "\x1b[F"; break;
        case Qt::Key_Delete: toWrite = "\x1b[3~"; break;
        default:
            if (event->modifiers() & Qt::ControlModifier) {
                const int k = event->key();
                if (k >= Qt::Key_A && k <= Qt::Key_Z) {
                    toWrite.append(char(k - Qt::Key_A + 1));
                }
            }
            if (toWrite.isEmpty()) {
                toWrite = event->text().toUtf8();
            }
            break;
    }

    if (!toWrite.isEmpty()) {
        pty_->write(toWrite);
    }
    // Do not chain to QPlainTextEdit::keyPressEvent — the PTY will echo back.
}

void TerminalView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    emitResize();
}

void TerminalView::emitResize() {
    if (!pty_) return;
    QFontMetrics fm(font());
    const int colWidth = fm.horizontalAdvance('M');
    const int rowHeight = fm.lineSpacing();
    if (colWidth <= 0 || rowHeight <= 0) return;
    const int cols = qMax(20, viewport()->width() / colWidth);
    const int rows = qMax(4, viewport()->height() / rowHeight);
    pty_->resize(rows, cols);
}

void TerminalView::setTerminalPalette(const QColor& defaultFg, const QColor& defaultBg,
                                      const std::array<QColor, 16>& ansi) {
    defaultFg_ = defaultFg;
    defaultBg_ = defaultBg;
    ansi_ = ansi;
    resetFormat();
}

void TerminalView::resetFormat() {
    boldOn_ = false;
    italicOn_ = false;
    underlineOn_ = false;
    reverseOn_ = false;
    fgIsDefault_ = true;
    bgIsDefault_ = true;
    fgColor_ = defaultFg_;
    bgColor_ = defaultBg_;
    currentFormat_ = QTextCharFormat();
    if (defaultFg_.isValid()) currentFormat_.setForeground(defaultFg_);
    // Leave background transparent so the widget's own background shows
    // through unless SGR explicitly requests a bg color.
}

// xterm 256-color: 0-15 = ANSI palette, 16-231 = 6x6x6 cube, 232-255 = grayscale.
QColor TerminalView::xtermColor(int index) const {
    if (index < 0) return {};
    if (index < 16) return ansi_[index];
    if (index < 232) {
        const int base = index - 16;
        const int r = base / 36;
        const int g = (base / 6) % 6;
        const int b = base % 6;
        auto scale = [](int v) { return v == 0 ? 0 : v * 40 + 55; };
        return QColor(scale(r), scale(g), scale(b));
    }
    if (index < 256) {
        const int level = (index - 232) * 10 + 8;
        return QColor(level, level, level);
    }
    return {};
}

void TerminalView::applySgr(const QByteArray& params) {
    // Empty params (bare "ESC [ m") is equivalent to "0" — reset.
    if (params.isEmpty()) {
        resetFormat();
        return;
    }

    // Split on ';' into integer parameters. Non-numeric segments are treated
    // as zero (the ECMA-48 default).
    QList<int> ps;
    ps.reserve(8);
    int cur = 0;
    bool haveDigit = false;
    for (char c : params) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            haveDigit = true;
        } else if (c == ';') {
            ps.append(haveDigit ? cur : 0);
            cur = 0;
            haveDigit = false;
        }
    }
    ps.append(haveDigit ? cur : 0);

    auto applyFmt = [this]() {
        currentFormat_ = QTextCharFormat();
        QColor fg = fgIsDefault_ ? defaultFg_ : fgColor_;
        QColor bg = bgIsDefault_ ? QColor() : bgColor_;
        if (reverseOn_) std::swap(fg, bg);
        if (fg.isValid()) currentFormat_.setForeground(fg);
        if (bg.isValid()) currentFormat_.setBackground(bg);
        if (boldOn_) currentFormat_.setFontWeight(QFont::Bold);
        currentFormat_.setFontItalic(italicOn_);
        currentFormat_.setFontUnderline(underlineOn_);
    };

    for (int i = 0; i < ps.size(); ++i) {
        const int p = ps[i];
        // 38 and 48 consume subsequent parameters.
        if (p == 38 || p == 48) {
            const bool isFg = (p == 38);
            if (i + 1 >= ps.size()) break;
            const int mode = ps[++i];
            QColor c;
            if (mode == 5 && i + 1 < ps.size()) {          // 256-color index
                c = xtermColor(ps[++i]);
            } else if (mode == 2 && i + 3 < ps.size()) {   // truecolor r;g;b
                const int r = ps[++i], g = ps[++i], b = ps[++i];
                c = QColor(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
            } else {
                continue;
            }
            if (isFg) { fgColor_ = c; fgIsDefault_ = !c.isValid(); }
            else      { bgColor_ = c; bgIsDefault_ = !c.isValid(); }
            continue;
        }
        switch (p) {
            case 0:                                        // reset
                resetFormat();
                continue;
            case 1: boldOn_ = true; break;
            case 3: italicOn_ = true; break;
            case 4: underlineOn_ = true; break;
            case 7: reverseOn_ = true; break;
            case 22: boldOn_ = false; break;
            case 23: italicOn_ = false; break;
            case 24: underlineOn_ = false; break;
            case 27: reverseOn_ = false; break;
            case 39: fgIsDefault_ = true; break;
            case 49: bgIsDefault_ = true; break;
            default:
                if (p >= 30 && p <= 37) {
                    fgColor_ = ansi_[p - 30];
                    fgIsDefault_ = false;
                } else if (p >= 40 && p <= 47) {
                    bgColor_ = ansi_[p - 40];
                    bgIsDefault_ = false;
                } else if (p >= 90 && p <= 97) {
                    fgColor_ = ansi_[8 + (p - 90)];
                    fgIsDefault_ = false;
                } else if (p >= 100 && p <= 107) {
                    bgColor_ = ansi_[8 + (p - 100)];
                    bgIsDefault_ = false;
                }
                // Other SGR params (blink, strike, etc.) — silently ignore.
                break;
        }
    }
    applyFmt();
}

}
