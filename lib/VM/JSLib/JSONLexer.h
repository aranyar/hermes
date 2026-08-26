/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_PARSER_JSONLEXER_H
#define HERMES_PARSER_JSONLEXER_H

#include "hermes/Support/JenkinsHash.h"
#include "hermes/Support/UTF16Stream.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"
#include "hermes/VM/TwineChar16.h"

#include "llvh/ADT/Optional.h"
#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SmallVector.h"

#include "dtoa/dtoa.h"

namespace hermes {
namespace vm {

class Runtime;

enum class JSONTokenKind {
  Number,
  String,
  True,
  False,
  Null,
  LBrace,
  RBrace,
  LSquare,
  RSquare,
  Comma,
  Colon,
  Eof,
  None
};

/// Encapsulates the information contained in the current token.
/// We only ever create one of these, but it is cleaner to keep the data
/// in a separate class.
class JSONToken {
  JSONTokenKind kind_{JSONTokenKind::None};
  double numberValue_{};

  /// The starting character of this token.
  char16_t firstChar_{};

  JSONToken(const JSONToken &) = delete;
  const JSONToken &operator=(const JSONToken &) = delete;

  struct : public Locals {
    PinnedValue<StringPrimitive> stringValue;
    PinnedValue<SymbolID> symbolValue;
  } lv_;
  LocalsRAII lraii_;

 public:
  explicit JSONToken(Runtime &runtime) : lraii_(runtime, &lv_) {}

  JSONTokenKind getKind() const {
    return kind_;
  }

  double getNumber() const {
    assert(getKind() == JSONTokenKind::Number);
    return numberValue_;
  }

  Handle<StringPrimitive> getStrAsPrim() const {
    assert(getKind() == JSONTokenKind::String);
    return lv_.stringValue;
  }

  Handle<SymbolID> getStrAsSymbol() const {
    assert(getKind() == JSONTokenKind::String);
    return lv_.symbolValue;
  }

  char16_t getFirstChar() const {
    return firstChar_;
  }
  void setFirstChar(char16_t firstChar) {
    firstChar_ = firstChar;
  }

  void setPunctuator(JSONTokenKind kind) {
    kind_ = kind;
  }
  void setEof() {
    kind_ = JSONTokenKind::Eof;
    firstChar_ = 0;
  }
  void invalidate() {
    kind_ = JSONTokenKind::None;
  }

  void setNumber(double number) {
    kind_ = JSONTokenKind::Number;
    numberValue_ = number;
  }
  void setString(StringPrimitive *str) {
    kind_ = JSONTokenKind::String;
    lv_.stringValue = str;
  }
  void setSymbol(SymbolID sym) {
    kind_ = JSONTokenKind::String;
    lv_.symbolValue = sym;
  }
};

/// JSON lexer. When [AllowPartial] is true (used by incremental/flow
/// parsing), hitting the end of input in the middle of a token is not an
/// error: the token is invalidated and tokenTruncated() reports it, so the
/// caller can roll back to the position saved before the advance and wait
/// for more input. The [AllowPartial] == false instantiation is
/// byte-for-byte the original behavior — all truncation handling is
/// compile-time (if constexpr).
template <bool AllowPartial>
class JSONLexerT {
 private:
  UTF16Stream curCharPtr_;

  Runtime &runtime_;

  JSONToken token_;

  /// Set by the last advance() call if it stopped in the middle of a token
  /// because the input ended (only when [AllowPartial] is true).
  bool tokenTruncated_{false};

  /// When true, the input is complete: tokens ending exactly at end-of-input
  /// are finalized (a number) or are genuine syntax errors (a truncated
  /// string/keyword/escape), not rollback points. Set by the incremental
  /// parser on the final feed.
  bool finalInput_{false};

  /// Absolute char16 offset of the start of the current stream. The stream
  /// is re-created over a subspan of the input by resetInput(); this keeps
  /// tell() absolute.
  uint32_t baseOffset_{0};

  using StrAsSymbol = std::true_type;
  using StrAsValue = std::false_type;

 public:
  JSONLexerT(Runtime &runtime, UTF16Stream &&stream)
      : curCharPtr_(std::move(stream)), runtime_(runtime), token_(runtime) {}

  /// \return the current token.
  const JSONToken *getCurToken() const {
    assert(
        token_.getKind() != JSONTokenKind::None &&
        "Obtaining an invalid token");
    return &token_;
  }

