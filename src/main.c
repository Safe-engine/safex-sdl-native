#define SDL_MAIN_USE_CALLBACKS 1

#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <quickjs.h>

#include "js_sdl3.h"

#define MAX_QUEUED_EVENTS 128

typedef struct AppState {
    JSRuntime *runtime;
    JSContext *context;
    bool active;
    bool reset_frame_clock;
    bool logic_running;
    bool logic_done;
    SDL_Thread *logic_thread;

    /* Startup synchronization */
    SDL_Mutex *init_mutex;
    SDL_Condition *init_cond;
    bool init_done;
    bool init_success;

    /* Thread-safe event queue */
    SDL_Mutex *event_mutex;
    SDL_Event event_queue[MAX_QUEUED_EVENTS];
    int event_head;
    int event_tail;
    int event_count;
} AppState;

static void enqueue_event(AppState *state, const SDL_Event *event)
{
    SDL_LockMutex(state->event_mutex);
    if (state->event_count < MAX_QUEUED_EVENTS) {
        SDL_Event queued_ev = *event;
        if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text) {
            queued_ev.text.text = SDL_strdup(event->text.text);
        }
        state->event_queue[state->event_head] = queued_ev;
        state->event_head = (state->event_head + 1) % MAX_QUEUED_EVENTS;
        state->event_count++;
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Event queue full, dropping event %u", event->type);
    }
    SDL_UnlockMutex(state->event_mutex);
}

static void process_queued_events(AppState *state)
{
    SDL_Event events[MAX_QUEUED_EVENTS];
    int count = 0;

    SDL_LockMutex(state->event_mutex);
    while (state->event_count > 0 && count < MAX_QUEUED_EVENTS) {
        events[count++] = state->event_queue[state->event_tail];
        state->event_tail = (state->event_tail + 1) % MAX_QUEUED_EVENTS;
        state->event_count--;
    }
    SDL_UnlockMutex(state->event_mutex);

    JSContext *ctx = state->context;
    for (int i = 0; i < count; i++) {
        const SDL_Event *event = &events[i];
        switch (event->type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                js_call_terminate(ctx);
                break;
            case SDL_EVENT_LOW_MEMORY:
                js_call_low_memory(ctx);
                break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
            case SDL_EVENT_DID_ENTER_BACKGROUND:
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
                state->active = false;
#endif
                js_call_interruption(ctx, 1);
                js_call_pause(ctx);
                js_call_background(ctx);
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
                state->active = false;
                js_call_pause(ctx);
                break;
            case SDL_EVENT_WILL_ENTER_FOREGROUND:
            case SDL_EVENT_DID_ENTER_FOREGROUND:
            case SDL_EVENT_WINDOW_SHOWN:
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                state->active = true;
                state->reset_frame_clock = true;
                js_call_foreground(ctx);
                js_call_resume(ctx);
                js_call_interruption(ctx, 0);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event->button.which != SDL_TOUCH_MOUSEID) {
                    js_call_touchStart(ctx, event->button.x, event->button.y);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (event->motion.which != SDL_TOUCH_MOUSEID && event->motion.state != 0) {
                    js_call_touchMove(ctx, event->motion.x, event->motion.y);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event->button.which != SDL_TOUCH_MOUSEID) {
                    js_call_touchEnd(ctx, event->button.x, event->button.y);
                }
                break;
            case SDL_EVENT_FINGER_DOWN:
                js_call_touchStart(ctx, event->tfinger.x, event->tfinger.y);
                break;
            case SDL_EVENT_FINGER_MOTION:
                js_call_touchMove(ctx, event->tfinger.x, event->tfinger.y);
                break;
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
                js_call_touchEnd(ctx, event->tfinger.x, event->tfinger.y);
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (event->text.text) {
                    js_call_textInput(ctx, event->text.text);
                    SDL_free((void *)event->text.text);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!event->key.repeat) {
                    js_call_keyDown(ctx, SDL_GetKeyName(event->key.key));
                }
                break;
            case SDL_EVENT_KEY_UP:
                js_call_keyUp(ctx, SDL_GetKeyName(event->key.key));
                break;
            case SDL_EVENT_DISPLAY_ORIENTATION: {
                int width;
                int height;
                js_get_window_size(&width, &height);
                js_call_orientation_change(
                    ctx,
                    (SDL_DisplayOrientation)event->display.data1,
                    width,
                    height);
                break;
            }
        }
    }
}

