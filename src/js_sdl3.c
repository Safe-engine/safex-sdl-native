#include "js_sdl3.h"
#ifdef JS_SDL_ENABLE_BOX2D_MODULE
#include "js_box2d.h"
#endif
#include <SDL3_image/SDL_image.h>
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- global state --- */
static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static int g_draw_calls = 0;
static int g_vertices = 0;
static double g_fps = 0;
static double g_frame_time_ms = 0;
static int g_win_w = 1280;
static int g_win_h = 720;
static SDL_RendererLogicalPresentation g_resolution_policy =
    SDL_LOGICAL_PRESENTATION_LETTERBOX;
static FT_Library g_ft_library = NULL;

#define MAX_TEXTURES 512
typedef enum TextureKind
{
  TEXTURE_FILE,
  TEXTURE_TEXT
} TextureKind;

typedef struct TextureAsset
{
  SDL_Texture *texture;
  char *key;
  int refs;
  int width;
  int height;
  TextureKind kind;
  int font_id;
  bool pma;
} TextureAsset;

static TextureAsset g_textures[MAX_TEXTURES];

static SDL_Vertex *g_mesh_vertices = NULL;
static int *g_mesh_indices = NULL;
static int g_mesh_vertex_capacity = 0;
static int g_mesh_index_capacity = 0;
static SDL_Texture *g_batch_texture = NULL;
static int g_batch_vertex_count = 0;
static int g_batch_index_count = 0;
static SDL_Vertex *g_input_vertices = NULL;
static int *g_input_indices = NULL;
static int g_input_vertex_capacity = 0;
static int g_input_index_capacity = 0;

typedef enum RenderBufferState
{
  RENDER_BUFFER_FREE = 0,
  RENDER_BUFFER_WRITING,
  RENDER_BUFFER_READY,
  RENDER_BUFFER_READING
} RenderBufferState;

typedef struct RenderFrame
{
  int32_t *commands;
  size_t commands_len;
  size_t commands_cap;

  float *floats;
  size_t floats_len;
  size_t floats_cap;

  uint32_t *uints;
  size_t uints_len;
  size_t uints_cap;

  uint16_t *shorts;
  size_t shorts_len;
  size_t shorts_cap;

  Uint64 timestamp_ns;
  RenderBufferState state;
} RenderFrame;

#define RENDER_BUFFER_COUNT 3
static RenderFrame g_render_buffers[RENDER_BUFFER_COUNT] = {0};
static SDL_Mutex *g_render_queue_mutex = NULL;
static SDL_Condition *g_render_queue_ready = NULL;
static bool g_render_queue_enabled = false;
static _Thread_local const RenderFrame *g_executing_render_frame = NULL;

typedef struct RenderStateCache
{
  SDL_BlendMode blend_mode;
  bool has_blend_mode;
  Uint8 r, g, b, a;
  bool has_draw_color;
  SDL_Rect clip_rect;
  bool clip_enabled;
} RenderStateCache;

static RenderStateCache g_state_cache = {0};

static void reset_render_state_cache(void)
{
  SDL_zero(g_state_cache);
}

static void cached_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
  if (!g_state_cache.has_blend_mode || g_state_cache.blend_mode != blendMode)
  {
    SDL_SetRenderDrawBlendMode(renderer, blendMode);
    g_state_cache.blend_mode = blendMode;
    g_state_cache.has_blend_mode = true;
  }
}

static void cached_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
  if (!g_state_cache.has_draw_color ||
      g_state_cache.r != r || g_state_cache.g != g ||
      g_state_cache.b != b || g_state_cache.a != a)
  {
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    g_state_cache.r = r;
    g_state_cache.g = g;
    g_state_cache.b = b;
    g_state_cache.a = a;
    g_state_cache.has_draw_color = true;
  }
}

static void cached_SetRenderClipRect(SDL_Renderer *renderer, const SDL_Rect *rect)
{
  if (!rect)
  {
    if (g_state_cache.clip_enabled)
    {
      SDL_SetRenderClipRect(renderer, NULL);
      g_state_cache.clip_enabled = false;
    }
  }
  else
  {
    if (!g_state_cache.clip_enabled ||
        g_state_cache.clip_rect.x != rect->x || g_state_cache.clip_rect.y != rect->y ||
        g_state_cache.clip_rect.w != rect->w || g_state_cache.clip_rect.h != rect->h)
    {
      SDL_SetRenderClipRect(renderer, rect);
      g_state_cache.clip_rect = *rect;
      g_state_cache.clip_enabled = true;
    }
  }
}

#if JS_SDL_ENABLE_PROFILING
static uint32_t g_prof_frame_count = 0;
static Uint64 g_prof_logic_total = 0;
static Uint64 g_prof_logic_max = 0;
static Uint64 g_prof_js_total = 0;

static Uint64 g_prof_exec_total = 0;
static Uint64 g_prof_exec_max = 0;
static Uint64 g_prof_present_total = 0;

static Uint64 g_prof_interval_total = 0;
static Uint64 g_prof_interval_max = 0;

static uint32_t g_prof_cmd_total = 0;
static uint32_t g_prof_buffer_grows = 0;
static uint32_t g_prof_dropped_frames = 0;

void js_prof_record_logic(Uint64 logic_ns, Uint64 js_update_ns, Uint64 serialize_ns, Uint64 wait_ns)
{
  (void)serialize_ns;
  (void)wait_ns;
  g_prof_logic_total += logic_ns;
  if (logic_ns > g_prof_logic_max)
    g_prof_logic_max = logic_ns;
  g_prof_js_total += js_update_ns;
}

void js_prof_record_render(Uint64 exec_ns, Uint64 present_ns, uint32_t cmd_count, Uint64 interval_ns)
{
  g_prof_exec_total += exec_ns;
  if (exec_ns > g_prof_exec_max)
    g_prof_exec_max = exec_ns;
  g_prof_present_total += present_ns;
  g_prof_cmd_total += cmd_count;

  if (interval_ns > 0)
  {
    g_prof_interval_total += interval_ns;
    if (interval_ns > g_prof_interval_max)
      g_prof_interval_max = interval_ns;
  }

  g_prof_frame_count++;
  if (g_prof_frame_count >= 180)
  {
    double logic_avg_ms = (double)g_prof_logic_total / (g_prof_frame_count * 1e6);
    double logic_max_ms = (double)g_prof_logic_max / 1e6;
    double js_avg_ms = (double)g_prof_js_total / (g_prof_frame_count * 1e6);

    double exec_avg_ms = (double)g_prof_exec_total / (g_prof_frame_count * 1e6);
    double exec_max_ms = (double)g_prof_exec_max / 1e6;
    double present_avg_ms = (double)g_prof_present_total / (g_prof_frame_count * 1e6);

    double interval_avg_ms = (double)g_prof_interval_total / (g_prof_frame_count * 1e6);
    double fps = interval_avg_ms > 0.0 ? 1000.0 / interval_avg_ms : 0.0;

    uint32_t cmd_avg = g_prof_cmd_total / g_prof_frame_count;

    SDL_Log("[PROFILER %u frames] FPS: %.1f | Logic avg/max: %.2f/%.2f ms (JS: %.2f ms) | Render exec avg/max: %.2f/%.2f ms | Present avg: %.2f ms | Cmds/frame: %u | Grows: %u | Dropped: %u",
            g_prof_frame_count, fps, logic_avg_ms, logic_max_ms, js_avg_ms,
            exec_avg_ms, exec_max_ms, present_avg_ms, cmd_avg,
            g_prof_buffer_grows, g_prof_dropped_frames);

    g_prof_frame_count = 0;
    g_prof_logic_total = 0;
    g_prof_logic_max = 0;
    g_prof_js_total = 0;
    g_prof_exec_total = 0;
    g_prof_exec_max = 0;
    g_prof_present_total = 0;
    g_prof_interval_total = 0;
    g_prof_interval_max = 0;
    g_prof_cmd_total = 0;
  }
}

void js_prof_record_buffer_grow(void)
{
  g_prof_buffer_grows++;
}

void js_prof_record_dropped_frame(void)
{
  g_prof_dropped_frames++;
}
#endif

#define MAX_FONTS 64
typedef struct FontAsset
{
  FT_Face face;
  void *data;
  char *path;
  int ptsize;
  int refs;
} FontAsset;

static FontAsset g_fonts[MAX_FONTS];

#define MAX_AUDIO_ASSETS 128
typedef struct AudioAsset
{
  Uint8 *data;
  Uint32 length;
  SDL_AudioSpec spec;
  char *path;
  int refs;
} AudioAsset;

static AudioAsset g_audio_assets[MAX_AUDIO_ASSETS];

#define MAX_AUDIO_VOICES 32
typedef struct AudioVoice
{
  SDL_AudioStream *stream;
  int audio_id;
  bool loop;
  bool paused;
  Uint64 started_at;
  Uint64 paused_at;
  Uint64 paused_duration;
  Uint64 duration;
} AudioVoice;

static AudioVoice g_audio_voices[MAX_AUDIO_VOICES];

#define MAX_STORAGE_ENTRIES 128
typedef struct StorageEntry
{
  char *key;
  char *value;
} StorageEntry;

static StorageEntry g_storage[MAX_STORAGE_ENTRIES];

#define MAX_CLIP_DEPTH 32
static SDL_Rect g_clip_stack[MAX_CLIP_DEPTH];
static int g_clip_depth = 0;

/* --- JS callbacks --- */
static JSValue g_onInit = JS_UNDEFINED;
static JSValue g_onUpdate = JS_UNDEFINED;
static JSValue g_onRender = JS_UNDEFINED;

static JSValue g_touchStart = JS_UNDEFINED;
static JSValue g_touchMove = JS_UNDEFINED;
static JSValue g_touchEnd = JS_UNDEFINED;
static JSValue g_textInput = JS_UNDEFINED;
static JSValue g_keyDown = JS_UNDEFINED;
static JSValue g_keyUp = JS_UNDEFINED;

static JSValue g_onPause = JS_UNDEFINED;
static JSValue g_onResume = JS_UNDEFINED;
static JSValue g_onBackground = JS_UNDEFINED;
static JSValue g_onForeground = JS_UNDEFINED;
static JSValue g_onInterruption = JS_UNDEFINED;
static JSValue g_onLowMemory = JS_UNDEFINED;
static JSValue g_onOrientationChange = JS_UNDEFINED;
static JSValue g_onTerminate = JS_UNDEFINED;

/* ---- helpers ---- */

static char *copy_string(const char *value)
{
  size_t length = strlen(value) + 1;
  char *copy = malloc(length);
  if (copy)
    memcpy(copy, value, length);
  return copy;
}

static StorageEntry *find_storage_entry(const char *key)
{
  for (int i = 0; i < MAX_STORAGE_ENTRIES; i++)
  {
    if (g_storage[i].key && strcmp(g_storage[i].key, key) == 0)
    {
      return &g_storage[i];
    }
  }
  return NULL;
}

static StorageEntry *find_free_storage_entry(void)
{
  for (int i = 0; i < MAX_STORAGE_ENTRIES; i++)
  {
    if (!g_storage[i].key)
      return &g_storage[i];
  }
  return NULL;
}

static bool has_resource_prefix(const char *path)
{
  return strncmp(path, "res/", 4) == 0 || strncmp(path, "res\\", 4) == 0;
}

static char *resource_prefixed_path(const char *path)
{
  if (!path || !*path || path[0] == '/' || strstr(path, "://") ||
      has_resource_prefix(path))
  {
    return NULL;
  }

  size_t length = strlen("res/") + strlen(path) + 1;
  char *resolved = malloc(length);
  if (!resolved)
    return NULL;
  snprintf(resolved, length, "res/%s", path);
  return resolved;
}

static bool path_exists(const char *path)
{
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  fclose(file);
  return true;
}

static char *resolve_resource_path(const char *path)
{
  if (!path || !*path || path[0] == '/' || strstr(path, "://"))
  {
    return copy_string(path);
  }

  const char *base_path = SDL_GetBasePath();
  if (!base_path)
    return copy_string(path);

  const char *resource_prefix = has_resource_prefix(path) ? "" : "res/";
  size_t length =
      strlen(base_path) +
      strlen(resource_prefix) +
      strlen(path) +
      1;
  char *resolved = malloc(length);
  if (!resolved)
    return NULL;
  snprintf(resolved, length, "%s%s%s", base_path, resource_prefix, path);

  if (path_exists(resolved))
    return resolved;

  /* `native:dev` launches from native/build while resources stay at ../res. */
  size_t development_length =
      strlen(base_path) + strlen("../../") + strlen(resource_prefix) +
      strlen(path) + 1;
  char *development_path = malloc(development_length);
  if (!development_path)
    return resolved;
  snprintf(
      development_path,
      development_length,
      "%s../../%s%s",
      base_path,
      resource_prefix,
      path);
  if (path_exists(development_path))
  {
    free(resolved);
    return development_path;
  }
  free(development_path);
  return resolved;
}

static int find_free_texture_slot(void)
{
  for (int i = 0; i < MAX_TEXTURES; i++)
  {
    if (!g_textures[i].texture)
      return i;
  }
  return -1;
}

static int find_free_font_slot(void)
{
  for (int i = 0; i < MAX_FONTS; i++)
  {
    if (!g_fonts[i].face)
      return i;
  }
  return -1;
}

static int valid_texture_id(int id)
{
  return id >= 0 && id < MAX_TEXTURES && g_textures[id].texture;
}

static int valid_font_id(int id)
{
  return id >= 0 && id < MAX_FONTS && g_fonts[id].face;
}

static int valid_audio_id(int id)
{
  return id >= 0 && id < MAX_AUDIO_ASSETS && g_audio_assets[id].data;
}

static int valid_audio_voice_id(int id)
{
  return id >= 0 && id < MAX_AUDIO_VOICES && g_audio_voices[id].stream;
}

static int find_free_audio_slot(void)
{
  for (int i = 0; i < MAX_AUDIO_ASSETS; i++)
  {
    if (!g_audio_assets[i].data)
      return i;
  }
  return -1;
}

static int find_free_audio_voice_slot(void)
{
  for (int i = 0; i < MAX_AUDIO_VOICES; i++)
  {
    if (!g_audio_voices[i].stream)
      return i;
  }
  return -1;
}

typedef struct LoadTextureTaskArgs
{
  const char *path;
  SDL_Texture *texture;
  bool pma;
} LoadTextureTaskArgs;

