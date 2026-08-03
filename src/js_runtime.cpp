#include "js_runtime.h"

#include <hermes/hermes.h>
#include <jsi/jsi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace jsi = facebook::jsi;

enum ValueTag { kUndefined = 0, kNull = 1, kBool = 2, kValue = 3, kSource = 4 };

struct ValueBox {
    ValueBox(JSContext *owner, jsi::Value &&next) : context(owner), value(std::move(next)) {}
    JSContext *context;
    jsi::Value value;
};

struct JSRuntime {
    std::unique_ptr<facebook::hermes::HermesRuntime> runtime;
    JSContext *context = nullptr;
};

struct JSContext {
    JSRuntime *owner = nullptr;
    std::string exception;
};

struct JSModuleDef {
    JSContext *context;
    std::string name;
    JSValue exports = JS_UNDEFINED;
};

static jsi::Runtime &rt(JSContext *context) {
    return *context->owner->runtime;
}

static JSValue boxed(JSContext *context, jsi::Value &&value) {
    return JSValue{new ValueBox(context, std::move(value)), 0, kValue};
}

static JSValue source_value(std::string source, std::string filename) {
    auto *data = new std::pair<std::string, std::string>(std::move(source), std::move(filename));
    return JSValue{data, 0, kSource};
}

static ValueBox *box(JSValueConst value) {
    return value.tag == kValue ? static_cast<ValueBox *>(value.payload) : nullptr;
}

static jsi::Value to_value(JSContext *context, JSValueConst value) {
    switch (value.tag) {
    case kUndefined: return jsi::Value::undefined();
    case kNull: return jsi::Value::null();
    case kBool: return jsi::Value(value.number != 0);
    case kValue: return jsi::Value(rt(context), box(value)->value);
    default: return jsi::Value::undefined();
    }
}

static JSValue from_value(JSContext *context, jsi::Value &&value) {
    if (value.isUndefined()) return JS_UNDEFINED;
    if (value.isNull()) return JS_NULL;
    if (value.isBool()) return JSValue{nullptr, value.getBool() ? 1.0 : 0.0, kBool};
    if (value.isNumber()) return JSValue{nullptr, value.getNumber(), kValue};
    return boxed(context, std::move(value));
}

static JSValue number_value(double number) {
    return JSValue{nullptr, number, kValue};
}

static jsi::Value number_to_js(JSValueConst value) {
    return jsi::Value(value.number);
}

static jsi::Value resolved_value(JSContext *context, JSValueConst value) {
    return value.tag == kValue && !value.payload ? number_to_js(value) : to_value(context, value);
}

static void set_exception(JSContext *context, const char *kind, const char *format, va_list args) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    context->exception = std::string(kind) + ": " + buffer;
}

static JSValue throw_formatted(JSContext *context, const char *kind, const char *format, va_list args) {
    set_exception(context, kind, format, args);
    return JS_EXCEPTION;
}

