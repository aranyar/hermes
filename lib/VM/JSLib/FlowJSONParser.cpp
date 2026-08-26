/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FlowJSONParser.h"

#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"

#include "llvh/Support/ConvertUTF.h"

namespace hermes {
namespace vm {

FlowJSONParser::FlowJSONParser(Runtime &runtime, ElementCallback elementCallback)
    : runtime_(runtime),
      flowMode_(elementCallback != nullptr),
      elementCallback_(std::move(elementCallback)) {}

HermesValue FlowJSONParser::getRootValue(Handle<JSArray> roots) const {
  return roots->at(runtime_, kRootValueSlot).unboxToHV(runtime_);
}

Handle<JSObject> FlowJSONParser::containerAt(
    Handle<JSArray> roots,
    uint32_t frameIndex) {
  return runtime_.makeHandle<JSObject>(
      roots->at(runtime_, containerSlot(frameIndex)).unboxToHV(runtime_));
}

ExecutionStatus FlowJSONParser::feed(
    llvh::ArrayRef<uint8_t> utf8,
    bool isFinal,
    Handle<JSArray> roots) {
  if (status_ == Status::Error || status_ == Status::Done) {
    return runtime_.raiseSyntaxError(
        TwineChar16("JSON Parse error: parser is finished"));
  }
  if (appendUtf8(utf8, isFinal) == ExecutionStatus::EXCEPTION) {
    status_ = Status::Error;
    return ExecutionStatus::EXCEPTION;
  }
  return parseFromBuffer(isFinal, roots);
}

ExecutionStatus FlowJSONParser::feedUtf16(
    llvh::ArrayRef<char16_t> input,
    bool isFinal,
    Handle<JSArray> roots) {
  if (status_ == Status::Error || status_ == Status::Done) {
    return runtime_.raiseSyntaxError(
        TwineChar16("JSON Parse error: parser is finished"));
  }
  buf_.append(input.begin(), input.end());
  return parseFromBuffer(isFinal, roots);
}

ExecutionStatus FlowJSONParser::parseFromBuffer(
    bool isFinal,
    Handle<JSArray> roots) {
  GCScope gcScope(runtime_);
  LV lv;
  LocalsRAII lraii(runtime_, &lv);

  // Discard the fully-consumed prefix to bound memory on large payloads.
  // Suspension offsets are always token boundaries, so nothing live points
  // into the discarded range (token strings/numbers are copied out of the
  // buffer when scanned).
  if (committedOffset_ > kPrefixDiscardThreshold) {
    buf_.erase(0, committedOffset_);
    committedOffset_ = 0;
  }

  FlowJSONLexer lexer(
      runtime_,
      UTF16Stream(llvh::ArrayRef<char16_t>(buf_.data(), buf_.size())));
  lexer.setFinalInput(isFinal);
  lexer.resetInput(
      llvh::ArrayRef<char16_t>(buf_.data(), buf_.size()), committedOffset_);
  return parseRound(isFinal, lexer, roots, lv);
}

ExecutionStatus FlowJSONParser::appendUtf8(
    llvh::ArrayRef<uint8_t> utf8,
    bool isFinal) {
  // Prepend the carried-over tail of an incomplete UTF-8 sequence.
  llvh::SmallVector<uint8_t, 8> combined;
  if (!utf8Tail_.empty()) {
    combined.append(utf8Tail_.begin(), utf8Tail_.end());
    combined.append(utf8.begin(), utf8.end());
    utf8 = combined;
    utf8Tail_.clear();
  }
  if (utf8.empty()) {
    return ExecutionStatus::RETURNED;
  }

  // A UTF-16 string is never longer (in units) than the UTF-8 source.
  size_t oldSize = buf_.size();
  buf_.resize(oldSize + utf8.size());
  const llvh::UTF8 *srcBegin = utf8.begin();
  const llvh::UTF8 *srcEnd = utf8.end();
  llvh::UTF16 *dstBegin =
      reinterpret_cast<llvh::UTF16 *>(buf_.data() + oldSize);
  llvh::UTF16 *dst = dstBegin;

  llvh::ConversionResult res = llvh::ConvertUTF8toUTF16(
      &srcBegin, srcEnd, &dst, dstBegin + utf8.size(), llvh::strictConversion);
  buf_.resize(oldSize + (dst - dstBegin));

  if (res == llvh::sourceIllegal) {
    return runtime_.raiseSyntaxError(
        TwineChar16("JSON Parse error: Invalid UTF-8 in input"));
  }
  // sourceExhausted: the tail is an incomplete multi-byte sequence.
  size_t leftover = srcEnd - srcBegin;
  if (leftover > 0) {
    if (isFinal) {
      return runtime_.raiseSyntaxError(
          TwineChar16("JSON Parse error: Truncated UTF-8 sequence"));
    }
    utf8Tail_.assign(reinterpret_cast<const char *>(srcBegin), leftover);
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus FlowJSONParser::parseRound(
    bool isFinal,
    FlowJSONLexer &lexer,
    Handle<JSArray> roots,
    LV &lv) {
  for (;;) {
    // Save the lexer position before the token; a truncated token rolls back
    // to here.
    uint32_t tokenStart = lexer.tell();

    ExecutionStatus st = lexer.advance();
    if (LLVM_UNLIKELY(st == ExecutionStatus::EXCEPTION)) {
      status_ = Status::Error;
      return ExecutionStatus::EXCEPTION;
    }

    if (lexer.tokenTruncated()) {
      if (isFinal) {
        status_ = Status::Error;
        return lexer.error("Unexpected end of input");
      }
      // Roll back to the token start and wait for more data.
      committedOffset_ = tokenStart;
      status_ = Status::NeedMoreData;
      return ExecutionStatus::RETURNED;
    }

    JSONTokenKind kind = lexer.getCurTokenKind();
    if (kind == JSONTokenKind::Eof) {
      if (!isFinal) {
        committedOffset_ = tokenStart;
        status_ = Status::NeedMoreData;
        return ExecutionStatus::RETURNED;
      }
      if (rootDone_ && stack_.empty()) {
        status_ = Status::Done;
        return ExecutionStatus::RETURNED;
      }
      status_ = Status::Error;
      return lexer.error("Unexpected end of input");
    }

    if (LLVM_UNLIKELY(
            processToken(kind, lexer, roots, lv) ==
            ExecutionStatus::EXCEPTION)) {
      status_ = Status::Error;
      return ExecutionStatus::EXCEPTION;
    }
    committedOffset_ = lexer.tell();
  }
}

bool FlowJSONParser::expectingValue() const {
  if (stack_.empty()) {
    return !rootDone_;
  }
  const Frame &top = stack_.back();
  return top.expect == Expect::ValueOrEnd || top.expect == Expect::Value ||
      top.expect == Expect::ObjectValue;
}

bool FlowJSONParser::expectingKey() const {
  if (stack_.empty() || stack_.back().isArray) {
    return false;
  }
  return stack_.back().expect == Expect::KeyOrEnd ||
      stack_.back().expect == Expect::Key;
}

ExecutionStatus FlowJSONParser::setRootsSlot(
    Handle<JSArray> roots,
    uint32_t slot,
    Handle<HermesValue> value,
    LV &lv) {
  lv.indexValue = HermesValue::encodeTrustedNumberValue(slot);
  auto res = JSObject::defineOwnComputedPrimitive(
      roots,
      runtime_,
      lv.indexValue,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      value);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus FlowJSONParser::processToken(
    JSONTokenKind kind,
    FlowJSONLexer &lexer,
    Handle<JSArray> roots,
    LV &lv) {
  switch (kind) {
    case JSONTokenKind::String: {
      if (expectingKey()) {
        // Persist the pending key as a plain string in the roots array.
        lv.scalarValue = lexer.getCurToken()->getStrAsPrim().getHermesValue();
        if (LLVM_UNLIKELY(
                setRootsSlot(
                    roots,
                    keySlot(stack_.size() - 1),
                    Handle<HermesValue>(lv.scalarValue),
                    lv) == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        stack_.back().expect = Expect::Colon;
        return ExecutionStatus::RETURNED;
      }
      if (!expectingValue()) {
        return lexer.errorUnexpectedChar();
      }
      lv.scalarValue = lexer.getCurToken()->getStrAsPrim().getHermesValue();
      return valueComplete(Handle<HermesValue>(lv.scalarValue), roots, lv);
    }
    case JSONTokenKind::Number:
      if (!expectingValue()) {
        return lexer.errorUnexpectedChar();
      }
      lv.scalarValue = HermesValue::encodeTrustedNumberValue(
          lexer.getCurToken()->getNumber());
      return valueComplete(Handle<HermesValue>(lv.scalarValue), roots, lv);

    case JSONTokenKind::True:
    case JSONTokenKind::False:
      if (!expectingValue()) {
        return lexer.errorUnexpectedChar();
      }
      lv.scalarValue =
          HermesValue::encodeBoolValue(kind == JSONTokenKind::True);
      return valueComplete(Handle<HermesValue>(lv.scalarValue), roots, lv);

    case JSONTokenKind::Null:
      if (!expectingValue()) {
        return lexer.errorUnexpectedChar();
      }
      lv.scalarValue = HermesValue::encodeNullValue();
      return valueComplete(Handle<HermesValue>(lv.scalarValue), roots, lv);

    case JSONTokenKind::LSquare:
    case JSONTokenKind::LBrace: {
      bool isArray = kind == JSONTokenKind::LSquare;
      if (!expectingValue()) {
        return lexer.errorUnexpectedChar();
      }
      if (stack_.empty() && flowMode_ && !isArray) {
        return lexer.error("Root value must be an array in flow mode");
      }
      if (stack_.size() >= kMaxDepth) {
        return runtime_.raiseStackOverflow(
            Runtime::StackOverflowKind::JSONParser);
      }

      uint32_t frameIndex = stack_.size();
      if (!(flowMode_ && stack_.empty())) {
        // Flow mode: the root array is virtual — elements are streamed to
        // the callback and never accumulated; its container slot stays
        // undefined.
        if (isArray) {
          auto arrRes = JSArray::create(runtime_, 4, 0);
          if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          lv.newContainer.castAndSetHermesValue<JSObject>(
              arrRes->getHermesValue());
        } else {
          lv.newContainer.castAndSetHermesValue<JSObject>(
              JSObject::create(runtime_).getHermesValue());
        }
        lv.scalarValue = lv.newContainer.getHermesValue();
        if (LLVM_UNLIKELY(
                setRootsSlot(
                    roots,
                    containerSlot(frameIndex),
                    Handle<HermesValue>(lv.scalarValue),
                    lv) == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
      stack_.push_back(
          Frame{0, isArray, isArray ? Expect::ValueOrEnd : Expect::KeyOrEnd});
      return ExecutionStatus::RETURNED;
    }

    case JSONTokenKind::RSquare: {
      if (stack_.empty() || !stack_.back().isArray) {
        return lexer.errorUnexpectedChar();
      }
      // ']' is allowed for a fresh (ValueOrEnd) or just-completed
      // (CommaOrEnd) array. After a comma (Value) it is a trailing comma —
      // an error, matching runtime JSON.parse (RuntimeJSONParser).
      if (stack_.back().expect == Expect::Value) {
        return lexer.errorUnexpectedChar();
      }
      bool isRoot = stack_.size() == 1;
      if (isRoot && flowMode_) {
        // The flow-mode root frame has no container.
        stack_.pop_back();
        rootDone_ = true;
        return ExecutionStatus::RETURNED;
      }
      Handle<JSObject> arr = containerAt(roots, stack_.size() - 1);
      stack_.pop_back();
      lv.scalarValue = arr.getHermesValue();
      return valueComplete(Handle<HermesValue>(lv.scalarValue), roots, lv);
    }

    case JSONTokenKind::RBrace: {
      if (stack_.empty() || stack_.back().isArray) {
        return lexer.errorUnexpectedChar();
      }
      Frame &f = stack_.back();
      if (f.expect != Expect::KeyOrEnd && f.expect != Expect::CommaOrEnd) {
        return lexer.errorUnexpectedChar();
      }
      Handle<JSObject> obj = containerAt(roots, stack_.size() - 1);
      stack_.pop_back();
      lv.scalarValue = obj.getHermesValue();
      return valueComplete(Handle<HermesValue>(lv.scalarValue), roots, lv);
    }

    case JSONTokenKind::Comma: {
      if (stack_.empty() || stack_.back().expect != Expect::CommaOrEnd) {
        return lexer.errorUnexpectedChar();
      }
      stack_.back().expect =
          stack_.back().isArray ? Expect::Value : Expect::Key;
      return ExecutionStatus::RETURNED;
    }

    case JSONTokenKind::Colon: {
      if (stack_.empty() || stack_.back().isArray ||
          stack_.back().expect != Expect::Colon) {
        return lexer.errorUnexpectedChar();
      }
      stack_.back().expect = Expect::ObjectValue;
      return ExecutionStatus::RETURNED;
    }

    default:
      return lexer.errorUnexpectedChar();
  }
}

ExecutionStatus FlowJSONParser::valueComplete(
    Handle<HermesValue> value,
    Handle<JSArray> roots,
    LV &lv) {
  if (stack_.empty()) {
    if (flowMode_) {
      return runtime_.raiseSyntaxError(
          TwineChar16("JSON Parse error: Root value must be an array in flow mode"));
    }
    if (LLVM_UNLIKELY(
            setRootsSlot(roots, kRootValueSlot, value, lv) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    rootDone_ = true;
    return ExecutionStatus::RETURNED;
  }

  Frame &top = stack_.back();
  if (top.isArray) {
    if (flowMode_ && stack_.size() == 1) {
      elementCallback_(value);
    } else {
      lv.indexValue = HermesValue::encodeTrustedNumberValue(top.nextIndex++);
      Handle<JSObject> container = containerAt(roots, stack_.size() - 1);
      (void)JSObject::defineOwnComputedPrimitive(
          container,
          runtime_,
          lv.indexValue,
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          value);
    }
  } else {
    lv.keyValue =
        roots->at(runtime_, keySlot(stack_.size() - 1)).unboxToHV(runtime_);
    Handle<JSObject> container = containerAt(roots, stack_.size() - 1);
    (void)JSObject::defineOwnComputedPrimitive(
        container,
        runtime_,
        lv.keyValue,
        DefinePropertyFlags::getDefaultNewPropertyFlags(),
        value);
  }
  top.expect = Expect::CommaOrEnd;
  return ExecutionStatus::RETURNED;
}

} // namespace vm
} // namespace hermes