static void load_texture_main_thread(void *userdata)
{
  LoadTextureTaskArgs *args = userdata;
  if (!g_renderer || !args->path)
  {
    args->texture = NULL;
    return;
  }
  args->texture = IMG_LoadTexture(g_renderer, args->path);
  if (args->texture && args->pma)
  {
    SDL_SetTextureBlendMode(args->texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
  }
}

static SDL_Texture *load_texture_on_main_thread(const char *path, bool pma)
{
  if (!path)
    return NULL;
  LoadTextureTaskArgs task_args = {.path = path, .texture = NULL, .pma = pma};
  js_run_on_main_thread(load_texture_main_thread, &task_args);
  return task_args.texture;
}

typedef struct CreateTextureFromSurfaceTaskArgs
{
  SDL_Surface *surface;
  SDL_Texture *texture;
} CreateTextureFromSurfaceTaskArgs;

static void create_texture_from_surface_main_thread(void *userdata)
{
  CreateTextureFromSurfaceTaskArgs *args = userdata;
  if (!g_renderer || !args->surface)
  {
    args->texture = NULL;
    return;
  }
  args->texture = SDL_CreateTextureFromSurface(g_renderer, args->surface);
}

static SDL_Texture *create_texture_from_surface_on_main_thread(SDL_Surface *surface)
{
  if (!surface)
    return NULL;
  CreateTextureFromSurfaceTaskArgs task_args = {.surface = surface, .texture = NULL};
  js_run_on_main_thread(create_texture_from_surface_main_thread, &task_args);
  return task_args.texture;
}

static void destroy_texture_main_thread(void *userdata)
{
  SDL_Texture *texture = userdata;
  if (texture)
  {
    SDL_DestroyTexture(texture);
  }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
bool js_is_main_thread(void)
{
  return true;
}

static void destroy_texture_on_main_thread(SDL_Texture *texture)
{
  if (!texture)
    return;
  js_run_on_main_thread(destroy_texture_main_thread, texture);
}

#define MAX_PENDING_DESTROY_TEXTURES 64
static void release_font_id(int id);
static SDL_Texture *g_pending_destroy_textures[MAX_PENDING_DESTROY_TEXTURES];
static int g_pending_destroy_texture_count = 0;
static SDL_Mutex *g_pending_destroy_texture_mutex = NULL;

static void queue_deferred_texture_destroy(SDL_Texture *texture)
{
  if (!texture)
    return;
  if (js_is_main_thread())
  {
    SDL_DestroyTexture(texture);
    return;
  }
  if (!g_pending_destroy_texture_mutex)
    return;
  SDL_LockMutex(g_pending_destroy_texture_mutex);
  if (g_pending_destroy_texture_count < MAX_PENDING_DESTROY_TEXTURES)
  {
    g_pending_destroy_textures[g_pending_destroy_texture_count++] = texture;
    texture = NULL;
  }
  SDL_UnlockMutex(g_pending_destroy_texture_mutex);
  if (texture)
  {
    destroy_texture_on_main_thread(texture);
  }
}

static void flush_deferred_texture_destroys(void)
{
  if (!g_pending_destroy_texture_mutex)
    return;
  SDL_Texture *to_destroy[MAX_PENDING_DESTROY_TEXTURES];
  int count = 0;

  SDL_LockMutex(g_pending_destroy_texture_mutex);
  count = g_pending_destroy_texture_count;
  for (int i = 0; i < count; i++)
  {
    to_destroy[i] = g_pending_destroy_textures[i];
  }
  g_pending_destroy_texture_count = 0;
  SDL_UnlockMutex(g_pending_destroy_texture_mutex);

  for (int i = 0; i < count; i++)
  {
    if (to_destroy[i])
      SDL_DestroyTexture(to_destroy[i]);
  }
}

void js_collect_retired_textures(void)
{
  if (!g_render_queue_mutex)
    return;

  SDL_LockMutex(g_render_queue_mutex);
  for (int i = 0; i < RENDER_BUFFER_COUNT; i++)
  {
    if (g_render_buffers[i].state != RENDER_BUFFER_FREE)
    {
      SDL_UnlockMutex(g_render_queue_mutex);
      return;
    }
  }
  SDL_UnlockMutex(g_render_queue_mutex);

  for (int i = 0; i < MAX_TEXTURES; i++)
  {
    TextureAsset *asset = &g_textures[i];
    if (!asset->texture || asset->refs != 0)
      continue;

    SDL_Texture *texture = asset->texture;
    char *key = asset->key;
    TextureKind kind = asset->kind;
    int font_id = asset->font_id;
    memset(asset, 0, sizeof(*asset));
    queue_deferred_texture_destroy(texture);
    free(key);
    if (kind == TEXTURE_TEXT)
      release_font_id(font_id);
  }
}

#define MAX_IDLE_AUDIO_STREAMS 16
static SDL_AudioStream *g_idle_audio_streams[MAX_IDLE_AUDIO_STREAMS];
static int g_idle_audio_stream_count = 0;

static SDL_AudioStream *acquire_audio_stream(const SDL_AudioSpec *spec)
{
  if (g_idle_audio_stream_count > 0)
  {
    SDL_AudioStream *stream = g_idle_audio_streams[--g_idle_audio_stream_count];
    if (SDL_SetAudioStreamFormat(stream, spec, NULL))
    {
      SDL_ClearAudioStream(stream);
      return stream;
    }
    SDL_DestroyAudioStream(stream);
  }
  return SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
      spec,
      NULL,
      NULL);
}

static void release_audio_stream(SDL_AudioStream *stream)
{
  if (!stream)
    return;
  SDL_ClearAudioStream(stream);
  if (g_idle_audio_stream_count < MAX_IDLE_AUDIO_STREAMS)
  {
    g_idle_audio_streams[g_idle_audio_stream_count++] = stream;
  }
  else
  {
    SDL_DestroyAudioStream(stream);
  }
}

static void free_idle_audio_streams(void)
{
  for (int i = 0; i < g_idle_audio_stream_count; i++)
  {
    if (g_idle_audio_streams[i])
    {
      SDL_DestroyAudioStream(g_idle_audio_streams[i]);
    }
  }
  g_idle_audio_stream_count = 0;
}

static void destroy_renderer_main_thread(void *userdata)
{
  (void)userdata;
  if (g_renderer)
  {
    SDL_DestroyRenderer(g_renderer);
    g_renderer = NULL;
  }
}

static void destroy_window_main_thread(void *userdata)
{
  (void)userdata;
  if (g_window)
  {
    SDL_DestroyWindow(g_window);
    g_window = NULL;
  }
}

static void start_text_input_main_thread(void *userdata)
{
  SDL_StartTextInput(userdata);
}

static void stop_text_input_main_thread(void *userdata)
{
  SDL_StopTextInput(userdata);
}

static void release_font_id(int id);
static void release_audio_id(int id);

static void release_texture_id(int id)
{
  if (!valid_texture_id(id))
    return;
  TextureAsset *asset = &g_textures[id];
  if (asset->refs <= 0)
    return;
  if (--asset->refs > 0)
    return;
  /* Keep the slot alive until every queued frame has finished using its id. */
}

static void release_font_id(int id)
{
  if (!valid_font_id(id))
    return;
  FontAsset *asset = &g_fonts[id];
  if (--asset->refs > 0)
    return;
  FT_Done_Face(asset->face);
  SDL_free(asset->data);
  free(asset->path);
  memset(asset, 0, sizeof(*asset));
}

static void release_audio_id(int id)
{
  if (!valid_audio_id(id))
    return;
  AudioAsset *asset = &g_audio_assets[id];
  if (--asset->refs > 0)
    return;
  SDL_free(asset->data);
  free(asset->path);
  memset(asset, 0, sizeof(*asset));
}

static void destroy_audio_voice(int id)
{
  if (!valid_audio_voice_id(id))
    return;
  AudioVoice *voice = &g_audio_voices[id];
  int audio_id = voice->audio_id;
  release_audio_stream(voice->stream);
  memset(voice, 0, sizeof(*voice));
  release_audio_id(audio_id);
}

static void js_print_exception(JSContext *ctx)
{
  JSValue exc = JS_GetException(ctx);
  const char *str = JS_ToCString(ctx, exc);
  fprintf(stderr, "JS exception: %s\n", str);
  JS_FreeCString(ctx, str);

  JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
  if (!JS_IsUndefined(stack))
  {
    const char *trace = JS_ToCString(ctx, stack);
    fprintf(stderr, "Stack trace:\n%s\n", trace);
    JS_FreeCString(ctx, trace);
  }
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, exc);
}

static void js_complete_callback(JSContext *ctx, JSValue result)
{
  if (JS_IsException(result))
    js_print_exception(ctx);
  JS_FreeValue(ctx, result);
  js_execute_pending_job(JS_GetRuntime(ctx));
}

static void js_call_void(JSContext *ctx, JSValue func)
{
  if (JS_IsUndefined(func))
    return;
  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
  js_complete_callback(ctx, ret);
}

static void js_call_touch(JSContext *ctx, JSValue func, float x, float y)
{
  if (JS_IsUndefined(func))
    return;
  JSValue argv[2];
  argv[0] = JS_NewFloat64(ctx, (double)x);
  argv[1] = JS_NewFloat64(ctx, (double)y);
  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 2, argv);
  JS_FreeValue(ctx, argv[0]);
  JS_FreeValue(ctx, argv[1]);
  js_complete_callback(ctx, ret);
}

static void js_call_string(JSContext *ctx, JSValue func, const char *value)
{
  if (JS_IsUndefined(func) || !value)
    return;
  JSValue arg = JS_NewString(ctx, value);
  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
  JS_FreeValue(ctx, arg);
  js_complete_callback(ctx, ret);
}

static void js_call_bool(JSContext *ctx, JSValue func, int value)
{
  if (JS_IsUndefined(func))
    return;
  JSValue arg = JS_NewBool(ctx, value);
  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &arg);
  JS_FreeValue(ctx, arg);
  js_complete_callback(ctx, ret);
}

static void js_call_orientation(
    JSContext *ctx, JSValue func, SDL_DisplayOrientation orientation,
    int width, int height)
{
  if (JS_IsUndefined(func))
    return;
  JSValue argv[3] = {
      JS_NewInt32(ctx, (int)orientation),
      JS_NewInt32(ctx, width),
      JS_NewInt32(ctx, height),
  };
  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 3, argv);
  for (int i = 0; i < 3; i++)
    JS_FreeValue(ctx, argv[i]);
  js_complete_callback(ctx, ret);
}

/* --- Binding: createWindow(title, w, h) --- */
static SDL_RendererLogicalPresentation js_resolution_policy(
    JSContext *ctx,
    JSValueConst value)
{
  if (JS_IsUndefined(value))
    return SDL_LOGICAL_PRESENTATION_LETTERBOX;
  const char *policy = JS_ToCString(ctx, value);
  if (!policy)
    return SDL_LOGICAL_PRESENTATION_LETTERBOX;

  SDL_RendererLogicalPresentation result = SDL_LOGICAL_PRESENTATION_LETTERBOX;
  if (strcmp(policy, "overscan") == 0)
  {
    result = SDL_LOGICAL_PRESENTATION_OVERSCAN;
  }
  else if (strcmp(policy, "stretch") == 0)
  {
    result = SDL_LOGICAL_PRESENTATION_STRETCH;
  }
  else if (strcmp(policy, "integer-scale") == 0)
  {
    result = SDL_LOGICAL_PRESENTATION_INTEGER_SCALE;
  }

  JS_FreeCString(ctx, policy);
  return result;
}

static SDL_FRect js_presentation_rect(int screen_w, int screen_h)
{
  float width = (float)screen_w;
  float height = (float)screen_h;
  if (g_resolution_policy != SDL_LOGICAL_PRESENTATION_STRETCH)
  {
    float scale_x = (float)screen_w / (float)g_win_w;
    float scale_y = (float)screen_h / (float)g_win_h;
    float scale = g_resolution_policy == SDL_LOGICAL_PRESENTATION_OVERSCAN
                      ? SDL_max(scale_x, scale_y)
                      : SDL_min(scale_x, scale_y);
    if (g_resolution_policy == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)
    {
      scale = SDL_max(1.0f, (float)((int)scale));
    }
    width = (float)g_win_w * scale;
    height = (float)g_win_h * scale;
  }

  return (SDL_FRect){
      ((float)screen_w - width) * 0.5f,
      ((float)screen_h - height) * 0.5f,
      width,
      height,
  };
}

typedef struct CreateWindowTaskArgs
{
  const char *title;
  int window_w;
  int window_h;
  SDL_WindowFlags window_flags;
  bool success;
  const char *error_msg;
} CreateWindowTaskArgs;

static void create_window_main_thread(void *userdata)
{
  CreateWindowTaskArgs *args = userdata;
  g_window = SDL_CreateWindow(args->title, args->window_w, args->window_h, args->window_flags);
  if (!g_window)
  {
    args->error_msg = SDL_GetError();
    return;
  }
  g_renderer = SDL_CreateRenderer(g_window, NULL);
  if (!g_renderer)
  {
    args->error_msg = SDL_GetError();
    SDL_DestroyWindow(g_window);
    g_window = NULL;
    return;
  }
  if (!SDL_SetRenderVSync(g_renderer, 1))
  {
    SDL_LogWarn(
        SDL_LOG_CATEGORY_RENDER,
        "Could not enable renderer vsync: %s",
        SDL_GetError());
  }

  SDL_SetWindowPosition(
      g_window,
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED);
  if (!SDL_ShowWindow(g_window))
  {
    args->error_msg = SDL_GetError();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    g_renderer = NULL;
    g_window = NULL;
    return;
  }
  SDL_RaiseWindow(g_window);
  SDL_Log("SDL window is visible and raised");
  SDL_SetRenderLogicalPresentation(
      g_renderer,
      g_win_w,
      g_win_h,
      g_resolution_policy);
  args->success = true;
}

static JSValue js_createWindow(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 3)
    return JS_ThrowTypeError(ctx, "createWindow requires title, width, and height");
  const char *title = JS_ToCString(ctx, argv[0]);
  if (!title)
    return JS_EXCEPTION;
  JS_ToInt32(ctx, &g_win_w, argv[1]);
  JS_ToInt32(ctx, &g_win_h, argv[2]);
  if (g_win_w <= 0 || g_win_h <= 0)
  {
    JS_FreeCString(ctx, title);
    return JS_ThrowRangeError(ctx, "window dimensions must be positive");
  }
  g_resolution_policy = js_resolution_policy(
      ctx,
      argc > 3 ? argv[3] : JS_UNDEFINED);

  int window_w = g_win_w;
  int window_h = g_win_h;
  SDL_Rect display_bounds;
  if (SDL_GetDisplayUsableBounds(
          SDL_GetPrimaryDisplay(),
          &display_bounds))
  {
    float scale_x = (float)display_bounds.w * 0.9f / (float)g_win_w;
    float scale_y = (float)display_bounds.h * 0.9f / (float)g_win_h;
    float scale = SDL_min(1.0f, SDL_min(scale_x, scale_y));
    window_w = (int)((float)g_win_w * scale);
    window_h = (int)((float)g_win_h * scale);
    SDL_Log(
        "Creating window: logical=%dx%d physical=%dx%d display=%dx%d",
        g_win_w,
        g_win_h,
        window_w,
        window_h,
        display_bounds.w,
        display_bounds.h);
  }
  else
  {
    SDL_Log(
        "Creating window: logical=%dx%d physical=%dx%d (display bounds unavailable: %s)",
        g_win_w,
        g_win_h,
        window_w,
        window_h,
        SDL_GetError());
  }

  SDL_WindowFlags window_flags = 0;
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
  window_flags = SDL_WINDOW_FULLSCREEN;
#endif

  CreateWindowTaskArgs task_args = {
      .title = title,
      .window_w = window_w,
      .window_h = window_h,
      .window_flags = window_flags,
      .success = false,
      .error_msg = NULL};
  js_run_on_main_thread(create_window_main_thread, &task_args);
  JS_FreeCString(ctx, title);

  if (!task_args.success)
  {
    return JS_ThrowInternalError(
        ctx,
        "SDL_CreateWindow failed: %s",
        task_args.error_msg ? task_args.error_msg : "Unknown error");
  }

  return JS_UNDEFINED;
}

typedef struct ViewportMetricsTaskArgs
{
  int screen_w;
  int screen_h;
  SDL_FRect viewport;
  float safe_x;
  float safe_y;
  float safe_right;
  float safe_bottom;
} ViewportMetricsTaskArgs;

static void get_viewport_metrics_main_thread(void *userdata)
{
  ViewportMetricsTaskArgs *args = userdata;
  args->screen_w = g_win_w;
  args->screen_h = g_win_h;
  args->viewport = js_presentation_rect(args->screen_w, args->screen_h);
  args->safe_x = 0.0f;
  args->safe_y = 0.0f;
  args->safe_right = (float)g_win_w;
  args->safe_bottom = (float)g_win_h;
  if (!g_window || !g_renderer)
    return;

  SDL_Rect safe = {0, 0, args->screen_w, args->screen_h};
  SDL_GetWindowSize(g_window, &args->screen_w, &args->screen_h);
  args->viewport = js_presentation_rect(args->screen_w, args->screen_h);
  if (!SDL_GetWindowSafeArea(g_window, &safe))
    safe = (SDL_Rect){0, 0, args->screen_w, args->screen_h};
  SDL_RenderCoordinatesFromWindow(g_renderer, (float)safe.x, (float)safe.y,
                                  &args->safe_x, &args->safe_y);
  SDL_RenderCoordinatesFromWindow(g_renderer, (float)(safe.x + safe.w),
                                  (float)(safe.y + safe.h),
                                  &args->safe_right, &args->safe_bottom);
  args->safe_x = SDL_clamp(args->safe_x, 0.0f, (float)g_win_w);
  args->safe_y = SDL_clamp(args->safe_y, 0.0f, (float)g_win_h);
  args->safe_right = SDL_clamp(args->safe_right, 0.0f, (float)g_win_w);
  args->safe_bottom = SDL_clamp(args->safe_bottom, 0.0f, (float)g_win_h);
}