static void print_js_exception(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, exception);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "JS exception: %s", message);
    JS_FreeCString(ctx, message);

    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stack)) {
        const char *trace = JS_ToCString(ctx, stack);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Stack trace:\n%s", trace);
        JS_FreeCString(ctx, trace);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);
}

static bool evaluate_bundle(JSContext *ctx)
{
    const char *base_path = SDL_GetBasePath();
    char *bundle_path = NULL;
    char *development_bundle_path = NULL;
    const char *loaded_bundle_path = bundle_path ? bundle_path : "dist/main.js";
    if (base_path) SDL_asprintf(&bundle_path, "%sdist/main.js", base_path);
    if (base_path) SDL_asprintf(&development_bundle_path, "%s../dist/main.js", base_path);

    size_t length = 0;
    SDL_Log("Loading JavaScript bundle from %s", bundle_path ? bundle_path : "dist/main.js");
    void *contents = SDL_LoadFile(
        bundle_path ? bundle_path : "dist/main.js",
        &length);
    if (contents) {
        loaded_bundle_path = bundle_path;
    } else if (development_bundle_path) {
        SDL_Log("Bundle not found; trying %s", development_bundle_path);
        contents = SDL_LoadFile(development_bundle_path, &length);
        if (contents) loaded_bundle_path = development_bundle_path;
    }
    if (!contents) {
        SDL_Log("Bundle not found; trying dist/main.js from the working directory");
        contents = SDL_LoadFile("dist/main.js", &length);
        if (contents) loaded_bundle_path = "dist/main.js";
    }
    if (!contents) {
        SDL_free(bundle_path);
        SDL_free(development_bundle_path);
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Cannot load dist/main.js: %s (run: bun run build)",
            SDL_GetError());
        return false;
    }

    SDL_Log("Loaded JavaScript bundle from %s (%zu bytes)", loaded_bundle_path, length);
    SDL_free(bundle_path);
    SDL_free(development_bundle_path);

    JSValue module = JS_Eval(
        ctx,
        contents,
        length,
        "dist/main.js",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    SDL_free(contents);

    if (JS_IsException(module)) {
        print_js_exception(ctx);
        JS_FreeValue(ctx, module);
        return false;
    }

    if (JS_ResolveModule(ctx, module) < 0) {
        print_js_exception(ctx);
        JS_FreeValue(ctx, module);
        return false;
    }

    JSValue result = JS_EvalFunction(ctx, module);
    if (JS_IsException(result)) {
        print_js_exception(ctx);
        JS_FreeValue(ctx, result);
        return false;
    }
    SDL_Log("JavaScript module execution returned tag %d", JS_VALUE_GET_TAG(result));
    js_execute_pending_job(JS_GetRuntime(ctx));
    JSPromiseStateEnum promise_state = JS_PromiseState(ctx, result);
    SDL_Log("JavaScript module promise state %d", promise_state);
    if (promise_state == JS_PROMISE_REJECTED) {
        JS_Throw(ctx, JS_PromiseResult(ctx, result));
        print_js_exception(ctx);
        JS_FreeValue(ctx, result);
        return false;
    }
    JS_FreeValue(ctx, result);
    SDL_Log("JavaScript bundle initialized");
    return true;
}

