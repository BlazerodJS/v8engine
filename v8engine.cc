#include "v8engine.h"

#include "v8.h"

#include "libplatform/libplatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include "_cgo_export.h"

using namespace v8;

auto defaultAllocator = ArrayBuffer::Allocator::NewDefaultAllocator();
auto defaultPlatform = platform::NewDefaultPlatform();

typedef struct {
  Persistent<Context> ptr;
  Isolate* isolate;

  Persistent<Function> cb;

  std::map<std::string, Eternal<Module>> modules;
  std::map<int, std::map<std::string, Eternal<Module>>> resolved;
} m_ctx;

typedef struct {
  Persistent<Value> ptr;
  m_ctx* context;
} m_value;

// Utils

const char* CopyString(std::string str) {
  int len = str.length();
  char* mem = (char*)malloc(len + 1);
  memcpy(mem, str.data(), len);
  mem[len] = 0;
  return mem;
}

const char* CopyString(String::Utf8Value& value) {
  if (value.length() == 0) {
    return nullptr;
  }
  return CopyString(*value);
}

// Runtime

void Fprint(FILE* out, const FunctionCallbackInfo<Value>& args) {
  bool first = true;
  for (int i = 0; i < args.Length(); i++) {
    Isolate* isolate = args.GetIsolate();
    HandleScope handle_scope(isolate);
    if (first) {
      first = false;
    } else {
      fprintf(out, " ");
    }
    String::Utf8Value str(isolate, args[i]);
    const char* cstr = CopyString(str);
    fprintf(out, "%s", cstr);
  }
  fprintf(out, "\n");
  fflush(out);
}

void Print(const FunctionCallbackInfo<Value>& args) {
  Fprint(stdout, args);
}

void Log(const FunctionCallbackInfo<Value>& args) {
  Fprint(stderr, args);
}

void cb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  m_ctx* ctx = (m_ctx*)isolate->GetData(0);
  assert(ctx->isolate == isolate);

  HandleScope handle_scope(isolate);
  Local<Context> context = ctx->ptr.Get(isolate);

  Local<Value> v = args[0];
  assert(v->IsFunction());
  Local<Function> func = Local<Function>::Cast(v);

  ctx->cb.Reset(isolate, func);
}

// Errors

RtnError ExceptionError(TryCatch& try_catch,
                        Isolate* isolate,
                        Local<Context> context) {
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);

  RtnError rtn = {nullptr, nullptr, nullptr};

  if (try_catch.HasTerminated()) {
    rtn.msg =
        CopyString("ExecutionTerminated: script execution has been terminated");
    return rtn;
  }

  String::Utf8Value exception(isolate, try_catch.Exception());
  rtn.msg = CopyString(exception);

  Local<Message> msg = try_catch.Message();
  if (!msg.IsEmpty()) {
    String::Utf8Value origin(isolate, msg->GetScriptOrigin().ResourceName());
    std::ostringstream sb;
    sb << *origin;
    Maybe<int> line = try_catch.Message()->GetLineNumber(context);
    if (line.IsJust()) {
      sb << ":" << line.ToChecked();
    }
    Maybe<int> start = try_catch.Message()->GetStartColumn(context);
    if (start.IsJust()) {
      sb << ":"
         << start.ToChecked() + 1;  // + 1 to match output from stack trace
    }
    rtn.location = CopyString(sb.str());
  }

  MaybeLocal<Value> mstack = try_catch.StackTrace(context);
  if (!mstack.IsEmpty()) {
    String::Utf8Value stack(isolate, mstack.ToLocalChecked());
    rtn.stack = CopyString(stack);
  }

  return rtn;
}

// Initialize V8

void InitV8() {
  V8::InitializePlatform(defaultPlatform.get());
  V8::Initialize();
}

// Contexts