/* --- Binding: getViewportMetrics() --- */
static JSValue js_getViewportMetrics(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  (void)argc;
  (void)argv;

  ViewportMetricsTaskArgs metrics;
  js_run_on_main_thread(get_viewport_metrics_main_thread, &metrics);

  double values[] = {
      g_win_w,
      g_win_h,
      metrics.screen_w,
      metrics.screen_h,
      metrics.viewport.x,
      metrics.viewport.y,
      metrics.viewport.w,
      metrics.viewport.h,
      metrics.safe_x,
      metrics.safe_y,
      metrics.safe_right - metrics.safe_x,
      metrics.safe_bottom - metrics.safe_y,
  };
  JSValue result = JS_NewArray(ctx);
  for (uint32_t i = 0; i < 12; i++)
  {
    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, values[i]));
  }
  return result;
}

/* --- Binding: getWinSize() --- */
typedef struct WindowSizeTaskArgs
{
  int width;
  int height;
} WindowSizeTaskArgs;

static void get_window_size_in_pixels_main_thread(void *userdata)
{
  WindowSizeTaskArgs *args = userdata;
  args->width = g_win_w;
  args->height = g_win_h;
  if (g_window && SDL_GetWindowSizeInPixels(g_window, &args->width, &args->height))
  {
    return;
  }
  if (g_window && SDL_GetWindowSize(g_window, &args->width, &args->height))
  {
    return;
  }
}

static void get_window_size_in_pixels(int *width, int *height)
{
  WindowSizeTaskArgs args;
  js_run_on_main_thread(get_window_size_in_pixels_main_thread, &args);
  *width = args.width;
  *height = args.height;
}

static JSValue js_getWinSize(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  (void)argc;
  (void)argv;

  int width;
  int height;
  get_window_size_in_pixels(&width, &height);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, width));
  JS_SetPropertyStr(ctx, result, "height", JS_NewInt32(ctx, height));
  return result;
}

static void *load_file_contents(const char *path, size_t *length)
{
  char *resolved_path = resolve_resource_path(path);
  void *contents = resolved_path
                       ? SDL_LoadFile(resolved_path, length)
                       : NULL;
  if (!contents && resolved_path && strcmp(resolved_path, path) != 0)
  {
    contents = SDL_LoadFile(path, length);
  }
  char *prefixed_path = resource_prefixed_path(path);
  if (!contents && prefixed_path)
  {
    contents = SDL_LoadFile(prefixed_path, length);
  }
  free(prefixed_path);
  free(resolved_path);
  return contents;
}

/* --- Binding: loadTextFile(path) -> string|null --- */
static JSValue js_loadTextFile(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_NULL;

  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  size_t length = 0;
  void *contents = load_file_contents(path, &length);
  JS_FreeCString(ctx, path);

  if (!contents)
    return JS_NULL;
  JSValue result = JS_NewStringLen(ctx, contents, length);
  SDL_free(contents);
  return result;
}

/* --- Binding: loadBinaryFile(path) -> ArrayBuffer|null --- */
static JSValue js_loadBinaryFile(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_NULL;

  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  size_t length = 0;
  void *contents = load_file_contents(path, &length);
  JS_FreeCString(ctx, path);

  if (!contents)
    return JS_NULL;
  JSValue result = JS_NewArrayBufferCopy(ctx, contents, length);
  SDL_free(contents);
  return result;
}

/* --- Binding: loadTexture(path, pma?) → id --- */
static JSValue js_loadTexture(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_NewInt32(ctx, -1);
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  bool pma = false;
  if (argc >= 2)
  {
    pma = JS_ToBool(ctx, argv[1]);
  }

  for (int i = 0; i < MAX_TEXTURES; i++)
  {
    TextureAsset *asset = &g_textures[i];
    if (asset->texture && asset->kind == TEXTURE_FILE &&
        asset->pma == pma &&
        strcmp(asset->key, path) == 0)
    {
      asset->refs++;
      JS_FreeCString(ctx, path);
      return JS_NewInt32(ctx, i);
    }
  }

  int id = find_free_texture_slot();
  if (id < 0)
  {
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  char *resolved_path = resolve_resource_path(path);
  SDL_Texture *tex = resolved_path
                         ? load_texture_on_main_thread(resolved_path, pma)
                         : NULL;
  if (!tex && resolved_path && strcmp(resolved_path, path) != 0)
  {
    tex = load_texture_on_main_thread(path, pma);
  }
  char *prefixed_path = resource_prefixed_path(path);
  if (!tex && prefixed_path)
  {
    tex = load_texture_on_main_thread(prefixed_path, pma);
  }
  free(prefixed_path);
  free(resolved_path);
  if (!tex)
  {
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }
  char *key = copy_string(path);
  if (!key)
  {
    destroy_texture_on_main_thread(tex);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  TextureAsset *asset = &g_textures[id];
  asset->texture = tex;
  asset->key = key;
  asset->refs = 1;
  asset->kind = TEXTURE_FILE;
  asset->pma = pma;
  float width = 0;
  float height = 0;
  SDL_GetTextureSize(tex, &width, &height);
  asset->width = (int)width;
  asset->height = (int)height;
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, id);
}

static bool ensure_freetype(void)
{
  return g_ft_library || FT_Init_FreeType(&g_ft_library) == 0;
}

static const char *next_utf8_codepoint(const char *text, FT_ULong *codepoint)
{
  const unsigned char *cursor = (const unsigned char *)text;
  unsigned char first = cursor[0];

  if (first < 0x80)
  {
    *codepoint = first;
    return (const char *)(cursor + 1);
  }
  if ((first & 0xE0) == 0xC0 && (cursor[1] & 0xC0) == 0x80)
  {
    *codepoint = ((FT_ULong)(first & 0x1F) << 6) |
                 (FT_ULong)(cursor[1] & 0x3F);
    return (const char *)(cursor + 2);
  }
  if ((first & 0xF0) == 0xE0 &&
      (cursor[1] & 0xC0) == 0x80 &&
      (cursor[2] & 0xC0) == 0x80)
  {
    *codepoint = ((FT_ULong)(first & 0x0F) << 12) |
                 ((FT_ULong)(cursor[1] & 0x3F) << 6) |
                 (FT_ULong)(cursor[2] & 0x3F);
    return (const char *)(cursor + 3);
  }
  if ((first & 0xF8) == 0xF0 &&
      (cursor[1] & 0xC0) == 0x80 &&
      (cursor[2] & 0xC0) == 0x80 &&
      (cursor[3] & 0xC0) == 0x80)
  {
    *codepoint = ((FT_ULong)(first & 0x07) << 18) |
                 ((FT_ULong)(cursor[1] & 0x3F) << 12) |
                 ((FT_ULong)(cursor[2] & 0x3F) << 6) |
                 (FT_ULong)(cursor[3] & 0x3F);
    return (const char *)(cursor + 4);
  }

  *codepoint = 0xFFFD;
  return (const char *)(cursor + 1);
}

static Uint8 glyph_bitmap_alpha(const FT_Bitmap *bitmap, int x, int y)
{
  const unsigned char *row = bitmap->buffer +
                             (bitmap->pitch >= 0 ? y : (int)bitmap->rows - 1 - y) *
                                 abs(bitmap->pitch);

  if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
  {
    return (row[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
  }
  if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY)
  {
    return row[x];
  }
  return 0;
}

static void measure_text_bounds(
    FT_Face face,
    const char *text,
    int *out_min_x,
    int *out_min_y,
    int *out_max_x,
    int *out_max_y)
{
  int min_x = 0;
  int min_y = 0;
  int max_x = 1;
  int max_y = 1;
  int pen_x = 0;
  int baseline = face->size ? (int)(face->size->metrics.ascender >> 6) : 0;
  bool saw_glyph = false;
  FT_UInt previous_glyph = 0;

  for (const char *cursor = text; *cursor;)
  {
    FT_ULong codepoint;
    cursor = next_utf8_codepoint(cursor, &codepoint);
    FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);

    if (previous_glyph && glyph_index && FT_HAS_KERNING(face))
    {
      FT_Vector delta;
      if (FT_Get_Kerning(
              face,
              previous_glyph,
              glyph_index,
              FT_KERNING_DEFAULT,
              &delta) == 0)
      {
        pen_x += (int)(delta.x >> 6);
      }
    }

    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) == 0)
    {
      FT_GlyphSlot glyph = face->glyph;
      int bearing_x = (int)(glyph->metrics.horiBearingX >> 6);
      int bearing_y = (int)(glyph->metrics.horiBearingY >> 6);
      int width = (int)(glyph->metrics.width >> 6);
      int height = (int)(glyph->metrics.height >> 6);

      int x0 = pen_x + bearing_x;
      int y0 = baseline - bearing_y;
      int x1 = x0 + width;
      int y1 = y0 + height;

      if (!saw_glyph || x0 < min_x)
        min_x = x0;
      if (!saw_glyph || y0 < min_y)
        min_y = y0;
      if (!saw_glyph || x1 > max_x)
        max_x = x1;
      if (!saw_glyph || y1 > max_y)
        max_y = y1;
      pen_x += (int)(glyph->advance.x >> 6);
      if (pen_x > max_x)
        max_x = pen_x;
      saw_glyph = true;
    }

    previous_glyph = glyph_index;
  }

  if (!saw_glyph)
  {
    int line_height = face->size ? (int)(face->size->metrics.height >> 6) : 1;
    max_y = line_height > 0 ? line_height : 1;
  }

  *out_min_x = min_x;
  *out_min_y = min_y;
  *out_max_x = max_x > min_x ? max_x : min_x + 1;
  *out_max_y = max_y > min_y ? max_y : min_y + 1;
}

static SDL_Surface *render_text_surface(
    FT_Face face,
    const char *text,
    SDL_Color color)
{
  int min_x;
  int min_y;
  int max_x;
  int max_y;
  measure_text_bounds(face, text, &min_x, &min_y, &max_x, &max_y);

  SDL_Surface *surface = SDL_CreateSurface(
      max_x - min_x,
      max_y - min_y,
      SDL_PIXELFORMAT_RGBA32);
  if (!surface)
    return NULL;
  SDL_memset(surface->pixels, 0, (size_t)surface->pitch * surface->h);

  int pen_x = -min_x;
  int baseline = (face->size ? (int)(face->size->metrics.ascender >> 6) : 0) -
                 min_y;
  FT_UInt previous_glyph = 0;

  for (const char *cursor = text; *cursor;)
  {
    FT_ULong codepoint;
    cursor = next_utf8_codepoint(cursor, &codepoint);
    FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);

    if (previous_glyph && glyph_index && FT_HAS_KERNING(face))
    {
      FT_Vector delta;
      if (FT_Get_Kerning(
              face,
              previous_glyph,
              glyph_index,
              FT_KERNING_DEFAULT,
              &delta) == 0)
      {
        pen_x += (int)(delta.x >> 6);
      }
    }

    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER) == 0)
    {
      FT_GlyphSlot glyph = face->glyph;
      FT_Bitmap *bitmap = &glyph->bitmap;
      int origin_x = pen_x + glyph->bitmap_left;
      int origin_y = baseline - glyph->bitmap_top;

      for (int y = 0; y < (int)bitmap->rows; y++)
      {
        int dst_y = origin_y + y;
        if (dst_y < 0 || dst_y >= surface->h)
          continue;

        for (int x = 0; x < (int)bitmap->width; x++)
        {
          int dst_x = origin_x + x;
          if (dst_x < 0 || dst_x >= surface->w)
            continue;

          Uint8 coverage = glyph_bitmap_alpha(bitmap, x, y);
          if (coverage == 0)
            continue;

          Uint8 alpha = (Uint8)((coverage * color.a + 127) / 255);
          Uint32 *pixel = (Uint32 *)((Uint8 *)surface->pixels +
                                     dst_y * surface->pitch +
                                     dst_x * (int)sizeof(Uint32));
          *pixel = SDL_MapSurfaceRGBA(
              surface,
              color.r,
              color.g,
              color.b,
              alpha);
        }
      }

      pen_x += (int)(glyph->advance.x >> 6);
    }

    previous_glyph = glyph_index;
  }

  return surface;
}

/* --- Binding: loadFont(path, ptsize) → id --- */
static JSValue js_loadFont(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 2)
    return JS_NewInt32(ctx, -1);
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;
  int ptsize = 24;
  JS_ToInt32(ctx, &ptsize, argv[1]);

  for (int i = 0; i < MAX_FONTS; i++)
  {
    FontAsset *asset = &g_fonts[i];
    if (asset->face && asset->ptsize == ptsize &&
        strcmp(asset->path, path) == 0)
    {
      asset->refs++;
      JS_FreeCString(ctx, path);
      return JS_NewInt32(ctx, i);
    }
  }

  int id = find_free_font_slot();
  if (id < 0)
  {
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  if (!ensure_freetype())
  {
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  size_t length = 0;
  void *data = load_file_contents(path, &length);
  FT_Face face = NULL;
  if (data)
  {
    FT_New_Memory_Face(g_ft_library, data, (FT_Long)length, 0, &face);
  }
  if (!face)
  {
    SDL_free(data);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ptsize) != 0)
  {
    FT_Done_Face(face);
    SDL_free(data);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }
  char *stored_path = copy_string(path);
  if (!stored_path)
  {
    FT_Done_Face(face);
    SDL_free(data);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  FontAsset *asset = &g_fonts[id];
  asset->face = face;
  asset->data = data;
  asset->path = stored_path;
  asset->ptsize = ptsize;
  asset->refs = 1;
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, id);
}

/* --- Binding: loadTextTexture(fontId, text) -> texture id --- */
static JSValue js_loadTextTexture(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 2)
    return JS_NewInt32(ctx, -1);
  int font_id;
  JS_ToInt32(ctx, &font_id, argv[0]);
  if (!valid_font_id(font_id))
    return JS_NewInt32(ctx, -1);

  const char *text = JS_ToCString(ctx, argv[1]);
  if (!text)
    return JS_EXCEPTION;

  char stack_key[256];
  char *key_str = stack_key;
  size_t text_len = strlen(text);
  size_t needed = text_len + 32;
  if (needed > sizeof(stack_key))
  {
    key_str = malloc(needed);
    if (!key_str)
    {
      JS_FreeCString(ctx, text);
      return JS_NewInt32(ctx, -1);
    }
  }
  snprintf(key_str, needed, "%d:%s", font_id, text);

  for (int i = 0; i < MAX_TEXTURES; i++)
  {
    TextureAsset *asset = &g_textures[i];
    if (asset->texture && asset->kind == TEXTURE_TEXT && asset->key &&
        strcmp(asset->key, key_str) == 0)
    {
      asset->refs++;
      if (key_str != stack_key)
        free(key_str);
      JS_FreeCString(ctx, text);
      return JS_NewInt32(ctx, i);
    }
  }

  int id = find_free_texture_slot();
  if (id < 0)
  {
    if (key_str != stack_key)
      free(key_str);
    JS_FreeCString(ctx, text);
    return JS_NewInt32(ctx, -1);
  }

  SDL_Color color = {255, 255, 255, 255};
  SDL_Surface *surface = render_text_surface(g_fonts[font_id].face, text, color);
  JS_FreeCString(ctx, text);
  if (!surface)
  {
    if (key_str != stack_key)
      free(key_str);
    return JS_NewInt32(ctx, -1);
  }

  SDL_Texture *texture = create_texture_from_surface_on_main_thread(surface);
  if (!texture)
  {
    SDL_DestroySurface(surface);
    if (key_str != stack_key)
      free(key_str);
    return JS_NewInt32(ctx, -1);
  }

  char *stored_key = (key_str != stack_key) ? key_str : strdup(stack_key);
  TextureAsset *asset = &g_textures[id];
  asset->texture = texture;
  asset->key = stored_key;
  asset->refs = 1;
  asset->width = surface->w;
  asset->height = surface->h;
  asset->kind = TEXTURE_TEXT;
  asset->font_id = font_id;
  g_fonts[font_id].refs++;
  SDL_DestroySurface(surface);
  return JS_NewInt32(ctx, id);
}

static JSValue js_releaseTexture(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_UNDEFINED;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  release_texture_id(id);
  return JS_UNDEFINED;
}

