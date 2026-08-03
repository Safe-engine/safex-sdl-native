#pragma once

/*
 * Minimal C-facing JavaScript host API backed by Hermes.  The SDL bindings use
 * this interface so their rendering code remains C while the engine itself is
 * embedded through Hermes JSI in js_runtime.cpp.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct JSModuleDef JSModuleDef;

typedef struct JSValue {
    void *payload;
    double number;
    int tag;
} JSValue;
typedef JSValue JSValueConst;
typedef const char *JSAtom;

typedef JSValue (*JSCFunction)(JSContext *, JSValueConst, int, JSValueConst *);
typedef int (*JSModuleInitFunc)(JSContext *, JSModuleDef *);

typedef struct JSCFunctionListEntry {
    const char *name;
    int length;
    JSCFunction function;
} JSCFunctionListEntry;

#define JS_CFUNC_DEF(name, length, function) { name, length, function }
#define JS_ATOM_NULL ((JSAtom)0)
#define JS_UNDEFINED ((JSValue){ NULL, 0, 0 })
#define JS_NULL ((JSValue){ NULL, 0, 1 })
#define JS_FALSE ((JSValue){ NULL, 0, 2 })
#define JS_TRUE ((JSValue){ NULL, 1, 2 })
#define JS_EXCEPTION ((JSValue){ NULL, 0, -1 })

enum {
    JS_EVAL_TYPE_GLOBAL = 0,
    JS_EVAL_TYPE_MODULE = 1,
    JS_EVAL_FLAG_COMPILE_ONLY = 2,
    JS_TYPED_ARRAY_UINT16 = 1,
    JS_TYPED_ARRAY_UINT32 = 2,
    JS_TYPED_ARRAY_INT32 = 3,
    JS_TYPED_ARRAY_FLOAT32 = 4,
    JS_PROMISE_FULFILLED = 1,
    JS_PROMISE_REJECTED = 2,
};

#ifdef __cplusplus
extern "C" {
#endif

JSRuntime *JS_NewRuntime(void);
void JS_FreeRuntime(JSRuntime *runtime);
JSContext *JS_NewContext(JSRuntime *runtime);
void JS_FreeContext(JSContext *context);
JSRuntime *JS_GetRuntime(JSContext *context);

JSValue JS_Eval(JSContext *context, const char *source, size_t length,
                const char *filename, int flags);
int JS_ResolveModule(JSContext *context, JSValueConst module);
JSValue JS_EvalFunction(JSContext *context, JSValue function);
int JS_ExecutePendingJob(JSRuntime *runtime, JSContext **context);
void JS_RunGC(JSRuntime *runtime);

int JS_IsException(JSValueConst value);
int JS_IsUndefined(JSValueConst value);
int JS_IsObject(JSValueConst value);
int JS_IsFunction(JSContext *context, JSValueConst value);
int JS_ToBool(JSContext *context, JSValueConst value);
int JS_ToInt32(JSContext *context, int32_t *result, JSValueConst value);
int JS_ToFloat64(JSContext *context, double *result, JSValueConst value);
const char *JS_ToCString(JSContext *context, JSValueConst value);
void JS_FreeCString(JSContext *context, const char *string);

JSValue JS_NewInt32(JSContext *context, int32_t value);
JSValue JS_NewFloat64(JSContext *context, double value);
JSValue JS_NewBool(JSContext *context, int value);
JSValue JS_NewString(JSContext *context, const char *value);
JSValue JS_NewStringLen(JSContext *context, const char *value, size_t length);
JSValue JS_NewObject(JSContext *context);
JSValue JS_NewArray(JSContext *context);
JSValue JS_NewArrayBufferCopy(JSContext *context, const uint8_t *data, size_t length);
JSValue JS_NewCFunction(JSContext *context, JSCFunction function, const char *name, int length);

JSValue JS_DupValue(JSContext *context, JSValueConst value);
void JS_FreeValue(JSContext *context, JSValue value);
JSValue JS_GetException(JSContext *context);
JSValue JS_Throw(JSContext *context, JSValue value);
JSValue JS_ThrowTypeError(JSContext *context, const char *format, ...);
JSValue JS_ThrowRangeError(JSContext *context, const char *format, ...);
JSValue JS_ThrowReferenceError(JSContext *context, const char *format, ...);
JSValue JS_ThrowInternalError(JSContext *context, const char *format, ...);

JSValue JS_GetGlobalObject(JSContext *context);
JSValue JS_GetProperty(JSContext *context, JSValueConst object, JSAtom name);
JSValue JS_GetPropertyStr(JSContext *context, JSValueConst object, const char *name);
JSValue JS_GetPropertyUint32(JSContext *context, JSValueConst object, uint32_t index);
int JS_SetProperty(JSContext *context, JSValueConst object, JSAtom name, JSValue value);
int JS_SetPropertyStr(JSContext *context, JSValueConst object, const char *name, JSValue value);
int JS_SetPropertyUint32(JSContext *context, JSValueConst object, uint32_t index, JSValue value);
int JS_GetLength(JSContext *context, JSValueConst object, int64_t *length);
JSAtom JS_NewAtom(JSContext *context, const char *name);
void JS_FreeAtom(JSContext *context, JSAtom atom);

JSValue JS_Call(JSContext *context, JSValueConst function, JSValueConst this_value,
                int argc, JSValueConst *argv);
int JS_GetTypedArrayType(JSValueConst value);
JSValue JS_GetTypedArrayBuffer(JSContext *context, JSValueConst value,
                               size_t *byte_offset, size_t *byte_length,
                               size_t *bytes_per_element);
uint8_t *JS_GetArrayBuffer(JSContext *context, size_t *size, JSValueConst value);

JSModuleDef *JS_NewCModule(JSContext *context, const char *name, JSModuleInitFunc init);
int JS_SetModuleExportList(JSContext *context, JSModuleDef *module,
                           const JSCFunctionListEntry *entries, int count);
int JS_AddModuleExportList(JSContext *context, JSModuleDef *module,
                           const JSCFunctionListEntry *entries, int count);
int JS_SetModuleExport(JSContext *context, JSModuleDef *module,
                       const char *name, JSValue value);
int JS_AddModuleExport(JSContext *context, JSModuleDef *module, const char *name);

int JS_PromiseState(JSContext *context, JSValueConst promise);
JSValue JS_PromiseResult(JSContext *context, JSValueConst promise);

#ifdef __cplusplus
}
#endif
