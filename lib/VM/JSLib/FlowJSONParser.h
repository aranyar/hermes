/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_FLOWJSONPARSER_H
#define HERMES_VM_JSLIB_FLOWJSONPARSER_H

#include "JSONLexer.h"

#include <functional>
#include <string>

#include "llvh/ADT/SmallVector.h"

namespace hermes {
namespace vm {

class JSArray;

/// Incremental (flow) JSON parser. Input is fed in chunks as it arrives
/// (e.g. from the network), and parsing advances as far as the available data
/// allows.
///
/// Non-recursive: the currently open arrays/objects are kept on an explicit
/// stack; each new lexer token either continues or closes the frames on top
/// of the stack. A token that is incomplete at the end of the current input
/// ("truncated") is not an error: the lexer rolls back to the token start and
/// parsing suspends until more data is fed.
///
/// GC rooting: the parser object itself holds NO GC handles (it can be
/// created/destroyed at arbitrary points, e.g. by a GC finalizer). All
/// in-flight containers and pending object keys live in a caller-supplied
/// "roots" JS array which the caller keeps rooted between feed() calls:
///   roots[0]      - completed root value (default mode only)
///   roots[1+2i]   - container of frame i (undefined for the flow-mode root)
///   roots[2+2i]   - pending object key of frame i (string, or undefined)
/// All handles used during a feed are created in a feed-local GCScope.
///
/// Two modes:
///  - default: the whole document is built; the result lands in roots[0].
///  - flow mode (elementCallback != nullptr): the root MUST be a JSON array;
///    each completed element is reported through the callback instead of
///    being accumulated, bounding memory for large list payloads.
class FlowJSONParser {
 public:
  enum class Status {
    /// All complete input consumed; waiting for more data.
    NeedMoreData,
    /// The document is complete and fully consumed (only after a final feed).
    Done,
    /// Malformed input; a SyntaxError is pending in the runtime.
    Error,
  };

  /// Invoked with each completed element of the root array (flow mode).
  /// The handle is valid only for the duration of the call.
  using ElementCallback = std::function<void(Handle<HermesValue>)>;

  explicit FlowJSONParser(
      Runtime &runtime,
      ElementCallback elementCallback = nullptr);

  /// Feed a chunk of UTF-8 text; incomplete multi-byte sequences at the chunk
  /// boundary are carried over to the next feed. [roots] is the GC-rooted
  /// array described above; the same array must be passed to every feed.
  /// When [isFinal] is true the input is complete: a truncated token or an
  /// unfinished document is a syntax error.
  /// \return EXCEPTION with a pending SyntaxError on malformed input
  /// (status() == Status::Error), RETURNED otherwise.
  LLVM_NODISCARD ExecutionStatus feed(
      llvh::ArrayRef<uint8_t> utf8,
      bool isFinal,
      Handle<JSArray> roots);

  /// Same as feed(), but the chunk is already UTF-16 (e.g. a JS string).
  LLVM_NODISCARD ExecutionStatus feedUtf16(
      llvh::ArrayRef<char16_t> input,
      bool isFinal,
      Handle<JSArray> roots);

  Status status() const {
    return status_;
  }

  /// Whether this parser was created with an element callback (root array
  /// elements are streamed to the callback instead of being accumulated).
  bool flowMode() const {
    return flowMode_;
  }

  /// The parsed root value (default mode). \pre status() == Status::Done.
  /// Read from roots[0]; only valid while [roots] stays rooted.
  HermesValue getRootValue(Handle<JSArray> roots) const;

 private:
  /// Grammar state within the top-of-stack container.
  enum class Expect {
    /// Array: just opened — first value or ']'.
    ValueOrEnd,
    /// Array: after ',' — a value is required.
    Value,
    /// Array/object: after a value — ',' or closing bracket.
    CommaOrEnd,
    /// Object: just opened — first key or '}'.
    KeyOrEnd,
    /// Object: after ',' — a key is required.
    Key,
    /// Object: after a key — ':'.
    Colon,
    /// Object: after ':' — a value is required.
    ObjectValue,
  };