static std::string rewrite_native_imports(std::string source) {
    static const std::regex namespace_import(
        R"(import\s+\*\s+as\s+([A-Za-z_$][A-Za-z0-9_$]*)\s+from\s+['"](sdl3|box2d)['"]\s*;?)");
    static const std::regex named_import(
        R"(import\s*\{([^}]*)\}\s*from\s*['"](sdl3|box2d)['"]\s*;?)");
    source = std::regex_replace(
        source, namespace_import,
        "const $1 = globalThis.__safexNativeModules.$2;");
    source = std::regex_replace(
        source, named_import,
        "const {$1} = globalThis.__safexNativeModules.$2;");
    return source;
}

static JSValue evaluate(JSContext *context, const std::string &source, const std::string &filename) {
    try {
        auto buffer = std::make_shared<jsi::StringBuffer>(rewrite_native_imports(source));
        return from_value(context, rt(context).evaluateJavaScript(buffer, filename));
    } catch (const jsi::JSError &error) {
        context->exception = error.getMessage();
    } catch (const std::exception &error) {
        context->exception = error.what();
    }
    return JS_EXCEPTION;
}

extern "C" {

JSRuntime *JS_NewRuntime(void) {
    auto *runtime = new JSRuntime();
    runtime->runtime = facebook::hermes::makeHermesRuntime();
    return runtime;
}

void JS_FreeRuntime(JSRuntime *runtime) { delete runtime; }

JSContext *JS_NewContext(JSRuntime *runtime) {
    if (!runtime || !runtime->runtime) return nullptr;
    auto *context = new JSContext();
    context->owner = runtime;
    runtime->context = context;
    return context;
}

void JS_FreeContext(JSContext *context) {
    if (context && context->owner) context->owner->context = nullptr;
    delete context;
}

JSRuntime *JS_GetRuntime(JSContext *context) { return context ? context->owner : nullptr; }

JSValue JS_Eval(JSContext *context, const char *source, size_t length, const char *filename, int flags) {
    std::string text(source, length);
    if (flags & JS_EVAL_FLAG_COMPILE_ONLY) return source_value(std::move(text), filename ? filename : "<script>");
    return evaluate(context, text, filename ? filename : "<script>");
}

int JS_ResolveModule(JSContext *, JSValueConst) { return 0; }

JSValue JS_EvalFunction(JSContext *context, JSValue function) {
    if (function.tag != kSource) return function;
    auto *data = static_cast<std::pair<std::string, std::string> *>(function.payload);
    JSValue result = evaluate(context, data->first, data->second);
    delete data;
    return result;
}

int JS_ExecutePendingJob(JSRuntime *runtime, JSContext **context) {
    if (!runtime || !runtime->runtime) return 0;
    try {
        runtime->runtime->drainMicrotasks();
        return 0;
    } catch (const jsi::JSError &error) {
        if (runtime->context) runtime->context->exception = error.getMessage();
        if (context) *context = runtime->context;
        return -1;
    }
}

void JS_RunGC(JSRuntime *) {}
int JS_IsException(JSValueConst value) { return value.tag == -1; }
int JS_IsUndefined(JSValueConst value) { return value.tag == kUndefined; }
int JS_IsObject(JSValueConst value) {
    return value.tag == kValue && value.payload && box(value)->value.isObject();
}

int JS_IsFunction(JSContext *context, JSValueConst value) {
    try {
        return value.tag == kValue && value.payload && box(value)->value.isObject() &&
            box(value)->value.asObject(rt(context)).isFunction(rt(context));
    } catch (...) { return 0; }
}

int JS_ToBool(JSContext *context, JSValueConst value) {
    if (value.tag == kUndefined || value.tag == kNull) return 0;
    if (value.tag == kBool) return value.number != 0;
    if (value.tag == kValue && !value.payload) return value.number != 0;
    try {
        jsi::Value next = to_value(context, value);
        if (next.isBool()) return next.getBool();
        if (next.isNumber()) return next.getNumber() != 0;
        return !next.isNull() && !next.isUndefined();
    } catch (...) { return 0; }
}

int JS_ToInt32(JSContext *context, int32_t *result, JSValueConst value) {
    if (!result) return -1;
    try {
        if (value.tag == kValue && !value.payload) *result = static_cast<int32_t>(value.number);
        else if (value.tag == kBool) *result = value.number != 0;
        else {
            jsi::Value next = to_value(context, value);
            if (next.isNumber()) *result = static_cast<int32_t>(next.getNumber());
            else return -1;
        }
        return 0;
    } catch (...) { return -1; }
}

int JS_ToFloat64(JSContext *context, double *result, JSValueConst value) {
    if (!result) return -1;
    try {
        if (value.tag == kValue && !value.payload) *result = value.number;
        else if (value.tag == kBool) *result = value.number;
        else {
            jsi::Value next = to_value(context, value);
            if (!next.isNumber()) return -1;
            *result = next.getNumber();
        }
        return 0;
    } catch (...) { return -1; }
}

const char *JS_ToCString(JSContext *context, JSValueConst value) {
    try {
        std::string string;
        if (value.tag == kUndefined) string = "undefined";
        else if (value.tag == kNull) string = "null";
        else if (value.tag == kBool) string = value.number ? "true" : "false";
        else if (value.tag == kValue && !value.payload) string = std::to_string(value.number);
        else {
            jsi::Value next = to_value(context, value);
            if (next.isString()) string = next.asString(rt(context)).utf8(rt(context));
            else if (next.isNumber()) string = std::to_string(next.getNumber());
            else return nullptr;
        }
        return strdup(string.c_str());
    } catch (...) { return nullptr; }
}

void JS_FreeCString(JSContext *, const char *string) { free((void *)string); }
JSValue JS_NewInt32(JSContext *, int32_t value) { return number_value(value); }
JSValue JS_NewFloat64(JSContext *, double value) { return number_value(value); }
JSValue JS_NewBool(JSContext *, int value) { return JSValue{nullptr, value != 0 ? 1.0 : 0.0, kBool}; }

JSValue JS_NewString(JSContext *context, const char *value) {
    return boxed(context, jsi::Value(jsi::String::createFromUtf8(rt(context), value ? value : "")));
}

JSValue JS_NewStringLen(JSContext *context, const char *value, size_t length) {
    return boxed(context, jsi::Value(jsi::String::createFromUtf8(
        rt(context), reinterpret_cast<const uint8_t *>(value), length)));
}

JSValue JS_NewObject(JSContext *context) { return boxed(context, jsi::Value(jsi::Object(rt(context)))); }
JSValue JS_NewArray(JSContext *context) { return boxed(context, jsi::Value(jsi::Array(rt(context), 0))); }

JSValue JS_NewArrayBufferCopy(JSContext *context, const uint8_t *data, size_t length) {
    class Buffer final : public jsi::MutableBuffer {
     public:
        explicit Buffer(size_t length) : contents(length) {}
        size_t size() const override { return contents.size(); }
        uint8_t *data() override { return contents.data(); }
        std::vector<uint8_t> contents;
    };
    auto buffer = std::make_shared<Buffer>(length);
    if (length) memcpy(buffer->data(), data, length);
    return boxed(context, jsi::Value(jsi::ArrayBuffer(rt(context), buffer)));
}

JSValue JS_NewCFunction(JSContext *context, JSCFunction function, const char *name, int length) {
    auto host = [context, function](jsi::Runtime &runtime, const jsi::Value &this_value,
                                    const jsi::Value *arguments, size_t count) -> jsi::Value {
        std::vector<JSValue> argv;
        argv.reserve(count);
        for (size_t i = 0; i < count; i++) argv.push_back(boxed(context, jsi::Value(runtime, arguments[i])));
        JSValue this_arg = boxed(context, jsi::Value(runtime, this_value));
        JSValue result = function(context, this_arg, static_cast<int>(count), argv.data());
        JS_FreeValue(context, this_arg);
        for (JSValue value : argv) JS_FreeValue(context, value);
        if (JS_IsException(result)) throw jsi::JSError(runtime, context->exception);
        jsi::Value converted = resolved_value(context, result);
        JS_FreeValue(context, result);
        return converted;
    };
    auto id = jsi::PropNameID::forUtf8(rt(context), name ? name : "native");
    return boxed(context, jsi::Value(jsi::Function::createFromHostFunction(rt(context), id, length, std::move(host))));
}

JSValue JS_DupValue(JSContext *context, JSValueConst value) {
    if (value.tag == kValue && value.payload) return boxed(context, jsi::Value(rt(context), box(value)->value));
    return value;
}

void JS_FreeValue(JSContext *, JSValue value) {
    if (value.tag == kValue && value.payload) delete box(value);
    if (value.tag == kSource && value.payload) delete static_cast<std::pair<std::string, std::string> *>(value.payload);
}

JSValue JS_GetException(JSContext *context) {
    std::string message = context->exception.empty() ? "JavaScript error" : context->exception;
    context->exception.clear();
    return JS_NewString(context, message.c_str());
}

JSValue JS_Throw(JSContext *context, JSValue value) {
    const char *message = JS_ToCString(context, value);
    context->exception = message ? message : "JavaScript error";
    JS_FreeCString(context, message);
    return JS_EXCEPTION;
}

JSValue JS_ThrowTypeError(JSContext *context, const char *format, ...) {
    va_list args; va_start(args, format); JSValue result = throw_formatted(context, "TypeError", format, args); va_end(args); return result;
}
JSValue JS_ThrowRangeError(JSContext *context, const char *format, ...) {
    va_list args; va_start(args, format); JSValue result = throw_formatted(context, "RangeError", format, args); va_end(args); return result;
}
JSValue JS_ThrowReferenceError(JSContext *context, const char *format, ...) {
    va_list args; va_start(args, format); JSValue result = throw_formatted(context, "ReferenceError", format, args); va_end(args); return result;
}
JSValue JS_ThrowInternalError(JSContext *context, const char *format, ...) {
    va_list args; va_start(args, format); JSValue result = throw_formatted(context, "Error", format, args); va_end(args); return result;
}

JSValue JS_GetGlobalObject(JSContext *context) { return boxed(context, jsi::Value(rt(context).global())); }

static JSValue get_property(JSContext *context, JSValueConst object, const char *name) {
    try { return from_value(context, to_value(context, object).asObject(rt(context)).getProperty(rt(context), name)); }
    catch (const std::exception &error) { context->exception = error.what(); return JS_EXCEPTION; }
}
JSValue JS_GetProperty(JSContext *context, JSValueConst object, JSAtom name) { return get_property(context, object, name); }
JSValue JS_GetPropertyStr(JSContext *context, JSValueConst object, const char *name) { return get_property(context, object, name); }
JSValue JS_GetPropertyUint32(JSContext *context, JSValueConst object, uint32_t index) {
    try { return from_value(context, to_value(context, object).asObject(rt(context)).getProperty(rt(context), std::to_string(index).c_str())); }
    catch (const std::exception &error) { context->exception = error.what(); return JS_EXCEPTION; }
}

static int set_property(JSContext *context, JSValueConst object, const char *name, JSValue value) {
    try {
        to_value(context, object).asObject(rt(context)).setProperty(rt(context), name, resolved_value(context, value));
        JS_FreeValue(context, value);
        return 0;
    }
    catch (const std::exception &error) { context->exception = error.what(); return -1; }
}
int JS_SetProperty(JSContext *context, JSValueConst object, JSAtom name, JSValue value) { return set_property(context, object, name, value); }
int JS_SetPropertyStr(JSContext *context, JSValueConst object, const char *name, JSValue value) { return set_property(context, object, name, value); }
int JS_SetPropertyUint32(JSContext *context, JSValueConst object, uint32_t index, JSValue value) { return set_property(context, object, std::to_string(index).c_str(), value); }

int JS_GetLength(JSContext *context, JSValueConst object, int64_t *length) {
    if (!length) return -1;
    JSValue result = JS_GetPropertyStr(context, object, "length");
    int32_t next = 0;
    int status = JS_ToInt32(context, &next, result);
    JS_FreeValue(context, result);
    *length = next;
    return status;
}

JSAtom JS_NewAtom(JSContext *, const char *name) { return strdup(name); }
void JS_FreeAtom(JSContext *, JSAtom atom) { free((void *)atom); }

JSValue JS_Call(JSContext *context, JSValueConst function, JSValueConst this_value, int argc, JSValueConst *argv) {
    try {
        auto callable = to_value(context, function).asObject(rt(context)).asFunction(rt(context));
        std::vector<jsi::Value> arguments;
        arguments.reserve(argc);
        for (int i = 0; i < argc; i++) arguments.push_back(resolved_value(context, argv[i]));
        jsi::Value this_argument = resolved_value(context, this_value);
        return from_value(context, rt(context).call(
            callable, this_argument, arguments.data(), arguments.size()));
    } catch (const jsi::JSError &error) { context->exception = error.getMessage(); }
    catch (const std::exception &error) { context->exception = error.what(); }
    return JS_EXCEPTION;
}

int JS_GetTypedArrayType(JSValueConst value) {
    if (value.tag != kValue || !value.payload) return 0;
    JSContext *context = box(value)->context;
    try {
        auto object = box(value)->value.asObject(rt(context));
        if (!object.isTypedArray(rt(context))) return 0;
        auto constructor = object.getProperty(rt(context), "constructor").asObject(rt(context));
        auto name = constructor.getProperty(rt(context), "name").asString(rt(context)).utf8(rt(context));
        if (name == "Uint16Array") return JS_TYPED_ARRAY_UINT16;
        if (name == "Uint32Array") return JS_TYPED_ARRAY_UINT32;
        if (name == "Int32Array") return JS_TYPED_ARRAY_INT32;
        if (name == "Float32Array") return JS_TYPED_ARRAY_FLOAT32;
    } catch (...) {
    }
    return 0;
}
JSValue JS_GetTypedArrayBuffer(JSContext *context, JSValueConst value, size_t *byte_offset, size_t *byte_length, size_t *bytes_per_element) {
    try {
        auto typed = to_value(context, value).asObject(rt(context)).getTypedArray(rt(context));
        if (byte_offset) *byte_offset = typed.byteOffset(rt(context));
        if (byte_length) *byte_length = typed.byteLength(rt(context));
        if (bytes_per_element) *bytes_per_element = typed.length(rt(context)) ? typed.byteLength(rt(context)) / typed.length(rt(context)) : 0;
        return boxed(context, jsi::Value(typed.buffer(rt(context))));
    } catch (const std::exception &error) { context->exception = error.what(); return JS_EXCEPTION; }
}

uint8_t *JS_GetArrayBuffer(JSContext *context, size_t *size, JSValueConst value) {
    try {
        auto buffer = to_value(context, value).asObject(rt(context)).getArrayBuffer(rt(context));
        if (size) *size = buffer.size(rt(context));
        return buffer.data(rt(context));
    } catch (const std::exception &error) { context->exception = error.what(); return nullptr; }
}

static JSValue modules(JSContext *context) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue table = JS_GetPropertyStr(context, global, "__safexNativeModules");
    if (JS_IsUndefined(table)) {
        table = JS_NewObject(context);
        JS_SetPropertyStr(context, global, "__safexNativeModules", JS_DupValue(context, table));
    }
    JS_FreeValue(context, global);
    return table;
}

JSModuleDef *JS_NewCModule(JSContext *context, const char *name, JSModuleInitFunc init) {
    auto *module = new JSModuleDef{context, name, JS_NewObject(context)};
    if (init && init(context, module) < 0) { delete module; return nullptr; }
    JSValue table = modules(context);
    JS_SetPropertyStr(context, table, name, JS_DupValue(context, module->exports));
    JS_FreeValue(context, table);
    return module;
}

int JS_SetModuleExportList(JSContext *context, JSModuleDef *module, const JSCFunctionListEntry *entries, int count) {
    if (!module) return -1;
    for (int i = 0; i < count; i++) {
        JS_SetPropertyStr(context, module->exports, entries[i].name,
                          JS_NewCFunction(context, entries[i].function, entries[i].name, entries[i].length));
    }
    return 0;
}
int JS_AddModuleExportList(JSContext *, JSModuleDef *, const JSCFunctionListEntry *, int) { return 0; }
int JS_SetModuleExport(JSContext *context, JSModuleDef *module, const char *name, JSValue value) { return module ? JS_SetPropertyStr(context, module->exports, name, value) : -1; }
int JS_AddModuleExport(JSContext *, JSModuleDef *, const char *) { return 0; }
int JS_PromiseState(JSContext *, JSValueConst) { return JS_PROMISE_FULFILLED; }
JSValue JS_PromiseResult(JSContext *, JSValueConst) { return JS_UNDEFINED; }

} // extern "C"