ContextPtr NewContext() {
  Isolate::CreateParams params;
  params.array_buffer_allocator = defaultAllocator;
  Isolate* isolate = Isolate::New(params);

  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);

  isolate->SetCaptureStackTraceForUncaughtExceptions(true);

  Local<ObjectTemplate> global = ObjectTemplate::New(isolate);
  Local<ObjectTemplate> v8engine = ObjectTemplate::New(isolate);

  global->Set(isolate, "V8Engine", v8engine);

  v8engine->Set(isolate, "print", FunctionTemplate::New(isolate, Print));
  v8engine->Set(isolate, "log", FunctionTemplate::New(isolate, Log));
  v8engine->Set(isolate, "cb", FunctionTemplate::New(isolate, cb));

  m_ctx* ctx = new m_ctx;
  ctx->ptr.Reset(isolate, Context::New(isolate, NULL, global));
  ctx->isolate = isolate;
  isolate->SetData(0, ctx);
  return static_cast<ContextPtr>(ctx);
}

RtnValue Run(ContextPtr ptr, const char* source, const char* origin) {
  m_ctx* ctx = static_cast<m_ctx*>(ptr);
  Isolate* isolate = ctx->isolate;
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);
  TryCatch try_catch(isolate);

  Local<Context> lContext = ctx->ptr.Get(isolate);
  Context::Scope context_scope(lContext);

  Local<String> lSource =
      String::NewFromUtf8(isolate, source, NewStringType::kNormal)
          .ToLocalChecked();
  Local<String> lOrigin =
      String::NewFromUtf8(isolate, source, NewStringType::kNormal)
          .ToLocalChecked();

  RtnValue rtn = {nullptr, nullptr};

  ScriptOrigin script_origin(lOrigin);
  MaybeLocal<Script> script =
      Script::Compile(lContext, lSource, &script_origin);
  if (script.IsEmpty()) {
    rtn.error = ExceptionError(try_catch, isolate, lContext);
    return rtn;
  }

  MaybeLocal<v8::Value> result = script.ToLocalChecked()->Run(lContext);
  if (result.IsEmpty()) {
    rtn.error = ExceptionError(try_catch, isolate, lContext);
    return rtn;
  }
  m_value* val = new m_value;
  val->context = ctx;
  val->ptr.Reset(isolate, Persistent<Value>(isolate, result.ToLocalChecked()));

  rtn.value = static_cast<ValuePtr>(val);
  return rtn;
}

MaybeLocal<Module> ResolveCallback(Local<Context> context,
                                   Local<String> specifier,
                                   Local<Module> referrer) {
  Isolate* isolate = Isolate::GetCurrent();
  m_ctx* ctx = (m_ctx*)isolate->GetData(0);

  HandleScope handle_scope(isolate);

  String::Utf8Value str(isolate, specifier);
  const char* moduleName = *str;

  if (ctx->resolved.count(referrer->GetIdentityHash()) == 0) {
    return MaybeLocal<Module>();
  }

  std::map<std::string, Eternal<Module>> localResolve =
      ctx->resolved[referrer->GetIdentityHash()];
  if (localResolve.count(moduleName) == 0) {
    return MaybeLocal<Module>();
  }

  return localResolve[moduleName].Get(isolate);
}