static int run_logic_loop(void *userdata)
{
    AppState *state = userdata;

    state->runtime = JS_NewRuntime();
    state->context = state->runtime ? JS_NewContext(state->runtime) : NULL;
    state->active = true;

    bool success = false;
    if (state->runtime && state->context &&
        js_init_sdl3(state->context) >= 0 &&
        js_enable_render_queue() &&
        evaluate_bundle(state->context)) {
        js_execute_pending_job(state->runtime);
        js_call_onInit(state->context);
        SDL_Log("JavaScript onInit callback completed");
        success = true;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Native startup failed");
    }

    SDL_LockMutex(state->init_mutex);
    state->init_success = success;
    state->init_done = true;
    SDL_SignalCondition(state->init_cond);
    SDL_UnlockMutex(state->init_mutex);

    if (!success) {
        if (state->context) {
            js_sdl3_shutdown(state->context);
            JS_FreeContext(state->context);
        }
        if (state->runtime) {
            JS_FreeRuntime(state->runtime);
        }
        SDL_LockMutex(state->event_mutex);
        state->logic_done = true;
        SDL_UnlockMutex(state->event_mutex);
        return -1;
    }

    const Uint64 TARGET_FRAME_NS = 1000000000ULL / 60ULL;
    Uint64 frame_start_ns = SDL_GetTicksNS();
    Uint64 previous_ticks = frame_start_ns;

    for (;;) {
        SDL_LockMutex(state->event_mutex);
        bool running = state->logic_running;
        SDL_UnlockMutex(state->event_mutex);
        if (!running) break;

        frame_start_ns = SDL_GetTicksNS();
        float delta_time = state->reset_frame_clock
            ? (1.0f / 60.0f)
            : (float)(frame_start_ns - previous_ticks) / 1000000000.0f;
        previous_ticks = frame_start_ns;
        state->reset_frame_clock = false;

        process_queued_events(state);

        js_execute_pending_job(state->runtime);
        if (state->active) {
            js_set_frame_timing(delta_time);
            js_call_onUpdate_dt(state->context, delta_time);
            js_call_onRender(state->context);
        }

        Uint64 frame_elapsed_ns = SDL_GetTicksNS() - frame_start_ns;
        if (frame_elapsed_ns < TARGET_FRAME_NS) {
            SDL_DelayNS(TARGET_FRAME_NS - frame_elapsed_ns);
        } else {
            SDL_Delay(1);
        }
    }

    js_disable_render_queue();
    js_destroy_render_queue();
    js_sdl3_shutdown(state->context);
    JS_FreeContext(state->context);
    JS_FreeRuntime(state->runtime);

    SDL_LockMutex(state->event_mutex);
    state->logic_done = true;
    SDL_UnlockMutex(state->event_mutex);

    return 0;
}

typedef struct MainThreadJob {
    js_main_thread_fn fn;
    void *arg;
    SDL_Mutex *mutex;
    SDL_Condition *cond;
    bool done;
    struct MainThreadJob *next;
} MainThreadJob;

static SDL_Mutex *g_main_job_mutex = NULL;
static MainThreadJob *g_main_job_head = NULL;
static MainThreadJob *g_main_job_tail = NULL;
static SDL_ThreadID g_main_thread_id = 0;

void js_run_on_main_thread(js_main_thread_fn fn, void *arg)
{
    if (!fn) return;

    if (g_main_thread_id == 0 || SDL_GetCurrentThreadID() == g_main_thread_id) {
        fn(arg);
        return;
    }

    MainThreadJob job;
    job.fn = fn;
    job.arg = arg;
    job.mutex = SDL_CreateMutex();
    job.cond = SDL_CreateCondition();
    job.done = false;
    job.next = NULL;

    if (!job.mutex || !job.cond) {
        fn(arg);
        if (job.mutex) SDL_DestroyMutex(job.mutex);
        if (job.cond) SDL_DestroyCondition(job.cond);
        return;
    }

    if (g_main_job_mutex) SDL_LockMutex(g_main_job_mutex);
    if (!g_main_job_head) {
        g_main_job_head = &job;
        g_main_job_tail = &job;
    } else {
        g_main_job_tail->next = &job;
        g_main_job_tail = &job;
    }
    if (g_main_job_mutex) SDL_UnlockMutex(g_main_job_mutex);

    SDL_LockMutex(job.mutex);
    while (!job.done) {
        SDL_WaitCondition(job.cond, job.mutex);
    }
    SDL_UnlockMutex(job.mutex);

    SDL_DestroyCondition(job.cond);
    SDL_DestroyMutex(job.mutex);
}

