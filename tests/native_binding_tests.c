#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <quickjs.h>

#include "js_sdl3.h"

static int failures = 0;

static void print_exception(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, exception);
    fprintf(stderr, "JS exception: %s\n", message ? message : "<unknown>");
    JS_FreeCString(ctx, message);

    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stack)) {
        const char *trace = JS_ToCString(ctx, stack);
        fprintf(stderr, "Stack trace:\n%s\n", trace ? trace : "<none>");
        JS_FreeCString(ctx, trace);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);
}

static void expect_true(const char *label, bool condition)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", label);
    failures++;
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual == expected) return;
    fprintf(stderr, "FAIL: %s: expected %d, got %d\n", label, expected, actual);
    failures++;
}

static void expect_string(const char *label, const char *actual, const char *expected)
{
    if (actual && strcmp(actual, expected) == 0) return;
    fprintf(
        stderr,
        "FAIL: %s: expected %s, got %s\n",
        label,
        expected,
        actual ? actual : "<null>");
    failures++;
}

static JSValue eval_js(JSContext *ctx, const char *source, int flags)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "native-binding-test.js", flags);
    if (JS_IsException(result)) {
        print_exception(ctx);
        failures++;
    }
    return result;
}

static void test_callbacks_and_invalid_resource_paths(JSContext *ctx)
{
    const char *source =
        "import {"
        "  drawTexture, drawTextureMesh, drawTextureRegionRotated, drawTextureRotated,"
        "  getTextureHeight, getTextureWidth, isAudioPlaying, isNative,"
        "  loadTextFile,"
        "  onBackground, onForeground, onInit, onInterruption, onLowMemory,"
        "  onOrientationChange, onPause, onRender, onResume, onTerminate,"
        "  onTouchEnd, onTouchMove, onTouchStart, onUpdate, pauseAudio,"
        "  releaseAudio, releaseFont, releaseTexture, resumeAudio,"
        "  setAudioVolume, stopAudio, updateAudio"
        "} from 'sdl3';"
        "globalThis.calls = [];"
        "globalThis.nativeRuntime = isNative;"
        "globalThis.invalids = ["
        "  getTextureWidth(-1),"
        "  getTextureHeight(999),"
        "  isAudioPlaying(12),"
        "  loadTextFile('__missing__.json')"
        "];"
        "releaseTexture(-1);"
        "releaseFont(-1);"
        "releaseAudio(-1);"
        "stopAudio(-1);"
        "pauseAudio(-1);"
        "resumeAudio(-1);"
        "setAudioVolume(-1, 0.5);"
        "updateAudio();"
        "drawTexture(-1, 1, 2);"
        "drawTextureMesh(-1, new Float32Array([0, 0, 1, 0, 0, 1]),"
        "  new Float32Array([0, 0, 1, 0, 0, 1]), new Uint16Array([0, 1, 2]),"
        "  255, 255, 255, 255, 10, 20, 2, 3, 1, 0);"
        "drawTextureRotated(-1, 1, 2, 3, 4, 5, 6, 7, 1, 0);"
        "drawTextureRegionRotated(-1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 1);"
        "onInit(() => calls.push(['init']));"
        "onUpdate((dt) => calls.push(['update', Math.round(dt * 1000)]));"
        "onRender(() => calls.push(['render']));"
        "onTouchStart((x, y) => calls.push(['start', x, y]));"
        "onTouchMove((x, y) => calls.push(['move', x, y]));"
        "onTouchEnd((x, y) => calls.push(['end', x, y]));"
        "onPause(() => calls.push(['pause']));"
        "onResume(() => calls.push(['resume']));"
        "onBackground(() => calls.push(['background']));"
        "onForeground(() => calls.push(['foreground']));"
        "onInterruption((active) => calls.push(['interruption', active]));"
        "onLowMemory(() => calls.push(['lowMemory']));"
        "onOrientationChange((orientation, width, height) => "
        "  calls.push(['orientation', orientation, width, height]));"
        "onTerminate(() => calls.push(['terminate']));";

    JSValue result = eval_js(ctx, source, JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, result);
    if (failures > 0) return;

    js_call_onInit(ctx);
    js_call_onUpdate_dt(ctx, 0.016f);
    js_call_onRender(ctx);
    js_call_touchStart(ctx, 10.5f, 20.25f);
    js_call_touchMove(ctx, 30.0f, 40.0f);
    js_call_touchEnd(ctx, 50.0f, 60.0f);
    js_call_pause(ctx);
    js_call_resume(ctx);
    js_call_background(ctx);
    js_call_foreground(ctx);
    js_call_interruption(ctx, 1);
    js_call_low_memory(ctx);
    js_call_orientation_change(ctx, SDL_ORIENTATION_LANDSCAPE, 1280, 720);
    js_call_terminate(ctx);

    JSValue calls = eval_js(
        ctx,
        "JSON.stringify(globalThis.calls)",
        JS_EVAL_TYPE_GLOBAL);
    const char *calls_json = JS_ToCString(ctx, calls);
    expect_string(
        "registered callbacks receive native dispatch arguments",
        calls_json,
        "[[\"init\"],[\"update\",16],[\"render\"],[\"start\",10.5,20.25],"
        "[\"move\",30,40],[\"end\",50,60],[\"pause\"],[\"resume\"],"
        "[\"background\"],[\"foreground\"],[\"interruption\",true],"
        "[\"lowMemory\"],[\"orientation\",1,1280,720],[\"terminate\"]]");
    JS_FreeCString(ctx, calls_json);
    JS_FreeValue(ctx, calls);

    JSValue invalids = eval_js(
        ctx,
        "JSON.stringify(globalThis.invalids)",
        JS_EVAL_TYPE_GLOBAL);
    const char *invalids_json = JS_ToCString(ctx, invalids);
    expect_string(
        "invalid resource queries return neutral values",
        invalids_json,
        "[0,0,false,null]");
    JS_FreeCString(ctx, invalids_json);
    JS_FreeValue(ctx, invalids);

    JSValue native_runtime = eval_js(
        ctx,
        "globalThis.nativeRuntime",
        JS_EVAL_TYPE_GLOBAL);
    expect_true(
        "sdl3 identifies the native runtime",
        JS_ToBool(ctx, native_runtime));
    JS_FreeValue(ctx, native_runtime);
}