static JSValue js_releaseFont(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_UNDEFINED;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  release_font_id(id);
  return JS_UNDEFINED;
}

static JSValue js_getTextureWidth(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_NewInt32(ctx, 0);
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  return JS_NewInt32(ctx, valid_texture_id(id) ? g_textures[id].width : 0);
}

static JSValue js_getTextureHeight(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_NewInt32(ctx, 0);
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  return JS_NewInt32(ctx, valid_texture_id(id) ? g_textures[id].height : 0);
}

static JSValue js_loadAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_NewInt32(ctx, -1);
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  for (int i = 0; i < MAX_AUDIO_ASSETS; i++)
  {
    AudioAsset *asset = &g_audio_assets[i];
    if (asset->data && strcmp(asset->path, path) == 0)
    {
      asset->refs++;
      JS_FreeCString(ctx, path);
      return JS_NewInt32(ctx, i);
    }
  }

  int id = find_free_audio_slot();
  if (id < 0)
  {
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  char *resolved_path = resolve_resource_path(path);
  SDL_AudioSpec spec;
  Uint8 *data = NULL;
  Uint32 length = 0;
  bool loaded = resolved_path &&
                SDL_LoadWAV(resolved_path, &spec, &data, &length);
  if (!loaded && resolved_path && strcmp(resolved_path, path) != 0)
  {
    loaded = SDL_LoadWAV(path, &spec, &data, &length);
  }
  char *prefixed_path = resource_prefixed_path(path);
  if (!loaded && prefixed_path)
  {
    loaded = SDL_LoadWAV(prefixed_path, &spec, &data, &length);
  }

  if (!loaded)
  {
    const char *audio_paths[] = {resolved_path, path, prefixed_path};
    for (size_t i = 0; i < SDL_arraysize(audio_paths) && !loaded; i++)
    {
      if (!audio_paths[i])
        continue;

      size_t encoded_length = 0;
      void *encoded = SDL_LoadFile(audio_paths[i], &encoded_length);
      if (!encoded)
        continue;

      drmp3_config config;
      drmp3_uint64 frame_count;
      drmp3_int16 *pcm = drmp3_open_memory_and_read_pcm_frames_s16(
          encoded, encoded_length, &config, &frame_count, NULL);
      SDL_free(encoded);
      if (!pcm || !config.channels || !config.sampleRate ||
          frame_count > UINT32_MAX / config.channels / sizeof(*pcm))
      {
        drmp3_free(pcm, NULL);
        continue;
      }

      length = (Uint32)(frame_count * config.channels * sizeof(*pcm));
      data = SDL_malloc(length);
      if (!data)
      {
        drmp3_free(pcm, NULL);
        break;
      }
      memcpy(data, pcm, length);
      drmp3_free(pcm, NULL);
      spec.format = SDL_AUDIO_S16LE;
      spec.channels = config.channels;
      spec.freq = config.sampleRate;
      loaded = true;
    }
  }
  free(prefixed_path);
  free(resolved_path);
  if (!loaded)
  {
    SDL_LogError(
        SDL_LOG_CATEGORY_AUDIO,
        "Cannot load audio '%s': %s",
        path,
        SDL_GetError());
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, -1);
  }

  char *stored_path = copy_string(path);
  JS_FreeCString(ctx, path);
  if (!stored_path)
  {
    SDL_free(data);
    return JS_NewInt32(ctx, -1);
  }

  AudioAsset *asset = &g_audio_assets[id];
  asset->data = data;
  asset->length = length;
  asset->spec = spec;
  asset->path = stored_path;
  asset->refs = 1;
  return JS_NewInt32(ctx, id);
}

static JSValue js_releaseAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_UNDEFINED;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  release_audio_id(id);
  return JS_UNDEFINED;
}

static JSValue js_playAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 2)
    return JS_NewInt32(ctx, -1);
  int audio_id;
  int loop = 0;
  double volume = 1.0;
  JS_ToInt32(ctx, &audio_id, argv[0]);
  JS_ToInt32(ctx, &loop, argv[1]);
  if (argc > 2)
    JS_ToFloat64(ctx, &volume, argv[2]);
  if (!valid_audio_id(audio_id))
    return JS_NewInt32(ctx, -1);

  int voice_id = find_free_audio_voice_slot();
  if (voice_id < 0)
    return JS_NewInt32(ctx, -1);

  AudioAsset *asset = &g_audio_assets[audio_id];
  SDL_AudioStream *stream = acquire_audio_stream(&asset->spec);
  if (!stream ||
      !SDL_SetAudioStreamGain(stream, (float)SDL_clamp(volume, 0.0, 1.0)) ||
      !SDL_PutAudioStreamData(stream, asset->data, (int)asset->length) ||
      (loop && !SDL_PutAudioStreamData(
                   stream, asset->data, (int)asset->length)) ||
      !SDL_ResumeAudioStreamDevice(stream))
  {
    if (stream)
      release_audio_stream(stream);
    return JS_NewInt32(ctx, -1);
  }

  int bytes_per_frame =
      SDL_AUDIO_BYTESIZE(asset->spec.format) * asset->spec.channels;
  Uint64 frames = bytes_per_frame > 0
                      ? asset->length / (Uint32)bytes_per_frame
                      : 0;
  AudioVoice *voice = &g_audio_voices[voice_id];
  voice->stream = stream;
  voice->audio_id = audio_id;
  voice->loop = loop != 0;
  voice->started_at = SDL_GetTicks();
  voice->duration = asset->spec.freq > 0
                        ? (frames * 1000) / (Uint64)asset->spec.freq
                        : 0;
  asset->refs++;
  return JS_NewInt32(ctx, voice_id);
}

static JSValue js_stopAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_UNDEFINED;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  destroy_audio_voice(id);
  return JS_UNDEFINED;
}

static JSValue js_pauseAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_UNDEFINED;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  if (valid_audio_voice_id(id) && !g_audio_voices[id].paused)
  {
    SDL_PauseAudioStreamDevice(g_audio_voices[id].stream);
    g_audio_voices[id].paused = true;
    g_audio_voices[id].paused_at = SDL_GetTicks();
  }
  return JS_UNDEFINED;
}

static JSValue js_resumeAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_UNDEFINED;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  if (valid_audio_voice_id(id) && g_audio_voices[id].paused)
  {
    AudioVoice *voice = &g_audio_voices[id];
    voice->paused_duration += SDL_GetTicks() - voice->paused_at;
    voice->paused = false;
    SDL_ResumeAudioStreamDevice(voice->stream);
  }
  return JS_UNDEFINED;
}

static JSValue js_setAudioVolume(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 2)
    return JS_UNDEFINED;
  (void)argc;
  int id;
  double volume;
  JS_ToInt32(ctx, &id, argv[0]);
  JS_ToFloat64(ctx, &volume, argv[1]);
  if (valid_audio_voice_id(id))
  {
    SDL_SetAudioStreamGain(
        g_audio_voices[id].stream,
        (float)SDL_clamp(volume, 0.0, 1.0));
  }
  return JS_UNDEFINED;
}

static bool audio_voice_finished(int id)
{
  AudioVoice *voice = &g_audio_voices[id];
  if (!voice->stream || voice->loop || voice->paused)
    return false;
  return SDL_GetTicks() >=
         voice->started_at + voice->paused_duration + voice->duration;
}

static JSValue js_isAudioPlaying(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_FALSE;
  (void)argc;
  int id;
  JS_ToInt32(ctx, &id, argv[0]);
  if (!valid_audio_voice_id(id))
    return JS_NewBool(ctx, false);
  if (audio_voice_finished(id))
  {
    destroy_audio_voice(id);
    return JS_NewBool(ctx, false);
  }
  return JS_NewBool(ctx, true);
}

static JSValue js_updateAudio(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)ctx;
  (void)this_val;
  (void)argc;
  (void)argv;
  for (int i = 0; i < MAX_AUDIO_VOICES; i++)
  {
    AudioVoice *voice = &g_audio_voices[i];
    if (!voice->stream || voice->paused)
      continue;
    if (voice->loop)
    {
      AudioAsset *asset = &g_audio_assets[voice->audio_id];
      int queued = SDL_GetAudioStreamQueued(voice->stream);
      int min_target_queue = asset->length > 65536 ? 32768 : (int)asset->length;
      if (queued >= 0 && queued < min_target_queue)
      {
        SDL_PutAudioStreamData(
            voice->stream, asset->data, (int)asset->length);
      }
    }
    else if (audio_voice_finished(i))
    {
      destroy_audio_voice(i);
    }
  }
  return JS_UNDEFINED;
}

/* --- Binding: clear() --- */
static bool reserve_draw_batch(int vertices, int indices)
{
  if (vertices > g_mesh_vertex_capacity)
  {
    int new_cap = g_mesh_vertex_capacity == 0 ? 1024 : g_mesh_vertex_capacity * 2;
    while (new_cap < vertices)
      new_cap *= 2;
    SDL_Vertex *buffer = SDL_realloc(
        g_mesh_vertices, (size_t)new_cap * sizeof(SDL_Vertex));
    if (!buffer)
      return false;
    g_mesh_vertices = buffer;
    g_mesh_vertex_capacity = new_cap;
  }
  if (indices > g_mesh_index_capacity)
  {
    int new_cap = g_mesh_index_capacity == 0 ? 2048 : g_mesh_index_capacity * 2;
    while (new_cap < indices)
      new_cap *= 2;
    int *buffer = SDL_realloc(g_mesh_indices, (size_t)new_cap * sizeof(int));
    if (!buffer)
      return false;
    g_mesh_indices = buffer;
    g_mesh_index_capacity = new_cap;
  }
  return true;
}

static bool reserve_input_mesh(int vertices, int indices)
{
  if (vertices > g_input_vertex_capacity)
  {
    int new_cap = g_input_vertex_capacity == 0 ? 512 : g_input_vertex_capacity * 2;
    while (new_cap < vertices)
      new_cap *= 2;
    SDL_Vertex *buffer = SDL_realloc(
        g_input_vertices, (size_t)new_cap * sizeof(SDL_Vertex));
    if (!buffer)
      return false;
    g_input_vertices = buffer;
    g_input_vertex_capacity = new_cap;
  }
  if (indices > g_input_index_capacity)
  {
    int new_cap = g_input_index_capacity == 0 ? 1024 : g_input_index_capacity * 2;
    while (new_cap < indices)
      new_cap *= 2;
    int *buffer = SDL_realloc(g_input_indices, (size_t)new_cap * sizeof(int));
    if (!buffer)
      return false;
    g_input_indices = buffer;
    g_input_index_capacity = new_cap;
  }
  return true;
}

static void flush_draw_batch(void)
{
  if (g_batch_vertex_count == 0)
    return;
  SDL_RenderGeometry(
      g_renderer, g_batch_texture, g_mesh_vertices, g_batch_vertex_count,
      g_mesh_indices, g_batch_index_count);
  g_draw_calls++;
  g_batch_texture = NULL;
  g_batch_vertex_count = 0;
  g_batch_index_count = 0;
}

static bool append_draw_batch(
    SDL_Texture *texture,
    const SDL_Vertex *vertices,
    int vertex_count,
    const int *indices,
    int index_count)
{
  if (g_batch_vertex_count > 0 && g_batch_texture != texture)
    flush_draw_batch();
  if (!reserve_draw_batch(
          g_batch_vertex_count + vertex_count,
          g_batch_index_count + index_count))
    return false;

  int vertex_offset = g_batch_vertex_count;
  SDL_memcpy(
      &g_mesh_vertices[vertex_offset], vertices,
      (size_t)vertex_count * sizeof(SDL_Vertex));
  for (int i = 0; i < index_count; i++)
  {
    g_mesh_indices[g_batch_index_count + i] = indices[i] + vertex_offset;
  }
  g_batch_texture = texture;
  g_batch_vertex_count += vertex_count;
  g_batch_index_count += index_count;
  g_vertices += vertex_count;
  return true;
}

static JSValue js_clear(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (g_render_queue_enabled && !g_executing_render_frame)
    return JS_UNDEFINED;
  flush_draw_batch();
  g_draw_calls = 0;
  g_vertices = 0;
  g_clip_depth = 0;
  reset_render_state_cache();
  cached_SetRenderDrawColor(g_renderer, 9, 15, 29, 255);
  SDL_RenderClear(g_renderer);
  return JS_UNDEFINED;
}

static bool append_texture_region(
    int id,
    double sx, double sy, double sw, double sh,
    double dx, double dy, double dw, double dh,
    double angle, double center_x, double center_y,
    int flip_x, int flip_y,
    double red, double green, double blue, double alpha)
{
  double radians = angle * SDL_PI_D / 180.0;
  double cosine = SDL_cos(radians);
  double sine = SDL_sin(radians);
  double points[4][2] = {
      {0, 0},
      {dw, 0},
      {0, dh},
      {dw, dh},
  };
  SDL_FColor color = {
      (float)(SDL_clamp(red, 0, 255) / 255.0),
      (float)(SDL_clamp(green, 0, 255) / 255.0),
      (float)(SDL_clamp(blue, 0, 255) / 255.0),
      (float)(SDL_clamp(alpha, 0, 255) / 255.0),
  };
  double u0 = sx / g_textures[id].width;
  double v0 = sy / g_textures[id].height;
  double u1 = (sx + sw) / g_textures[id].width;
  double v1 = (sy + sh) / g_textures[id].height;
  if (flip_x)
  {
    double swap = u0;
    u0 = u1;
    u1 = swap;
  }
  if (flip_y)
  {
    double swap = v0;
    v0 = v1;
    v1 = swap;
  }
  SDL_Vertex vertices[4];
  const double uvs[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
  for (int i = 0; i < 4; i++)
  {
    double local_x = points[i][0] - center_x;
    double local_y = points[i][1] - center_y;
    vertices[i].position.x =
        (float)(dx + center_x + local_x * cosine - local_y * sine);
    vertices[i].position.y =
        (float)(dy + center_y + local_x * sine + local_y * cosine);
    vertices[i].color = color;
    vertices[i].tex_coord.x = (float)uvs[i][0];
    vertices[i].tex_coord.y = (float)uvs[i][1];
  }
  const int indices[6] = {0, 1, 2, 2, 1, 3};
  return append_draw_batch(
      g_textures[id].texture, vertices, 4, indices, 6);
}

/* --- Binding: drawTexture(id, x, y) --- */
static JSValue js_drawTexture(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 3)
    return JS_UNDEFINED;
  int id;
  double dx, dy;
  JS_ToInt32(ctx, &id, argv[0]);
  JS_ToFloat64(ctx, &dx, argv[1]);
  JS_ToFloat64(ctx, &dy, argv[2]);

  if (!valid_texture_id(id))
    return JS_UNDEFINED;
  append_texture_region(
      id,
      0, 0, g_textures[id].width, g_textures[id].height,
      dx, dy, 64, 64,
      0, 0, 0, 0, 0,
      255, 255, 255, 255);
  return JS_UNDEFINED;
}

/* --- Binding: drawTextureRotated(id, x, y, w, h, angle, centerX, centerY, flipX, flipY) --- */
static JSValue js_drawTextureRotated(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 10)
    return JS_UNDEFINED;
  int id, flipX, flipY;
  double dx, dy, dw, dh, angle, centerX, centerY;
  double red = 255, green = 255, blue = 255, alpha = 255;
  JS_ToInt32(ctx, &id, argv[0]);
  JS_ToFloat64(ctx, &dx, argv[1]);
  JS_ToFloat64(ctx, &dy, argv[2]);
  JS_ToFloat64(ctx, &dw, argv[3]);
  JS_ToFloat64(ctx, &dh, argv[4]);
  JS_ToFloat64(ctx, &angle, argv[5]);
  JS_ToFloat64(ctx, &centerX, argv[6]);
  JS_ToFloat64(ctx, &centerY, argv[7]);
  JS_ToInt32(ctx, &flipX, argv[8]);
  JS_ToInt32(ctx, &flipY, argv[9]);
  if (argc > 10)
    JS_ToFloat64(ctx, &red, argv[10]);
  if (argc > 11)
    JS_ToFloat64(ctx, &green, argv[11]);
  if (argc > 12)
    JS_ToFloat64(ctx, &blue, argv[12]);
  if (argc > 13)
    JS_ToFloat64(ctx, &alpha, argv[13]);

  if (!valid_texture_id(id))
    return JS_UNDEFINED;
  append_texture_region(
      id,
      0, 0, g_textures[id].width, g_textures[id].height,
      dx, dy, dw, dh,
      angle, centerX, centerY, flipX, flipY,
      red, green, blue, alpha);
  return JS_UNDEFINED;
}

