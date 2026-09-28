#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <quickjs.h>

#include "js_sdl3.h"

void js_run_on_main_thread(js_main_thread_fn fn, void *arg)
{
    if (fn) {
        fn(arg);
    }
}

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
        "import * as sdl from 'sdl3';"
        "import {"
        "  getTextureHeight, getTextureWidth, isAudioPlaying, isNative,"
        "  loadTextFile, submitCommandBuffer,"
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
        "globalThis.legacyDrawBindingsRemoved = !('drawTexture' in sdl) && !('drawTextureMesh' in sdl) && !('drawTextureRegionRotated' in sdl) && !('drawTextureRotated' in sdl) && !('drawTextureQuad' in sdl) && !('drawRect' in sdl) && !('drawLine' in sdl) && !('drawPoint' in sdl) && !('drawCircle' in sdl) && !('drawPolyline' in sdl) && !('pushClipRect' in sdl) && !('popClipRect' in sdl);"
        "submitCommandBuffer({"
        "  commands: new Int32Array([1, 4, 0]),"
        "  floatBuffer: new Float32Array([0, 0, 10, 10, 0, 0, 0, 0, 0, 10, 20, 30, 40]),"
        "  uintBuffer: new Uint32Array([0, 4294967295, 4294967295]),"
        "  shortBuffer: new Uint16Array([0])"
        "});"
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

    JSValue legacy_draw_bindings_removed = eval_js(
        ctx,
        "globalThis.legacyDrawBindingsRemoved",
        JS_EVAL_TYPE_GLOBAL);
    expect_true(
        "native renderer exposes only command-buffer drawing",
        JS_ToBool(ctx, legacy_draw_bindings_removed));
    JS_FreeValue(ctx, legacy_draw_bindings_removed);
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