static void test_development_resource_path(JSContext *ctx)
{
    JSValue module = eval_js(
        ctx,
        "import { loadTextFile } from 'sdl3';"
        "globalThis.resourceFound = loadTextFile('Json/items.json') !== null;",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, module);

    JSValue value = eval_js(
        ctx,
        "globalThis.resourceFound",
        JS_EVAL_TYPE_GLOBAL);
    expect_true(
        "native build resolves resources from the repository root",
        JS_ToBool(ctx, value));
    JS_FreeValue(ctx, value);
}

static void test_mp3_audio_loading(JSContext *ctx)
{
    JSValue module = eval_js(
        ctx,
        "import { loadAudio, releaseAudio } from 'sdl3';"
        "globalThis.mp3AudioId = loadAudio('Audio/Button.mp3');"
        "if (globalThis.mp3AudioId >= 0) releaseAudio(globalThis.mp3AudioId);",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, module);
    if (failures > 0) return;

    JSValue value = eval_js(ctx, "globalThis.mp3AudioId", JS_EVAL_TYPE_GLOBAL);
    int id = -1;
    JS_ToInt32(ctx, &id, value);
    expect_true("native binding loads MP3 audio", id >= 0);
    JS_FreeValue(ctx, value);
}

static void test_local_storage(JSContext *ctx)
{
    JSValue value = eval_js(
        ctx,
        "localStorage.setItem('key', 'value');"
        "localStorage.getItem('key') === 'value'",
        JS_EVAL_TYPE_GLOBAL);
    expect_true("localStorage stores string values", JS_ToBool(ctx, value));
    JS_FreeValue(ctx, value);
}