/* --- Binding: drawTextureRegionRotated(id, sx, sy, sw, sh, x, y, w, h, angle, centerX, centerY, flipX, flipY) --- */
static JSValue js_drawTextureRegionRotated(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 14)
    return JS_UNDEFINED;
  int id, flipX, flipY;
  double sx, sy, sw, sh, dx, dy, dw, dh, angle, centerX, centerY;
  double red = 255, green = 255, blue = 255, alpha = 255;
  JS_ToInt32(ctx, &id, argv[0]);
  JS_ToFloat64(ctx, &sx, argv[1]);
  JS_ToFloat64(ctx, &sy, argv[2]);
  JS_ToFloat64(ctx, &sw, argv[3]);
  JS_ToFloat64(ctx, &sh, argv[4]);
  JS_ToFloat64(ctx, &dx, argv[5]);
  JS_ToFloat64(ctx, &dy, argv[6]);
  JS_ToFloat64(ctx, &dw, argv[7]);
  JS_ToFloat64(ctx, &dh, argv[8]);
  JS_ToFloat64(ctx, &angle, argv[9]);
  JS_ToFloat64(ctx, &centerX, argv[10]);
  JS_ToFloat64(ctx, &centerY, argv[11]);
  JS_ToInt32(ctx, &flipX, argv[12]);
  JS_ToInt32(ctx, &flipY, argv[13]);
  if (argc > 14)
    JS_ToFloat64(ctx, &red, argv[14]);
  if (argc > 15)
    JS_ToFloat64(ctx, &green, argv[15]);
  if (argc > 16)
    JS_ToFloat64(ctx, &blue, argv[16]);
  if (argc > 17)
    JS_ToFloat64(ctx, &alpha, argv[17]);

  if (!valid_texture_id(id))
    return JS_UNDEFINED;
  append_texture_region(
      id,
      sx, sy, sw, sh,
      dx, dy, dw, dh,
      angle, centerX, centerY, flipX, flipY,
      red, green, blue, alpha);
  return JS_UNDEFINED;
}

/* --- Binding: drawTextureQuad(id, x0, y0, u0, v0, ... x3, y3, u3, v3, red, green, blue, alpha) --- */
static JSValue js_drawTextureQuad(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 17)
    return JS_UNDEFINED;
  int id;
  double values[16];
  double red = 255, green = 255, blue = 255, alpha = 255;
  JS_ToInt32(ctx, &id, argv[0]);
  for (int i = 0; i < 16; i++)
  {
    JS_ToFloat64(ctx, &values[i], argv[i + 1]);
  }
  if (argc > 17)
    JS_ToFloat64(ctx, &red, argv[17]);
  if (argc > 18)
    JS_ToFloat64(ctx, &green, argv[18]);
  if (argc > 19)
    JS_ToFloat64(ctx, &blue, argv[19]);
  if (argc > 20)
    JS_ToFloat64(ctx, &alpha, argv[20]);

  if (!valid_texture_id(id))
    return JS_UNDEFINED;
  SDL_FColor color = {
      (float)(SDL_clamp(red, 0, 255) / 255.0),
      (float)(SDL_clamp(green, 0, 255) / 255.0),
      (float)(SDL_clamp(blue, 0, 255) / 255.0),
      (float)(SDL_clamp(alpha, 0, 255) / 255.0),
  };
  SDL_Vertex vertices[4];
  for (int i = 0; i < 4; i++)
  {
    int offset = i * 4;
    vertices[i].position.x = (float)values[offset];
    vertices[i].position.y = (float)values[offset + 1];
    vertices[i].color = color;
    vertices[i].tex_coord.x = (float)values[offset + 2];
    vertices[i].tex_coord.y = (float)values[offset + 3];
  }
  int indices[6] = {0, 1, 2, 2, 1, 3};
  append_draw_batch(g_textures[id].texture, vertices, 4, indices, 6);
  return JS_UNDEFINED;
}

/* --- Binding: drawTextureMesh(id, positions, uvs, indices, color, transform) --- */
static JSValue js_drawTextureMesh(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 4)
    return JS_UNDEFINED;
  int id;
  size_t position_offset, position_length, position_size;
  size_t uv_offset, uv_length, uv_size;
  size_t index_offset, index_length, index_size;
  size_t position_buffer_size, uv_buffer_size, index_buffer_size;
  double red = 255, green = 255, blue = 255, alpha = 255;
  double translate_x = 0, translate_y = 0;
  double scale_x = 1, scale_y = 1, cosine = 1, sine = 0;

  (void)this_val;
  if (argc < 4)
    return JS_UNDEFINED;
  JS_ToInt32(ctx, &id, argv[0]);
  if (!valid_texture_id(id) || JS_GetTypedArrayType(argv[1]) != JS_TYPED_ARRAY_FLOAT32 || JS_GetTypedArrayType(argv[2]) != JS_TYPED_ARRAY_FLOAT32 || JS_GetTypedArrayType(argv[3]) != JS_TYPED_ARRAY_UINT16)
  {
    return JS_UNDEFINED;
  }
  if (argc > 4)
    JS_ToFloat64(ctx, &red, argv[4]);
  if (argc > 5)
    JS_ToFloat64(ctx, &green, argv[5]);
  if (argc > 6)
    JS_ToFloat64(ctx, &blue, argv[6]);
  if (argc > 7)
    JS_ToFloat64(ctx, &alpha, argv[7]);
  if (argc > 8)
    JS_ToFloat64(ctx, &translate_x, argv[8]);
  if (argc > 9)
    JS_ToFloat64(ctx, &translate_y, argv[9]);
  if (argc > 10)
    JS_ToFloat64(ctx, &scale_x, argv[10]);
  if (argc > 11)
    JS_ToFloat64(ctx, &scale_y, argv[11]);
  if (argc > 12)
    JS_ToFloat64(ctx, &cosine, argv[12]);
  if (argc > 13)
    JS_ToFloat64(ctx, &sine, argv[13]);

  JSValue position_buffer = JS_GetTypedArrayBuffer(
      ctx, argv[1], &position_offset, &position_length, &position_size);
  JSValue uv_buffer = JS_GetTypedArrayBuffer(
      ctx, argv[2], &uv_offset, &uv_length, &uv_size);
  JSValue index_buffer = JS_GetTypedArrayBuffer(
      ctx, argv[3], &index_offset, &index_length, &index_size);
  uint8_t *positions = JS_GetArrayBuffer(ctx, &position_buffer_size, position_buffer);
  uint8_t *uvs = JS_GetArrayBuffer(ctx, &uv_buffer_size, uv_buffer);
  uint8_t *indices = JS_GetArrayBuffer(ctx, &index_buffer_size, index_buffer);

  if (!positions || !uvs || !indices || position_size != sizeof(float) || uv_size != sizeof(float) || index_size != sizeof(uint16_t) || position_length != uv_length || position_length % (sizeof(float) * 2) != 0 || index_length % (sizeof(uint16_t) * 3) != 0)
  {
    JS_FreeValue(ctx, position_buffer);
    JS_FreeValue(ctx, uv_buffer);
    JS_FreeValue(ctx, index_buffer);
    return JS_UNDEFINED;
  }

  int vertex_count = (int)(position_length / (sizeof(float) * 2));
  int index_count = (int)(index_length / sizeof(uint16_t));
  if (vertex_count <= 0 || index_count <= 0)
    goto cleanup;

  SDL_FColor color = {
      (float)(SDL_clamp(red, 0, 255) / 255.0),
      (float)(SDL_clamp(green, 0, 255) / 255.0),
      (float)(SDL_clamp(blue, 0, 255) / 255.0),
      (float)(SDL_clamp(alpha, 0, 255) / 255.0),
  };
  const float *position_data = (const float *)(positions + position_offset);
  const float *uv_data = (const float *)(uvs + uv_offset);
  const uint16_t *index_data = (const uint16_t *)(indices + index_offset);
  if (!reserve_input_mesh(vertex_count, index_count))
    goto cleanup;
  for (int i = 0; i < index_count; i++)
  {
    if (index_data[i] >= vertex_count)
    {
      goto cleanup;
    }
  }
  for (int i = 0; i < vertex_count; i++)
  {
    double x = (double)position_data[i * 2] * scale_x;
    double y = (double)position_data[i * 2 + 1] * scale_y;
    g_input_vertices[i].position.x =
        (float)(translate_x + x * cosine - y * sine);
    g_input_vertices[i].position.y =
        (float)(translate_y + x * sine + y * cosine);
    g_input_vertices[i].color = color;
    g_input_vertices[i].tex_coord.x = uv_data[i * 2];
    g_input_vertices[i].tex_coord.y = uv_data[i * 2 + 1];
  }
  for (int i = 0; i < index_count; i++)
  {
    g_input_indices[i] = index_data[i];
  }
  append_draw_batch(
      g_textures[id].texture, g_input_vertices, vertex_count,
      g_input_indices, index_count);

cleanup:
  JS_FreeValue(ctx, position_buffer);
  JS_FreeValue(ctx, uv_buffer);
  JS_FreeValue(ctx, index_buffer);
  return JS_UNDEFINED;
}

static JSValue js_drawRect(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 7)
    return JS_UNDEFINED;
  double x, y, width, height, red, green, blue, alpha = 255;
  JS_ToFloat64(ctx, &x, argv[0]);
  JS_ToFloat64(ctx, &y, argv[1]);
  JS_ToFloat64(ctx, &width, argv[2]);
  JS_ToFloat64(ctx, &height, argv[3]);
  JS_ToFloat64(ctx, &red, argv[4]);
  JS_ToFloat64(ctx, &green, argv[5]);
  JS_ToFloat64(ctx, &blue, argv[6]);
  if (argc > 7)
    JS_ToFloat64(ctx, &alpha, argv[7]);

  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_FColor color = {
      (float)(SDL_clamp(red, 0, 255) / 255.0),
      (float)(SDL_clamp(green, 0, 255) / 255.0),
      (float)(SDL_clamp(blue, 0, 255) / 255.0),
      (float)(SDL_clamp(alpha, 0, 255) / 255.0),
  };
  SDL_Vertex vertices[4] = {
      {{(float)x, (float)y}, color, {0, 0}},
      {{(float)(x + width), (float)y}, color, {0, 0}},
      {{(float)x, (float)(y + height)}, color, {0, 0}},
      {{(float)(x + width), (float)(y + height)}, color, {0, 0}},
  };
  const int indices[6] = {0, 1, 2, 2, 1, 3};
  append_draw_batch(NULL, vertices, 4, indices, 6);
  return JS_UNDEFINED;
}

static void js_set_draw_color(
    double red, double green, double blue, double alpha)
{
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(
      g_renderer,
      (Uint8)SDL_clamp(red, 0, 255),
      (Uint8)SDL_clamp(green, 0, 255),
      (Uint8)SDL_clamp(blue, 0, 255),
      (Uint8)SDL_clamp(alpha, 0, 255));
}

static JSValue js_drawLine(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 7)
    return JS_UNDEFINED;
  flush_draw_batch();
  (void)this_val;
  double x1, y1, x2, y2, red, green, blue, alpha = 255;
  JS_ToFloat64(ctx, &x1, argv[0]);
  JS_ToFloat64(ctx, &y1, argv[1]);
  JS_ToFloat64(ctx, &x2, argv[2]);
  JS_ToFloat64(ctx, &y2, argv[3]);
  JS_ToFloat64(ctx, &red, argv[4]);
  JS_ToFloat64(ctx, &green, argv[5]);
  JS_ToFloat64(ctx, &blue, argv[6]);
  if (argc > 7)
    JS_ToFloat64(ctx, &alpha, argv[7]);

  js_set_draw_color(red, green, blue, alpha);
  SDL_RenderLine(g_renderer, (float)x1, (float)y1, (float)x2, (float)y2);
  g_draw_calls++;
  g_vertices += 2;
  return JS_UNDEFINED;
}

static JSValue js_drawPoint(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 5)
    return JS_UNDEFINED;
  flush_draw_batch();
  (void)this_val;
  double x, y, red, green, blue, alpha = 255;
  JS_ToFloat64(ctx, &x, argv[0]);
  JS_ToFloat64(ctx, &y, argv[1]);
  JS_ToFloat64(ctx, &red, argv[2]);
  JS_ToFloat64(ctx, &green, argv[3]);
  JS_ToFloat64(ctx, &blue, argv[4]);
  if (argc > 5)
    JS_ToFloat64(ctx, &alpha, argv[5]);

  js_set_draw_color(red, green, blue, alpha);
  SDL_RenderPoint(g_renderer, (float)x, (float)y);
  g_draw_calls++;
  g_vertices++;
  return JS_UNDEFINED;
}

static JSValue js_drawCircle(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 6)
    return JS_UNDEFINED;
  flush_draw_batch();
  (void)this_val;
  double x, y, radius, red, green, blue, alpha = 255;
  int fill = 0;
  JS_ToFloat64(ctx, &x, argv[0]);
  JS_ToFloat64(ctx, &y, argv[1]);
  JS_ToFloat64(ctx, &radius, argv[2]);
  JS_ToFloat64(ctx, &red, argv[3]);
  JS_ToFloat64(ctx, &green, argv[4]);
  JS_ToFloat64(ctx, &blue, argv[5]);
  if (argc > 6)
    JS_ToFloat64(ctx, &alpha, argv[6]);
  if (argc > 7)
    JS_ToInt32(ctx, &fill, argv[7]);

  js_set_draw_color(red, green, blue, alpha);
  int r = (int)SDL_max(0.0, radius);
  int cx = (int)x;
  int cy = (int)y;

  for (int dy = -r; dy <= r; dy++)
  {
    int dx_limit = (int)SDL_sqrt((float)(r * r - dy * dy));
    if (fill)
    {
      SDL_RenderLine(
          g_renderer,
          (float)(cx - dx_limit),
          (float)(cy + dy),
          (float)(cx + dx_limit),
          (float)(cy + dy));
      g_draw_calls++;
      g_vertices += 2;
    }
    else
    {
      SDL_RenderPoint(g_renderer, (float)(cx - dx_limit), (float)(cy + dy));
      SDL_RenderPoint(g_renderer, (float)(cx + dx_limit), (float)(cy + dy));
      g_draw_calls += 2;
      g_vertices += 2;
    }
  }
  return JS_UNDEFINED;
}

static JSValue js_drawPolyline(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 4)
    return JS_UNDEFINED;
  flush_draw_batch();
  (void)this_val;
  if (argc < 5)
    return JS_UNDEFINED;
  int closed = 0;
  double red, green, blue, alpha = 255;
  JS_ToFloat64(ctx, &red, argv[1]);
  JS_ToFloat64(ctx, &green, argv[2]);
  JS_ToFloat64(ctx, &blue, argv[3]);
  if (argc > 4)
    JS_ToFloat64(ctx, &alpha, argv[4]);
  if (argc > 5)
    JS_ToInt32(ctx, &closed, argv[5]);

  int64_t length_value = 0;
  JS_GetLength(ctx, argv[0], &length_value);
  if (length_value < 2)
    return JS_UNDEFINED;

  js_set_draw_color(red, green, blue, alpha);
  double first_x = 0, first_y = 0, previous_x = 0, previous_y = 0;
  for (int64_t i = 0; i < length_value; i++)
  {
    JSValue point = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
    JSValue x_value = JS_GetPropertyStr(ctx, point, "x");
    JSValue y_value = JS_GetPropertyStr(ctx, point, "y");
    double x = 0, y = 0;
    JS_ToFloat64(ctx, &x, x_value);
    JS_ToFloat64(ctx, &y, y_value);
    JS_FreeValue(ctx, x_value);
    JS_FreeValue(ctx, y_value);
    JS_FreeValue(ctx, point);

    if (i == 0)
    {
      first_x = previous_x = x;
      first_y = previous_y = y;
      continue;
    }
    SDL_RenderLine(
        g_renderer,
        (float)previous_x, (float)previous_y,
        (float)x, (float)y);
    g_draw_calls++;
    g_vertices += 2;
    previous_x = x;
    previous_y = y;
  }
  if (closed)
  {
    SDL_RenderLine(
        g_renderer,
        (float)previous_x, (float)previous_y,
        (float)first_x, (float)first_y);
    g_draw_calls++;
    g_vertices += 2;
  }
  return JS_UNDEFINED;
}

