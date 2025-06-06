// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/unittests/compiler/function-tester.h"

#include "include/v8-function.h"
#include "src/codegen/assembler.h"
#include "src/codegen/compiler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/js-heap-broker.h"
#include "src/compiler/linkage.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/turbofan-graph.h"
#include "src/execution/execution.h"
#include "src/handles/handles.h"
#include "src/objects/objects-inl.h"
#include "src/parsing/parse-info.h"

namespace v8 {
namespace internal {
namespace compiler {

namespace {
v8::Local<v8::Value> CompileRun(Isolate* isolate, const char* source) {
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::Local<v8::Context> context = v8_isolate->GetCurrentContext();
  v8::Local<v8::Script> script =
      v8::Script::Compile(
          context, v8::String::NewFromUtf8(v8_isolate, source).ToLocalChecked())
          .ToLocalChecked();
  return script->Run(context).ToLocalChecked();
}
}  // namespace

FunctionTester::FunctionTester(Isolate* isolate, const char* source,
                               uint32_t flags)
    : isolate(isolate),
      function((v8_flags.allow_natives_syntax = true, NewFunction(source))),
      flags_(flags) {
  Compile(function);
  const uint32_t supported_flags = OptimizedCompilationInfo::kInlining;
  CHECK_EQ(0u, flags_ & ~supported_flags);
}

FunctionTester::FunctionTester(Isolate* isolate, TFGraph* graph,
                               int param_count)
    : isolate(isolate),
      function(NewFunction(BuildFunction(param_count).c_str())),
      flags_(0) {
  CompileGraph(graph);
}

FunctionTester::FunctionTester(Isolate* isolate, DirectHandle<Code> code,
                               int param_count)
    : isolate(isolate),
      function((v8_flags.allow_natives_syntax = true,
                NewFunction(BuildFunction(param_count).c_str()))),
      flags_(0) {
  CHECK(!code.is_null());
  Compile(function);
  function->UpdateCode(isolate, *code);
}

void FunctionTester::CheckThrows(Handle<Object> a) {
  TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
  MaybeDirectHandle<Object> no_result = Call(a);
  CHECK(isolate->has_exception());
  CHECK(try_catch.HasCaught());
  CHECK(no_result.is_null());
}

void FunctionTester::CheckThrows(Handle<Object> a, Handle<Object> b) {
  TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
  MaybeDirectHandle<Object> no_result = Call(a, b);
  CHECK(isolate->has_exception());
  CHECK(try_catch.HasCaught());
  CHECK(no_result.is_null());
}

v8::Local<v8::Message> FunctionTester::CheckThrowsReturnMessage(
    Handle<Object> a, Handle<Object> b) {
  TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
  MaybeDirectHandle<Object> no_result = Call(a, b);
  CHECK(isolate->has_exception());
  CHECK(try_catch.HasCaught());
  CHECK(no_result.is_null());
  CHECK(!try_catch.Message().IsEmpty());
  return try_catch.Message();
}

void FunctionTester::CheckCall(DirectHandle<Object> expected, Handle<Object> a,
                               Handle<Object> b, Handle<Object> c,
                               Handle<Object> d) {
  DirectHandle<Object> result = Call(a, b, c, d).ToHandleChecked();
  CHECK(Object::SameValue(*expected, *result));
}

Handle<JSFunction> FunctionTester::NewFunction(const char* source) {
  return Cast<JSFunction>(v8::Utils::OpenHandle(
      *v8::Local<v8::Function>::Cast(CompileRun(isolate, source))));
}

Handle<JSObject> FunctionTester::NewObject(const char* source) {
  return Cast<JSObject>(v8::Utils::OpenHandle(
      *v8::Local<v8::Object>::Cast(CompileRun(isolate, source))));
}

Handle<String> FunctionTester::NewString(const char* string) {
  return isolate->factory()->InternalizeUtf8String(string);
}

Handle<Object> FunctionTester::NewNumber(double value) {
  return isolate->factory()->NewNumber(value);
}

DirectHandle<Object> FunctionTester::infinity() {
  return isolate->factory()->infinity_value();
}

DirectHandle<Object> FunctionTester::minus_infinity() {
  return NewNumber(-V8_INFINITY);
}

Handle<Object> FunctionTester::nan() { return isolate->factory()->nan_value(); }

Handle<Object> FunctionTester::undefined() {
  return isolate->factory()->undefined_value();
}

Handle<Object> FunctionTester::null() {
  return isolate->factory()->null_value();
}

Handle<Object> FunctionTester::true_value() {
  return isolate->factory()->true_value();
}

Handle<Object> FunctionTester::false_value() {
  return isolate->factory()->false_value();
}

Handle<JSFunction> FunctionTester::Compile(Handle<JSFunction> f) {
  Zone zone(isolate->allocator(), ZONE_NAME);
  return Optimize(f, &zone, flags_);
}

// Compile the given machine graph instead of the source of the function
// and replace the JSFunction's code with the result.
Handle<JSFunction> FunctionTester::CompileGraph(TFGraph* graph) {
  Handle<SharedFunctionInfo> shared(function->shared(), isolate);
  Zone zone(isolate->allocator(), ZONE_NAME);
  OptimizedCompilationInfo info(&zone, isolate, shared, function,
                                CodeKind::TURBOFAN_JS);

  auto call_descriptor = Linkage::ComputeIncoming(&zone, &info);
  DirectHandle<Code> code =
      Pipeline::GenerateCodeForTesting(&info, isolate, call_descriptor, graph,
                                       AssemblerOptions::Default(isolate))
          .ToHandleChecked();
  function->UpdateOptimizedCode(isolate, *code);
  return function;
}

Handle<JSFunction> FunctionTester::Optimize(Handle<JSFunction> target_function,
                                            Zone* zone, uint32_t flags) {
  Handle<SharedFunctionInfo> shared(target_function->shared(), isolate);
  IsCompiledScope is_compiled_scope(shared->is_compiled_scope(isolate));
  CHECK(is_compiled_scope.is_compiled() ||
        Compiler::Compile(isolate, target_function, Compiler::CLEAR_EXCEPTION,
                          &is_compiled_scope));

  CHECK_NOT_NULL(zone);

  OptimizedCompilationInfo info(zone, isolate, shared, target_function,
                                CodeKind::TURBOFAN_JS);

  if (flags & ~OptimizedCompilationInfo::kInlining) UNIMPLEMENTED();
  if (flags & OptimizedCompilationInfo::kInlining) {
    info.set_inlining();
  }

  CHECK(info.shared_info()->HasBytecodeArray());
  JSFunction::EnsureFeedbackVector(isolate, target_function,
                                   &is_compiled_scope);

  DirectHandle<Code> code =
      compiler::Pipeline::GenerateCodeForTesting(&info, isolate)
          .ToHandleChecked();
  target_function->UpdateOptimizedCode(isolate, *code);
  return target_function;
}
}  // namespace compiler
}  // namespace internal
}  // namespace v8