  /// The current token kind. Unlike getCurToken(), may be called when the
  /// token is invalid (None) — e.g. after a truncated token in partial mode.
  JSONTokenKind getCurTokenKind() const {
    return token_.getKind();
  }

  /// Whether the last advance() stopped in the middle of a token because the
  /// input ended (only possible when [AllowPartial] is true). The lexer
  /// position is past the token start; the caller should roll back with
  /// resetInput() to the position saved before the advance.
  bool tokenTruncated() const {
    return tokenTruncated_;
  }

  /// See [finalInput_]. Meaningful only when [AllowPartial] is true.
  void setFinalInput(bool isFinal) {
    finalInput_ = isFinal;
  }

  /// Current absolute position in the input, in char16 units.
  /// \pre the lexer reads UTF-16 input (not UTF-8).
  uint32_t tell() {
    return baseOffset_ + curCharPtr_.tell();
  }

  /// Reset the input to \p input positioned at absolute \p offset (in char16
  /// units) and invalidate the current token. Used by incremental parsing
  /// when the backing buffer grew or when rolling back a truncated token.
  void resetInput(llvh::ArrayRef<char16_t> input, uint32_t offset) {
    assert(offset <= input.size());
    curCharPtr_ = UTF16Stream(input.drop_front(offset));
    baseOffset_ = offset;
    tokenTruncated_ = false;
    token_.invalidate();
  }

  /// Whether the stream is fully consumed. Note: in partial mode a Number
  /// token that reaches the end of input is reported as truncated, since more
  /// digits could follow.
  bool atEnd() {
    return !curCharPtr_.hasChar();
  }

  /// Scan the next token, and store it in token_.
  /// \return Exception if error occurs.
  /// All whitespace is skipped before the new token.
  LLVM_NODISCARD ExecutionStatus advance() {
    return advanceHelper(false);
  }

  /// Same as advance, except if a string is encountered, it will parse it into
  /// the current token's symbol field.
  LLVM_NODISCARD ExecutionStatus advanceStrAsSymbol() {
    return advanceHelper(true);
  }

  /// Raise a JSON parse exception with message \p msg.
  /// token_ will also be invalidated.
  LLVM_NODISCARD ExecutionStatus error(const TwineChar16 &msg) {
    token_.invalidate();
    return runtime_.raiseSyntaxError(
        TwineChar16("JSON Parse error: ").concat(msg));
  }

  /// Report a JSON parse exception with message \p msg,
  /// at the character \p ch.
  LLVM_NODISCARD ExecutionStatus
  errorWithChar(const TwineChar16 &msg, char16_t ch) {
    return error(msg.concat(UTF16Ref(&ch, 1)));
  }

  /// Raise a JSON parse unexpected character error.
  LLVM_NODISCARD ExecutionStatus errorUnexpectedChar() {
    if (token_.getKind() == JSONTokenKind::Eof) {
      return error("Unexpected end of input");
    }
    return errorWithChar(
        u"Unexpected character: ", getCurToken()->getFirstChar());
  }

 private:
  static bool isJSONWhiteSpace(char16_t ch) {
    // JSONWhiteSpace includes <TAB>, <CR>, <LF>, <SP>.
    return (ch == u'\t' || ch == u'\r' || ch == u'\n' || ch == u' ');
  }

  /// Mark the in-progress token as truncated by end-of-input (partial mode).
  void truncated() {
    static_assert(AllowPartial, "truncated() is only for partial mode");
    tokenTruncated_ = true;
    token_.invalidate();
  }