static JSValue js_pushClipRect(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 4)
    return JS_UNDEFINED;
  flush_draw_batch();
  double x, y, width, height;
  JS_ToFloat64(ctx, &x, argv[0]);
  JS_ToFloat64(ctx, &y, argv[1]);
  JS_ToFloat64(ctx, &width, argv[2]);
  JS_ToFloat64(ctx, &height, argv[3]);
  if (g_clip_depth >= MAX_CLIP_DEPTH)
    return JS_UNDEFINED;

  SDL_Rect clip = {
      (int)x, (int)y, (int)SDL_max(0.0, width), (int)SDL_max(0.0, height)};
  if (g_clip_depth > 0)
  {
    SDL_Rect intersection;
    if (SDL_GetRectIntersection(
            &g_clip_stack[g_clip_depth - 1], &clip, &intersection))
    {
      clip = intersection;
    }
    else
    {
      clip = (SDL_Rect){0, 0, 0, 0};
    }
  }
  g_clip_stack[g_clip_depth++] = clip;
  SDL_SetRenderClipRect(g_renderer, &clip);
  return JS_UNDEFINED;
}

static JSValue js_popClipRect(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  flush_draw_batch();
  if (g_clip_depth > 0)
    g_clip_depth--;
  SDL_SetRenderClipRect(
      g_renderer,
      g_clip_depth > 0 ? &g_clip_stack[g_clip_depth - 1] : NULL);
  return JS_UNDEFINED;
}

static void free_render_frame(RenderFrame *frame)
{
  if (!frame)
    return;
  SDL_free(frame->commands);
  SDL_free(frame->floats);
  SDL_free(frame->uints);
  SDL_free(frame->shorts);
  SDL_zero(*frame);
}

static JSAtom g_atom_commands = JS_ATOM_NULL;
static JSAtom g_atom_floatBuffer = JS_ATOM_NULL;
static JSAtom g_atom_uintBuffer = JS_ATOM_NULL;
static JSAtom g_atom_shortBuffer = JS_ATOM_NULL;

static void ensure_js_sdl3_atoms(JSContext *ctx)
{
  if (g_atom_commands == JS_ATOM_NULL && ctx)
  {
    g_atom_commands = JS_NewAtom(ctx, "commands");
    g_atom_floatBuffer = JS_NewAtom(ctx, "floatBuffer");
    g_atom_uintBuffer = JS_NewAtom(ctx, "uintBuffer");
    g_atom_shortBuffer = JS_NewAtom(ctx, "shortBuffer");
  }
}

static bool ensure_capacity(void **data, size_t *capacity, size_t required, size_t elem_size)
{
  if (*capacity >= required)
    return true;
  size_t min_init = 65536;
  size_t new_cap = *capacity == 0 ? (min_init > required ? min_init : required) : *capacity * 2;
  while (new_cap < required)
  {
    if (new_cap > SIZE_MAX / 2)
      return false;
    new_cap *= 2;
  }
  void *ptr = SDL_realloc(*data, new_cap);
  if (!ptr)
    return false;
  *data = ptr;
  *capacity = new_cap;
#if JS_SDL_ENABLE_PROFILING
  js_prof_record_buffer_grow();
#endif
  return true;
}

/* --- Binding: submitCommandBuffer(buffer) --- */
static JSValue js_submitCommandBuffer(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (!g_executing_render_frame && (argc < 1 || !JS_IsObject(argv[0])))
  {
    return JS_UNDEFINED;
  }

  JSValue val_cmds = JS_UNDEFINED;
  JSValue val_floats = JS_UNDEFINED;
  JSValue val_uints = JS_UNDEFINED;
  JSValue val_shorts = JS_UNDEFINED;

  size_t cmds_off = 0, cmds_len = 0, cmds_size = 0;
  size_t floats_off = 0, floats_len = 0, floats_size = 0;
  size_t uints_off = 0, uints_len = 0, uints_size = 0;
  size_t shorts_off = 0, shorts_len = 0, shorts_size = 0;

  JSValue buf_cmds = JS_UNDEFINED;
  JSValue buf_floats = JS_UNDEFINED;
  JSValue buf_uints = JS_UNDEFINED;
  JSValue buf_shorts = JS_UNDEFINED;

  size_t sz_cmds = 0, sz_floats = 0, sz_uints = 0, sz_shorts = 0;
  uint8_t *ptr_cmds = NULL;
  uint8_t *ptr_floats = NULL;
  uint8_t *ptr_uints = NULL;
  uint8_t *ptr_shorts = NULL;

  if (g_executing_render_frame)
  {
    ptr_cmds = (uint8_t *)g_executing_render_frame->commands;
    ptr_floats = (uint8_t *)g_executing_render_frame->floats;
    ptr_uints = (uint8_t *)g_executing_render_frame->uints;
    ptr_shorts = (uint8_t *)g_executing_render_frame->shorts;
    cmds_len = g_executing_render_frame->commands_len;
    floats_len = g_executing_render_frame->floats_len;
    uints_len = g_executing_render_frame->uints_len;
    shorts_len = g_executing_render_frame->shorts_len;
    cmds_size = sizeof(int32_t);
    floats_size = sizeof(float);
    uints_size = sizeof(uint32_t);
    shorts_size = sizeof(uint16_t);
    sz_cmds = cmds_len;
    sz_floats = floats_len;
    sz_uints = uints_len;
    sz_shorts = shorts_len;
  }
  else
  {
    ensure_js_sdl3_atoms(ctx);
    val_cmds = JS_GetProperty(ctx, argv[0], g_atom_commands);
    val_floats = JS_GetProperty(ctx, argv[0], g_atom_floatBuffer);
    val_uints = JS_GetProperty(ctx, argv[0], g_atom_uintBuffer);
    val_shorts = JS_GetProperty(ctx, argv[0], g_atom_shortBuffer);
    buf_cmds = JS_GetTypedArrayBuffer(ctx, val_cmds, &cmds_off, &cmds_len, &cmds_size);
    buf_floats = JS_GetTypedArrayBuffer(ctx, val_floats, &floats_off, &floats_len, &floats_size);
    buf_uints = JS_GetTypedArrayBuffer(ctx, val_uints, &uints_off, &uints_len, &uints_size);
    buf_shorts = JS_GetTypedArrayBuffer(ctx, val_shorts, &shorts_off, &shorts_len, &shorts_size);
    ptr_cmds = JS_GetArrayBuffer(ctx, &sz_cmds, buf_cmds);
    ptr_floats = JS_GetArrayBuffer(ctx, &sz_floats, buf_floats);
    ptr_uints = JS_GetArrayBuffer(ctx, &sz_uints, buf_uints);
    ptr_shorts = JS_GetArrayBuffer(ctx, &sz_shorts, buf_shorts);
  }

  bool valid_buffers = ptr_cmds && ptr_floats && ptr_uints &&
                       cmds_size == sizeof(int32_t) &&
                       floats_size == sizeof(float) &&
                       uints_size == sizeof(uint32_t) &&
                       (!shorts_len || (ptr_shorts && shorts_size == sizeof(uint16_t))) &&
                       cmds_off <= sz_cmds && cmds_len <= sz_cmds - cmds_off &&
                       floats_off <= sz_floats && floats_len <= sz_floats - floats_off &&
                       uints_off <= sz_uints && uints_len <= sz_uints - uints_off &&
                       shorts_off <= sz_shorts && shorts_len <= sz_shorts - shorts_off &&
                       cmds_len % sizeof(int32_t) == 0 &&
                       floats_len % sizeof(float) == 0 &&
                       uints_len % sizeof(uint32_t) == 0 &&
                       shorts_len % sizeof(uint16_t) == 0;

  if (g_render_queue_enabled && !g_executing_render_frame && valid_buffers)
  {

    SDL_LockMutex(g_render_queue_mutex);
    if (!g_render_queue_enabled)
    {
      SDL_UnlockMutex(g_render_queue_mutex);
      goto cleanup_js_values;
    }

    int target_idx = -1;
    for (int i = 0; i < RENDER_BUFFER_COUNT; i++)
    {
      if (g_render_buffers[i].state == RENDER_BUFFER_FREE)
      {
        target_idx = i;
        break;
      }
    }
    if (target_idx < 0)
    {
      for (int i = 0; i < RENDER_BUFFER_COUNT; i++)
      {
        if (g_render_buffers[i].state == RENDER_BUFFER_READY)
        {
          target_idx = i;
#if JS_SDL_ENABLE_PROFILING
          js_prof_record_dropped_frame();
#endif
          break;
        }
      }
    }

    if (target_idx < 0)
    {
      SDL_UnlockMutex(g_render_queue_mutex);
      goto cleanup_js_values;
    }

    RenderFrame *buf = &g_render_buffers[target_idx];
    buf->state = RENDER_BUFFER_WRITING;
    SDL_UnlockMutex(g_render_queue_mutex);

    if (!ensure_capacity((void **)&buf->commands, &buf->commands_cap, cmds_len, 1) ||
        !ensure_capacity((void **)&buf->floats, &buf->floats_cap, floats_len, 1) ||
        !ensure_capacity((void **)&buf->uints, &buf->uints_cap, uints_len, 1) ||
        (shorts_len && !ensure_capacity((void **)&buf->shorts, &buf->shorts_cap, shorts_len, 1)))
    {
      SDL_LockMutex(g_render_queue_mutex);
      buf->state = RENDER_BUFFER_FREE;
      SDL_UnlockMutex(g_render_queue_mutex);
      goto cleanup_js_values;
    }

    SDL_memcpy(buf->commands, ptr_cmds + cmds_off, cmds_len);
    SDL_memcpy(buf->floats, ptr_floats + floats_off, floats_len);
    SDL_memcpy(buf->uints, ptr_uints + uints_off, uints_len);
    if (shorts_len && buf->shorts)
    {
      SDL_memcpy(buf->shorts, ptr_shorts + shorts_off, shorts_len);
    }
    buf->commands_len = cmds_len;
    buf->floats_len = floats_len;
    buf->uints_len = uints_len;
    buf->shorts_len = shorts_len;
    buf->timestamp_ns = SDL_GetTicksNS();

    SDL_LockMutex(g_render_queue_mutex);
    buf->state = RENDER_BUFFER_READY;
    SDL_SignalCondition(g_render_queue_ready);
    SDL_UnlockMutex(g_render_queue_mutex);

  cleanup_js_values:
    JS_FreeValue(ctx, buf_cmds);
    JS_FreeValue(ctx, buf_floats);
    JS_FreeValue(ctx, buf_uints);
    JS_FreeValue(ctx, buf_shorts);
    JS_FreeValue(ctx, val_cmds);
    JS_FreeValue(ctx, val_floats);
    JS_FreeValue(ctx, val_uints);
    JS_FreeValue(ctx, val_shorts);
    return JS_UNDEFINED;
  }

  if (valid_buffers)
  {
    const int32_t *cmds = (const int32_t *)(ptr_cmds + cmds_off);
    const float *floats = (const float *)(ptr_floats + floats_off);
    const uint32_t *uints = (const uint32_t *)(ptr_uints + uints_off);
    const uint16_t *shorts = ptr_shorts ? (const uint16_t *)(ptr_shorts + shorts_off) : NULL;

    size_t num_cmds = cmds_len / sizeof(int32_t);
    size_t float_count = floats_len / sizeof(float);
    size_t uint_count = uints_len / sizeof(uint32_t);
    size_t short_count = shorts_len / sizeof(uint16_t);
    size_t cmd_idx = 0;
    size_t float_idx = 0;
    size_t uint_idx = 0;
    size_t short_idx = 0;

    while (cmd_idx < num_cmds)
    {
      int op = cmds[cmd_idx++];
      if (op == 0)
        break;

      if (op == 1)
      { // CMD_DRAW_SPRITE
        if (uint_count - uint_idx < 2 || float_count - float_idx < 9)
          break;
        int id = (int)uints[uint_idx++];
        uint32_t c = uints[uint_idx++];
        double dx = floats[float_idx++];
        double dy = floats[float_idx++];
        double dw = floats[float_idx++];
        double dh = floats[float_idx++];
        double angle = floats[float_idx++];
        double cx = floats[float_idx++];
        double cy = floats[float_idx++];
        int fx = (int)floats[float_idx++];
        int fy = (int)floats[float_idx++];

        double r = (c >> 24) & 0xFF;
        double g = (c >> 16) & 0xFF;
        double b = (c >> 8) & 0xFF;
        double a = c & 0xFF;

        if (id >= 0 && id < MAX_TEXTURES && g_textures[id].texture)
        {
          append_texture_region(id, 0, 0, g_textures[id].width, g_textures[id].height, dx, dy, dw, dh, angle, cx, cy, fx, fy, r, g, b, a);
        }
      }
      else if (op == 8)
      { // CMD_DRAW_REGION
        if (uint_count - uint_idx < 2 || float_count - float_idx < 13)
          break;
        int id = (int)uints[uint_idx++];
        uint32_t c = uints[uint_idx++];
        double sx = floats[float_idx++];
        double sy = floats[float_idx++];
        double sw = floats[float_idx++];
        double sh = floats[float_idx++];
        double dx = floats[float_idx++];
        double dy = floats[float_idx++];
        double dw = floats[float_idx++];
        double dh = floats[float_idx++];
        double angle = floats[float_idx++];
        double cx = floats[float_idx++];
        double cy = floats[float_idx++];
        int fx = (int)floats[float_idx++];
        int fy = (int)floats[float_idx++];

        double r = (c >> 24) & 0xFF;
        double g = (c >> 16) & 0xFF;
        double b = (c >> 8) & 0xFF;
        double a = c & 0xFF;

        if (id >= 0 && id < MAX_TEXTURES && g_textures[id].texture)
        {
          append_texture_region(id, sx, sy, sw, sh, dx, dy, dw, dh, angle, cx, cy, fx, fy, r, g, b, a);
        }
      }
      else if (op == 2)
      { // CMD_DRAW_QUAD
        if (uint_count - uint_idx < 2 || float_count - float_idx < 16)
          break;
        int id = (int)uints[uint_idx++];
        uint32_t c = uints[uint_idx++];
        double x0 = floats[float_idx++], y0 = floats[float_idx++];
        double u0 = floats[float_idx++], v0 = floats[float_idx++];
        double x1 = floats[float_idx++], y1 = floats[float_idx++];
        double u1 = floats[float_idx++], v1 = floats[float_idx++];
        double x2 = floats[float_idx++], y2 = floats[float_idx++];
        double u2 = floats[float_idx++], v2 = floats[float_idx++];
        double x3 = floats[float_idx++], y3 = floats[float_idx++];
        double u3 = floats[float_idx++], v3 = floats[float_idx++];

        double r = (c >> 24) & 0xFF;
        double g = (c >> 16) & 0xFF;
        double b = (c >> 8) & 0xFF;
        double a = c & 0xFF;

        if (id >= 0 && id < MAX_TEXTURES && g_textures[id].texture)
        {
          SDL_FColor color = {
              (float)(r / 255.0), (float)(g / 255.0), (float)(b / 255.0), (float)(a / 255.0)};
          SDL_Vertex vertices[4] = {
              {{(float)x0, (float)y0}, color, {(float)u0, (float)v0}},
              {{(float)x1, (float)y1}, color, {(float)u1, (float)v1}},
              {{(float)x2, (float)y2}, color, {(float)u2, (float)v2}},
              {{(float)x3, (float)y3}, color, {(float)u3, (float)v3}},
          };
          const int indices[6] = {0, 1, 2, 2, 1, 3};
          append_draw_batch(g_textures[id].texture, vertices, 4, indices, 6);
        }
      }
      else if (op == 3)
      { // CMD_DRAW_MESH
        if (uint_count - uint_idx < 4)
          break;
        int id = (int)uints[uint_idx++];
        uint32_t c = uints[uint_idx++];
        uint32_t v_count_raw = uints[uint_idx++];
        uint32_t i_count_raw = uints[uint_idx++];
        if (v_count_raw > INT_MAX || i_count_raw > INT_MAX ||
            float_count - float_idx < 6 || i_count_raw > short_count - short_idx)
          break;
        if (v_count_raw > (float_count - float_idx - 6) / 4)
          break;
        int v_count = (int)v_count_raw;
        int i_count = (int)i_count_raw;

        const float *pos_ptr = &floats[float_idx];
        float_idx += v_count * 2;
        const float *uv_ptr = &floats[float_idx];
        float_idx += v_count * 2;
        double tx = floats[float_idx++], ty = floats[float_idx++];
        double sx = floats[float_idx++], sy = floats[float_idx++];
        double cosine = floats[float_idx++], sine = floats[float_idx++];

        const uint16_t *idx_ptr = shorts ? &shorts[short_idx] : NULL;
        short_idx += i_count;

        double r = (c >> 24) & 0xFF;
        double g = (c >> 16) & 0xFF;
        double b = (c >> 8) & 0xFF;
        double a = c & 0xFF;

        if (id >= 0 && id < MAX_TEXTURES && g_textures[id].texture && v_count > 0 && i_count > 0 && idx_ptr)
        {
          if (reserve_input_mesh(v_count, i_count))
          {
            SDL_FColor color = {
                (float)(r / 255.0), (float)(g / 255.0), (float)(b / 255.0), (float)(a / 255.0)};
            for (int i = 0; i < v_count; i++)
            {
              double x = pos_ptr[i * 2] * sx;
              double y = pos_ptr[i * 2 + 1] * sy;
              g_input_vertices[i].position.x = (float)(tx + x * cosine - y * sine);
              g_input_vertices[i].position.y = (float)(ty + x * sine + y * cosine);
              g_input_vertices[i].color = color;
              g_input_vertices[i].tex_coord.x = uv_ptr[i * 2];
              g_input_vertices[i].tex_coord.y = uv_ptr[i * 2 + 1];
            }
            for (int i = 0; i < i_count; i++)
            {
              g_input_indices[i] = idx_ptr[i];
            }
            append_draw_batch(g_textures[id].texture, g_input_vertices, v_count, g_input_indices, i_count);
          }
        }
      }
      else if (op == 4)
      { // CMD_DRAW_RECT
        if (uint_count - uint_idx < 1 || float_count - float_idx < 4)
          break;
        uint32_t c = uints[uint_idx++];
        double x = floats[float_idx++];
        double y = floats[float_idx++];
        double w = floats[float_idx++];
        double h = floats[float_idx++];

        double r = (c >> 24) & 0xFF;
        double g = (c >> 16) & 0xFF;
        double b = (c >> 8) & 0xFF;
        double a = c & 0xFF;

        cached_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        SDL_FColor color = {
            (float)(r / 255.0), (float)(g / 255.0), (float)(b / 255.0), (float)(a / 255.0)};
        SDL_Vertex vertices[4] = {
            {{(float)x, (float)y}, color, {0, 0}},
            {{(float)(x + w), (float)y}, color, {0, 0}},
            {{(float)x, (float)(y + h)}, color, {0, 0}},
            {{(float)(x + w), (float)(y + h)}, color, {0, 0}},
        };
        const int indices[6] = {0, 1, 2, 2, 1, 3};
        append_draw_batch(NULL, vertices, 4, indices, 6);
      }
      else if (op == 5)
      { // CMD_DRAW_LINE
        if (uint_count - uint_idx < 1 || float_count - float_idx < 4)
          break;
        uint32_t c = uints[uint_idx++];
        double x1 = floats[float_idx++];
        double y1 = floats[float_idx++];
        double x2 = floats[float_idx++];
        double y2 = floats[float_idx++];

        double r = (c >> 24) & 0xFF;
        double g = (c >> 16) & 0xFF;
        double b = (c >> 8) & 0xFF;
        double a = c & 0xFF;

        flush_draw_batch();
        cached_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        cached_SetRenderDrawColor(g_renderer, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a);
        SDL_RenderLine(g_renderer, (float)x1, (float)y1, (float)x2, (float)y2);
        g_draw_calls++;
      }
      else if (op == 6)
      { // CMD_PUSH_CLIP
        if (float_count - float_idx < 4)
          break;
        double x = floats[float_idx++];
        double y = floats[float_idx++];
        double w = floats[float_idx++];
        double h = floats[float_idx++];

        flush_draw_batch();
        SDL_Rect clip = {(int)x, (int)y, (int)SDL_max(0.0, w), (int)SDL_max(0.0, h)};
        if (g_clip_depth > 0)
        {
          SDL_Rect intersection;
          if (SDL_GetRectIntersection(&g_clip_stack[g_clip_depth - 1], &clip, &intersection))
          {
            clip = intersection;
          }
          else
          {
            clip = (SDL_Rect){0, 0, 0, 0};
          }
        }
        if (g_clip_depth >= MAX_CLIP_DEPTH)
          continue;
        g_clip_stack[g_clip_depth++] = clip;
        cached_SetRenderClipRect(g_renderer, &clip);
      }
      else if (op == 7)
      { // CMD_POP_CLIP
        flush_draw_batch();
        if (g_clip_depth > 0)
          g_clip_depth--;
        cached_SetRenderClipRect(g_renderer, g_clip_depth > 0 ? &g_clip_stack[g_clip_depth - 1] : NULL);
      }
      else
      {
        break;
      }
    }
  }

  if (!g_executing_render_frame)
  {
    JS_FreeValue(ctx, buf_cmds);
    JS_FreeValue(ctx, buf_floats);
    JS_FreeValue(ctx, buf_uints);
    JS_FreeValue(ctx, buf_shorts);

    JS_FreeValue(ctx, val_cmds);
    JS_FreeValue(ctx, val_floats);
    JS_FreeValue(ctx, val_uints);
    JS_FreeValue(ctx, val_shorts);
  }

  return JS_UNDEFINED;
}

