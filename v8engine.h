#ifndef V8ENGINE_H
#define V8ENGINE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

typedef void* ContextPtr;
typedef void* IsolatePtr;
typedef void* ValuePtr;

typedef struct {
  const char* msg;
  const char* location;
  const char* stack;
} RtnError;

typedef struct {
  ValuePtr value;
  RtnError error;
} RtnValue;

// Initialize V8
extern void InitV8();

// Contexts
extern ContextPtr NewContext();
extern RtnValue Run(ContextPtr context, const char* source, const char* origin);
extern int LoadModule(ContextPtr ptr,
                      char* source_s,
                      char* name_s,
                      int callback_index);
extern void DisposeContext(ContextPtr context);

// Values
const char* ValueToString(ValuePtr ptr);
extern void DisposeValue(ValuePtr value);

// V8 version
const char* Version();

// Send
int Send(ContextPtr context, size_t length, void* data);

#ifdef __cplusplus
}
#endif
#endif
