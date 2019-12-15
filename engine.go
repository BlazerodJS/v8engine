package v8engine

// #cgo CXXFLAGS: -fno-rtti -fpic -Ideps/include -std=c++11
// #cgo LDFLAGS: -pthread -lv8
// #cgo darwin LDFLAGS: -Ldeps/darwin-x86_64
// #cgo linux LDFLAGS: -Ldeps/linux-x86_64
// #include <stdlib.h>
// #include "v8engine.h"
import "C"

import (
	"fmt"
	"io"
	"runtime"
	"sync"
	"unsafe"
)

var v8init sync.Once

// Engine is a standalone instance of the V8 engine (isolate + context)
type Engine struct {
	contextPtr C.ContextPtr
}

// NewEngine creates a new V8 engine (isolate + context)
func NewEngine() *Engine {
	v8init.Do(func() {
		C.InitV8()
	})

	contextPtr := C.NewContext()

	engine := &Engine{
		contextPtr: contextPtr,
	}

	runtime.SetFinalizer(engine, (*Engine).finalizer)

	return engine
}

// Run executes a script in the engine, returning the result
func (e *Engine) Run(source string, origin string) (*Value, error) {
	cSource := C.CString(source)
	cOrigin := C.CString(origin)
	defer C.free(unsafe.Pointer(cSource))
	defer C.free(unsafe.Pointer(cOrigin))

	rtn := C.Run(e.contextPtr, cSource, cOrigin)
	return getValue(rtn), getError(rtn)
}

// LoadModule executes a script in the engine, returning the result
func (e *Engine) LoadModule(source string, origin string, resolve ModuleResolverCallback) int {
	cSource := C.CString(source)
	cOrigin := C.CString(origin)
	defer C.free(unsafe.Pointer(cSource))
	defer C.free(unsafe.Pointer(cOrigin))

	resolverTableLock.Lock()
	nextResolverToken++
	token := nextResolverToken
	resolverFuncs[token] = resolve
	resolverTableLock.Unlock()

	cToken := C.int(token)

	rtn := C.LoadModule(e.contextPtr, cSource, cOrigin, cToken)

	resolverTableLock.Lock()
	delete(resolverFuncs, token)
	resolverTableLock.Unlock()

	return int(rtn)
}

// Send sends bytes to V8
func (e *Engine) Send(msg []byte) error {
	msgPointer := C.CBytes(msg)

	code := C.Send(e.contextPtr, C.size_t(len(msg)), msgPointer)
	if code != 0 {
		return fmt.Errorf("expected 0, got %d", code)
	}
	return nil
}

func (e *Engine) finalizer() {
	C.DisposeContext(e.contextPtr)
	e.contextPtr = nil

	runtime.SetFinalizer(e, nil)
}

func getValue(rtn C.RtnValue) *Value {
	if rtn.value == nil {
		return nil
	}
	v := &Value{rtn.value}
	runtime.SetFinalizer(v, (*Value).finalizer)
	return v
}

func getError(rtn C.RtnValue) error {
	if rtn.error.msg == nil {
		return nil
	}
	err := &JSError{
		Message:    C.GoString(rtn.error.msg),
		Location:   C.GoString(rtn.error.location),
		StackTrace: C.GoString(rtn.error.stack),
	}
	C.free(unsafe.Pointer(rtn.error.msg))
	C.free(unsafe.Pointer(rtn.error.location))
	C.free(unsafe.Pointer(rtn.error.stack))
	return err
}

// Value represents a JavaScript value
type Value struct {
	ptr C.ValuePtr
}

// String returns the string representation of the value
func (v *Value) String() string {
	s := C.ValueToString(v.ptr)
	defer C.free(unsafe.Pointer(s))

	return C.GoString(s)
}

func (v *Value) finalizer() {
	C.DisposeValue(v.ptr)
	v.ptr = nil
	runtime.SetFinalizer(v, nil)
}

// Version returns the version of the V8 engine
func Version() string {
	return C.GoString(C.Version())
}


var (
	resolverTableLock sync.Mutex
	nextResolverToken int
	resolverFuncs     = make(map[int]ModuleResolverCallback)
)

// ModuleResolverCallback is a callback function type used to resolve modules
type ModuleResolverCallback func(moduleName, referrerName string) (string, int)

// ResolveModule resolves module requests to source contents
//export ResolveModule
func ResolveModule(moduleSpecifier *C.char, referrerSpecifier *C.char, resolverToken int) (*C.char, C.int) {
	moduleName := C.GoString(moduleSpecifier)
	referrerName := C.GoString(referrerSpecifier)

	resolverTableLock.Lock()
	resolve := resolverFuncs[resolverToken]
	resolverTableLock.Unlock()

	if resolve == nil {
		return nil, C.int(1)
	}
	canon, ret := resolve(moduleName, referrerName)

	return C.CString(canon), C.int(ret)
}


// JSError is an error that is returned if there is are any
// JavaScript exceptions handled in the context. When used with the fmt
// verb `%+v`, will output the JavaScript stack trace, if available.
type JSError struct {
	Message    string
	Location   string
	StackTrace string
}

func (e *JSError) Error() string {
	return e.Message
}

// Format implements the fmt.Formatter interface to provide a custom formatter
// primarily to output the javascript stack trace with %+v
func (e *JSError) Format(s fmt.State, verb rune) {
	switch verb {
	case 'v':
		if s.Flag('+') && e.StackTrace != "" {
			io.WriteString(s, e.StackTrace)
			return
		}
		fallthrough
	case 's':
		io.WriteString(s, e.Message)
	case 'q':
		fmt.Fprintf(s, "%q", e.Message)
	}
}