/* --- Binding: present() --- */
static JSValue js_present(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (g_render_queue_enabled && !g_executing_render_frame)
    return JS_UNDEFINED;
  flush_draw_batch();
  SDL_RenderPresent(g_renderer);
  return JS_UNDEFINED;
}

static JSValue js_getRendererStats(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  (void)argc;
  (void)argv;
  JSValue stats = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, stats, "fps", JS_NewFloat64(ctx, g_fps));
  JS_SetPropertyStr(ctx, stats, "frameTimeMs", JS_NewFloat64(ctx, g_frame_time_ms));
  JS_SetPropertyStr(ctx, stats, "drawCalls", JS_NewInt32(ctx, g_draw_calls));
  JS_SetPropertyStr(ctx, stats, "vertices", JS_NewInt32(ctx, g_vertices));
  return stats;
}

/* --- Binding: onInit(cb) --- */
static JSValue js_onInit(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onInit requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_onInit);
  g_onInit = JS_DupValue(ctx, argv[0]);
  SDL_Log("JavaScript onInit callback registered");
  return JS_UNDEFINED;
}

/* --- Binding: onUpdate(cb) --- */
static JSValue js_onUpdate(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onUpdate requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_onUpdate);
  g_onUpdate = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onRender(cb) --- */
static JSValue js_onRender(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onRender requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_onRender);
  g_onRender = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onTouchStart(cb) --- */
static JSValue js_onTouchStart(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onTouchStart requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_touchStart);
  g_touchStart = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onTouchMove(cb) --- */
static JSValue js_onTouchMove(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onTouchMove requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_touchMove);
  g_touchMove = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onTouchEnd(cb) --- */
static JSValue js_onTouchEnd(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onTouchEnd requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_touchEnd);
  g_touchEnd = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onTextInput(cb) --- */
static JSValue js_onTextInput(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onTextInput requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_textInput);
  g_textInput = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onKeyDown(cb) --- */
static JSValue js_onKeyDown(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onKeyDown requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_keyDown);
  g_keyDown = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

/* --- Binding: onKeyUp(cb) --- */
static JSValue js_onKeyUp(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "onKeyUp requires a callback");
  if (!JS_IsFunction(ctx, argv[0]))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, g_keyUp);
  g_keyUp = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

static JSValue js_startTextInput(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)ctx;
  (void)this_val;
  (void)argc;
  (void)argv;
  js_run_on_main_thread(start_text_input_main_thread, g_window);
  return JS_UNDEFINED;
}

static JSValue js_stopTextInput(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)ctx;
  (void)this_val;
  (void)argc;
  (void)argv;
  js_run_on_main_thread(stop_text_input_main_thread, g_window);
  return JS_UNDEFINED;
}

static JSValue js_set_callback(
    JSContext *ctx, JSValueConst callback, JSValue *slot)
{
  if (!JS_IsFunction(ctx, callback))
    return JS_EXCEPTION;
  JS_FreeValue(ctx, *slot);
  *slot = JS_DupValue(ctx, callback);
  return JS_UNDEFINED;
}

#define DEFINE_CALLBACK_BINDING(name, slot)            \
  static JSValue name(                                 \
      JSContext *ctx, JSValueConst this_val, int argc, \
      JSValueConst *argv)                              \
  {                                                    \
    (void)this_val;                                    \
    if (argc < 1)                                      \
      return JS_ThrowTypeError(ctx, "callback is required"); \
    return js_set_callback(ctx, argv[0], &slot);       \
  }

DEFINE_CALLBACK_BINDING(js_onPause, g_onPause)
DEFINE_CALLBACK_BINDING(js_onResume, g_onResume)
DEFINE_CALLBACK_BINDING(js_onBackground, g_onBackground)
DEFINE_CALLBACK_BINDING(js_onForeground, g_onForeground)
DEFINE_CALLBACK_BINDING(js_onInterruption, g_onInterruption)
DEFINE_CALLBACK_BINDING(js_onLowMemory, g_onLowMemory)
DEFINE_CALLBACK_BINDING(js_onOrientationChange, g_onOrientationChange)
DEFINE_CALLBACK_BINDING(js_onTerminate, g_onTerminate)

#undef DEFINE_CALLBACK_BINDING

/* --- module export table --- */
static const JSCFunctionListEntry funcs[] =
    {
        JS_CFUNC_DEF("createWindow", 3, js_createWindow),
        JS_CFUNC_DEF("getViewportMetrics", 0, js_getViewportMetrics),
        JS_CFUNC_DEF("getWinSize", 0, js_getWinSize),
        JS_CFUNC_DEF("loadTextFile", 1, js_loadTextFile),
        JS_CFUNC_DEF("loadBinaryFile", 1, js_loadBinaryFile),
        JS_CFUNC_DEF("loadTexture", 1, js_loadTexture),
        JS_CFUNC_DEF("loadFont", 2, js_loadFont),
        JS_CFUNC_DEF("loadTextTexture", 2, js_loadTextTexture),
        JS_CFUNC_DEF("releaseTexture", 1, js_releaseTexture),
        JS_CFUNC_DEF("releaseFont", 1, js_releaseFont),
        JS_CFUNC_DEF("getTextureWidth", 1, js_getTextureWidth),
        JS_CFUNC_DEF("getTextureHeight", 1, js_getTextureHeight),
        JS_CFUNC_DEF("loadAudio", 1, js_loadAudio),
        JS_CFUNC_DEF("releaseAudio", 1, js_releaseAudio),
        JS_CFUNC_DEF("playAudio", 3, js_playAudio),
        JS_CFUNC_DEF("stopAudio", 1, js_stopAudio),
        JS_CFUNC_DEF("pauseAudio", 1, js_pauseAudio),
        JS_CFUNC_DEF("resumeAudio", 1, js_resumeAudio),
        JS_CFUNC_DEF("setAudioVolume", 2, js_setAudioVolume),
        JS_CFUNC_DEF("isAudioPlaying", 1, js_isAudioPlaying),
        JS_CFUNC_DEF("updateAudio", 0, js_updateAudio),
        JS_CFUNC_DEF("clear", 0, js_clear),
        JS_CFUNC_DEF("submitCommandBuffer", 1, js_submitCommandBuffer),
        JS_CFUNC_DEF("present", 0, js_present),
        JS_CFUNC_DEF("getRendererStats", 0, js_getRendererStats),
        JS_CFUNC_DEF("onInit", 1, js_onInit),
        JS_CFUNC_DEF("onUpdate", 1, js_onUpdate),
        JS_CFUNC_DEF("onRender", 1, js_onRender),
        JS_CFUNC_DEF("onTouchStart", 1, js_onTouchStart),
        JS_CFUNC_DEF("onTouchMove", 1, js_onTouchMove),
        JS_CFUNC_DEF("onTouchEnd", 1, js_onTouchEnd),
        JS_CFUNC_DEF("onTextInput", 1, js_onTextInput),
        JS_CFUNC_DEF("onKeyDown", 1, js_onKeyDown),
        JS_CFUNC_DEF("onKeyUp", 1, js_onKeyUp),
        JS_CFUNC_DEF("startTextInput", 0, js_startTextInput),
        JS_CFUNC_DEF("stopTextInput", 0, js_stopTextInput),
        JS_CFUNC_DEF("onPause", 1, js_onPause),
        JS_CFUNC_DEF("onResume", 1, js_onResume),
        JS_CFUNC_DEF("onBackground", 1, js_onBackground),
        JS_CFUNC_DEF("onForeground", 1, js_onForeground),
        JS_CFUNC_DEF("onInterruption", 1, js_onInterruption),
        JS_CFUNC_DEF("onLowMemory", 1, js_onLowMemory),
        JS_CFUNC_DEF("onOrientationChange", 1, js_onOrientationChange),
        JS_CFUNC_DEF("onTerminate", 1, js_onTerminate),
};

static int js_sdl3_init(JSContext *ctx, JSModuleDef *m)
{
  if (JS_SetModuleExportList(
          ctx, m, funcs,
          sizeof(funcs) / sizeof(JSCFunctionListEntry)) < 0)
    return -1;
  return JS_SetModuleExport(ctx, m, "isNative", JS_TRUE);
}

JSModuleDef *js_init_module_sdl3(JSContext *ctx, const char *module_name)
{
  JSModuleDef *m = JS_NewCModule(ctx, module_name, js_sdl3_init);
  JS_AddModuleExportList(
      ctx, m, funcs,
      sizeof(funcs) / sizeof(JSCFunctionListEntry));
  JS_AddModuleExport(ctx, m, "isNative");
  return m;
}

/* --- console.log binding --- */
static JSValue js_consoleLog(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  char *message = SDL_strdup("");
  if (!message)
    return JS_UNDEFINED;
  for (int i = 0; i < argc; i++)
  {
    const char *str = JS_ToCString(ctx, argv[i]);
    char *next = NULL;
    SDL_asprintf(&next, "%s%s%s", message, i > 0 ? " " : "", str ? str : "undefined");
    SDL_free(message);
    message = next;
    JS_FreeCString(ctx, str);
    if (!message)
      return JS_UNDEFINED;
  }
  SDL_Log("%s", message);
  SDL_free(message);
  return JS_UNDEFINED;
}

static JSValue js_consoleAssert(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  if (argc > 0 && JS_ToBool(ctx, argv[0]))
  {
    return JS_UNDEFINED;
  }

  printf("Assertion failed");
  for (int i = 1; i < argc; i++)
  {
    const char *str = JS_ToCString(ctx, argv[i]);
    printf("%s%s", i == 1 ? ": " : " ", str ? str : "undefined");
    JS_FreeCString(ctx, str);
  }
  printf("\n");
  fflush(stdout);
  return JS_UNDEFINED;
}

static JSValue js_storage_getItem(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_NULL;
  const char *key = JS_ToCString(ctx, argv[0]);
  if (!key)
    return JS_EXCEPTION;
  StorageEntry *entry = find_storage_entry(key);
  JSValue result = entry ? JS_NewString(ctx, entry->value) : JS_NULL;
  JS_FreeCString(ctx, key);
  return result;
}