static void test_async_await(JSContext *ctx)
{
    const char *module_source =
        "globalThis.topLevelAwaitValue = await Promise.resolve('ready');";
    JSValue module = JS_Eval(
        ctx,
        module_source,
        strlen(module_source),
        "native-async-await-test.mjs",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    expect_true("top-level await module compiles", !JS_IsException(module));
    if (JS_IsException(module)) {
        print_exception(ctx);
        JS_FreeValue(ctx, module);
        return;
    }

    expect_int("top-level await module resolves", JS_ResolveModule(ctx, module), 0);
    JSValue result = JS_EvalFunction(ctx, module);
    expect_true("top-level await returns a promise", JS_IsObject(result));
    js_execute_pending_job(JS_GetRuntime(ctx));
    expect_int(
        "top-level await promise is fulfilled",
        JS_PromiseState(ctx, result),
        JS_PROMISE_FULFILLED);
    JS_FreeValue(ctx, result);

    JSValue value = eval_js(
        ctx,
        "globalThis.topLevelAwaitValue",
        JS_EVAL_TYPE_GLOBAL);
    const char *value_string = JS_ToCString(ctx, value);
    expect_string("top-level await resumes", value_string, "ready");
    JS_FreeCString(ctx, value_string);
    JS_FreeValue(ctx, value);

    JSValue callback_module = eval_js(
        ctx,
        "import { onInit } from 'sdl3';"
        "globalThis.asyncCallbackValue = 'pending';"
        "async function getProfile() { return {}; }"
        "async function loadAssets() { await Promise.resolve(); }"
        "onInit(async () => {"
        "  await getProfile();"
        "  await loadAssets();"
        "  globalThis.asyncCallbackValue = 'ready';"
        "});",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, callback_module);
    if (failures > 0) return;

    js_call_onInit(ctx);
    JSValue callback_value = eval_js(
        ctx,
        "globalThis.asyncCallbackValue",
        JS_EVAL_TYPE_GLOBAL);
    const char *callback_value_string = JS_ToCString(ctx, callback_value);
    expect_string(
        "async lifecycle callback drains chained awaits before returning to native",
        callback_value_string,
        "ready");
    JS_FreeCString(ctx, callback_value_string);
    JS_FreeValue(ctx, callback_value);
}

static void test_window_size_defaults(void)
{
    int width = 0;
    int height = 0;

    js_get_window_size(&width, &height);
    expect_int("default window width", width, 1280);
    expect_int("default window height", height, 720);
    expect_int("js_get_win_w default", js_get_win_w(), 1280);
    expect_int("js_get_win_h default", js_get_win_h(), 720);
}

static void test_coordinate_conversion_without_renderer(void)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 15.0f;
    event.motion.y = 25.0f;

    js_convert_event_to_render_coordinates(&event);
    expect_true(
        "coordinate conversion is a no-op without a renderer",
        fabsf(event.motion.x - 15.0f) < 0.001f &&
            fabsf(event.motion.y - 25.0f) < 0.001f);
}