  /// Advance the lexer by a single token. The parameter forKey determines how
  /// strings are stored in the lexer.
  LLVM_NODISCARD ExecutionStatus advanceHelper(bool forKey) {
    if constexpr (AllowPartial) {
      tokenTruncated_ = false;
    }

    // Skip whitespaces.
    while (curCharPtr_.hasChar() && isJSONWhiteSpace(*curCharPtr_)) {
      ++curCharPtr_;
    }

    // End of buffer.
    if (!curCharPtr_.hasChar()) {
      token_.setEof();
      return ExecutionStatus::RETURNED;
    }

    token_.setFirstChar(*curCharPtr_);

#define PUNC(ch, tok)          \
  case ch:                     \
    token_.setPunctuator(tok); \
    ++curCharPtr_;             \
    return ExecutionStatus::RETURNED

#define WORD(ch, word, tok) \
  case ch:                  \
    return scanWord(word, tok)

    switch (*curCharPtr_) {
      PUNC(u'{', JSONTokenKind::LBrace);
      PUNC(u'}', JSONTokenKind::RBrace);
      PUNC(u'[', JSONTokenKind::LSquare);
      PUNC(u']', JSONTokenKind::RSquare);
      PUNC(u',', JSONTokenKind::Comma);
      PUNC(u':', JSONTokenKind::Colon);
      WORD(u't', "true", JSONTokenKind::True);
      WORD(u'f', "false", JSONTokenKind::False);
      WORD(u'n', "null", JSONTokenKind::Null);

      case u'-':
      case u'0':
      case u'1':
      case u'2':
      case u'3':
      case u'4':
      case u'5':
      case u'6':
      case u'7':
      case u'8':
      case u'9':
        return scanNumber();

      case u'"':
        if (forKey) {
          return scanString<StrAsSymbol>();
        } else {
          return scanString<StrAsValue>();
        }

      default:
        return errorWithChar(u"Unexpected character: ", *curCharPtr_);
    }
#undef PUNC
#undef WORD
  }

  /// Parse a unicode code point and \return the char16 value.
  /// On error, \return llvh::None.
  CallResult<char16_t> consumeUnicode() {
    uint16_t val = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (!curCharPtr_.hasChar()) {
        if constexpr (AllowPartial) {
          if (!finalInput_) {
            // Truncated \uXXXX escape: signal truncation instead of raising.
            truncated();
            return static_cast<char16_t>(0);
          }
        }
        return error("Unexpected end of input");
      }
      int ch = *curCharPtr_ | 32;
      if (ch >= '0' && ch <= '9') {
        ch -= '0';
      } else if (ch >= 'a' && ch <= 'f') {
        ch -= 'a' - 10;
      } else {
        return errorWithChar(u"Invalid unicode point character: ", *curCharPtr_);
      }
      val = (val << 4) + ch;
      ++curCharPtr_;
    }

    return static_cast<char16_t>(val);
  }

  /// Parse a JSONNumber.
  LLVM_NODISCARD ExecutionStatus scanNumber() {
    llvh::SmallVector<char, 32> str8;
    while (curCharPtr_.hasChar()) {
      auto ch = *curCharPtr_;
      if (!(ch == u'-' || ch == u'+' || ch == u'.' || (ch | 32) == u'e' ||
            (ch >= u'0' && ch <= u'9'))) {
        break;
      }
      str8.push_back(ch);
      ++curCharPtr_;
    }

    if constexpr (AllowPartial) {
      // A number at the end of the input may continue when more data
      // arrives; treat it as truncated unless the input is final.
      if (!finalInput_ && !curCharPtr_.hasChar()) {
        truncated();
        return ExecutionStatus::RETURNED;
      }
    }

    size_t len = str8.size();
    assert(len > 0 && "scanNumber must be called on a number-looking char");
    if (str8[0] == '0' && len > 1 && str8[1] >= '0' && str8[1] <= '9') {
      // The integer part cannot start with 0, unless it's 0.
      return errorWithChar(u"Unexpected character in number: ", str8[1]);
    }

    str8.push_back('\0');

    char *endPtr;
    double value = ::hermes_g_strtod(str8.data(), &endPtr);
    if (endPtr != str8.data() + len) {
      return errorWithChar(u"Unexpected character in number: ", *endPtr);
    }
    token_.setNumber(value);
    return ExecutionStatus::RETURNED;
  }