  /// POD grammar state of one open container. The GC-managed container and
  /// pending key are NOT stored here; they live in the roots array (see the
  /// class comment), so the parser object itself owns no GC handles.
  struct Frame {
    uint32_t nextIndex = 0;
    bool isArray;
    Expect expect;
  };

  /// Scratch values that only live for the duration of one feed() call.
  struct LV : public Locals {
    /// Last scalar value / closed container.
    PinnedValue<HermesValue> scalarValue;
    /// Newly created container, before it lands in the roots array.
    PinnedValue<JSObject> newContainer;
    /// Array index / roots slot as a computed key.
    PinnedValue<HermesValue> indexValue;
    /// Pending object key, read back from the roots array.
    PinnedValue<HermesValue> keyValue;
  };

  Runtime &runtime_;

  /// All input fed so far, decoded to UTF-16. The consumed prefix is
  /// discarded periodically (see kPrefixDiscardThreshold).
  std::basic_string<char16_t> buf_;

  /// Carried-over tail bytes of an incomplete UTF-8 sequence (0-3 bytes).
  std::string utf8Tail_;

  /// Offset in [buf_] of the start of the first unconsumed token. The lexer
  /// is positioned at this offset on every feed (the buffer may have moved).
  uint32_t committedOffset_ = 0;

  llvh::SmallVector<Frame, 16> stack_;

  /// Root value completed (in flow mode: the root array closed).
  bool rootDone_ = false;

  Status status_ = Status::NeedMoreData;

  const bool flowMode_;

  ElementCallback elementCallback_;

  /// When the consumed prefix exceeds this, it is discarded from [buf_].
  static constexpr uint32_t kPrefixDiscardThreshold = 64 * 1024;

  /// Mirrors RuntimeJSONParser::MAX_RECURSION_DEPTH.
  static constexpr uint32_t kMaxDepth = 512;

  /// roots[0] holds the completed root value in default mode.
  static constexpr uint32_t kRootValueSlot = 0;
  static constexpr uint32_t containerSlot(uint32_t frameIndex) {
    return 1 + 2 * frameIndex;
  }
  static constexpr uint32_t keySlot(uint32_t frameIndex) {
    return 2 + 2 * frameIndex;
  }

  /// Append [utf8] (decoded to UTF-16) to [buf_], carrying over an incomplete
  /// UTF-8 tail sequence to the next feed.
  ExecutionStatus appendUtf8(llvh::ArrayRef<uint8_t> utf8, bool isFinal);

  /// Shared tail of feed()/feedUtf16(): discard the consumed prefix, position
  /// the lexer and parse as far as the input allows.
  ExecutionStatus parseFromBuffer(bool isFinal, Handle<JSArray> roots);

  /// Parse as far as the current input allows.
  ExecutionStatus parseRound(
      bool isFinal,
      FlowJSONLexer &lexer,
      Handle<JSArray> roots,
      LV &lv);

  /// Apply the grammar action for the current token.
  ExecutionStatus processToken(
      JSONTokenKind kind,
      FlowJSONLexer &lexer,
      Handle<JSArray> roots,
      LV &lv);

  /// Attach a completed value to the top-of-stack container, report it as a
  /// root-array element (flow mode), or complete the root value.
  ExecutionStatus valueComplete(
      Handle<HermesValue> value,
      Handle<JSArray> roots,
      LV &lv);

  /// Write [value] into roots[slot] (growing the array as needed).
  ExecutionStatus setRootsSlot(
      Handle<JSArray> roots,
      uint32_t slot,
      Handle<HermesValue> value,
      LV &lv);

  /// The container of frame [frameIndex] as a fresh handle (undefined for the
  /// flow-mode root frame — callers must not call it there).
  Handle<JSObject> containerAt(Handle<JSArray> roots, uint32_t frameIndex);

  bool expectingValue() const;
  bool expectingKey() const;
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSLIB_FLOWJSONPARSER_H
