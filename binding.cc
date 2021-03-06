#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include "v8.h"
#include "libplatform/libplatform.h"
#include "binding.h"

using namespace v8;

struct context_s {
  int id;
  void* data;
  Persistent<Context> context;	
  worker_recv_cb cb;
  worker_recvSync_cb req_cb;
  Persistent<Function> recv;
  Persistent<Function> recv_sync_handler;
};

struct worker_s {  
  void* data;
  int32_t contextIndex;
  std::string last_exception;  
  std::map<int32_t, context*> contexts;
  
  Isolate* isolate;
};

// Extracts a C string from a V8 Utf8Value.
const char* ToCString(const String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

class ArrayBufferAllocator : public ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) { free(data); }
};


// Exception details will be appended to the first argument.
std::string ExceptionString(Isolate* isolate, TryCatch* try_catch) {
  std::string out;
  size_t scratchSize = 20;
  char scratch[scratchSize]; // just some scratch space for sprintf

  HandleScope handle_scope(isolate);
  String::Utf8Value exception(try_catch->Exception());
  const char* exception_string = ToCString(exception);

  Handle<Message> message = try_catch->Message();

  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // print the exception.
    out.append(exception_string);
    out.append("\n");
  } else {
    // Print (filename):(line number)
    String::Utf8Value filename(message->GetScriptOrigin().ResourceName());
    const char* filename_string = ToCString(filename);
    int linenum = message->GetLineNumber();

    snprintf(scratch, scratchSize, "%i", linenum);
    out.append(filename_string);
    out.append(":");
    out.append(scratch);
    out.append("\n");

    // Print line of source code.
    String::Utf8Value sourceline(message->GetSourceLine());
    const char* sourceline_string = ToCString(sourceline);

    out.append(sourceline_string);
    out.append("\n");

    // Print wavy underline (GetUnderline is deprecated).
    int start = message->GetStartColumn();
    for (int i = 0; i < start; i++) {
      out.append(" ");
    }
    int end = message->GetEndColumn();
    for (int i = start; i < end; i++) {
      out.append("^");
    }
    out.append("\n");
    String::Utf8Value stack_trace(try_catch->StackTrace());
    if (stack_trace.length() > 0) {
      const char* stack_trace_string = ToCString(stack_trace);
      out.append(stack_trace_string);
      out.append("\n");
    } else {
      out.append(exception_string);
      out.append("\n");
    }
  }
  return out;
}


extern "C" {
#include "_cgo_export.h"

void go_recv_cb(const char* msg, void* data) {
  recvCb((char*)msg, data);
}

const char* go_recvSync_cb(const char* msg, void* data) {
  return recvSyncCb((char*)msg, data);
}

const char* worker_version() {
  return V8::GetVersion();
}

const char* worker_last_exception(worker* w) {
  return w->last_exception.c_str();
}

int worker_load(worker* w, context *c, char* name_s, char* source_s) {
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, c->context);
  Context::Scope context_scope(context);

  TryCatch try_catch;

  Local<String> name = String::NewFromUtf8(w->isolate, name_s);
  Local<String> source = String::NewFromUtf8(w->isolate, source_s);

  ScriptOrigin origin(name);

  Local<Script> script = Script::Compile(source, &origin);

  if (script.IsEmpty()) {
    assert(try_catch.HasCaught());
    w->last_exception = ExceptionString(w->isolate, &try_catch);
    return 1;
  }
  
  Handle<Value> result = script->Run();

  if (result.IsEmpty()) {
    assert(try_catch.HasCaught());
    w->last_exception = ExceptionString(w->isolate, &try_catch);
    return 2;
  }

  return 0;
}

void Print(const FunctionCallbackInfo<Value>& args) {
  bool first = true;
  for (int i = 0; i < args.Length(); i++) {
    HandleScope handle_scope(args.GetIsolate());
    if (first) {
      first = false;
    } else {
      printf(" ");
    }
    String::Utf8Value str(args[i]);
    const char* cstr = ToCString(str);
    printf("%s", cstr);
  }
  printf("\n");
  fflush(stdout);
}

// sets the recv callback.
void Recv(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  worker* w = (worker*)isolate->GetData(0);
  assert(w->isolate == isolate);

  HandleScope handle_scope(isolate);

  Local<Context> ctx = isolate->GetCurrentContext();
  Context::Scope context_scope(ctx);
    
  int value = ctx->Global()->Get(String::NewFromUtf8(w->isolate, "$context"))->Int32Value();  
  context* c = w->contexts[value];

  Local<Value> v = args[0];
  assert(v->IsFunction());
  Local<Function> func = Local<Function>::Cast(v);

  c->recv.Reset(isolate, func);
}

void RecvSync(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  worker* w = (worker*)isolate->GetData(0);
 
  assert(w->isolate == isolate);

  HandleScope handle_scope(isolate);

  Local<Context> ctx = isolate->GetCurrentContext();
  Context::Scope context_scope(ctx);
    
  int value = ctx->Global()->Get(String::NewFromUtf8(w->isolate, "$context"))->Int32Value();  
  context* c = w->contexts[value];

  Local<Value> v = args[0];
  assert(v->IsFunction());
  Local<Function> func = Local<Function>::Cast(v);

  c->recv_sync_handler.Reset(isolate, func);
}