  /// Parse a JSONString. If ForKey is std::true_type, then the string will be
  /// parsed into a symbol. If ForKey is std::false_type, the scanned string
  /// will be turned into a new StringPrimitive.
  template <typename ForKey>
  LLVM_NODISCARD ExecutionStatus scanString() {
    assert(*curCharPtr_ == '"');
    ++curCharPtr_;
    bool hasEscape = false;
    // Ideally we don't have to use tmpStorage. In the case of a plain string
    // with no escapes, we construct an ArrayRef at the end of scanning that
    // points to the beginning and end of the string.
    SmallU16String<32> tmpStorage;
    curCharPtr_.beginCapture();
    // Make sure we don't somehow leave a dangling open capture.
    auto ensureCaptureClosed =
        llvh::make_scope_exit([this] { curCharPtr_.cancelCapture(); });
    bool allAscii = true;
    hermes::JenkinsHash hash = hermes::JenkinsHashInit;

    while (curCharPtr_.hasChar()) {
      if (*curCharPtr_ == '"') {
        // End of string.
        llvh::ArrayRef<char16_t> strRef =
            hasEscape ? tmpStorage.arrayRef() : curCharPtr_.endCapture();
        ++curCharPtr_;
        if constexpr (ForKey::value) {
          auto symRes = runtime_.getIdentifierTable().getSymbolHandle(
              runtime_, strRef, hash);
          if (symRes == ExecutionStatus::EXCEPTION)
            return ExecutionStatus::EXCEPTION;
          token_.setSymbol(symRes->get());
          return ExecutionStatus::RETURNED;
        }
        auto strRes =
            StringPrimitive::createWithKnownEncoding(runtime_, strRef, allAscii);
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        token_.setString(vmcast<StringPrimitive>(*strRes));
        return ExecutionStatus::RETURNED;
      } else if (*curCharPtr_ <= '\u001F') {
        return error(u"U+0000 thru U+001F is not allowed in string");
      }
      char16_t scannedChar = -1;
      if (*curCharPtr_ == u'\\') {
        if (!hasEscape) {
          // This is the first escape character encountered, so append
          // everything we've seen so far to tmpStorage.
          tmpStorage.append(curCharPtr_.endCapture());
        }
        hasEscape = true;
        ++curCharPtr_;
        if (!curCharPtr_.hasChar()) {
          if constexpr (AllowPartial) {
            if (!finalInput_) {
              truncated();
              return ExecutionStatus::RETURNED;
            }
          }
          return error("Unexpected end of input");
        }
        switch (*curCharPtr_) {
#define CONSUME_VAL(v)     \
  tmpStorage.push_back(v); \
  ++curCharPtr_;

          case u'"':
          case u'/':
          case u'\\':
            CONSUME_VAL(*curCharPtr_)
            break;
          case 'b':
            CONSUME_VAL(8)
            break;
          case 'f':
            CONSUME_VAL(12)
            break;
          case 'n':
            CONSUME_VAL(10)
            break;
          case 'r':
            CONSUME_VAL(13)
            break;
          case 't':
            CONSUME_VAL(9)
            break;
          case 'u': {
            ++curCharPtr_;
            CallResult<char16_t> cr = consumeUnicode();
            if constexpr (AllowPartial) {
              if (tokenTruncated_)
                return ExecutionStatus::RETURNED;
            }
            if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
            tmpStorage.push_back(*cr);
            break;
          }

          default:
            return errorWithChar(u"Invalid escape sequence: ", *curCharPtr_);
        }
        scannedChar = tmpStorage.back();
      } else {
        scannedChar = *curCharPtr_;
        if (hasEscape)
          tmpStorage.push_back(scannedChar);
        ++curCharPtr_;
      }
      if constexpr (ForKey::value) {
        hash = hermes::updateJenkinsHash(hash, scannedChar);
      } else {
        allAscii &= isASCII(scannedChar);
      }
    }
    if constexpr (AllowPartial) {
      if (!finalInput_) {
        truncated();
        return ExecutionStatus::RETURNED;
      }
    }
    return error("Unexpected end of input");
#undef CONSUME_VAL
  }

  /// Parse a reserved keyword.
  LLVM_NODISCARD ExecutionStatus scanWord(const char *word, JSONTokenKind kind) {
    while (*word && curCharPtr_.hasChar()) {
      if (*curCharPtr_ != *word) {
        return errorWithChar(u"Unexpected character: ", *curCharPtr_);
      }
      ++curCharPtr_;
      ++word;
    }
    if (*word) {
      if constexpr (AllowPartial) {
        if (!finalInput_) {
          truncated();
          return ExecutionStatus::RETURNED;
        }
      }
      return error("Unexpected end of input");
    }
    token_.setPunctuator(kind);
    return ExecutionStatus::RETURNED;
  }
};

/// The lexer used by JSON.parse — no partial-token support.
using JSONLexer = JSONLexerT<false>;

/// The lexer used by FlowJSONParser: truncated tokens at end of input are
/// reported via tokenTruncated() instead of raising, so the parser can roll
/// back and wait for more input.
using FlowJSONLexer = JSONLexerT<true>;

} // namespace vm
} // namespace hermes

#endif // HERMES_PARSER_JSONLEXER_H