static JSValue js_storage_setItem(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 2)
    return JS_UNDEFINED;
  const char *key = JS_ToCString(ctx, argv[0]);
  const char *value = JS_ToCString(ctx, argv[1]);
  if (!key || !value)
  {
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return JS_EXCEPTION;
  }
  StorageEntry *entry = find_storage_entry(key);
  if (!entry)
    entry = find_free_storage_entry();
  if (!entry)
  {
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return JS_ThrowInternalError(ctx, "localStorage is full");
  }
  char *stored_key = entry->key ? NULL : copy_string(key);
  char *stored_value = copy_string(value);
  if ((!entry->key && !stored_key) || !stored_value)
  {
    free(stored_key);
    free(stored_value);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return JS_EXCEPTION;
  }
  free(entry->value);
  entry->key = entry->key ? entry->key : stored_key;
  entry->value = stored_value;
  JS_FreeCString(ctx, key);
  JS_FreeCString(ctx, value);
  return JS_UNDEFINED;
}

static JSValue js_storage_removeItem(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_UNDEFINED;
  const char *key = JS_ToCString(ctx, argv[0]);
  if (!key)
    return JS_EXCEPTION;
  StorageEntry *entry = find_storage_entry(key);
  if (entry)
  {
    free(entry->key);
    free(entry->value);
    memset(entry, 0, sizeof(*entry));
  }
  JS_FreeCString(ctx, key);
  return JS_UNDEFINED;
}

static JSValue js_storage_clear(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
  (void)ctx;
  (void)this_val;
  (void)argc;
  (void)argv;
  for (int i = 0; i < MAX_STORAGE_ENTRIES; i++)
  {
    free(g_storage[i].key);
    free(g_storage[i].value);
    memset(&g_storage[i], 0, sizeof(g_storage[i]));
  }
  return JS_UNDEFINED;
}

static void js_init_local_storage(JSContext *ctx)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue storage = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, storage, "getItem",
                    JS_NewCFunction(ctx, js_storage_getItem, "getItem", 1));
  JS_SetPropertyStr(ctx, storage, "setItem",
                    JS_NewCFunction(ctx, js_storage_setItem, "setItem", 2));
  JS_SetPropertyStr(ctx, storage, "removeItem",
                    JS_NewCFunction(ctx, js_storage_removeItem, "removeItem", 1));
  JS_SetPropertyStr(ctx, storage, "clear",
                    JS_NewCFunction(ctx, js_storage_clear, "clear", 0));
  JS_SetPropertyStr(ctx, global, "localStorage", storage);
  JS_FreeValue(ctx, global);
}

int js_init_console(JSContext *ctx)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, console, "log",
                    JS_NewCFunction(ctx, js_consoleLog, "log", 1));
  JS_SetPropertyStr(ctx, console, "info",
                    JS_NewCFunction(ctx, js_consoleLog, "info", 1));
  JS_SetPropertyStr(ctx, console, "warn",
                    JS_NewCFunction(ctx, js_consoleLog, "warn", 1));
  JS_SetPropertyStr(ctx, console, "error",
                    JS_NewCFunction(ctx, js_consoleLog, "error", 1));
  JS_SetPropertyStr(ctx, console, "assert",
                    JS_NewCFunction(ctx, js_consoleAssert, "assert", 1));
  JS_SetPropertyStr(ctx, global, "console", console);
  JS_FreeValue(ctx, global);
  return 0;
}

int js_init_sdl3(JSContext *ctx)
{
  if (!g_pending_destroy_texture_mutex)
    g_pending_destroy_texture_mutex = SDL_CreateMutex();
  if (!g_pending_destroy_texture_mutex)
    return -1;
  js_init_module_sdl3(ctx, "sdl3");
#ifdef JS_SDL_ENABLE_BOX2D_MODULE
  js_init_box2d(ctx);
#endif
  js_init_console(ctx);
  js_init_local_storage(ctx);
  return 0;
}

void js_sdl3_shutdown(JSContext *ctx)
{
#ifdef JS_SDL_ENABLE_BOX2D_MODULE
  js_box2d_shutdown();
#endif

  JS_FreeValue(ctx, g_onInit);
  JS_FreeValue(ctx, g_onUpdate);
  JS_FreeValue(ctx, g_onRender);
  JS_FreeValue(ctx, g_touchStart);
  JS_FreeValue(ctx, g_touchMove);
  JS_FreeValue(ctx, g_touchEnd);
  JS_FreeValue(ctx, g_textInput);
  JS_FreeValue(ctx, g_keyDown);
  JS_FreeValue(ctx, g_keyUp);
  JS_FreeValue(ctx, g_onPause);
  JS_FreeValue(ctx, g_onResume);
  JS_FreeValue(ctx, g_onBackground);
  JS_FreeValue(ctx, g_onForeground);
  JS_FreeValue(ctx, g_onInterruption);
  JS_FreeValue(ctx, g_onLowMemory);
  JS_FreeValue(ctx, g_onOrientationChange);
  JS_FreeValue(ctx, g_onTerminate);
  g_onInit = g_onUpdate = g_onRender = JS_UNDEFINED;
  g_touchStart = g_touchMove = g_touchEnd = JS_UNDEFINED;
  g_textInput = g_keyDown = g_keyUp = JS_UNDEFINED;
  g_onPause = g_onResume = JS_UNDEFINED;
  g_onBackground = g_onForeground = JS_UNDEFINED;
  g_onInterruption = g_onLowMemory = JS_UNDEFINED;
  g_onOrientationChange = g_onTerminate = JS_UNDEFINED;

  js_storage_clear(ctx, JS_UNDEFINED, 0, NULL);

  for (int i = 0; i < MAX_TEXTURES; i++)
  {
    if (g_textures[i].texture)
    {
      destroy_texture_on_main_thread(g_textures[i].texture);
      free(g_textures[i].key);
      memset(&g_textures[i], 0, sizeof(g_textures[i]));
    }
  }
  for (int i = 0; i < MAX_FONTS; i++)
  {
    if (g_fonts[i].face)
    {
      FT_Done_Face(g_fonts[i].face);
      SDL_free(g_fonts[i].data);
      free(g_fonts[i].path);
      memset(&g_fonts[i], 0, sizeof(g_fonts[i]));
    }
  }
  for (int i = 0; i < MAX_AUDIO_VOICES; i++)
  {
    if (g_audio_voices[i].stream)
    {
      SDL_DestroyAudioStream(g_audio_voices[i].stream);
      memset(&g_audio_voices[i], 0, sizeof(g_audio_voices[i]));
    }
  }
  for (int i = 0; i < MAX_AUDIO_ASSETS; i++)
  {
    if (g_audio_assets[i].data)
    {
      SDL_free(g_audio_assets[i].data);
      free(g_audio_assets[i].path);
      memset(&g_audio_assets[i], 0, sizeof(g_audio_assets[i]));
    }
  }
  SDL_free(g_mesh_vertices);
  SDL_free(g_mesh_indices);
  SDL_free(g_input_vertices);
  SDL_free(g_input_indices);
  g_mesh_vertices = NULL;
  g_mesh_indices = NULL;
  g_input_vertices = NULL;
  g_input_indices = NULL;
  g_mesh_vertex_capacity = 0;
  g_mesh_index_capacity = 0;
  g_input_vertex_capacity = 0;
  g_input_index_capacity = 0;
  free_idle_audio_streams();
  if (g_atom_commands != JS_ATOM_NULL)
  {
    JS_FreeAtom(ctx, g_atom_commands);
    JS_FreeAtom(ctx, g_atom_floatBuffer);
    JS_FreeAtom(ctx, g_atom_uintBuffer);
    JS_FreeAtom(ctx, g_atom_shortBuffer);
    g_atom_commands = JS_ATOM_NULL;
    g_atom_floatBuffer = JS_ATOM_NULL;
    g_atom_uintBuffer = JS_ATOM_NULL;
    g_atom_shortBuffer = JS_ATOM_NULL;
  }
  flush_deferred_texture_destroys();
  if (g_pending_destroy_texture_mutex)
  {
    SDL_DestroyMutex(g_pending_destroy_texture_mutex);
    g_pending_destroy_texture_mutex = NULL;
  }

  if (g_renderer)
  {
    js_run_on_main_thread(destroy_renderer_main_thread, NULL);
  }
  if (g_window)
  {
    js_run_on_main_thread(destroy_window_main_thread, NULL);
  }
  if (g_ft_library)
  {
    FT_Done_FreeType(g_ft_library);
    g_ft_library = NULL;
  }
}

void js_execute_pending_job(JSRuntime *rt)
{
  int jobs = 0;
  for (;;)
  {
    JSContext *job_ctx = NULL;
    int status = JS_ExecutePendingJob(rt, &job_ctx);
    if (status > 0)
    {
      jobs++;
      continue;
    }
    if (status < 0 && job_ctx)
      js_print_exception(job_ctx);
    if (jobs > 0)
      SDL_Log("Executed %d pending JavaScript job(s)", jobs);
    return;
  }
}

void js_set_frame_timing(float delta_time)
{
  g_frame_time_ms = (double)delta_time * 1000.0;
  g_fps = delta_time > 0.0f ? 1.0 / (double)delta_time : 0.0;
}

bool js_enable_render_queue(void)
{
  if (!g_render_queue_mutex)
  {
    g_render_queue_mutex = SDL_CreateMutex();
    g_render_queue_ready = SDL_CreateCondition();
  }
  if (!g_render_queue_mutex || !g_render_queue_ready)
    return false;
  SDL_LockMutex(g_render_queue_mutex);
  g_render_queue_enabled = true;
  SDL_UnlockMutex(g_render_queue_mutex);
  return true;
}

void js_disable_render_queue(void)
{
  if (!g_render_queue_mutex)
    return;
  SDL_LockMutex(g_render_queue_mutex);
  g_render_queue_enabled = false;
  SDL_SignalCondition(g_render_queue_ready);
  SDL_UnlockMutex(g_render_queue_mutex);
}

void js_destroy_render_queue(void)
{
  if (!g_render_queue_mutex)
    return;
  js_disable_render_queue();
  SDL_DestroyCondition(g_render_queue_ready);
  SDL_DestroyMutex(g_render_queue_mutex);
  g_render_queue_ready = NULL;
  g_render_queue_mutex = NULL;
  for (int i = 0; i < RENDER_BUFFER_COUNT; i++)
  {
    free_render_frame(&g_render_buffers[i]);
  }
}

bool js_render_pending_frame(void)
{
  flush_deferred_texture_destroys();
  if (!g_render_queue_enabled || !g_render_queue_mutex)
    return false;

  SDL_LockMutex(g_render_queue_mutex);
  bool waited_for_frame = false;
find_newest_frame:
  ;
  int newest_idx = -1;
  Uint64 newest_time = 0;

  for (int i = 0; i < RENDER_BUFFER_COUNT; i++)
  {
    if (g_render_buffers[i].state == RENDER_BUFFER_READY)
    {
      if (newest_idx < 0 || g_render_buffers[i].timestamp_ns > newest_time)
      {
        if (newest_idx >= 0)
        {
          g_render_buffers[newest_idx].state = RENDER_BUFFER_FREE;
#if JS_SDL_ENABLE_PROFILING
          js_prof_record_dropped_frame();
#endif
        }
        newest_idx = i;
        newest_time = g_render_buffers[i].timestamp_ns;
      }
      else
      {
        g_render_buffers[i].state = RENDER_BUFFER_FREE;
#if JS_SDL_ENABLE_PROFILING
        js_prof_record_dropped_frame();
#endif
      }
    }
  }

  if (newest_idx < 0)
  {
    if (waited_for_frame)
    {
      SDL_UnlockMutex(g_render_queue_mutex);
      return false;
    }
    waited_for_frame = true;
    SDL_WaitConditionTimeout(g_render_queue_ready, g_render_queue_mutex, 1);
    if (!g_render_queue_enabled)
    {
      SDL_UnlockMutex(g_render_queue_mutex);
      return false;
    }
    goto find_newest_frame;
  }

  RenderFrame *frame = &g_render_buffers[newest_idx];
  frame->state = RENDER_BUFFER_READING;
  SDL_UnlockMutex(g_render_queue_mutex);

#if JS_SDL_ENABLE_PROFILING
  static Uint64 last_render_ticks = 0;
  Uint64 now_ticks = SDL_GetTicksNS();
  Uint64 interval_ns = last_render_ticks > 0 ? now_ticks - last_render_ticks : 0;
  last_render_ticks = now_ticks;

  Uint64 exec_start_ns = SDL_GetTicksNS();
#endif

  g_executing_render_frame = frame;
  js_clear(NULL, JS_UNDEFINED, 0, NULL);
  js_submitCommandBuffer(NULL, JS_UNDEFINED, 0, NULL);

#if JS_SDL_ENABLE_PROFILING
  Uint64 exec_end_ns = SDL_GetTicksNS();
  Uint64 present_start_ns = exec_end_ns;
#endif

  js_present(NULL, JS_UNDEFINED, 0, NULL);

#if JS_SDL_ENABLE_PROFILING
  Uint64 present_end_ns = SDL_GetTicksNS();
  uint32_t cmd_count = (uint32_t)(frame->commands_len / sizeof(int32_t));
  js_prof_record_render(
      exec_end_ns - exec_start_ns,
      present_end_ns - present_start_ns,
      cmd_count,
      interval_ns);
#endif

  g_executing_render_frame = NULL;

  SDL_LockMutex(g_render_queue_mutex);
  frame->state = RENDER_BUFFER_FREE;
  SDL_UnlockMutex(g_render_queue_mutex);

  return true;
}

/* --- public API for main.c --- */
void js_call_onInit(JSContext *ctx) { js_call_void(ctx, g_onInit); }
void js_call_onUpdate(JSContext *ctx) { js_call_void(ctx, g_onUpdate); }
void js_call_onUpdate_dt(JSContext *ctx, float dt)
{
  if (JS_IsUndefined(g_onUpdate))
    return;
  JSValue dt_val = JS_NewFloat64(ctx, (double)dt);
  JSValue ret = JS_Call(ctx, g_onUpdate, JS_UNDEFINED, 1, &dt_val);
  JS_FreeValue(ctx, dt_val);
  js_complete_callback(ctx, ret);
}
void js_call_onRender(JSContext *ctx) { js_call_void(ctx, g_onRender); }

void js_call_touchStart(JSContext *ctx, float x, float y)
{
  js_call_touch(ctx, g_touchStart, x, y);
}
void js_call_touchMove(JSContext *ctx, float x, float y)
{
  js_call_touch(ctx, g_touchMove, x, y);
}
void js_call_touchEnd(JSContext *ctx, float x, float y)
{
  js_call_touch(ctx, g_touchEnd, x, y);
}
void js_call_textInput(JSContext *ctx, const char *text)
{
  js_call_string(ctx, g_textInput, text);
}
void js_call_keyDown(JSContext *ctx, const char *key)
{
  js_call_string(ctx, g_keyDown, key);
}
void js_call_keyUp(JSContext *ctx, const char *key)
{
  js_call_string(ctx, g_keyUp, key);
}
void js_call_pause(JSContext *ctx)
{
  js_call_void(ctx, g_onPause);
  if (ctx)
    JS_RunGC(JS_GetRuntime(ctx));
}
void js_call_resume(JSContext *ctx) { js_call_void(ctx, g_onResume); }
void js_call_background(JSContext *ctx) { js_call_void(ctx, g_onBackground); }
void js_call_foreground(JSContext *ctx) { js_call_void(ctx, g_onForeground); }
void js_call_interruption(JSContext *ctx, int active)
{
  js_call_bool(ctx, g_onInterruption, active);
}
void js_call_low_memory(JSContext *ctx)
{
  js_call_void(ctx, g_onLowMemory);
  if (ctx)
    JS_RunGC(JS_GetRuntime(ctx));
}
void js_call_orientation_change(
    JSContext *ctx, SDL_DisplayOrientation orientation, int width, int height)
{
  js_call_orientation(
      ctx, g_onOrientationChange, orientation, width, height);
}
void js_call_terminate(JSContext *ctx) { js_call_void(ctx, g_onTerminate); }

void js_get_window_size(int *width, int *height)
{
  get_window_size_in_pixels(width, height);
}

int js_get_win_w(void) { return g_win_w; }
int js_get_win_h(void) { return g_win_h; }

void js_convert_event_to_render_coordinates(SDL_Event *event)
{
  if (g_renderer)
  {
    SDL_ConvertEventToRenderCoordinates(g_renderer, event);
  }
}