static void test_box2d_module_registration(JSContext *ctx)
{
#ifdef JS_SDL_ENABLE_BOX2D_MODULE
#ifdef JS_SDL_HAS_BOX2D
    JSValue result = eval_js(
        ctx,
        "import {"
        "  createBody, createBoxShape, createWorld, destroyWorld, getDebugDraw"
        "} from 'box2d';"
        "globalThis.box2dCreateWorldType = typeof createWorld;"
        "globalThis.box2dCreateWorld = createWorld;"
        "globalThis.box2dCreateBody = createBody;"
        "globalThis.box2dCreateBoxShape = createBoxShape;"
        "globalThis.box2dDestroyWorld = destroyWorld;"
        "globalThis.box2dGetDebugDraw = getDebugDraw;",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, result);
    if (failures > 0) return;

    JSValue type = eval_js(ctx, "globalThis.box2dCreateWorldType", JS_EVAL_TYPE_GLOBAL);
    const char *type_string = JS_ToCString(ctx, type);
    expect_string("box2d module exports createWorld", type_string, "function");
    JS_FreeCString(ctx, type_string);
    JS_FreeValue(ctx, type);

    JSValue debug_count = eval_js(
        ctx,
        "globalThis.box2dDebugPrimitiveCount = (() => {"
        "  try {"
        "    const world = globalThis.box2dCreateWorld({ x: 0, y: 0 });"
        "    const body = globalThis.box2dCreateBody(world, 0, { x: 1, y: 2 }, 0, 1, 1);"
        "    globalThis.box2dCreateBoxShape(body, 0.5, 0.5, { x: 0, y: 0 }, 0, 1, 0.2, 0, false);"
        "    const count = globalThis.box2dGetDebugDraw(world, 32).length;"
        "    globalThis.box2dDestroyWorld(world);"
        "    return count;"
        "  } catch (_) {"
        "    return -1;"
        "  }"
        "})()",
        JS_EVAL_TYPE_GLOBAL);
    int count = 0;
    JS_ToInt32(ctx, &count, debug_count);
    expect_true("box2d debug draw returns primitives when linked", count != 0);
    JS_FreeValue(ctx, debug_count);
#else
    JSValue result = eval_js(
        ctx,
        "import { createWorld, getDebugDraw } from 'box2d';"
        "globalThis.box2dCreateWorldType = typeof createWorld;"
        "globalThis.box2dUnavailableThrows = (() => {"
        "  try {"
        "    createWorld({ x: 0, y: 0 });"
        "    return false;"
        "  } catch (_) {"
        "    return true;"
        "  }"
        "})();"
        "globalThis.box2dGetDebugDrawType = typeof getDebugDraw;",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, result);
    if (failures > 0) return;

    JSValue type = eval_js(ctx, "globalThis.box2dCreateWorldType", JS_EVAL_TYPE_GLOBAL);
    const char *type_string = JS_ToCString(ctx, type);
    expect_string("box2d fallback exports createWorld", type_string, "function");
    JS_FreeCString(ctx, type_string);
    JS_FreeValue(ctx, type);

    JSValue get_debug_draw_type = eval_js(ctx, "globalThis.box2dGetDebugDrawType", JS_EVAL_TYPE_GLOBAL);
    const char *get_debug_draw_type_string = JS_ToCString(ctx, get_debug_draw_type);
    expect_string("box2d fallback exports getDebugDraw", get_debug_draw_type_string, "function");
    JS_FreeCString(ctx, get_debug_draw_type_string);
    JS_FreeValue(ctx, get_debug_draw_type);

    JSValue throws = eval_js(ctx, "globalThis.box2dUnavailableThrows", JS_EVAL_TYPE_GLOBAL);
    expect_true("box2d fallback throws when not linked", JS_ToBool(ctx, throws));
    JS_FreeValue(ctx, throws);
#endif
#else
    (void)ctx;
#endif
}

int main(void)
{
    if (!SDL_Init(0)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = runtime ? JS_NewContext(runtime) : NULL;
    if (!runtime || !ctx) {
        fprintf(stderr, "Failed to create QuickJS runtime\n");
        if (ctx) JS_FreeContext(ctx);
        if (runtime) JS_FreeRuntime(runtime);
        SDL_Quit();
        return 1;
    }

    js_init_sdl3(ctx);
    test_callbacks_and_invalid_resource_paths(ctx);
    test_development_resource_path(ctx);
    test_mp3_audio_loading(ctx);
    test_local_storage(ctx);
    test_async_await(ctx);
    test_box2d_module_registration(ctx);
    test_window_size_defaults();
    test_coordinate_conversion_without_renderer();
    js_sdl3_shutdown(ctx);

    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    SDL_Quit();

    if (failures == 0) {
        printf("native binding tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