static void process_main_thread_jobs(void)
{
    if (!g_main_job_mutex) return;

    SDL_LockMutex(g_main_job_mutex);
    MainThreadJob *jobs = g_main_job_head;
    g_main_job_head = NULL;
    g_main_job_tail = NULL;
    SDL_UnlockMutex(g_main_job_mutex);

    while (jobs) {
        MainThreadJob *job = jobs;
        jobs = jobs->next;

        job->fn(job->arg);

        SDL_LockMutex(job->mutex);
        job->done = true;
        SDL_SignalCondition(job->cond);
        SDL_UnlockMutex(job->mutex);
    }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    g_main_thread_id = SDL_GetCurrentThreadID();
    if (!g_main_job_mutex) {
        g_main_job_mutex = SDL_CreateMutex();
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "SDL_Init failed: %s",
            SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_Log("SDL initialized (video driver: %s)", SDL_GetCurrentVideoDriver());

    AppState *state = SDL_calloc(1, sizeof(*state));
    if (!state) {
        SDL_Quit();
        return SDL_APP_FAILURE;
    }

    state->init_mutex = SDL_CreateMutex();
    state->init_cond = SDL_CreateCondition();
    state->event_mutex = SDL_CreateMutex();
    state->logic_running = true;

    if (!state->init_mutex || !state->init_cond || !state->event_mutex) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create mutexes/conditions");
        if (state->init_mutex) SDL_DestroyMutex(state->init_mutex);
        if (state->init_cond) SDL_DestroyCondition(state->init_cond);
        if (state->event_mutex) SDL_DestroyMutex(state->event_mutex);
        SDL_free(state);
        SDL_Quit();
        return SDL_APP_FAILURE;
    }

    state->logic_thread = SDL_CreateThread(run_logic_loop, "game-logic", state);
    if (!state->logic_thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not start game logic thread: %s", SDL_GetError());
        SDL_DestroyMutex(state->init_mutex);
        SDL_DestroyCondition(state->init_cond);
        SDL_DestroyMutex(state->event_mutex);
        SDL_free(state);
        SDL_Quit();
        return SDL_APP_FAILURE;
    }

    SDL_LockMutex(state->init_mutex);
    while (!state->init_done) {
        SDL_UnlockMutex(state->init_mutex);
        process_main_thread_jobs();
        SDL_Delay(1);
        SDL_LockMutex(state->init_mutex);
    }
    bool success = state->init_success;
    SDL_UnlockMutex(state->init_mutex);

    if (!success) {
        SDL_WaitThread(state->logic_thread, NULL);
        SDL_DestroyMutex(state->init_mutex);
        SDL_DestroyCondition(state->init_cond);
        SDL_DestroyMutex(state->event_mutex);
        SDL_free(state);
        SDL_Quit();
        return SDL_APP_FAILURE;
    }

    *appstate = state;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    AppState *state = appstate;

    js_convert_event_to_render_coordinates(event);
    enqueue_event(state, event);

    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_TERMINATING) {
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    (void)appstate;
    process_main_thread_jobs();
    js_render_pending_frame();
    SDL_Delay(1);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    SDL_Log("SDL_AppQuit called with result: %d", result);

    AppState *state = appstate;
    if (state) {
        SDL_LockMutex(state->event_mutex);
        state->logic_running = false;
        SDL_UnlockMutex(state->event_mutex);

        for (;;) {
            bool done;
            SDL_LockMutex(state->event_mutex);
            done = state->logic_done;
            SDL_UnlockMutex(state->event_mutex);
            if (done) break;

            process_main_thread_jobs();
            SDL_Delay(1);
        }

        SDL_WaitThread(state->logic_thread, NULL);

        /* Clean up any leftover unhandled queued events */
        SDL_LockMutex(state->event_mutex);
        while (state->event_count > 0) {
            SDL_Event *ev = &state->event_queue[state->event_tail];
            if (ev->type == SDL_EVENT_TEXT_INPUT && ev->text.text) {
                SDL_free((void *)ev->text.text);
            }
            state->event_tail = (state->event_tail + 1) % MAX_QUEUED_EVENTS;
            state->event_count--;
        }
        SDL_UnlockMutex(state->event_mutex);

        SDL_DestroyMutex(state->init_mutex);
        SDL_DestroyCondition(state->init_cond);
        SDL_DestroyMutex(state->event_mutex);
        SDL_free(state);
    }

    if (g_main_job_mutex) {
        SDL_DestroyMutex(g_main_job_mutex);
        g_main_job_mutex = NULL;
    }
    g_main_thread_id = 0;

    SDL_Quit();

#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
    exit(result == SDL_APP_FAILURE ? 1 : 0);
#endif
}