static void test_local_storage_persistence(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = runtime ? JS_NewContext(runtime) : NULL;
    expect_true("creates JavaScript context for storage persistence", ctx != NULL);
    if (!ctx) {
        if (runtime) JS_FreeRuntime(runtime);
        return;
    }

    js_init_sdl3(ctx);
    JSValue value = eval_js(
        ctx,
        "localStorage.setItem('__persistence_test__', 'survives restart')",
        JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, value);
    js_sdl3_shutdown(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);

    runtime = JS_NewRuntime();
    ctx = runtime ? JS_NewContext(runtime) : NULL;
    expect_true("recreates JavaScript context for storage persistence", ctx != NULL);
    if (!ctx) {
        if (runtime) JS_FreeRuntime(runtime);
        return;
    }

    js_init_sdl3(ctx);
    value = eval_js(
        ctx,
        "localStorage.getItem('__persistence_test__') === 'survives restart'",
        JS_EVAL_TYPE_GLOBAL);
    expect_true("localStorage survives a native restart", JS_ToBool(ctx, value));
    JS_FreeValue(ctx, value);
    value = eval_js(
        ctx,
        "localStorage.removeItem('__persistence_test__')",
        JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, value);
    js_sdl3_shutdown(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
}

static void test_invalid_binding_arguments(JSContext *ctx)
{
    JSValue module = eval_js(
        ctx,
        "import { createWindow, loadAudio, loadFont, loadTextTexture, "
        "releaseTexture, submitCommandBuffer } from 'sdl3';"
        "globalThis.invalidArgumentsSafe = (() => {"
        "  let createWindowThrows = false;"
        "  try { createWindow(); } catch (_) { createWindowThrows = true; }"
        "  releaseTexture();"
        "  submitCommandBuffer({ commands: new Int32Array([1]), "
        "    floatBuffer: new Float32Array(), uintBuffer: new Uint32Array(), "
        "    shortBuffer: new Uint16Array() });"
        "  return createWindowThrows && loadAudio() === -1 && loadFont() === -1 "
        "    && loadTextTexture() === -1;"
        "})();",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, module);
    if (failures > 0) return;

    JSValue value = eval_js(
        ctx,
        "globalThis.invalidArgumentsSafe",
        JS_EVAL_TYPE_GLOBAL);
    expect_true("invalid native binding arguments are handled safely", JS_ToBool(ctx, value));
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

static void test_network_bindings(JSContext *ctx)
{
    JSValue value = eval_js(
        ctx,
        "typeof fetch === 'function' && "
        "typeof WebSocket === 'function' && "
        "WebSocket.CONNECTING === 0 && WebSocket.OPEN === 1 && "
        "typeof WebSocket.prototype.send === 'function' && "
        "typeof WebSocket.prototype.close === 'function'",
        JS_EVAL_TYPE_GLOBAL);
    expect_true("fetch and WebSocket globals are registered", JS_ToBool(ctx, value));
    JS_FreeValue(ctx, value);
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

static SDL_Texture *create_solid_texture(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_Surface *surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;
    SDL_WriteSurfacePixel(surface, 0, 0, r, g, b, a);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

static void expect_pixel(
    const char *label, SDL_Surface *surface, int x, int r, int g, int b)
{
    Uint8 pr = 0, pg = 0, pb = 0, pa = 0;
    SDL_ReadSurfacePixel(surface, x, 0, &pr, &pg, &pb, &pa);
    if (abs(pr - r) <= 3 && abs(pg - g) <= 3 && abs(pb - b) <= 3) return;
    fprintf(
        stderr,
        "FAIL: %s: expected (%d,%d,%d), got (%d,%d,%d)\n",
        label, r, g, b, pr, pg, pb);
    failures++;
}

static void expect_blend_mode(const char *label, SDL_Texture *texture, SDL_BlendMode expected)
{
    SDL_BlendMode actual = SDL_BLENDMODE_INVALID;
    SDL_GetTextureBlendMode(texture, &actual);
    if (actual == expected) return;
    fprintf(stderr, "FAIL: %s: expected blend 0x%x, got 0x%x\n", label, expected, actual);
    failures++;
}

/*
 * Draws command-buffer primitives over the clear colour (9,15,29) and checks
 * they match the web renderer: the high bit of a texture id means additive, and
 * PMA textures blend premultiplied. SDL's software renderer cannot rasterise
 * premultiplied geometry, so PMA cases check the blend mode chosen instead.
 */
static void test_command_buffer_blend_modes(JSContext *ctx)
{
    SDL_Surface *target = SDL_CreateSurface(6, 1, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *renderer = target ? SDL_CreateSoftwareRenderer(target) : NULL;
    if (!renderer) {
        fprintf(stderr, "FAIL: software renderer: %s\n", SDL_GetError());
        failures++;
        return;
    }
    js_sdl3_test_set_renderer(renderer);
    SDL_Texture *straight_texture = create_solid_texture(renderer, 200, 0, 0, 255);
    SDL_Texture *pma_texture = create_solid_texture(renderer, 64, 64, 64, 128);
    int straight = js_sdl3_test_register_texture(straight_texture, 1, 1, false);
    int pma = js_sdl3_test_register_texture(pma_texture, 1, 1, true);

    JSValue module = eval_js(
        ctx,
        "import { clear, present, submitCommandBuffer } from 'sdl3';"
        "globalThis.ADD = 0x80000000;"
        "globalThis.sprite = (id, x) => ({ op: 1, uints: [id >>> 0, 0xffffffff],"
        "  floats: [x, 0, 1, 1, 0, 0, 0, 0, 0] });"
        "globalThis.quad = (id, x) => ({ op: 2, uints: [id >>> 0, 0xffffffff],"
        "  floats: [x, 0, 0, 0, x + 1, 0, 1, 0, x, 1, 0, 1, x + 1, 1, 1, 1] });"
        "globalThis.mesh = (id, x) => ({ op: 3, uints: [id >>> 0, 0xffffffff, 4, 6],"
        "  floats: [0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, x, 0, 1, 1, 1, 0],"
        "  shorts: [0, 1, 2, 2, 1, 3] });"
        "globalThis.affineMesh = (id, a, b, c, d, tx, ty) => ({ op: 9, uints: [id >>> 0, 0xffffffff, 4, 6],"
        "  floats: [0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, a, b, c, d, tx, ty],"
        "  shorts: [0, 1, 2, 2, 1, 3] });"
        "globalThis.drawFrame = (draws) => {"
        "  clear();"
        "  submitCommandBuffer({"
        "    commands: new Int32Array([...draws.map(d => d.op), 0]),"
        "    uintBuffer: new Uint32Array(draws.flatMap(d => d.uints)),"
        "    floatBuffer: new Float32Array(draws.flatMap(d => d.floats)),"
        "    shortBuffer: new Uint16Array(draws.flatMap(d => d.shorts ?? [])),"
        "  });"
        "  present();"
        "};",
        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(ctx, module);

    char source[512];
    snprintf(
        source, sizeof(source),
        "drawFrame([sprite(%d, 0), sprite(%d | ADD, 1), quad(%d | ADD, 4), mesh(%d | ADD, 5)])",
        straight, straight, straight, straight);
    JS_FreeValue(ctx, eval_js(ctx, source, JS_EVAL_TYPE_GLOBAL));
    expect_pixel("straight sprite, normal blend", target, 0, 200, 0, 0);
    expect_pixel("straight sprite, additive blend", target, 1, 209, 15, 29);
    expect_pixel("straight quad, additive blend", target, 4, 209, 15, 29);
    expect_pixel("straight mesh, additive blend", target, 5, 209, 15, 29);

    /* Affine mesh: the unit square translated to x=2 and stretched to 2px wide. */
    snprintf(source, sizeof(source), "drawFrame([affineMesh(%d, 2, 0, 0, 1, 2, 0)])", straight);
    JS_FreeValue(ctx, eval_js(ctx, source, JS_EVAL_TYPE_GLOBAL));
    expect_pixel("affine mesh leaves pixel 1 untouched", target, 1, 9, 15, 29);
    expect_pixel("affine mesh covers pixel 2", target, 2, 200, 0, 0);
    expect_pixel("affine mesh covers pixel 3", target, 3, 200, 0, 0);
    expect_pixel("affine mesh stops before pixel 4", target, 4, 9, 15, 29);

    snprintf(source, sizeof(source), "drawFrame([sprite(%d, 2)])", pma);
    JS_FreeValue(ctx, eval_js(ctx, source, JS_EVAL_TYPE_GLOBAL));
    expect_blend_mode("PMA sprite blends premultiplied", pma_texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);

    snprintf(source, sizeof(source), "drawFrame([sprite(%d | ADD, 3)])", pma);
    JS_FreeValue(ctx, eval_js(ctx, source, JS_EVAL_TYPE_GLOBAL));
    expect_blend_mode("additive PMA sprite adds premultiplied", pma_texture, SDL_BLENDMODE_ADD_PREMULTIPLIED);

    snprintf(source, sizeof(source), "drawFrame([sprite(%d, 0)])", straight);
    JS_FreeValue(ctx, eval_js(ctx, source, JS_EVAL_TYPE_GLOBAL));
    expect_blend_mode("straight sprite returns to normal blend", straight_texture, SDL_BLENDMODE_BLEND);

    js_sdl3_test_unregister_texture(straight);
    js_sdl3_test_unregister_texture(pma);
    js_sdl3_test_set_renderer(NULL);
    SDL_DestroyTexture(straight_texture);
    SDL_DestroyTexture(pma_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(target);
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
    test_invalid_binding_arguments(ctx);
    test_async_await(ctx);
    test_network_bindings(ctx);
    test_box2d_module_registration(ctx);
    test_command_buffer_blend_modes(ctx);
    test_window_size_defaults();
    test_coordinate_conversion_without_renderer();
    js_sdl3_shutdown(ctx);

    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);

    test_local_storage_persistence();
    SDL_Quit();

    if (failures == 0) {
        printf("native binding tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
