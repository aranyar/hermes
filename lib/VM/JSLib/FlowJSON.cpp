/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FlowJSONParser.h"

#include "JSLibInternal.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/NativeState.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Predefined.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

/// JS surface for the incremental (flow) JSON parser, registered as the
/// global %FlowJSON object (gated on JSLibFlags::enableFlowJsonParser):
///
///   const p = FlowJSON.createParser(onElement /* optional */);
///   p.feed(chunk /* string */, isFinal /* optional boolean */)
///       // -> "needMoreData" | "done"; throws SyntaxError on malformed input
///   p.root()  // default mode only: the parsed document (must be done)
///
/// With onElement the parser runs in flow mode: the document must be a JSON
/// array and onElement(value) is invoked synchronously for each completed
/// element of the root array during feed(). Without it the whole document is
/// accumulated and read back with root().
namespace {

/// Property names on the parser object. Non-enumerable and not part of the
/// public API.
constexpr const char *kRootsPropName = "$flowJsonRoots";
constexpr const char *kCallbackPropName = "$flowJsonOnElement";

/// Native state behind every parser object. Holds only native memory, so the
/// finalizer may run at an arbitrary GC point. The in-flight parse state
/// (open containers, pending keys) lives in the parser object's roots array
/// property (see FlowJSONParser), which the JS object graph keeps alive.
struct FlowJSONParserHolder {
  FlowJSONParserHolder(Runtime &runtime, bool flowMode)
      : parser(
            runtime,
            flowMode ? FlowJSONParser::ElementCallback(
                           [this](Handle<HermesValue> el) { onElement(el); })
                     : nullptr),
        runtime(runtime),
        currentCallback(Runtime::makeNullHandle<Callable>()) {}

  FlowJSONParser parser;
  Runtime &runtime;
  /// The JS element callback; set from the parser object property at the
  /// start of every feed() and cleared at the end (the handle lives in the
  /// feed's GC scope, so it is only valid while a feed is running).
  Handle<Callable> currentCallback;
  /// Set when the JS element callback threw; feed() then rethrows.
  bool callbackThrew = false;

  void onElement(Handle<HermesValue> el) {
    if (callbackThrew) {
      return;
    }
    auto res = Callable::executeCall1(
        currentCallback,
        runtime,
        Runtime::getUndefinedValue(),
        el.getHermesValue());
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      callbackThrew = true;
    }
  }
};

void finalizeFlowJSONParser(GC &, NativeState *ns) {
  delete reinterpret_cast<FlowJSONParserHolder *>(ns->context());
}

Handle<SymbolID> internSymbol(Runtime &runtime, const char *name) {
  return runtime.ignoreAllocationFailure(
      runtime.getIdentifierTable().getSymbolHandle(
          runtime, createASCIIRef(name)));
}

/// Extract (self, holder) from a parser method call, raising TypeError on a
/// foreign receiver.
CallResult<FlowJSONParserHolder *> getHolder(
    Runtime &runtime,
    NativeArgs args,
    Handle<JSObject> &selfOut) {
  selfOut = args.dyncastThis<JSObject>();
  if (!selfOut) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON parser method called on incompatible receiver"));
  }
  auto propRes = JSObject::getNamed_RJS(
      selfOut,
      runtime,
      Predefined::getSymbolID(Predefined::InternalPropertyNativeState));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!vmisa<NativeState>(propRes->getHermesValue())) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON parser method called on incompatible receiver"));
  }
  auto *ns = vmcast<NativeState>(propRes->getHermesValue());
  return reinterpret_cast<FlowJSONParserHolder *>(ns->context());
}

/// Read the parser object's roots array property into [rootsOut] (a fresh
/// pinned value, so it stays valid across GCs triggered while parsing).
ExecutionStatus readRoots(
    Runtime &runtime,
    Handle<JSObject> self,
    PinnedValue<JSArray> &rootsOut) {
  auto propRes = JSObject::getNamed_RJS(
      self, runtime, *internSymbol(runtime, kRootsPropName));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!vmisa<JSArray>(propRes->getHermesValue())) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON parser internal state was tampered with"));
  }
  rootsOut.castAndSetHermesValue<JSArray>(propRes->getHermesValue());
  return ExecutionStatus::RETURNED;
}