int LoadModule(ContextPtr ptr,
               char* source_s,
               char* name_s,
               int callback_index) {
  m_ctx* ctx = static_cast<m_ctx*>(ptr);
  Isolate* isolate = ctx->isolate;
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);
  TryCatch try_catch(isolate);

  Local<Context> context = ctx->ptr.Get(isolate);
  Context::Scope context_scope(context);

  Local<String> name =
      String::NewFromUtf8(isolate, name_s, NewStringType::kNormal)
          .ToLocalChecked();
  Local<String> source_text =
      String::NewFromUtf8(isolate, source_s, NewStringType::kNormal)
          .ToLocalChecked();

  Local<Integer> resource_line_offset = Integer::New(isolate, 0);
  Local<Integer> resource_column_offset = Integer::New(isolate, 0);
  Local<Boolean> resource_is_shared_cross_origin = True(isolate);
  Local<Integer> script_id = Local<Integer>();
  Local<Value> source_map_url = Local<Value>();
  Local<Boolean> resource_is_opaque = False(isolate);
  Local<Boolean> is_wasm = False(isolate);
  Local<Boolean> is_module = True(isolate);
  Local<PrimitiveArray> host_defined_options = Local<PrimitiveArray>();

  ScriptOrigin origin(name, resource_line_offset, resource_column_offset,
                      resource_is_shared_cross_origin, script_id,
                      source_map_url, resource_is_opaque, is_wasm, is_module,
                      host_defined_options);

  ScriptCompiler::Source source(source_text, origin);
  Local<Module> module;

  if (!ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) {
    assert(try_catch.HasCaught());
    auto err = ExceptionError(try_catch, isolate, context);
    printf("%s\n", err.msg);
    return 1;
  }

  std::map<std::string, Eternal<Module>> resolved;

  for (int i = 0; i < module->GetModuleRequestsLength(); i++) {
    Local<String> dependency = module->GetModuleRequest(i);
    String::Utf8Value str(isolate, dependency);
    char* dependencySpecifier = *str;

    auto retval = ResolveModule(dependencySpecifier, name_s, callback_index);

    if (retval.r1 != 0) {
      return retval.r1;
    }

    if (ctx->modules.count(retval.r0) == 0) {
      return 2;
    }

    resolved[dependencySpecifier] = ctx->modules[retval.r0];
  }

  Eternal<Module> persistentModule(isolate, module);
  ctx->modules[name_s] = persistentModule;
  ctx->resolved[module->GetIdentityHash()] = resolved;

  Maybe<bool> ok = module->InstantiateModule(context, ResolveCallback);
  if (!ok.FromMaybe(false)) {
    return 3;
  }

  MaybeLocal<Value> result = module->Evaluate(context);

  if (result.IsEmpty()) {
    assert(try_catch.HasCaught());
    auto err = ExceptionError(try_catch, isolate, context);
    printf("%s\n", err.msg);
    return 4;
  }

  return 0;
}

void DisposeContext(ContextPtr ptr) {
  if (ptr == nullptr) {
    return;
  }

  m_ctx* ctx = static_cast<m_ctx*>(ptr);
  Isolate* isolate = ctx->isolate;
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  ctx->ptr.Reset();
  isolate->Dispose();
  delete ctx;
}

// Values

const char* ValueToString(ValuePtr ptr) {
  m_value* val = static_cast<m_value*>(ptr);
  m_ctx* ctx = val->context;
  Isolate* isolate = ctx->isolate;

  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);
  Context::Scope context_scope(ctx->ptr.Get(isolate));

  Local<Value> value = val->ptr.Get(isolate);
  String::Utf8Value utf8(isolate, value);

  return CopyString(utf8);
}

void DisposeValue(ValuePtr ptr) {
  m_value* val = static_cast<m_value*>(ptr);

  if (val == nullptr) {
    return;
  }

  m_ctx* ctx = val->context;

  if (ctx == nullptr) {
    return;
  }

  Isolate* isolate = ctx->isolate;
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);

  val->ptr.Reset();
  delete val;
}

// Version

const char* Version() {
  return V8::GetVersion();
}

// Send

int Send(ContextPtr ptr, size_t length, void* data) {
  m_ctx* ctx = static_cast<m_ctx*>(ptr);
  Isolate* isolate = ctx->isolate;
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);
  TryCatch try_catch(isolate);

  Local<Context> context = ctx->ptr.Get(isolate);
  Context::Scope context_scope(context);

  Local<Function> cb = Local<Function>::New(isolate, ctx->cb);
  if (cb.IsEmpty()) {
    return 2;
  }

  Local<Value> args[1];

  auto callback = [](void* data, size_t length, void* deleter_data) {
    if (deleter_data == nullptr) {
      return;
    }

    static_cast<ArrayBuffer::Allocator*>(deleter_data)->Free(data, length);
  };
  std::unique_ptr<BackingStore> backing = ArrayBuffer::NewBackingStore(
      data, length, callback, isolate->GetArrayBufferAllocator());
  args[0] = ArrayBuffer::New(isolate, std::move(backing));
  assert(!args[0].IsEmpty());
  assert(!try_catch.HasCaught());

  auto ret = cb->Call(context, context->Global(), 1, args);

  if (try_catch.HasCaught()) {
    auto err = ExceptionError(try_catch, isolate, context);
    printf("%s\n", err.msg);
    printf("%s\n", err.stack);
    return 3;
  }

  return 0;
}