// Called from javascript. Must route message to golang.
void Send(const FunctionCallbackInfo<Value>& args) {
  std::string msg;
  worker* w = NULL;  
  context *c = NULL;
  {
    Isolate* isolate = args.GetIsolate();
    w = static_cast<worker*>(isolate->GetData(0));	
    assert(w->isolate == isolate);

    Locker locker(w->isolate);
    HandleScope handle_scope(isolate);

	Local<Context> ctx = isolate->GetCurrentContext();
	Context::Scope context_scope(ctx);
	
	int value = ctx->Global()->Get(String::NewFromUtf8(w->isolate, "$context"))->Int32Value();
    c = w->contexts[value];
	
    Local<Value> v = args[0];
    assert(v->IsString());

    String::Utf8Value str(v);
    msg = ToCString(str);
  }

  // XXX should we use Unlocker?
  c->cb(msg.c_str(), c->data);
}

// Called from javascript using $request.
// Must route message (string) to golang and send back message (string) as return value.
void SendSync(const FunctionCallbackInfo<Value>& args) {
  std::string msg;
  worker* w = NULL;
  context *c = NULL;
  {
    Isolate* isolate = args.GetIsolate();
    w = static_cast<worker*>(isolate->GetData(0));	
    assert(w->isolate == isolate);

    Locker locker(w->isolate);
    HandleScope handle_scope(isolate);

    Local<Context> ctx = isolate->GetCurrentContext();
	Context::Scope context_scope(ctx);
	
	int value = ctx->Global()->Get(String::NewFromUtf8(w->isolate, "$context"))->Int32Value();  
    c = w->contexts[value];

    Local<Value> v = args[0];
    assert(v->IsString());

    String::Utf8Value str(v);
    msg = ToCString(str);
  }
  const char* returnMsg = c->req_cb(msg.c_str(), c->data);
  Local<String> returnV = String::NewFromUtf8(w->isolate, returnMsg);
  args.GetReturnValue().Set(returnV);
}

// Called from golang. Must route message to javascript lang.
// non-zero return value indicates error. check worker_last_exception().
int worker_send(worker* w, context* c, const char* msg) {
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, c->context);
  Context::Scope context_scope(context);

  TryCatch try_catch;

  Local<Function> recv = Local<Function>::New(w->isolate, c->recv);
  if (recv.IsEmpty()) {
    w->last_exception = "$recv not called";
    return 1;
  }

  Local<Value> args[1];
  args[0] = String::NewFromUtf8(w->isolate, msg);

  assert(!try_catch.HasCaught());
  
  recv->Call(context->Global(), 1, args);

  if (try_catch.HasCaught()) {
    w->last_exception = ExceptionString(w->isolate, &try_catch);
    return 2;
  }

  return 0;
}

// Called from golang. Must route message to javascript lang.
// It will call the $recv_sync_handler callback function and return its string value.
const char* worker_sendSync(worker* w, context* c, const char* msg) {
  std::string out;
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, c->context);
  Context::Scope context_scope(context);

  Local<Function> recv_sync_handler = Local<Function>::New(w->isolate, c->recv_sync_handler);
  if (recv_sync_handler.IsEmpty()) {
    out.append("err: $recvSync not called");
    return out.c_str();
  }

  Local<Value> args[1];
  args[0] = String::NewFromUtf8(w->isolate, msg);
    
  Local<Value> response_value = recv_sync_handler->Call(context->Global(), 1, args);

  if (response_value->IsString()) {
    String::Utf8Value response(response_value->ToString());
    out.append(*response);
  } else {
    out.append("err: non-string return value");
  }
  return out.c_str();
}

static ArrayBufferAllocator array_buffer_allocator;

void v8_init() {
  V8::Initialize();

  Platform* platform = platform::CreateDefaultPlatform();
  V8::InitializePlatform(platform);
}

context* context_new(worker* w, worker_recv_cb cb, worker_recvSync_cb recvSync_cb, void* data) {  
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<ObjectTemplate> global = ObjectTemplate::New(w->isolate);

  global->Set(String::NewFromUtf8(w->isolate, "$context"),
			  Integer::New(w->isolate, w->contextIndex));
  
  global->Set(String::NewFromUtf8(w->isolate, "$print"),
              FunctionTemplate::New(w->isolate, Print));

  global->Set(String::NewFromUtf8(w->isolate, "$recv"),
              FunctionTemplate::New(w->isolate, Recv));

  global->Set(String::NewFromUtf8(w->isolate, "$send"),
              FunctionTemplate::New(w->isolate, Send));

  global->Set(String::NewFromUtf8(w->isolate, "$sendSync"),
              FunctionTemplate::New(w->isolate, SendSync));

  global->Set(String::NewFromUtf8(w->isolate, "$recvSync"),
              FunctionTemplate::New(w->isolate, RecvSync));
			  
  Local<Context> localContext = Context::New(w->isolate, NULL, global);
  
  context* c = new(context);
  c->id = w->contextIndex;
  c->data = data;
  c->cb = cb;
  c->req_cb = recvSync_cb;
  c->context.Reset(w->isolate, localContext);

  w->contexts.insert(std::make_pair(w->contextIndex, c));
  w->contextIndex++;
  
  return c;
}

void worker_terminate(worker* w) {
	V8::TerminateExecution(w->isolate);
}

worker* worker_new(void* data) {
  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = &array_buffer_allocator;
  Isolate* isolate = Isolate::New(create_params);
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);

  worker* w = new(worker);
  w->isolate = isolate;
  w->isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  w->isolate->SetData(0, w);
  w->data = data;
  w->contextIndex = 0;
  
  return w;
}

void worker_dispose(worker* w) {
  w->isolate->Dispose();
  delete(w);
}

void context_dispose(context* c) {
  c->context.Reset();
  delete(c);
}

}