/// \code
///   parser.feed = function (chunk, isFinal) {}
/// \endcode
CallResult<HermesValue> flowJSONParserFeed(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSArray> roots;
    PinnedValue<StringPrimitive> chunkStr;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScope gcScope(runtime);

  Handle<JSObject> self = Runtime::makeNullHandle<JSObject>();
  auto holderRes = getHolder(runtime, args, self);
  if (LLVM_UNLIKELY(holderRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto *holder = *holderRes;

  HermesValue chunkArg = args.getArg(0);
  if (!chunkArg.isString()) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON feed: chunk must be a string"));
  }
  lv.chunkStr = chunkArg.getString();
  bool isFinal = args.getArgCount() > 1 && toBoolean(args.getArg(1));

  if (LLVM_UNLIKELY(
          readRoots(runtime, self, lv.roots) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (holder->parser.flowMode()) {
    auto cbRes = JSObject::getNamed_RJS(
        self, runtime, *internSymbol(runtime, kCallbackPropName));
    if (LLVM_UNLIKELY(cbRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!vmisa<Callable>(cbRes->getHermesValue())) {
      return runtime.raiseTypeError(
          TwineChar16("FlowJSON feed: onElement is not callable"));
    }
    // The handle lives in this native call's GC scope, so it stays valid for
    // the whole feed (cleared below before returning).
    holder->currentCallback =
        runtime.makeHandle<Callable>(cbRes->getHermesValue());
    holder->callbackThrew = false;
  }

  llvh::SmallVector<char16_t, 256> u16;
  lv.chunkStr->appendUTF16String(u16);
  ExecutionStatus st = holder->parser.feedUtf16(
      llvh::ArrayRef<char16_t>(u16.data(), u16.size()),
      isFinal,
      Handle<JSArray>(lv.roots));

  if (holder->parser.flowMode()) {
    holder->currentCallback = Runtime::makeNullHandle<Callable>();
    if (LLVM_UNLIKELY(holder->callbackThrew)) {
      // The JS element callback threw; its exception is pending.
      return ExecutionStatus::EXCEPTION;
    }
  }
  if (LLVM_UNLIKELY(st == ExecutionStatus::EXCEPTION)) {
    // A SyntaxError (or OOM) is pending.
    return ExecutionStatus::EXCEPTION;
  }

  bool done = holder->parser.status() == FlowJSONParser::Status::Done;
  auto statusStr = StringPrimitive::create(
      runtime, createASCIIRef(done ? "done" : "needMoreData"));
  if (LLVM_UNLIKELY(statusStr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *statusStr;
}

/// \code
///   parser.root = function () {}
/// \endcode
CallResult<HermesValue> flowJSONParserRoot(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSArray> roots;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScope gcScope(runtime);

  Handle<JSObject> self = Runtime::makeNullHandle<JSObject>();
  auto holderRes = getHolder(runtime, args, self);
  if (LLVM_UNLIKELY(holderRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto *holder = *holderRes;

  if (holder->parser.flowMode()) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON root: flow-mode parser has no root value"));
  }
  if (holder->parser.status() != FlowJSONParser::Status::Done) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON root: document is not complete"));
  }
  if (LLVM_UNLIKELY(
          readRoots(runtime, self, lv.roots) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return holder->parser.getRootValue(Handle<JSArray>(lv.roots));
}

/// \code
///   FlowJSON.createParser = function (onElement) {}
/// \endcode
CallResult<HermesValue> flowJSONCreateParser(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> parserObj;
    PinnedValue<JSArray> roots;
    PinnedValue<NativeState> ns;
    PinnedValue<NativeFunction> method;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScope gcScope(runtime);

  HermesValue cbArg = args.getArg(0);
  bool flowMode = vmisa<Callable>(cbArg);
  if (!cbArg.isUndefined() && !flowMode) {
    return runtime.raiseTypeError(
        TwineChar16("FlowJSON.createParser: onElement must be a function"));
  }

  DefinePropertyFlags hiddenDPF =
      DefinePropertyFlags::getDefaultNewPropertyFlags();
  hiddenDPF.enumerable = 0;

  auto *holder = new FlowJSONParserHolder(runtime, flowMode);

  lv.parserObj = JSObject::create(runtime);

  // The native state slot.
  lv.ns = NativeState::create(runtime, holder, finalizeFlowJSONParser);
  auto res = JSObject::defineOwnProperty(
      lv.parserObj,
      runtime,
      Predefined::getSymbolID(Predefined::InternalPropertyNativeState),
      hiddenDPF,
      lv.ns);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // The roots array (GC home of the parser's in-flight state).
  auto arrRes = JSArray::create(runtime, 16, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.roots = arrRes->get();
  res = JSObject::defineOwnProperty(
      lv.parserObj,
      runtime,
      *internSymbol(runtime, kRootsPropName),
      hiddenDPF,
      lv.roots);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // The element callback (flow mode).
  if (flowMode) {
    res = JSObject::defineOwnProperty(
        lv.parserObj,
        runtime,
        *internSymbol(runtime, kCallbackPropName),
        hiddenDPF,
        runtime.makeHandle(cbArg));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  // Methods.
  struct {
    const char *name;
    NativeFunctionPtr fn;
    unsigned params;
  } methods[] = {
      {"feed", flowJSONParserFeed, 2},
      {"root", flowJSONParserRoot, 0},
  };
  for (const auto &m : methods) {
    Handle<SymbolID> nameSym = internSymbol(runtime, m.name);
    lv.method = NativeFunction::create(
        runtime,
        runtime.functionPrototype,
        Runtime::makeNullHandle<Environment>(),
        nullptr,
        m.fn,
        *nameSym,
        m.params,
        Runtime::makeNullHandle<JSObject>());
    res = JSObject::defineOwnProperty(
        lv.parserObj, runtime, *nameSym, hiddenDPF, lv.method);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  return lv.parserObj.getHermesValue();
}

} // namespace

HermesValue createFlowJSONObject(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScope gcScope{runtime};

  DefinePropertyFlags constantDPF =
      DefinePropertyFlags::getDefaultNewPropertyFlags();
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  lv.obj = JSObject::create(runtime);
  Handle<SymbolID> createParserSym = internSymbol(runtime, "createParser");
  (void)defineMethod(
      runtime,
      lv.obj,
      *createParserSym,
      *createParserSym,
      nullptr /* context */,
      flowJSONCreateParser,
      1,
      constantDPF);
  return lv.obj.getHermesValue();
}

} // namespace vm
} // namespace hermes
